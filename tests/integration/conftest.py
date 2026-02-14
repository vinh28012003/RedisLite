"""
Shared fixtures for integration tests.
Verifies redis-lite is running before tests execute.
"""
import socket
import time
import pytest

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 6379


def wait_for_port(host, port, timeout=5.0):
    """Block until the server is accepting connections."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"Server not ready on {host}:{port} after {timeout}s")


@pytest.fixture(scope="session", autouse=True)
def redis_server():
    """Verify redis-lite is running. Start the container first:
        docker compose -f docker/docker-compose.yml up -d
    """
    try:
        wait_for_port(SERVER_HOST, SERVER_PORT)
    except TimeoutError:
        pytest.exit("redis-lite not running. Start it with: docker compose -f docker/docker-compose.yml up -d")