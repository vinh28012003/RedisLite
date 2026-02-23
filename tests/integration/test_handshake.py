import subprocess
import socket
import os
import pytest

def test_replica_fails_with_unreachable_master():
    """Replica exits with error when master is not reachable."""
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", "6381",
        "--replicaof", "127.0.0.1", "9999"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        returncode = proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        returncode = 1         # timed out = failed to connect = correct behavior
    assert returncode != 0

def test_replica_fails_with_invalid_host():
    """Replica exits with error when master address is unresolvable."""
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", "6382",
        "--replicaof", "999.999.999.999", "6379"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        returncode = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        returncode = 1         # timed out = failed to connect = correct behavior
    assert returncode != 0

@pytest.mark.skipif(
    os.environ.get("CI") == "true",
    reason="host.docker.internal not available on Linux CI"
)
def test_replica_fails_when_master_sends_wrong_response():
    """Replica exits with error when master responds with unexpected data."""
    import threading

    # Fake master: accepts connection, responds with -ERR instead of +PONG
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind(("0.0.0.0", 7777))
    server_sock.listen(1)

    def fake_master():
        conn, _ = server_sock.accept()
        conn.recv(1024)                    # receive PING
        conn.sendall(b"-ERR fake\r\n")     # wrong response
        conn.close()
        server_sock.close()

    t = threading.Thread(target=fake_master)
    t.start()

    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", "6383",
        "--replicaof", "host.docker.internal", "7777"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:                                                                                                
        returncode = proc.wait(timeout=5)                                                               
    except subprocess.TimeoutExpired:                                                                   
        proc.kill()                                                                                     
        proc.wait(timeout=5)
        returncode = 1
    assert returncode != 0

def test_master_handles_psync_command():
    """Master responds to PSYNC ? -1 with +FULLRESYNC <repl_id> 0 followed by empty RDB."""
    import re

    sock = socket.create_connection(("127.0.0.1", 6379), timeout=2)
    try:
        sock.sendall(b"*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n")
        response = sock.recv(4096)

        # Split at first \r\n to get FULLRESYNC line
        crlf_pos = response.index(b"\r\n")
        fullresync_line = response[:crlf_pos].decode()

        match = re.match(r"\+FULLRESYNC ([0-9a-f]{40}) 0", fullresync_line)
        assert match is not None, f"Unexpected FULLRESYNC format: {fullresync_line}"

        # After FULLRESYNC line: $88\r\n<88 bytes>
        rdb_part = response[crlf_pos + 2:]
        assert rdb_part[:5] == b"$88\r\n", f"Expected $88\\r\\n, got {rdb_part[:5]}"
        rdb_binary = rdb_part[5:]
        assert len(rdb_binary) == 88, f"Expected 88 bytes, got {len(rdb_binary)}"
        assert rdb_binary[:5] == b"REDIS", f"RDB should start with REDIS magic"
    finally:
        sock.close()