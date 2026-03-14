"""Integration tests for master-driven expiry propagation (Phase 5.5)."""
import socket
import time
import pytest
from conftest import _start_replica, wait_for_ready


def send_command(sock, cmd):
    """Send raw bytes and receive response."""
    sock.sendall(cmd)
    return sock.recv(1024)


def encode_command(*args):
    """Encode a command as RESP array."""
    parts = [f"*{len(args)}\r\n"]
    for arg in args:
        parts.append(f"${len(arg)}\r\n{arg}\r\n")
    return "".join(parts).encode()


@pytest.fixture(scope="module")
def replica_6382():
    yield from _start_replica(6382)


class TestExpiryPropagation:
    """Master sweeps expired keys and propagates DEL to replicas."""

    def test_master_expiry_propagates_del_to_replica(self, redis_server, replica_6382):
        """SET PX on master → wait for expiry + sweep → replica GET returns null."""
        key = "exp_prop_key"
        set_cmd = encode_command("SET", key, "value", "PX", "200")
        get_cmd = encode_command("GET", key)

        # SET on master with 200ms TTL
        with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
            resp = send_command(s, set_cmd)
            assert resp == b"+OK\r\n"

        # Wait for expiry (200ms) + master sweep interval (100ms) + propagation
        time.sleep(0.5)

        # Replica should return null — master swept and propagated DEL
        with socket.create_connection(("127.0.0.1", 6382), timeout=2) as s:
            resp = send_command(s, get_cmd)
            assert resp == b"$-1\r\n", f"Expected null, got {resp}"

    def test_replica_reads_filter_expired_before_master_sweeps(self, redis_server, replica_6382):
        """Replica's local clock filters expired keys even before master's DEL arrives."""
        key = "exp_filter_key"
        set_cmd = encode_command("SET", key, "value", "PX", "300")
        get_cmd = encode_command("GET", key)

        # SET on master — propagated to replica with expiry via replication stream
        with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
            resp = send_command(s, set_cmd)
            assert resp == b"+OK\r\n"

        # Small delay for propagation to reach replica
        time.sleep(0.1)

        # Verify replica has the key before expiry
        with socket.create_connection(("127.0.0.1", 6382), timeout=2) as s:
            resp = send_command(s, get_cmd)
            assert resp == b"$5\r\nvalue\r\n", f"Expected value before expiry, got {resp}"

        # Wait past TTL — replica's local clock should filter it
        time.sleep(0.4)

        with socket.create_connection(("127.0.0.1", 6382), timeout=2) as s:
            resp = send_command(s, get_cmd)
            assert resp == b"$-1\r\n", f"Expected null after local expiry, got {resp}"
