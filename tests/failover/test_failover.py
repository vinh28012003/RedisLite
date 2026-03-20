"""
Failover integration tests — ordered, stateful sequence.
Requires: docker compose -f docker/docker-compose.failover.yml up -d
"""

import pytest
from conftest import (
    send_command, get_role, get_info_field,
    compose_stop, compose_start, wait_for_condition, NODES,
)


def _ping_ok(host, port):
    """PING check that returns False instead of raising on failure."""
    try:
        return b"PONG" in send_command(host, port, "PING")
    except OSError:
        return False


class TestFailoverSequence:
    """
    Stateful test sequence. Tests run top-to-bottom.
    Class attributes share state between tests (e.g., new_master_port).
    """

    new_master_name = None
    new_master_port = None

    # --- Test 1: Health Check ---

    def test_all_nodes_healthy(self):
        """All 5 nodes respond to PING."""
        for name, port in NODES.items():
            resp = send_command("127.0.0.1", port, "PING")
            assert b"PONG" in resp, f"{name} (port {port}) did not PONG"

    # --- Test 2: Initial Topology ---

    def test_initial_topology(self):
        """redis-1 is master, redis-2 through redis-5 are replicas."""
        assert get_role("127.0.0.1", NODES["redis-1"]) == "master"
        for name in ["redis-2", "redis-3", "redis-4", "redis-5"]:
            role = get_role("127.0.0.1", NODES[name])
            assert role == "slave", f"{name} expected slave, got {role}"

    # --- Test 3: Failure Detection ---

    def test_failure_detection(self):
        """Stop redis-1 (master). Controller should detect within ~2s."""
        compose_stop("redis-1")

        def redis1_unreachable():
            try:
                send_command("127.0.0.1", NODES["redis-1"], "PING", timeout=1)
                return False
            except (ConnectionRefusedError, OSError):
                return True

        wait_for_condition(
            redis1_unreachable, timeout=5,
            description="redis-1 to become unreachable",
        )

    # --- Test 4: Promotion ---

    def test_promotion(self):
        """One of redis-2 through redis-5 gets promoted to master."""

        def find_new_master():
            for name in ["redis-2", "redis-3", "redis-4", "redis-5"]:
                try:
                    if get_role("127.0.0.1", NODES[name]) == "master":
                        return name
                except OSError:
                    continue
            return None

        new_master = wait_for_condition(
            find_new_master, timeout=15,
            description="a replica to be promoted to master",
        )

        TestFailoverSequence.new_master_name = new_master
        TestFailoverSequence.new_master_port = NODES[new_master]
        assert new_master is not None

    # --- Test 5: Sibling Reconfiguration ---

    def test_sibling_reconfiguration(self):
        """Remaining replicas follow the new master."""
        assert self.new_master_name, "No new master — test_promotion must pass first"

        siblings = [
            n for n in ["redis-2", "redis-3", "redis-4", "redis-5"]
            if n != self.new_master_name
        ]

        def all_siblings_follow():
            for name in siblings:
                try:
                    role = get_role("127.0.0.1", NODES[name])
                    if role != "slave":
                        return False
                except OSError:
                    return False
            return True

        wait_for_condition(
            all_siblings_follow, timeout=15,
            description="all siblings to follow new master",
        )

    # --- Test 6: Write/Read After Failover ---

    def test_write_read_after_failover(self):
        """SET on new master, GET from replicas returns the value."""
        assert self.new_master_port, "No new master — test_promotion must pass first"

        # Write to new master
        resp = send_command("127.0.0.1", self.new_master_port, "SET", "failover-key", "alive")
        assert b"OK" in resp, f"SET failed: {resp}"

        # Read from each sibling replica
        siblings = [
            n for n in ["redis-2", "redis-3", "redis-4", "redis-5"]
            if n != self.new_master_name
        ]

        def replicas_have_key():
            for name in siblings:
                try:
                    resp = send_command("127.0.0.1", NODES[name], "GET", "failover-key")
                    if b"alive" not in resp:
                        return False
                except OSError:
                    return False
            return True

        wait_for_condition(
            replicas_have_key, timeout=10,
            description="replicas to replicate failover-key",
        )

    # --- Test 7: Old Master Recovery ---

    def test_old_master_recovery(self):
        """Restart redis-1 — controller reconfigures it as replica of new master."""
        assert self.new_master_name, "No new master — test_promotion must pass first"

        # Bring old master back
        compose_start("redis-1")

        # Wait for redis-1 to respond to PING
        wait_for_condition(
            lambda: _ping_ok("127.0.0.1", NODES["redis-1"]),
            timeout=15,
            description="redis-1 to come back online",
        )

        # Controller should detect redis-1 alive with role "master" and
        # send REPLICAOF <new-master> to demote it — wait for role to flip
        def redis1_is_replica():
            try:
                return get_role("127.0.0.1", NODES["redis-1"]) == "slave"
            except OSError:
                return False

        wait_for_condition(
            redis1_is_replica, timeout=15,
            description="redis-1 to be reconfigured as replica",
        )

        # Verify data written post-failover replicates to recovered node
        def redis1_has_key():
            try:
                resp = send_command("127.0.0.1", NODES["redis-1"], "GET", "failover-key")
                return b"alive" in resp
            except OSError:
                return False

        wait_for_condition(
            redis1_has_key, timeout=10,
            description="redis-1 to replicate failover-key from new master",
        )
