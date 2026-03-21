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
REPLICA_PORTS = [6380, 6381, 6382]     

def wait_for_ready(host, port, timeout=15.0):
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


def _kill_container_replica(port=None):
    """Kill leftover redis-lite replica(s) inside the container."""
    pattern = f'redis-lite.*{port}' if port else 'redis-lite.*638[0-9]'
    subprocess.run(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "bash", "-c", f"pkill -f '{pattern}' 2>/dev/null || true"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5
    )
    time.sleep(0.5)

def _start_replica(port):
    """Generator: start replica on given port, yield, cleanup."""
    _kill_container_replica(port)
    # Stagger replica starts — concurrent FULLRESYNC from the same master
    # causes contention on slow CI runners (GitHub Actions shared VMs)
    time.sleep(1.0)
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "redis-lite", "--port", str(port),
        "--replicaof", "localhost", "6379"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    try:
        wait_for_ready(SERVER_HOST, port)
    except TimeoutError:
        _kill_container_replica(port)
        proc.kill()
        pytest.fail(f"replica server failed to start on port {port}")
    yield
    _kill_container_replica(port)
    proc.kill()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


@pytest.fixture(scope="session")
def replica_server():
    yield from _start_replica(6380)


@pytest.fixture(scope="session")
def replica_6381():
    yield from _start_replica(6381)


@pytest.fixture(scope="session")
def replica_6382():
    yield from _start_replica(6382)
