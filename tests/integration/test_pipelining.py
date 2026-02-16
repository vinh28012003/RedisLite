"""Integration tests for pipelining."""
import socket


def test_pipeline_multiple_commands(redis_server):
    """Send 3 commands in one write, get 3 responses back."""
    ping = b"*1\r\n$4\r\nPING\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        s.sendall(ping + ping + ping)  # 3 commands, one TCP write
        response = b""
        while response.count(b"+PONG\r\n") < 3:
            response += s.recv(1024)
        assert response == b"+PONG\r\n+PONG\r\n+PONG\r\n"