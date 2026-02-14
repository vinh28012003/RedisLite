"""Skeleton test — proves pytest + server pipeline works. Delete when real tests exist."""
import socket


def test_server_accepts_connection(redis_server):
    """Verify the server is up and accepts a TCP connection."""
    with socket.create_connection(("127.0.0.1", 6379), timeout=2):
        pass  # Connection succeeded — server is alive
