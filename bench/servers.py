# SPDX-License-Identifier: Apache-2.0
"""Real redis / memcached baselines for head-to-head benchmarking.

These wrap a real ``redis-server`` and a real ``memcached`` (both native
binaries) so the suite can chart fastcached against the actual competitors.
Both are optional: if the binary is unavailable, discovery returns None and the
caller skips that baseline.

Running both competitors natively — rather than memcached in Docker — keeps the
comparison fair, since Docker's userland port-forwarding adds latency that would
penalise the dockerized server. Custom high ports are used throughout (some low
ports are blocked).
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

    # redis-server speaks both RESP2 and (since 6.0) RESP3, so it can serve as a
    # baseline for either fastcached protocol client in --vs mode.
    protocols = frozenset({"redis", "redis-resp3"})

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
    """A native ``memcached`` binary run as a subprocess.

    Running the native binary (rather than a Docker container) keeps the
    comparison fair: Docker's userland port-forwarding adds latency that would
    penalise memcached relative to the natively-run fastcached and redis.
    """

    protocols = frozenset({"memcached-text", "memcached-binary"})

    def __init__(self, binary: str, host: str, port: int) -> None:
        self.name = "memcached"
        self._binary = binary
        self.host = host
        self.port = port
        self._process: subprocess.Popen | None = None

    def start(self) -> None:
        # -l binds the listen address, -p the TCP port. Defaults (64 MiB, etc.)
        # match a stock cache; no persistence to configure (memcached is RAM-only).
        self._process = subprocess.Popen(
            [self._binary, "-l", self.host, "-p", str(self.port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if not _wait_port(self.host, self.port, READY_TIMEOUT_SECONDS):
            self.stop()
            raise RuntimeError(f"memcached did not become ready on port {self.port}")

    def stop(self) -> None:
        if self._process is not None:
            self._process.terminate()
            try:
                self._process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._process.kill()
            self._process = None


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


def discover_memcached() -> str | None:
    """Locate a native ``memcached`` binary on PATH, or None if unavailable."""
    return shutil.which("memcached")


def build_baseline(name: str, host: str, port: int, redis_server: str | None) -> RealServer | None:
    """Construct a baseline server by name, or None if its dependency is missing."""
    if name == "redis":
        binary = discover_redis(redis_server)
        return RedisServer(binary, host, port) if binary else None
    if name == "memcached":
        binary = discover_memcached()
        return MemcachedServer(binary, host, port) if binary else None
    raise ValueError(f"unknown baseline {name!r}")
