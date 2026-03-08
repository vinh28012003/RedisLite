"""
Integration tests for WAIT command (stages 16-18).
Uses function-scoped fixtures for tests that need specific replica counts.
"""
import socket
import subprocess
import time
import pytest

from conftest import wait_for_ready, _kill_container_replica

MASTER_HOST = "127.0.0.1"
MASTER_PORT = 6379

# Dedicated ports for WAIT tests — avoids collision with session-scoped replicas
WAIT_REPLICA_1 = 6383
WAIT_REPLICA_2 = 6384


def send_command(host, port, *args, timeout=2):
    """Send RESP array command and return raw response."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{str(arg)}\r\n"

    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(cmd.encode())
        return s.recv(4096).decode()


def send_command_long(host, port, *args, timeout=5):
    """Send command with longer timeout (for WAIT that may block)."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{str(arg)}\r\n"

    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(cmd.encode())
        s.settimeout(timeout)
        return s.recv(4096).decode()


def _start_wait_replica(port):
    """Start a replica for WAIT tests. Returns (proc,) for cleanup."""
    _kill_container_replica(port)
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "./build/redis-lite", "--port", str(port),
         "--replicaof", "localhost", "6379"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    # Wait for replica's listener to be ready (handshake completes before run())
    wait_for_ready(MASTER_HOST, port)
    time.sleep(0.3)  # Extra settling time for master to tag REPLICA
    return proc


def _stop_wait_replica(port, proc):
    """Kill replica inside container and host-side process."""
    _kill_container_replica(port)
    proc.kill()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


@pytest.fixture
def one_replica():
    """Function-scoped: start 1 fresh replica, cleanup after test."""
    proc = _start_wait_replica(WAIT_REPLICA_1)
    yield
    _stop_wait_replica(WAIT_REPLICA_1, proc)


@pytest.fixture
def two_replicas():
    """Function-scoped: start 2 fresh replicas, cleanup after test."""
    proc1 = _start_wait_replica(WAIT_REPLICA_1)
    proc2 = _start_wait_replica(WAIT_REPLICA_2)
    yield
    _stop_wait_replica(WAIT_REPLICA_1, proc1)
    _stop_wait_replica(WAIT_REPLICA_2, proc2)


class TestWaitNoReplicas:
    """Stage 16: WAIT with no replicas connected — runs before any replica fixtures."""

    def test_wait_zero_returns_zero(self):
        """WAIT 0 0 → fast path, returns 0 or current caught-up count."""
        resp = send_command(MASTER_HOST, MASTER_PORT, "WAIT", "0", "0")
        # WAIT 0 always succeeds (need 0 replicas)
        count = int(resp.strip().lstrip(":"))
        assert count >= 0

    def test_wait_error_missing_args(self):
        """WAIT with missing args → error."""
        resp = send_command(MASTER_HOST, MASTER_PORT, "WAIT")
        assert resp.startswith("-ERR")

    def test_wait_error_non_numeric(self):
        """WAIT with non-numeric args → error."""
        resp = send_command(MASTER_HOST, MASTER_PORT, "WAIT", "abc", "def")
        assert resp.startswith("-ERR")


class TestWaitNoCommands:
    """Stage 17: WAIT with replicas connected but no write commands propagated to them."""

    def test_wait_one_replica_no_commands(self, one_replica):
        """One fresh replica, no writes → caught up → returns >= 1."""
        resp = send_command_long(MASTER_HOST, MASTER_PORT, "WAIT", "1", "500")
        count = int(resp.strip().lstrip(":"))
        assert count >= 1

    def test_wait_two_replicas_no_commands(self, two_replicas):
        """Two fresh replicas, no writes → both caught up → returns >= 2."""
        resp = send_command_long(MASTER_HOST, MASTER_PORT, "WAIT", "2", "500")
        count = int(resp.strip().lstrip(":"))
        assert count >= 2


class TestWaitWithCommands:
    """Stage 18: WAIT after write commands — exercises full GETACK → ACK → resolve flow."""

    def test_wait_after_single_set(self, one_replica):
        """SET then WAIT → replica ACKs → returns >= 1."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wt_key1", "val1")
        resp = send_command_long(MASTER_HOST, MASTER_PORT, "WAIT", "1", "1000")
        count = int(resp.strip().lstrip(":"))
        assert count >= 1

    def test_wait_after_multiple_sets(self, one_replica):
        """Multiple SETs then WAIT → replica ACKs all → returns >= 1."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wt_key2", "aaa")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wt_key3", "bbb")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wt_key4", "ccc")
        resp = send_command_long(MASTER_HOST, MASTER_PORT, "WAIT", "1", "1000")
        count = int(resp.strip().lstrip(":"))
        assert count >= 1

    def test_wait_two_replicas_after_set(self, two_replicas):
        """Two replicas, SET then WAIT 2 → both ACK → returns >= 2."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wt_key5", "multi")
        resp = send_command_long(MASTER_HOST, MASTER_PORT, "WAIT", "2", "1000")
        count = int(resp.strip().lstrip(":"))
        assert count >= 2

    def test_wait_timeout_returns_actual_count(self, one_replica):
        """WAIT 99 with 1 replica → timeout → returns actual count (< 99)."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wt_key6", "timeout")
        start = time.time()
        resp = send_command_long(MASTER_HOST, MASTER_PORT, "WAIT", "99", "500")
        elapsed = time.time() - start
        count = int(resp.strip().lstrip(":"))
        assert count >= 1      # at least 1 replica ACK'd
        assert count < 99      # can't reach 99
        assert elapsed >= 0.4  # waited near the timeout

    def test_wait_returns_integer_format(self, one_replica):
        """WAIT response is RESP integer format :<N>\\r\\n."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wt_fmt", "x")
        resp = send_command_long(MASTER_HOST, MASTER_PORT, "WAIT", "1", "1000")
        assert resp.startswith(":")
        assert resp.endswith("\r\n")
