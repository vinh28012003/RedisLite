"""Integration tests for key expiration (PX/EX)."""
import socket
import time


def send_command(sock, cmd):
    """Send raw bytes and receive response."""
    sock.sendall(cmd)
    return sock.recv(1024)


def test_set_with_px_then_get(redis_server):
    """SET with PX, GET immediately returns value."""
    set_cmd = b"*5\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$2\r\nPX\r\n$3\r\n500\r\n"
    get_cmd = b"*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        send_command(s, set_cmd)
        response = send_command(s, get_cmd)
        assert response == b"$3\r\nbar\r\n"


def test_set_with_px_expires(redis_server):
    """SET with PX, GET after expiry returns nil."""
    set_cmd = b"*5\r\n$3\r\nSET\r\n$6\r\nexpkey\r\n$3\r\nval\r\n$2\r\nPX\r\n$3\r\n100\r\n"
    get_cmd = b"*2\r\n$3\r\nGET\r\n$6\r\nexpkey\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        send_command(s, set_cmd)
        time.sleep(0.2)
        response = send_command(s, get_cmd)
        assert response == b"$-1\r\n"


def test_px_case_insensitive(redis_server):
    """px (lowercase) works the same as PX."""
    set_cmd = b"*5\r\n$3\r\nSET\r\n$5\r\ncikey\r\n$3\r\nval\r\n$2\r\npx\r\n$3\r\n500\r\n"
    get_cmd = b"*2\r\n$3\r\nGET\r\n$5\r\ncikey\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        send_command(s, set_cmd)
        response = send_command(s, get_cmd)
        assert response == b"$3\r\nval\r\n"