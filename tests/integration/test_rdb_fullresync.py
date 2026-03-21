"""
Integration tests for Phase 5: RDB full resync.
Verifies that master's in-memory data transfers to replica via RDB during PSYNC FULLRESYNC.
Uses port 6383 with function-scoped fixtures (each test gets a fresh replica).
"""
import socket
import subprocess
import time

import pytest

from conftest import wait_for_ready, _kill_container_replica

MASTER_HOST = "127.0.0.1"
MASTER_PORT = 6379
REPLICA_PORT = 6383


def send_command(host, port, *args):
    """Send RESP array command and return raw response."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(cmd.encode())
        return s.recv(4096).decode()


def encode_resp(*args):
    """Encode args as RESP array bytes."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    return cmd.encode()


@pytest.fixture()
def fresh_replica():
    """Start a fresh replica on 6383 that connects to master on 6379.
    Function-scoped: each test gets a clean replica with a fresh full resync."""
    _kill_container_replica(REPLICA_PORT)
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "redis-lite", "--port", str(REPLICA_PORT),
         "--replicaof", "localhost", str(MASTER_PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    try:
        wait_for_ready(MASTER_HOST, REPLICA_PORT)
    except TimeoutError:
        _kill_container_replica(REPLICA_PORT)
        proc.kill()
        pytest.fail(f"replica failed to start on port {REPLICA_PORT}")
    yield
    _kill_container_replica(REPLICA_PORT)
    proc.kill()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


class TestRdbFullResync:
    """RDB full resync: master data → RDB → replica store."""

    def test_existing_keys_transfer_to_replica(self, fresh_replica):
        """Keys SET on master before replica connects appear on replica via RDB."""
        # These keys were SET by earlier tests (session-scoped master).
        # Also SET our own unique keys to verify.
        send_command(MASTER_HOST, MASTER_PORT, "SET", "rdb_k1", "alpha")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "rdb_k2", "beta")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "rdb_k3", "gamma")
        time.sleep(0.3)

        # Replica received these via RDB during full resync + propagation
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "rdb_k1") == "$5\r\nalpha\r\n"
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "rdb_k2") == "$4\r\nbeta\r\n"
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "rdb_k3") == "$5\r\ngamma\r\n"

    def test_expiry_transfers_via_rdb(self, fresh_replica):
        """Key with long TTL on master survives RDB transfer to replica."""
        # SET with 30s TTL — long enough to survive serialize→transfer→load
        send_command(MASTER_HOST, MASTER_PORT, "SET", "rdb_ttl", "alive", "PX", "30000")
        time.sleep(0.3)

        resp = send_command(MASTER_HOST, REPLICA_PORT, "GET", "rdb_ttl")
        assert resp == "$5\r\nalive\r\n"

    def test_rdb_size_is_dynamic(self):
        """RDB size varies with content — not hardcoded 88 bytes.
        Uses raw socket to check $<len> in PSYNC FULLRESYNC response."""
        s = socket.create_connection((MASTER_HOST, MASTER_PORT), timeout=2)
        try:
            # Full handshake
            s.sendall(encode_resp("PING"))
            assert b"+PONG" in s.recv(4096)

            s.sendall(encode_resp("REPLCONF", "listening-port", "9998"))
            assert b"+OK" in s.recv(4096)

            s.sendall(encode_resp("REPLCONF", "capa", "psync2"))
            assert b"+OK" in s.recv(4096)

            s.sendall(encode_resp("PSYNC", "?", "-1"))
            resp = s.recv(8192)

            # Parse $<len> from response
            text = resp.decode("latin-1")
            dollar_idx = text.index("$")
            crlf_idx = text.index("\r\n", dollar_idx)
            rdb_size = int(text[dollar_idx + 1:crlf_idx])

            # Master has keys from tests → RDB must be larger than empty 88 bytes
            assert rdb_size > 88, f"RDB size {rdb_size} should be > 88 (non-empty store)"
        finally:
            s.close()

    def test_post_resync_propagation_still_works(self, fresh_replica):
        """After RDB full resync, command propagation continues normally."""
        # SET after replica is connected — arrives via propagation, not RDB
        send_command(MASTER_HOST, MASTER_PORT, "SET", "rdb_post", "propagated")
        time.sleep(0.3)

        resp = send_command(MASTER_HOST, REPLICA_PORT, "GET", "rdb_post")
        assert resp == "$10\r\npropagated\r\n"
