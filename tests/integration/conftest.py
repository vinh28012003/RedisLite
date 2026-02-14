"""
Shared fixtures for integration tests.
Starts/stops the redis-lite server process for each test session.
"""
import subprocess
import socket
import time
import pytest
import os

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 6379
# Resolve binary relative to this file's location
SERVER_BIN = os.path.join(os.path.dirname(__file__), "..", "..", "build", "redis-lite")


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


@pytest.fixture(scope="session")
def redis_server():
    """Start redis-lite before tests, kill it after."""
    proc = subprocess.Popen([SERVER_BIN], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_for_port(SERVER_HOST, SERVER_PORT)
        yield proc
    finally:
        proc.terminate()
        proc.wait(timeout=5)
