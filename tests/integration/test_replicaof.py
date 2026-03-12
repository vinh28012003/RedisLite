"""
Integration tests for ROLE, REPLICAOF, replica-read-only (Phase 2),
and dual replication IDs (Phase 3).
Uses function-scoped replicas since REPLICAOF changes server state.
"""
import re
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

    def test_replicaof_same_master_is_noop(self, replica_6383):
        """REPLICAOF to current master returns OK, stays replica."""
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "localhost", "6379")
        assert "OK" in resp
        # Still a replica
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "ROLE")
        assert "slave" in resp

    def test_replicaof_unreachable_host_reverts_to_master(self):
        """REPLICAOF to unreachable host fails handshake, server reverts to master."""
        # Start as master on port 6383 (no --replicaof)
        _kill_replica(REPLICAOF_PORT)
        proc = subprocess.Popen(
            ["docker", "compose", "-f", "docker/docker-compose.yml",
             "exec", "-T", "redis-lite",
             "./build/redis-lite", "--port", str(REPLICAOF_PORT)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        try:
            wait_for_ready(MASTER_HOST, REPLICAOF_PORT)
        except TimeoutError:
            _kill_replica(REPLICAOF_PORT)
            proc.kill()
            pytest.fail(f"server failed to start on port {REPLICAOF_PORT}")

        try:
            # Verify starts as master
            resp = send_command(MASTER_HOST, REPLICAOF_PORT, "ROLE")
            assert "master" in resp

            # Try to become replica of localhost on a port nothing listens on
            # connect() will fail fast with ECONNREFUSED (not a slow TCP timeout)
            resp = send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "localhost", "1")
            assert "OK" in resp  # OK sent before blocking handshake

            # Give time for handshake to fail and revert
            time.sleep(1)

            # Should have reverted to master
            resp = send_command(MASTER_HOST, REPLICAOF_PORT, "ROLE")
            assert "master" in resp
        finally:
            _kill_replica(REPLICAOF_PORT)
            proc.kill()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass


# --- Dual Replication IDs (Phase 3) ---

def _parse_info_field(info_resp, field):
    """Extract a field value from INFO response body."""
    match = re.search(rf"{field}:([^\r\n]+)", info_resp)
    return match.group(1) if match else None


class TestDualReplid:
    def test_fresh_master_has_random_replid(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "INFO", "replication")
        replid = _parse_info_field(resp, "master_replid")
        assert replid is not None
        assert len(replid) == 40
        assert re.match(r"^[0-9a-f]{40}$", replid)

    def test_fresh_master_replid2_is_zeros(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "INFO", "replication")
        replid2 = _parse_info_field(resp, "master_replid2")
        assert replid2 == "0" * 40

    def test_fresh_master_second_offset_is_negative_one(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "INFO", "replication")
        offset = _parse_info_field(resp, "second_repl_offset")
        assert offset == "-1"

    def test_promotion_shifts_replid(self, replica_6383):
        # Get replica's replid before promotion (inherited from master)
        resp_before = send_command(MASTER_HOST, REPLICAOF_PORT, "INFO", "replication")
        replid_before = _parse_info_field(resp_before, "master_replid")

        # Promote
        send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "NO", "ONE")

        # Get IDs after promotion
        resp_after = send_command(MASTER_HOST, REPLICAOF_PORT, "INFO", "replication")
        replid_after = _parse_info_field(resp_after, "master_replid")
        replid2_after = _parse_info_field(resp_after, "master_replid2")

        # New replid should be different from old
        assert replid_after != replid_before
        assert len(replid_after) == 40

        # Old replid should now be replid2
        assert replid2_after == replid_before

    def test_promotion_sets_second_offset(self, replica_6383):
        # Promote
        send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "NO", "ONE")

        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "INFO", "replication")
        offset = _parse_info_field(resp, "second_repl_offset")
        # Offset should be >= 0 (was the replica's offset at promotion time)
        assert int(offset) >= 0

    def test_new_replid_after_promotion_is_valid_hex(self, replica_6383):
        send_command(MASTER_HOST, REPLICAOF_PORT, "REPLICAOF", "NO", "ONE")
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "INFO", "replication")
        replid = _parse_info_field(resp, "master_replid")
        assert re.match(r"^[0-9a-f]{40}$", replid)

    def test_no_one_on_master_preserves_replid(self):
        # Get replid before no-op
        resp_before = send_command(MASTER_HOST, MASTER_PORT, "INFO", "replication")
        replid_before = _parse_info_field(resp_before, "master_replid")
        replid2_before = _parse_info_field(resp_before, "master_replid2")

        # No-op REPLICAOF NO ONE on master
        send_command(MASTER_HOST, MASTER_PORT, "REPLICAOF", "NO", "ONE")

        # IDs should not change
        resp_after = send_command(MASTER_HOST, MASTER_PORT, "INFO", "replication")
        assert _parse_info_field(resp_after, "master_replid") == replid_before
        assert _parse_info_field(resp_after, "master_replid2") == replid2_before

    def test_replica_info_has_replid2_fields(self, replica_6383):
        resp = send_command(MASTER_HOST, REPLICAOF_PORT, "INFO", "replication")
        assert "master_replid2:" in resp
        assert "second_repl_offset:" in resp
