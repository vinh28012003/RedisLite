"""Integration tests for INFO replication command."""
import socket

def send_command(sock, *args):
    """Encode args as a RESP array and send over socket."""
    # *N\r\n followed by $len\r\narg\r\n for each arg
    parts = [f"*{len(args)}\r\n"]
    for arg in args:
        parts.append(f"${len(arg)}\r\n{arg}\r\n")
    sock.sendall("".join(parts).encode())
    
def recv_response(sock):
    """Read full response from socket."""
    return sock.recv(4096)

def test_info_replication_returns_role_master(redis_server):
    """INFO replication returns bulk string containing role:master."""
    with socket.create_connection(("127.0.0.1", 6379), timeout = 2) as s:
        send_command(s, "INFO", "replication")
        response = recv_response(s)
        # Exact bulk string : $11\r\nrole:master\r\n
        assert response == b"$11\r\nrole:master\r\n"
        
def test_info_no_section_defaults_to_replication(redis_server):
    """INFO with no args still returns replication info."""
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        send_command(s, "INFO")
        response = recv_response(s)
        assert b"role:master" in response

def test_info_unknown_section_returns_empty_bulk(redis_server):
    """INFO with unrecognized section returns empty bulk string."""
    with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
        send_command(s, "INFO", "memory")
        response = recv_response(s)
        # Empty bulk string: $0\r\n\r\n
        assert response == b"$0\r\n\r\n"
        
def test_info_replication_returns_role_worker(replica_server):
    """INFO replication on replica instance returns role:worker."""
    with socket.create_connection(("127.0.0.1", 6380), timeout=2) as s:
        send_command(s, "INFO", "replication")
        response = recv_response(s)
        assert b"role:worker" in response
        