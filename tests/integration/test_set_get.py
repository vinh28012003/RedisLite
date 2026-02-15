"""Integration tests for SET/GET commands."""
import socket


def send_command(sock, cmd):
    """Send raw bytes and receive response."""
    sock.sendall(cmd)
    return sock.recv(1024)


def test_set_returns_ok(redis_server):
    """SET foo bar returns +OK."""
    cmd = b"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        response = send_command(s, cmd)
        assert response == b"+OK\r\n"


def test_set_then_get(redis_server):
    """SET then GET returns the value."""
    set_cmd = b"*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n"
    get_cmd = b"*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        send_command(s, set_cmd)
        response = send_command(s, get_cmd)
        assert response == b"$7\r\nmyvalue\r\n"


def test_get_missing_key(redis_server):
    """GET on nonexistent key returns null bulk string."""
    cmd = b"*2\r\n$3\r\nGET\r\n$11\r\nnonexistent\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        response = send_command(s, cmd)
        assert response == b"$-1\r\n"
