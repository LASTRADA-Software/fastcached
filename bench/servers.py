# SPDX-License-Identifier: Apache-2.0
"""Real redis / memcached baselines for head-to-head benchmarking.

These wrap a real ``redis-server`` (native binary) and ``memcached`` (Docker
image) so the suite can chart fastcached against the actual competitors. Both
are optional: if the binary / Docker image is unavailable, discovery returns
None and the caller skips that baseline.

On the Windows dev box: ``redis-server.exe`` is installed natively; memcached
runs via ``docker run memcached:latest``. Custom high ports are used throughout
(some low ports are blocked).
"""

from __future__ import annotations

import shutil
import socket
import subprocess
import time
from pathlib import Path

import protocols

READY_TIMEOUT_SECONDS = 20.0
_KNOWN_REDIS_PATHS = (r"C:\Program Files\Redis\redis-server.exe",)


def _wait_port(host: str, port: int, timeout: float) -> bool:
    """Poll until a TCP connection to host:port succeeds, or timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


class RealServer:
    """Common interface for a real baseline server."""

    name: str
    host: str
    port: int
    protocols: frozenset[str]

    def start(self) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    def stop(self) -> None:  # pragma: no cover - interface
        raise NotImplementedError


class RedisServer(RealServer):
    """A native ``redis-server`` with persistence disabled (pure in-memory)."""

    protocols = frozenset({"redis"})

    def __init__(self, binary: str, host: str, port: int) -> None:
        self.name = "redis"
        self._binary = binary
        self.host = host
        self.port = port
        self._process: subprocess.Popen | None = None

    def start(self) -> None:
        # --save "" and --appendonly no keep it a pure in-memory cache, matching
        # how fastcached's in-memory mode is measured.
        self._process = subprocess.Popen(
            [self._binary, "--port", str(self.port), "--save", "", "--appendonly", "no"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if not _wait_port(self.host, self.port, READY_TIMEOUT_SECONDS):
            self.stop()
            raise RuntimeError(f"redis-server did not become ready on port {self.port}")

    def stop(self) -> None:
        if self._process is not None:
            self._process.terminate()
            try:
                self._process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._process.kill()
            self._process = None


class MemcachedServer(RealServer):
    """``memcached:latest`` run as a Docker container (host port -> 11211)."""

    protocols = frozenset({"memcached-text", "memcached-binary"})

    def __init__(self, host: str, port: int, image: str = "memcached:latest") -> None:
        self.name = "memcached"
        self.host = host
        self.port = port
        self._image = image
        self._container = f"fcbench-memcached-{port}"

    def start(self) -> None:
        # Remove any stale container from a previous aborted run, then start.
        subprocess.run(["docker", "rm", "-f", self._container],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        result = subprocess.run(
            ["docker", "run", "--rm", "-d", "--name", self._container,
             "-p", f"{self.port}:11211", self._image],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(f"docker run memcached failed: {result.stderr.strip()}")
        if not _wait_port(self.host, self.port, READY_TIMEOUT_SECONDS):
            self.stop()
            raise RuntimeError(f"memcached container did not become ready on port {self.port}")

    def stop(self) -> None:
        subprocess.run(["docker", "stop", self._container],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def discover_redis(redis_server: str | None = None) -> str | None:
    """Locate a redis-server binary, or None if unavailable."""
    if redis_server:
        return redis_server if Path(redis_server).exists() else None
    found = shutil.which("redis-server")
    if found:
        return found
    for candidate in _KNOWN_REDIS_PATHS:
        if Path(candidate).exists():
            return candidate
    return None


def docker_available(image: str = "memcached:latest") -> bool:
    """True if docker is on PATH and the memcached image is present."""
    if shutil.which("docker") is None:
        return False
    result = subprocess.run(["docker", "image", "inspect", image],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    return result.returncode == 0


def build_baseline(name: str, host: str, port: int, redis_server: str | None) -> RealServer | None:
    """Construct a baseline server by name, or None if its dependency is missing."""
    if name == "redis":
        binary = discover_redis(redis_server)
        return RedisServer(binary, host, port) if binary else None
    if name == "memcached":
        return MemcachedServer(host, port) if docker_available() else None
    raise ValueError(f"unknown baseline {name!r}")
