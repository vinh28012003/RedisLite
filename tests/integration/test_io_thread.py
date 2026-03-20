"""
Integration tests for IO thread replica fan-out.
Covers: rapid connect/disconnect, large RDB transfer, sustained load,
GETACK delivery, concurrent propagation, writer thread shutdown.
"""
import re
import socket
import subprocess
import threading
import time

import pytest

MASTER_HOST = "127.0.0.1"
MASTER_PORT = 6379


# --- Helpers ---

def send_command(host, port, *args, timeout=3):
    """Send RESP array command and return decoded response."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(cmd.encode())
        return s.recv(65536).decode()


def send_command_raw(host, port, *args, timeout=3):
    """Send RESP array command and return raw bytes."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(cmd.encode())
        return s.recv(65536)


def encode_resp(*args):
    """Encode args as RESP array bytes."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    return cmd.encode()


def wait_for_ready(host, port, timeout=5.0):
    """Block until server responds to PING with +PONG."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            with socket.create_connection((host, port), timeout=0.5) as s:
                s.sendall(b"*1\r\n$4\r\nPING\r\n")
                if s.recv(1024) == b"+PONG\r\n":
                    return True
        except OSError:
            pass
        time.sleep(0.1)
    raise TimeoutError(f"Server not ready on {host}:{port} after {timeout}s")


def kill_server(port, proc=None):
    """Kill redis-lite on given port inside container."""
    subprocess.run(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "bash", "-c", f"pkill -f 'redis-lite.*--port {port}' 2>/dev/null || true"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5
    )
    if proc:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    time.sleep(0.3)


def start_server(port, replicaof=None):
    """Start redis-lite in container. Returns subprocess.Popen."""
    kill_server(port)
    cmd = ["docker", "compose", "-f", "docker/docker-compose.yml",
           "exec", "-T", "redis-lite",
           "./build/redis-lite", "--port", str(port)]
    if replicaof:
        cmd += ["--replicaof", replicaof[0], str(replicaof[1])]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_ready(MASTER_HOST, port)
    except TimeoutError:
        kill_server(port, proc)
        pytest.fail(f"Server failed to start on port {port}")
    return proc


# --- Fixtures ---

@pytest.fixture
def replica_6383():
    proc = start_server(6383, replicaof=("localhost", 6379))
    yield
    kill_server(6383, proc)


@pytest.fixture
def replica_6384():
    proc = start_server(6384, replicaof=("localhost", 6379))
    yield
    kill_server(6384, proc)


# --- Group 1: Rapid connect/disconnect ---

class TestRapidConnectDisconnect:
    """Stress the repl_queues_ map with rapid replica churn."""

    def test_rapid_replica_churn_master_survives(self, redis_server):
        """Start and stop replicas rapidly — master stays responsive."""
        for i in range(4):
            proc = start_server(6383, replicaof=("localhost", 6379))
            # Write while replica is connected
            resp = send_command(MASTER_HOST, MASTER_PORT, "SET", f"churn_{i}", f"v{i}")
            assert "+OK" in resp
            time.sleep(0.3)
            kill_server(6383, proc)

        # Master should still work after all the churn
        resp = send_command(MASTER_HOST, MASTER_PORT, "SET", "post_churn", "alive")
        assert "+OK" in resp
        resp = send_command(MASTER_HOST, MASTER_PORT, "GET", "post_churn")
        assert "alive" in resp

    def test_multiple_replicas_connect_disconnect_simultaneously(self, redis_server):
        """Connect 3 replicas, disconnect all at once, master survives."""
        procs = []
        for port in [6381, 6382, 6383]:
            procs.append((port, start_server(port, replicaof=("localhost", 6379))))
        time.sleep(0.5)

        # Write while all connected
        send_command(MASTER_HOST, MASTER_PORT, "SET", "multi_repl_key", "value")
        time.sleep(0.5)

        # Verify all replicas got the data
        for port, _ in procs:
            resp = send_command(MASTER_HOST, port, "GET", "multi_repl_key")
            assert "value" in resp, f"Replica on {port} missing key"

        # Kill all at once
        for port, proc in procs:
            kill_server(port, proc)

        # Master still works
        resp = send_command(MASTER_HOST, MASTER_PORT, "PING")
        assert "+PONG" in resp

    def test_disconnect_during_propagation(self, redis_server, replica_6383):
        """Kill replica while master is writing — master doesn't crash."""
        # Start a burst of writes
        for i in range(50):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"burst_{i}", f"v{i}")

        # Kill replica mid-stream
        kill_server(6383)

        # More writes after disconnect — master must stay alive
        for i in range(10):
            resp = send_command(MASTER_HOST, MASTER_PORT, "SET", f"after_kill_{i}", f"v{i}")
            assert "+OK" in resp


# --- Group 2: Large RDB transfer via writer thread ---

class TestLargeRdbTransfer:
    """FULLRESYNC with non-trivial RDB goes through writer thread."""

    def test_500_keys_transfer_to_replica(self, redis_server):
        """SET 500 keys on master, connect replica, verify all arrive via FULLRESYNC."""
        # Populate master
        for i in range(500):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"rdb_key_{i}", f"rdb_val_{i}")
        time.sleep(0.3)

        # Connect replica — FULLRESYNC sends all 500 keys via writer thread
        proc = start_server(6383, replicaof=("localhost", 6379))
        try:
            time.sleep(1)  # allow RDB transfer + load

            # Spot-check keys
            for i in [0, 99, 249, 499]:
                resp = send_command(MASTER_HOST, 6383, "GET", f"rdb_key_{i}")
                assert f"rdb_val_{i}" in resp, f"Key rdb_key_{i} missing on replica"
        finally:
            kill_server(6383, proc)

    def test_large_values_transfer(self, redis_server):
        """Keys with 10KB values transfer correctly via writer thread."""
        big_value = "X" * 10000
        for i in range(20):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"big_{i}", big_value)
        time.sleep(0.3)

        proc = start_server(6383, replicaof=("localhost", 6379))
        try:
            time.sleep(1)

            # Verify value length — use non-blocking recv loop
            with socket.create_connection((MASTER_HOST, 6383), timeout=3) as s:
                s.sendall(encode_resp("GET", "big_0"))
                data = b""
                # Read until we have the full RESP bulk string
                while b"\r\n" not in data:
                    data += s.recv(65536)
                # Parse $<len>\r\n header
                header_end = data.index(b"\r\n")
                body_len = int(data[1:header_end])
                total_needed = header_end + 2 + body_len + 2  # header + body + trailing \r\n
                while len(data) < total_needed:
                    data += s.recv(65536)
                assert body_len == len(big_value)
                body = data[header_end + 2:header_end + 2 + body_len]
                assert body == big_value.encode()
        finally:
            kill_server(6383, proc)


# --- Group 3: Sustained load ---

class TestSustainedLoad:
    """Writer thread handles high-throughput sustained propagation."""

    def test_10k_pipeline_propagation(self, redis_server, replica_6383):
        """Pipeline 10,000 SETs — last key arrives on replica."""
        # Build pipelined batch
        batch = b""
        for i in range(10000):
            batch += encode_resp("SET", f"pipe_{i}", f"v{i}")

        # Send as one TCP write (pipelining)
        with socket.create_connection((MASTER_HOST, MASTER_PORT), timeout=10) as s:
            s.sendall(batch)
            # Drain all +OK responses
            received = b""
            while received.count(b"+OK") < 10000:
                chunk = s.recv(65536)
                if not chunk:
                    break
                received += chunk

        assert received.count(b"+OK") == 10000

        # Wait for propagation
        time.sleep(2)

        # Verify first and last key on replica
        resp = send_command(MASTER_HOST, 6383, "GET", "pipe_0")
        assert "v0" in resp, "First pipelined key missing"

        resp = send_command(MASTER_HOST, 6383, "GET", "pipe_9999")
        assert "v9999" in resp, "Last pipelined key missing"

    def test_sustained_writes_with_two_replicas(self, redis_server, replica_6383, replica_6384):
        """1000 SETs with 2 replicas — both receive all data."""
        for i in range(1000):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"sust_{i}", f"v{i}")
        time.sleep(1.5)

        # Spot-check both replicas
        for port in [6383, 6384]:
            for idx in [0, 499, 999]:
                resp = send_command(MASTER_HOST, port, "GET", f"sust_{idx}")
                assert f"v{idx}" in resp, f"Key sust_{idx} missing on replica {port}"


# --- Group 4: GETACK delivery via writer thread ---

class TestGetackViaWriter:
    """REPLCONF GETACK * now goes through writer thread, not write_buf."""

    def test_wait_still_works_with_io_thread(self, redis_server, replica_6383):
        """WAIT resolves — proves GETACK arrives at replica via writer thread."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wait_io_key", "val")
        resp = send_command(MASTER_HOST, MASTER_PORT, "WAIT", "1", "2000")
        count = int(resp.strip().lstrip(":").split("\r\n")[0])
        assert count >= 1, f"WAIT should return ≥1, got {count}"

    def test_wait_with_multiple_replicas_via_writer(self, redis_server, replica_6383, replica_6384):
        """WAIT 2 with 2 replicas — both ACK via writer thread."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wait_multi_io", "val")
        resp = send_command(MASTER_HOST, MASTER_PORT, "WAIT", "2", "2000")
        count = int(resp.strip().lstrip(":").split("\r\n")[0])
        assert count >= 2, f"WAIT should return ≥2, got {count}"

    def test_wait_timeout_returns_partial_count(self, redis_server, replica_6383):
        """WAIT with unreachable count times out with partial count."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wait_timeout_io", "val")
        # Request 99 replicas — should timeout with partial count (fewer than 99)
        resp = send_command(MASTER_HOST, MASTER_PORT, "WAIT", "99", "500", timeout=5)
        count = int(resp.strip().lstrip(":").split("\r\n")[0])
        assert 1 <= count < 99, f"Expected partial count 1-98, got {count}"


# --- Group 5: Concurrent reads and writes ---

class TestConcurrentReadsWrites:
    """Main thread handles client reads while writer thread sends to replicas."""

    def test_reads_not_blocked_during_propagation(self, redis_server, replica_6383):
        """GET from master responds quickly while propagation is in-flight."""
        # Pre-populate a key
        send_command(MASTER_HOST, MASTER_PORT, "SET", "read_key", "read_val")
        time.sleep(0.3)

        # Start bulk writes in background thread
        errors = []

        def bulk_write():
            try:
                for i in range(500):
                    send_command(MASTER_HOST, MASTER_PORT, "SET", f"bg_{i}", f"v{i}")
            except Exception as e:
                errors.append(str(e))

        t = threading.Thread(target=bulk_write)
        t.start()

        # Concurrent GETs should still be fast
        for _ in range(10):
            resp = send_command(MASTER_HOST, MASTER_PORT, "GET", "read_key")
            assert "read_val" in resp

        t.join(timeout=30)
        assert not t.is_alive()
        assert not errors, f"Bulk write errors: {errors}"

    def test_new_replica_connects_during_load(self, redis_server):
        """Connect a replica while master is under write load."""
        errors = []

        def sustained_write():
            try:
                for i in range(500):
                    send_command(MASTER_HOST, MASTER_PORT, "SET", f"during_load_{i}", f"v{i}")
                    time.sleep(0.002)  # ~2ms between writes
            except Exception as e:
                errors.append(str(e))

        t = threading.Thread(target=sustained_write)
        t.start()
        time.sleep(0.2)  # let some writes happen first

        # Connect replica mid-load — triggers FULLRESYNC through writer thread
        proc = start_server(6383, replicaof=("localhost", 6379))
        try:
            t.join(timeout=30)
            assert not errors, f"Write errors: {errors}"

            # Poll for the last key instead of fixed sleep — CI runners may be slow
            deadline = time.time() + 10
            while time.time() < deadline:
                resp = send_command(MASTER_HOST, 6383, "GET", "during_load_499")
                if "v499" in resp:
                    break
                time.sleep(0.5)
            assert "v499" in resp, "Last key should be on replica"
        finally:
            kill_server(6383, proc)


# --- Group 6: Graceful shutdown with active replicas ---

class TestShutdownWithReplicas:
    """Writer thread shuts down cleanly when server stops."""

    def test_sigterm_with_active_replicas(self, redis_server):
        """SIGTERM with connected replicas — server exits cleanly, no hang."""
        # Start a standalone server on 6383 with a replica on 6384
        proc_master = start_server(6383)
        proc_replica = start_server(6384, replicaof=("localhost", 6383))
        try:
            # Write some data
            send_command(MASTER_HOST, 6383, "SET", "shutdown_key", "val")
            time.sleep(0.5)

            # Send SIGTERM to master — find PID first to avoid pkill matching bash
            subprocess.run(
                ["docker", "compose", "-f", "docker/docker-compose.yml",
                 "exec", "-T", "redis-lite",
                 "bash", "-c",
                 "pid=$(pgrep -f 'redis-lite.*--port 6383' | head -1) && [ -n \"$pid\" ] && kill -TERM $pid || true"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5
            )

            # Master should stop within 3 seconds (writer thread joins)
            time.sleep(3)

            # Master should be gone — connection refused or timeout
            gone = False
            try:
                with socket.create_connection((MASTER_HOST, 6383), timeout=1) as s:
                    s.sendall(b"*1\r\n$4\r\nPING\r\n")
                    resp = s.recv(1024)
                    if not resp:
                        gone = True
            except OSError:
                gone = True
            assert gone, "Master on 6383 should have stopped after SIGTERM"
        finally:
            kill_server(6384, proc_replica)
            kill_server(6383, proc_master)
