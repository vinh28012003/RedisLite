"""
Shared fixtures for integration tests.
Verifies redis-lite is running before tests execute.
"""
import socket
import time
import subprocess
import pytest

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 6379
REPLICA_PORT = 6380

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


@pytest.fixture(scope="session", autouse=True)
def redis_server():
    """Verify redis-lite is running. Start the container first:
        docker compose -f docker/docker-compose.yml up -d
    """
    try:
        wait_for_ready(SERVER_HOST, SERVER_PORT)
    except TimeoutError:
        pytest.exit("redis-lite not running. Start it with: docker compose -f docker/docker-compose.yml up -d")


@pytest.fixture(scope="session")
def replica_server():
    """Start a second redis-lite instance as replica inside the container."""
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", "6380",
        "--replicaof", "localhost 6379"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    try:
        wait_for_ready(SERVER_HOST, REPLICA_PORT)
    except TimeoutError:
        proc.kill()
        pytest.fail("replica server failed to start on port 6380")
    yield
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
