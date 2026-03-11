"""
Integration tests for ROLE, REPLICAOF, and replica-read-only (Phase 2).
Uses function-scoped replicas since REPLICAOF changes server state.
"""
import socket
import time
import subprocess
import pytest

MASTER_HOST = "127.0.0.1"
MASTER_PORT = 6379
REPLICAOF_PORT = 6383  # Dedicated port — avoids conflict with other test replicas


def send_command(host, port, *args):
    """Send RESP array command and return raw response."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(arg)}\r\n{arg}\r\n"

    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(cmd.encode())
        return s.recv(4096).decode()


def wait_for_ready(host, port, timeout=5.0):
    """Block until server responds to PING with +PONG."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            with socket.create_connection((host, port), timeout=0.5) as s:
                s.sendall(b"*1\r\n$4\r\nPING\r\n")
                response = s.recv(1024)
                if response == b"+PONG\r\n":
                    return True
        except OSError:
            pass
        time.sleep(0.1)
    raise TimeoutError(f"Server not ready on {host}:{port} after {timeout}s")


def _kill_replica(port):
    subprocess.run(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "bash", "-c", f"pkill -f 'redis-lite.*{port}' 2>/dev/null || true"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5
    )
    time.sleep(0.5)


@pytest.fixture
def replica_6383():
    """Function-scoped replica on port 6383 — fresh for each test."""
    _kill_replica(REPLICAOF_PORT)
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "./build/redis-lite", "--port", str(REPLICAOF_PORT),
         "--replicaof", "localhost", "6379"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    try:
        wait_for_ready(MASTER_HOST, REPLICAOF_PORT)
    except TimeoutError:
        _kill_replica(REPLICAOF_PORT)
        proc.kill()
        pytest.fail(f"replica failed to start on port {REPLICAOF_PORT}")
    yield
    _kill_replica(REPLICAOF_PORT)
    proc.kill()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


# --- ROLE command ---

class TestRole:
    def test_role_on_master(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "ROLE")
        # Master: *3\r\n $6\r\nmaster\r\n :<offset>\r\n *0\r\n
        assert "$6\r\nmaster\r\n" in resp

    def test_role_on_replica(self, replica_6383):
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "ROLE")
        assert "$5\r\nslave\r\n" in resp
        assert "localhost" in resp
        assert ":6379\r\n" in resp
        assert "connected" in resp

    def test_role_case_insensitive(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "role")
        assert "$6\r\nmaster\r\n" in resp


# --- Replica read-only ---

class TestReadOnly:
    @pytest.fixture(autouse=True)
    def _require_replica(self, replica_6383):
        pass

    def test_set_on_replica_rejected(self):
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "SET", "blocked", "val")
        assert "READONLY" in resp

    def test_get_on_replica_works(self):
        # Write on master, read from replica
        send_command(MASTER_HOST, MASTER_PORT, "SET", "readonly_test", "hello")
        time.sleep(0.5)
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "GET", "readonly_test")
        assert resp == "$5\r\nhello\r\n"

    def test_ping_on_replica_works(self):
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "PING")
        assert resp == "+PONG\r\n"

    def test_info_on_replica_works(self):
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "INFO", "replication")
        assert "role:worker" in resp


# --- REPLICAOF NO ONE ---

class TestReplicaofNoOne:
    def test_promote_replica_to_master(self, replica_6383):
        # Verify starts as replica
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "ROLE")
        assert "slave" in resp

        # Promote
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "NO", "ONE")
        assert resp == "+OK\r\n"

        # Verify now master
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "ROLE")
        assert "master" in resp

    def test_writes_accepted_after_promotion(self, replica_6383):
        send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "NO", "ONE")
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "SET", "promoted_key", "val")
        assert resp == "+OK\r\n"
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "GET", "promoted_key")
        assert resp == "$3\r\nval\r\n"

    def test_no_one_on_master_is_noop(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "REPLICAOF", "NO", "ONE")
        assert resp == "+OK\r\n"
        resp = send_command(MASTER_HOST, MASTER_PORT, "ROLE")
        assert "master" in resp

    def test_info_updates_after_promotion(self, replica_6383):
        send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "NO", "ONE")
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "INFO", "replication")
        assert "role:master" in resp


# --- REPLICAOF host port ---

class TestReplicaofSetMaster:
    def test_replicaof_error_missing_args(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "REPLICAOF")
        assert "ERR" in resp

    def test_replicaof_error_bad_port(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "REPLICAOF", "localhost", "abc")
        assert "ERR" in resp

    def test_replicaof_error_port_out_of_range(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "REPLICAOF", "localhost", "99999")
        assert "ERR" in resp
