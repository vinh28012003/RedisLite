"""
Consistency audit: stress tests for replication correctness under adversarial conditions.
Covers: data consistency across promotion, backlog overflow, WAIT + role change,
sibling reconfiguration (Go controller flow), multiple role switches, propagation ordering.
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

def send_command(host, port, *args):
    """Send RESP array command and return raw response."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(cmd.encode())
        return s.recv(4096).decode()


def send_command_raw(host, port, *args):
    """Send RESP array command and return raw bytes."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(cmd.encode())
        return s.recv(4096)


def encode_resp(*args):
    """Encode args as RESP array bytes."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(str(arg))}\r\n{arg}\r\n"
    return cmd.encode()


def wait_for_ready(host, port, timeout=15.0):
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


def parse_info_field(info_resp, field):
    """Extract a field value from INFO response."""
    match = re.search(rf"{field}:([^\r\n]+)", info_resp)
    return match.group(1) if match else None


def kill_server(port, proc=None):
    """Kill redis-lite on given port inside container."""
    subprocess.run(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "bash", "-c", f"pkill -f 'redis-lite.*{port}' 2>/dev/null || true"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5
    )
    if proc:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    time.sleep(0.5)


def start_server(port, replicaof=None):
    """Start redis-lite in container. Returns subprocess.Popen."""
    kill_server(port)
    cmd = ["docker", "compose", "-f", "docker/docker-compose.yml",
           "exec", "-T", "redis-lite",
           "redis-lite", "--port", str(port)]
    if replicaof:
        cmd += ["--replicaof", replicaof[0], str(replicaof[1])]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_ready(MASTER_HOST, port)
    except TimeoutError:
        kill_server(port, proc)
        pytest.fail(f"Server failed to start on port {port}")
    return proc


def do_full_handshake(host, port):
    """Full replica handshake. Returns (socket, replid, offset)."""
    s = socket.create_connection((host, port), timeout=3)
    s.sendall(encode_resp("PING"))
    assert b"+PONG" in s.recv(4096)

    s.sendall(encode_resp("REPLCONF", "listening-port", "9999"))
    assert b"+OK" in s.recv(4096)

    s.sendall(encode_resp("REPLCONF", "capa", "psync2"))
    assert b"+OK" in s.recv(4096)

    s.sendall(encode_resp("PSYNC", "?", "-1"))
    resp = s.recv(4096)

    line_end = resp.index(b"\r\n")
    fullresync_line = resp[:line_end].decode()
    match = re.match(r"\+FULLRESYNC ([0-9a-f]{40}) (\d+)", fullresync_line)
    assert match, f"Bad FULLRESYNC: {fullresync_line}"
    replid = match.group(1)
    offset = int(match.group(2))

    # Consume RDB
    rdb_part = resp[line_end + 2:]
    dollar = rdb_part.index(ord('$'))
    rdb_header_end = rdb_part.index(b"\r\n", dollar)
    rdb_len = int(rdb_part[dollar + 1:rdb_header_end])
    rdb_data = rdb_part[rdb_header_end + 2:]
    while len(rdb_data) < rdb_len:
        rdb_data += s.recv(4096)

    return s, replid, offset


def do_psync(host, port, replid, offset):
    """Handshake with specific replid/offset. Returns (socket, response_line, trailing)."""
    s = socket.create_connection((host, port), timeout=3)
    s.sendall(encode_resp("PING"))
    assert b"+PONG" in s.recv(4096)

    s.sendall(encode_resp("REPLCONF", "listening-port", "9999"))
    assert b"+OK" in s.recv(4096)

    s.sendall(encode_resp("REPLCONF", "capa", "psync2"))
    assert b"+OK" in s.recv(4096)

    s.sendall(encode_resp("PSYNC", replid, str(offset)))
    resp = s.recv(4096)
    line_end = resp.index(b"\r\n")
    response_line = resp[:line_end].decode()
    trailing = resp[line_end + 2:]
    return s, response_line, trailing


# --- Fixtures ---

@pytest.fixture
def server_6383():
    """Standalone master on 6383."""
    proc = start_server(6383)
    yield
    kill_server(6383, proc)


@pytest.fixture
def replica_6383():
    """Replica of 6379 on 6383."""
    proc = start_server(6383, replicaof=("localhost", 6379))
    yield
    kill_server(6383, proc)


@pytest.fixture
def replica_6384():
    """Replica of 6379 on 6384."""
    proc = start_server(6384, replicaof=("localhost", 6379))
    yield
    kill_server(6384, proc)


# --- Group 1: Data consistency across promotion ---

class TestDataConsistencyAcrossPromotion:

    def test_data_survives_promotion(self, redis_server, replica_6383):
        """All keys propagated before promotion survive on promoted replica."""
        # Write 5 keys on master
        for i in range(5):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"surv_{i}", f"val_{i}")
        time.sleep(0.5)  # propagation

        # Promote replica
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.2)

        # Verify all 5 keys on promoted replica
        for i in range(5):
            resp = send_command(MASTER_HOST, 6383, "GET", f"surv_{i}")
            assert f"val_{i}" in resp, f"Key surv_{i} missing after promotion"

    def test_writes_after_promotion_persist(self, redis_server, replica_6383):
        """New writes on promoted replica are accepted and readable."""
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.2)

        for i in range(3):
            resp = send_command(MASTER_HOST, 6383, "SET", f"post_promo_{i}", f"new_{i}")
            assert "+OK" in resp

        for i in range(3):
            resp = send_command(MASTER_HOST, 6383, "GET", f"post_promo_{i}")
            assert f"new_{i}" in resp

    def test_old_master_isolated_after_promotion(self, redis_server, replica_6383):
        """Writes on old master after promotion don't reach promoted replica."""
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.2)

        # Write on old master — no replication link to promoted replica
        send_command(MASTER_HOST, MASTER_PORT, "SET", "isolated_key", "should_not_appear")
        time.sleep(0.3)

        resp = send_command(MASTER_HOST, 6383, "GET", "isolated_key")
        assert "$-1" in resp, "Promoted replica should NOT have old master's new writes"


# --- Group 2: Backlog overflow ---

class TestBacklogOverflow:
    """Backlog tests use a standalone server on 6383 to avoid polluting the session master."""

    def test_partial_resync_within_backlog(self, redis_server, server_6383):
        """Writes within backlog range allow CONTINUE on reconnect."""
        s1, replid, offset = do_full_handshake(MASTER_HOST, 6383)
        s1.close()

        # Write ~100 keys — well within 1MB backlog
        for i in range(100):
            send_command(MASTER_HOST, 6383, "SET", f"bl_in_{i}", f"v{i}")
        time.sleep(0.3)

        s2, response_line, trailing = do_psync(MASTER_HOST, 6383, replid, offset)
        all_data = trailing
        try:
            s2.settimeout(0.5)
            all_data += s2.recv(65536)
        except socket.timeout:
            pass
        s2.close()

        assert "+CONTINUE" in response_line
        # Spot-check some keys in replayed data
        assert b"bl_in_0" in all_data
        assert b"bl_in_99" in all_data

    def test_fullresync_after_backlog_overflow(self, redis_server, server_6383):
        """Writes >1MB overflow backlog, stale offset forces FULLRESYNC."""
        s1, replid, offset = do_full_handshake(MASTER_HOST, 6383)
        s1.close()

        # Flood >1MB: 1100 keys × ~1KB value = ~1.1MB
        value = "x" * 1000
        for i in range(1100):
            send_command(MASTER_HOST, 6383, "SET", f"flood_{i}", value)
        time.sleep(0.5)

        # Old offset is now outside backlog
        s2, response_line, _ = do_psync(MASTER_HOST, 6383, replid, offset)
        s2.close()

        assert "+FULLRESYNC" in response_line, f"Expected FULLRESYNC after overflow, got: {response_line}"

    def test_wraparound_replay_correctness(self, redis_server, server_6383):
        """After backlog wraps, partial resync replays only recent data."""
        # Overflow the backlog first
        value = "x" * 1000
        for i in range(1100):
            send_command(MASTER_HOST, 6383, "SET", f"wrap_old_{i}", value)
        time.sleep(0.5)

        # Capture offset AFTER overflow
        s1, replid, offset = do_full_handshake(MASTER_HOST, 6383)
        s1.close()

        # Write 3 marker keys (within backlog)
        send_command(MASTER_HOST, 6383, "SET", "marker_a", "ma")
        send_command(MASTER_HOST, 6383, "SET", "marker_b", "mb")
        send_command(MASTER_HOST, 6383, "SET", "marker_c", "mc")
        time.sleep(0.3)

        # PSYNC at post-overflow offset — should CONTINUE with only markers
        s2, response_line, trailing = do_psync(MASTER_HOST, 6383, replid, offset)
        all_data = trailing
        try:
            s2.settimeout(0.5)
            all_data += s2.recv(65536)
        except socket.timeout:
            pass
        s2.close()

        assert "+CONTINUE" in response_line
        assert b"marker_a" in all_data
        assert b"marker_b" in all_data
        assert b"marker_c" in all_data


# --- Group 3: WAIT + role change interaction ---

class TestWaitRoleChangeInteraction:

    def test_pending_wait_resolves_on_replicaof_set_master(self, redis_server, replica_6383):
        """WAIT blocked on unreachable count resolves when REPLICAOF changes role."""
        # Write so WAIT has something to wait for
        send_command(MASTER_HOST, MASTER_PORT, "SET", "wait_rc_key", "val")
        time.sleep(0.5)

        wait_result = [None]
        wait_error = [None]

        def blocked_wait():
            """Send WAIT 99 (unreachable count) with 5s timeout on a persistent socket."""
            try:
                with socket.create_connection((MASTER_HOST, MASTER_PORT), timeout=6) as s:
                    cmd = encode_resp("WAIT", "99", "5000")
                    s.sendall(cmd)
                    wait_result[0] = s.recv(4096).decode()
            except Exception as e:
                wait_error[0] = str(e)

        # Start WAIT in background thread
        t = threading.Thread(target=blocked_wait)
        t.start()
        time.sleep(0.5)  # let WAIT park

        # Trigger role change — this resolves pending WAITs (server.cpp:494-500)
        # REPLICAOF to unreachable port — handshake fails, reverts to master
        # But pending_waits_ are resolved BEFORE the handshake attempt
        send_command(MASTER_HOST, MASTER_PORT, "REPLICAOF", "localhost", "1")
        time.sleep(1.5)  # handshake fail + revert

        t.join(timeout=5)
        assert not t.is_alive(), "WAIT thread should have resolved"
        assert wait_error[0] is None, f"WAIT thread errored: {wait_error[0]}"
        assert wait_result[0] is not None, "WAIT should have returned a result"
        # Result is :<count>\r\n — the current caught-up replica count
        assert wait_result[0].startswith(":"), f"Expected integer reply, got: {wait_result[0]}"

    def test_wait_works_on_promoted_master(self, redis_server, replica_6383):
        """After promotion, WAIT works with new replicas attached to promoted master."""
        # Promote 6383
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.3)

        # Start 6384 as replica of promoted 6383
        proc_6384 = start_server(6384, replicaof=("localhost", 6383))
        try:
            time.sleep(0.5)

            # Write on promoted master
            send_command(MASTER_HOST, 6383, "SET", "wait_promo_key", "val")

            # WAIT should succeed — 6384 is connected
            resp = send_command(MASTER_HOST, 6383, "WAIT", "1", "2000")
            count = int(resp.strip().lstrip(":").split("\r\n")[0])
            assert count >= 1, f"Expected ≥1 replica ACK, got {count}"
        finally:
            kill_server(6384, proc_6384)


# --- Group 4: Sibling reconfiguration (Go controller flow) ---

class TestSiblingReconfiguration:

    def test_sibling_partial_resync_to_promoted(self, redis_server, replica_6383, replica_6384):
        """After promotion, sibling can partial-resync to promoted via replid2."""
        # Get master's replid (both replicas inherited this)
        master_info = send_command(MASTER_HOST, MASTER_PORT, "INFO", "replication")
        master_replid = parse_info_field(master_info, "master_replid")

        # Write data so replicas have an offset
        send_command(MASTER_HOST, MASTER_PORT, "SET", "sib_key", "val")
        time.sleep(0.5)

        # Get replica-B's offset before promotion
        b_info = send_command(MASTER_HOST, 6384, "INFO", "replication")
        b_offset = int(parse_info_field(b_info, "master_repl_offset"))

        # Promote replica-A
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.3)

        # Verify A has replid2 = old master's replid
        a_info = send_command(MASTER_HOST, 6383, "INFO", "replication")
        a_replid2 = parse_info_field(a_info, "master_replid2")
        assert a_replid2 == master_replid

        # Raw PSYNC from sibling B's perspective to promoted A
        s, response_line, _ = do_psync(MASTER_HOST, 6383, master_replid, b_offset)
        s.close()
        assert "+CONTINUE" in response_line, f"Expected CONTINUE, got: {response_line}"

    def test_sibling_receives_post_promotion_writes(self, redis_server, replica_6383):
        """After reconfiguration, sibling gets new writes from promoted master."""
        # Write baseline data
        send_command(MASTER_HOST, MASTER_PORT, "SET", "sib_base", "base_val")
        time.sleep(0.5)

        # Promote A
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.3)

        # Start B as replica of promoted A (non-standard topology)
        proc_6384 = start_server(6384, replicaof=("localhost", 6383))
        try:
            time.sleep(0.5)

            # Write on promoted A
            send_command(MASTER_HOST, 6383, "SET", "sib_new", "new_val")
            time.sleep(0.5)

            # B should have the new write
            resp = send_command(MASTER_HOST, 6384, "GET", "sib_new")
            assert "new_val" in resp, f"Sibling should have post-promotion write, got: {resp}"
        finally:
            kill_server(6384, proc_6384)

    def test_full_failover_flow_end_to_end(self, redis_server, replica_6383):
        """Complete Go controller flow: promote A, reconfigure B → A, verify all data."""
        # Phase 1: Write on original master
        for i in range(3):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"ff_orig_{i}", f"orig_{i}")
        time.sleep(0.5)

        # Phase 2: Promote A
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.3)

        # Phase 3: Start B as replica of promoted A
        proc_6384 = start_server(6384, replicaof=("localhost", 6383))
        try:
            time.sleep(0.5)

            # Phase 4: Write on promoted A (new master)
            for i in range(3):
                send_command(MASTER_HOST, 6383, "SET", f"ff_new_{i}", f"new_{i}")
            time.sleep(0.5)

            # Phase 5: Verify B has ALL keys — original + new
            for i in range(3):
                resp = send_command(MASTER_HOST, 6384, "GET", f"ff_orig_{i}")
                assert f"orig_{i}" in resp, f"Sibling missing original key ff_orig_{i}"
            for i in range(3):
                resp = send_command(MASTER_HOST, 6384, "GET", f"ff_new_{i}")
                assert f"new_{i}" in resp, f"Sibling missing new key ff_new_{i}"
        finally:
            kill_server(6384, proc_6384)


# --- Group 5: Multiple role switches ---

class TestMultipleRoleSwitches:

    def test_master_replica_master_roundtrip(self, redis_server, server_6383):
        """master → replica → master roundtrip preserves functionality."""
        # Start as standalone master, write key
        resp = send_command(MASTER_HOST, 6383, "SET", "rt_key1", "v1")
        assert "+OK" in resp

        # Become replica of 6379
        send_command(MASTER_HOST, 6383, "REPLICAOF", "localhost", "6379")
        time.sleep(1)  # handshake

        # Verify replica role
        resp = send_command(MASTER_HOST, 6383, "ROLE")
        assert "slave" in resp

        # Promote back to master
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.3)

        # Should be master again and accept writes
        resp = send_command(MASTER_HOST, 6383, "ROLE")
        assert "master" in resp

        resp = send_command(MASTER_HOST, 6383, "SET", "rt_key2", "v2")
        assert "+OK" in resp

        resp = send_command(MASTER_HOST, 6383, "GET", "rt_key2")
        assert "v2" in resp

    def test_offset_continuity_after_promotion(self, redis_server, replica_6383):
        """After promotion, offsets are consistent for partial resync from siblings."""
        # Write keys so replica has a non-zero offset
        for i in range(5):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"oc_key_{i}", f"val_{i}")
        time.sleep(0.5)

        # Get replica offset before promotion
        info_before = send_command(MASTER_HOST, 6383, "INFO", "replication")
        offset_before = int(parse_info_field(info_before, "master_repl_offset"))
        assert offset_before > 0, "Replica should have non-zero offset after receiving writes"

        # Promote
        send_command(MASTER_HOST, 6383, "REPLICAOF", "NO", "ONE")
        time.sleep(0.3)

        # Check offset continuity
        info_after = send_command(MASTER_HOST, 6383, "INFO", "replication")
        second_offset = int(parse_info_field(info_after, "second_repl_offset"))
        master_offset = int(parse_info_field(info_after, "master_repl_offset"))

        # second_repl_offset = offset at promotion time
        assert second_offset == offset_before, \
            f"second_repl_offset ({second_offset}) should equal pre-promotion offset ({offset_before})"

        # master_repl_offset should equal second_repl_offset (no new writes yet)
        assert master_offset == second_offset, \
            f"master_repl_offset ({master_offset}) should equal second_repl_offset ({second_offset})"

        # Write on promoted — offset should increase
        send_command(MASTER_HOST, 6383, "SET", "oc_post", "val")
        time.sleep(0.2)

        info_final = send_command(MASTER_HOST, 6383, "INFO", "replication")
        final_offset = int(parse_info_field(info_final, "master_repl_offset"))
        assert final_offset > master_offset, \
            f"Offset should increase after write: {final_offset} > {master_offset}"


# --- Group 6: Propagation ordering ---

class TestPropagationOrdering:

    def test_set_del_interleaving(self, redis_server, replica_6383):
        """SET/DEL ordering is preserved on replica."""
        send_command(MASTER_HOST, MASTER_PORT, "SET", "ord_a", "val_a")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "ord_b", "val_b")
        send_command(MASTER_HOST, MASTER_PORT, "DEL", "ord_a")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "ord_c", "val_c")
        time.sleep(0.5)

        # a was deleted
        resp = send_command(MASTER_HOST, 6383, "GET", "ord_a")
        assert "$-1" in resp, f"ord_a should be deleted, got: {resp}"

        # b and c should exist
        resp = send_command(MASTER_HOST, 6383, "GET", "ord_b")
        assert "val_b" in resp

        resp = send_command(MASTER_HOST, 6383, "GET", "ord_c")
        assert "val_c" in resp

    def test_data_integrity_after_partial_resync(self, redis_server):
        """After CONTINUE, replayed data produces correct GET results on a real replica."""
        # Write baseline keys
        for i in range(5):
            send_command(MASTER_HOST, MASTER_PORT, "SET", f"di_key_{i}", f"di_val_{i}")
        time.sleep(0.3)

        # Start replica — gets baseline via FULLRESYNC
        proc_6383 = start_server(6383, replicaof=("localhost", 6379))
        try:
            time.sleep(0.5)

            # Verify baseline arrived
            for i in range(5):
                resp = send_command(MASTER_HOST, 6383, "GET", f"di_key_{i}")
                assert f"di_val_{i}" in resp, f"Baseline key di_key_{i} missing"

            # Write 5 more keys while replica is connected (propagated live)
            for i in range(5, 10):
                send_command(MASTER_HOST, MASTER_PORT, "SET", f"di_key_{i}", f"di_val_{i}")
            time.sleep(0.5)

            # Verify all 10 keys on replica
            for i in range(10):
                resp = send_command(MASTER_HOST, 6383, "GET", f"di_key_{i}")
                assert f"di_val_{i}" in resp, f"Key di_key_{i} missing or wrong"
        finally:
            kill_server(6383, proc_6383)
