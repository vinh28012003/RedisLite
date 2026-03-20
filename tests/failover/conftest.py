"""
Fixtures and helpers for failover integration tests.
Raw RESP over TCP — no redis-py dependency.
"""

import socket
import subprocess
import time


# --- RESP Protocol Helpers ---

def encode_resp(*args):
    """Encode command args into RESP array format."""
    parts = [f"*{len(args)}\r\n"]
    for arg in args:
        arg_str = str(arg)
        parts.append(f"${len(arg_str)}\r\n{arg_str}\r\n")
    return "".join(parts).encode()


def send_command(host, port, *args, timeout=2):
    """Send a RESP command and return raw response bytes."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect((host, int(port)))
        sock.sendall(encode_resp(*args))
        return sock.recv(4096)
    finally:
        sock.close()


def get_role(host, port):
    """ROLE command → return 'master' or 'slave'."""
    resp = send_command(host, port, "ROLE").decode(errors="replace")
    for line in resp.split("\r\n"):
        if line in ("master", "slave"):
            return line
    return "unknown"


def get_info_field(host, port, section, field):
    """INFO <section> → parse key:value, return field value or None."""
    resp = send_command(host, port, "INFO", section).decode(errors="replace")
    for line in resp.split("\r\n"):
        if ":" in line and not line.startswith("$"):
            k, v = line.split(":", 1)
            if k.strip() == field:
                return v.strip()
    return None


# --- Docker Compose Helpers ---

COMPOSE_FILE = "docker/docker-compose.failover.yml"


def compose_stop(service):
    """docker compose stop <service>."""
    subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, "stop", service],
        check=True, capture_output=True,
    )


def compose_start(service):
    """docker compose start <service>."""
    subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE, "start", service],
        check=True, capture_output=True,
    )


# --- Polling Helper ---

def wait_for_condition(fn, timeout=15, interval=0.5, description="condition"):
    """Poll fn() until truthy, raise on timeout."""
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        try:
            result = fn()
            if result:
                return result
        except Exception as e:
            last_err = e
        time.sleep(interval)
    msg = f"Timed out waiting for {description} ({timeout}s)"
    if last_err:
        msg += f" — last error: {last_err}"
    raise TimeoutError(msg)


# --- Port Mapping ---
# Container name → host port (compose maps container:6379 → host:port)
NODES = {
    "redis-1": 6379,
    "redis-2": 6380,
    "redis-3": 6381,
    "redis-4": 6382,
    "redis-5": 6383,
}
