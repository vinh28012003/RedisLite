import subprocess

def test_replica_fails_with_unreachable_master():
    """Replica exits with error when master is not reachable."""
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", "6381",
        "--replicaof", "127.0.0.1", "9999"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    returncode = proc.wait(timeout=5)
    assert returncode != 0

def test_replica_fails_with_invalid_host():
    """Replica exits with error when master address is unresolvable."""
    proc = subprocess.Popen(
        ["docker", "compose", "-f", "docker/docker-compose.yml",
        "exec", "-T", "redis-lite",
        "./build/redis-lite", "--port", "6382",
        "--replicaof", "999.999.999.999", "6379"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    returncode = proc.wait(timeout=5)
    assert returncode != 0