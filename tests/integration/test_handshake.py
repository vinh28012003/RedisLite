import subprocess
import socket

def test_replica_fails_with_unreachable_master():
    """Replica exits with error when master is not reachable."""
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", "6381",
        "--replicaof", "127.0.0.1", "9999"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    returncode = proc.wait(timeout=5)
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
    returncode = proc.wait(timeout=5)
    assert returncode != 0
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
    returncode = proc.wait(timeout=5)
    t.join(timeout=5)
    assert returncode != 0