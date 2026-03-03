"""
Integration tests for REPLCONF GETACK/ACK (stages 14-15).
Fake master on host — replica inside Docker connects via host.docker.internal.
Skipped on CI (no host.docker.internal on Linux).
"""
import os
import socket
import subprocess
import time
import pytest

CI = os.environ.get("CI", "").lower() in ("true", "1", "yes")
pytestmark = pytest.mark.skipif(CI, reason="host.docker.internal unavailable on Linux CI")

FAKE_MASTER_PORT = 6399
REPLICA_PORT = 6385

# Empty RDB (88 bytes) — same as command.cpp
EMPTY_RDB = bytes([
    0x52, 0x45, 0x44, 0x49, 0x53, 0x30, 0x30, 0x31, 0x31,
    0xfa, 0x09, 0x72, 0x65, 0x64, 0x69, 0x73, 0x2d, 0x76, 0x65, 0x72, 0x05,
    0x37, 0x2e, 0x32, 0x2e, 0x30,
    0xfa, 0x0a, 0x72, 0x65, 0x64, 0x69, 0x73, 0x2d, 0x62, 0x69, 0x74, 0x73,
    0xc0, 0x40,
    0xfa, 0x05, 0x63, 0x74, 0x69, 0x6d, 0x65, 0xc2, 0x6d, 0x08, 0xbc, 0x65,
    0xfa, 0x08, 0x75, 0x73, 0x65, 0x64, 0x2d, 0x6d, 0x65, 0x6d, 0xc2, 0xb0,
    0xc4, 0x10, 0x00,
    0xfa, 0x08, 0x61, 0x6f, 0x66, 0x2d, 0x62, 0x61, 0x73, 0x65, 0xc0, 0x00,
    0xff,
    0xf0, 0x6e, 0x3b, 0xfe, 0xc0, 0xff, 0x5a, 0xa2,
])


def encode_resp(*args):
    """Encode as RESP array: *N\\r\\n$len\\r\\narg\\r\\n..."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        arg_str = str(arg)
        cmd += f"${len(arg_str)}\r\n{arg_str}\r\n"
    return cmd.encode()


def parse_resp_array(data):
    """Parse one RESP array from bytes → list of strings."""
    text = data.decode()
    if not text.startswith("*"):
        return None
    lines = text.split("\r\n")
    count = int(lines[0][1:])
    args = []
    i = 1
    for _ in range(count):
        i += 1  # skip $N
        args.append(lines[i])
        i += 1
    return args


class FakeMaster:
    """TCP server mimicking a Redis master for handshake + replication commands."""

    def __init__(self, port):
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.listen(1)
        self.sock.settimeout(10)
        self.conn = None

    def accept_and_handshake(self):
        """Accept replica connection, complete 4-step handshake."""
        self.conn, _ = self.sock.accept()
        self.conn.settimeout(5)

        # 1. PING → +PONG
        self.conn.recv(4096)
        self.conn.sendall(b"+PONG\r\n")

        # 2. REPLCONF listening-port → +OK
        self.conn.recv(4096)
        self.conn.sendall(b"+OK\r\n")

        # 3. REPLCONF capa psync2 → +OK
        self.conn.recv(4096)
        self.conn.sendall(b"+OK\r\n")

        # 4. PSYNC → FULLRESYNC + empty RDB
        self.conn.recv(4096)
        fullresync = b"+FULLRESYNC 8371445d5fa15f7c5b4b11810dca924cff1b0276 0\r\n"
        fullresync += f"${len(EMPTY_RDB)}\r\n".encode() + EMPTY_RDB
        self.conn.sendall(fullresync)

    def send(self, *args):
        """Send RESP command over replication link."""
        self.conn.sendall(encode_resp(*args))

    def recv(self):
        """Read response from replica."""
        self.conn.settimeout(2)
        return self.conn.recv(4096)

    def close(self):
        if self.conn:
            self.conn.close()
        self.sock.close()


def _kill_replica(port):
    subprocess.run(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "bash", "-c", f"pkill -f 'redis-lite.*{port}' 2>/dev/null || true"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5,
    )
    time.sleep(0.3)


@pytest.fixture
def fake_master_and_replica():
    """Function-scoped: fresh fake master + replica per test (offset starts at 0)."""
    _kill_replica(REPLICA_PORT)
    master = FakeMaster(FAKE_MASTER_PORT)

    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", str(REPLICA_PORT),
        "--replicaof", "host.docker.internal", str(FAKE_MASTER_PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    try:
        master.accept_and_handshake()
        time.sleep(0.5)  # replica registers master_fd in epoll + enters run()
    except Exception:
        master.close()
        _kill_replica(REPLICA_PORT)
        proc.kill()
        raise

    yield master

    master.close()
    _kill_replica(REPLICA_PORT)
    proc.kill()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


class TestGetack:
    """REPLCONF GETACK/ACK — fake master verifies replica offset reporting."""

    def test_ack_with_no_commands(self, fake_master_and_replica):
        """Stage 14: Fresh replica, no propagated commands → offset 0."""
        master = fake_master_and_replica
        master.send("REPLCONF", "GETACK", "*")
        resp = parse_resp_array(master.recv())
        assert resp == ["REPLCONF", "ACK", "0"]

    def test_ack_after_one_set(self, fake_master_and_replica):
        """Stage 15: One SET propagated → offset = byte length of SET command."""
        master = fake_master_and_replica
        set_cmd = encode_resp("SET", "foo", "bar")
        master.conn.sendall(set_cmd)
        time.sleep(0.3)

        master.send("REPLCONF", "GETACK", "*")
        resp = parse_resp_array(master.recv())
        assert resp == ["REPLCONF", "ACK", str(len(set_cmd))]

    def test_ack_after_multiple_sets(self, fake_master_and_replica):
        """Stage 15: Offset accumulates across multiple propagated commands."""
        master = fake_master_and_replica
        cmd1 = encode_resp("SET", "k1", "v1")
        cmd2 = encode_resp("SET", "k2", "v2")
        master.conn.sendall(cmd1 + cmd2)
        time.sleep(0.3)

        master.send("REPLCONF", "GETACK", "*")
        resp = parse_resp_array(master.recv())
        assert resp == ["REPLCONF", "ACK", str(len(cmd1) + len(cmd2))]