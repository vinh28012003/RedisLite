"""Integration tests for ECHO command."""                                                               
import socket                                                                                           
                                                                                                        
                                                                                                        
RESP_ECHO_CMD = b"*2\r\n$4\r\nECHO\r\n$3\r\nhey\r\n"
RESP_ECHO_RESPONSE = b"$3\r\nhey\r\n"                                                                   
                                                                                                        

def send_command(sock, cmd):
    """Send raw bytes and receive response."""
    sock.sendall(cmd)
    return sock.recv(1024)


def test_echo_returns_argument(redis_server):
    """ECHO hey returns bulk string hey."""
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        response = send_command(s, RESP_ECHO_CMD)
        assert response == RESP_ECHO_RESPONSE


def test_echo_with_long_string(redis_server):
    """ECHO with a longer string returns correct bulk string."""
    msg = b"hello world"
    cmd = b"*2\r\n$4\r\nECHO\r\n$11\r\nhello world\r\n"
    expected = b"$11\r\nhello world\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        response = send_command(s, cmd)
        assert response == expected


def test_echo_case_insensitive(redis_server):
    """echo (lowercase) works the same as ECHO."""
    cmd = b"*2\r\n$4\r\necho\r\n$4\r\ntest\r\n"
    expected = b"$4\r\ntest\r\n"
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        response = send_command(s, cmd)
        assert response == expected