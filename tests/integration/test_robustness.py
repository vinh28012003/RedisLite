"""
Robustness tests — server survives malformed input and shuts down cleanly.
"""
import socket
import subprocess
import time
import signal

from conftest import SERVER_HOST, SERVER_PORT, wait_for_ready, _kill_container_replica


def _resp_array(*args: str) -> bytes:
    """Encode a RESP array from string arguments."""
    parts = [f"*{len(args)}\r\n"]
    for arg in args:
        parts.append(f"${len(arg)}\r\n{arg}\r\n")
    return "".join(parts).encode()


def test_malformed_replconf_ack_does_not_crash_server():
    """A replica sending a non-numeric ACK offset must not crash the master.

    Steps:
    1. Connect as a fake replica (PSYNC handshake)
    2. Send REPLCONF ACK with garbage offset
    3. Verify master still responds to PING on a new connection
    """
    sock = socket.create_connection((SERVER_HOST, SERVER_PORT), timeout=2)
    try:
        # Do PSYNC handshake to become a replica
        sock.sendall(_resp_array("REPLCONF", "listening-port", "9999"))
        resp = sock.recv(1024)
        assert b"+OK" in resp

        sock.sendall(_resp_array("REPLCONF", "capa", "psync2"))
        resp = sock.recv(1024)
        assert b"+OK" in resp

        sock.sendall(_resp_array("PSYNC", "?", "-1"))
        # Consume FULLRESYNC + RDB (don't care about contents)
        data = b""
        while len(data) < 100:  # Wait for enough data
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk

        assert b"+FULLRESYNC" in data

        # Now send malformed ACK — this is the crash-triggering payload
        sock.sendall(_resp_array("REPLCONF", "ACK", "not_a_number"))
        time.sleep(0.3)

        # Send another malformed ACK with empty string
        sock.sendall(_resp_array("REPLCONF", "ACK", ""))
        time.sleep(0.3)

    finally:
        sock.close()

    # Verify server is still alive — PING on a fresh connection
    verify = socket.create_connection((SERVER_HOST, SERVER_PORT), timeout=2)
    try:
        verify.sendall(_resp_array("PING"))
        resp = verify.recv(1024)
        assert resp == b"+PONG\r\n", f"Server crashed or unresponsive after malformed ACK: {resp}"
    finally:
        verify.close()


def test_graceful_shutdown_on_sigterm():
    """Server exits cleanly (exit code 0) when receiving SIGTERM.

    Starts a standalone server on port 6384 inside the container,
    sends SIGTERM, and verifies it exits with code 0.
    """
    _kill_container_replica(6384)

    # Start a standalone server on port 6384
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "./build/redis-lite", "--port", "6384"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        wait_for_ready(SERVER_HOST, 6384, timeout=5)
    except TimeoutError:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("Server on port 6384 failed to start")

    # Send SIGTERM to the redis-lite process inside the container
    subprocess.run(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
         "exec", "-T", "redis-lite",
         "bash", "-c", "kill -TERM $(pgrep -f 'redis-lite.*6384')"],
        timeout=5
    )

    # Wait for the process to exit
    try:
        returncode = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("Server did not exit after SIGTERM within 5s")

    # docker exec returns the process exit code
    # Exit code 0 = clean shutdown
    assert returncode == 0, f"Expected exit code 0 (clean shutdown), got {returncode}"
