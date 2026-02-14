"""Integration tests for PING/PONG stages."""
import socket
import threading


RESP_PONG = b"+PONG\r\n"


def send_ping(sock):
    """Send a raw RESP PING command."""
    sock.sendall(b"*1\r\n$4\r\nPING\r\n")


def test_ping_returns_pong(redis_server):
    """Stage 1: Server responds +PONG\r\n to a PING command."""
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        send_ping(s)
        response = s.recv(1024)
        assert response == RESP_PONG


def test_multiple_pings_same_connection(redis_server):
    """Stage 2: Multiple PINGs on the same connection each get PONG."""
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        for _ in range(3):
            send_ping(s)
            response = s.recv(1024)
            assert response == RESP_PONG


def test_concurrent_clients(redis_server):
    """Stage 3: Two clients connect and get PONG simultaneously."""
    results = [None, None]

    def ping_client(index):
        with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
            send_ping(s)
            results[index] = s.recv(1024)

    t1 = threading.Thread(target=ping_client, args=(0,))
    t2 = threading.Thread(target=ping_client, args=(1,))
    t1.start()
    t2.start()
    t1.join(timeout=5)
    t2.join(timeout=5)

    assert results[0] == RESP_PONG
    assert results[1] == RESP_PONG