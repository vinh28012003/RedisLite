"""                                                                                                 
Integration tests for command propagation (stages 11-13).                                           
Master propagates write commands to replica via replication connection.                             
"""                                                                                                 
import socket                                                                                       
import time
import pytest

MASTER_HOST = "127.0.0.1"
MASTER_PORT = 6379
REPLICA_PORT = 6380


def send_command(host, port, *args):
    """Send RESP array command and return raw response."""
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(arg)}\r\n{arg}\r\n"

    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(cmd.encode())
        return s.recv(4096).decode()


class TestPropagation:
    """Tests require replica_server fixture (master on 6379, replica on 6380)."""

    @pytest.fixture(autouse=True)
    def _require_replica(self, replica_server):
        pass

    def test_set_on_master_get_from_replica(self):
        resp = send_command(MASTER_HOST, MASTER_PORT, "SET", "propkey1", "hello")
        assert resp == "+OK\r\n"

        time.sleep(0.5)

        resp = send_command(MASTER_HOST, REPLICA_PORT, "GET", "propkey1")
        assert resp == "$5\r\nhello\r\n"

    def test_multiple_sets_propagate(self):
        send_command(MASTER_HOST, MASTER_PORT, "SET", "mk1", "aaa")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "mk2", "bbb")
        send_command(MASTER_HOST, MASTER_PORT, "SET", "mk3", "ccc")

        time.sleep(0.5)

        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "mk1") == "$3\r\naaa\r\n"
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "mk2") == "$3\r\nbbb\r\n"
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "mk3") == "$3\r\nccc\r\n"

    def test_replica_does_not_have_unpropagated_keys(self):
        resp = send_command(MASTER_HOST, REPLICA_PORT, "GET", "nonexistent_prop_key")
        assert resp == "$-1\r\n"
    
    def test_two_replicas_both_receive_propagation(self, replica_6381):
        send_command(MASTER_HOST, MASTER_PORT, "SET", "multi2key", "two")
        time.sleep(0.5)
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "multi2key") == "$3\r\ntwo\r\n"
        assert send_command(MASTER_HOST, 6381, "GET", "multi2key") == "$3\r\ntwo\r\n"

    def test_three_replicas_all_receive_propagation(self, replica_6381, replica_6382):
        send_command(MASTER_HOST, MASTER_PORT, "SET", "multi3key", "three")
        time.sleep(0.5)
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "multi3key") == "$5\r\nthree\r\n"
        assert send_command(MASTER_HOST, 6381, "GET", "multi3key") == "$5\r\nthree\r\n"
        assert send_command(MASTER_HOST, 6382, "GET", "multi3key") == "$5\r\nthree\r\n"
    
    def test_overwrite_propagates_latest_value(self):                                                   
        send_command(MASTER_HOST, MASTER_PORT, "SET", "overkey", "old")                                 
        send_command(MASTER_HOST, MASTER_PORT, "SET", "overkey", "new")                                 
        time.sleep(0.5)                                                                                 
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "overkey") == "$3\r\nnew\r\n" 
        
    def test_read_commands_do_not_propagate(self):
        send_command(MASTER_HOST, MASTER_PORT, "GET", "noexist_read")
        send_command(MASTER_HOST, MASTER_PORT, "PING")
        send_command(MASTER_HOST, MASTER_PORT, "ECHO", "hello")
        time.sleep(0.5)
        # Replica should have no side effects from read commands
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "noexist_read") == "$-1\r\n"
    
    def test_empty_value_propagates(self):
        send_command(MASTER_HOST, MASTER_PORT, "SET", "emptykey", "")
        time.sleep(0.5)
        assert send_command(MASTER_HOST, REPLICA_PORT, "GET", "emptykey") == "$0\r\n\r\n"