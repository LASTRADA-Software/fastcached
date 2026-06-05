# SPDX-License-Identifier: Apache-2.0
"""Daemon lifecycle, load generation, and metric aggregation.

The load generator spreads a scenario's keep-alive connections across a fixed
pool of worker **processes** (to get past Python's GIL on the client side); each
worker drives its connections concurrently with **threads** (socket I/O releases
the GIL). The work each connection performs is fully deterministic — derived
from a fixed seed and the connection index — so every repetition issues the
identical request sequence and results are reproducible.
"""

from __future__ import annotations

import concurrent.futures
import dataclasses
import random
import socket
import statistics
import subprocess
import threading
import time
from array import array
from pathlib import Path

import protocols
from workloads import OP_MIXES, Scenario

READY_MARKER = "ready, accepting connections"
READY_TIMEOUT_SECONDS = 20.0
STOP_TIMEOUT_SECONDS = 10.0


# --- Deterministic workload generation ----------------------------------------

def _build_op_sequence(rng: random.Random, mix: str, count: int) -> list[str]:
    """Expand a weighted op mix into a fixed-length op list (deterministic)."""
    ops, weights = zip(*OP_MIXES[mix])
    return rng.choices(ops, weights=weights, k=count)


def _key_for(index: int) -> bytes:
    return b"benchkey:%08d" % index


def _dispatch(client, op: str, key: bytes, value: bytes) -> None:
    if op == "set":
        client.set(key, value)
    elif op == "get":
        client.get(key)
    elif op == "delete":
        client.delete(key)
    elif op == "incr":
        client.incr(key, 1)
    else:  # pragma: no cover - guarded by Scenario validation
        raise ValueError(f"unknown op {op!r}")


def _run_connection(
    connection_index: int,
    protocol: str,
    host: str,
    port: int,
    ops_per_connection: int,
    op_mix: str,
    value_bytes: int,
    key_count: int,
    seed: int,
    op_timeout: float,
) -> tuple[list[float], int, int, int]:
    """Drive one keep-alive connection; return (latencies_ms, errors, timeouts, done)."""
    rng = random.Random(seed * 1_000_003 + connection_index)
    value = b"v" * value_bytes
    try:
        client = protocols.make_client(protocol, host, port, op_timeout)
    except OSError:
        return ([], 1, 0, 0)

    latencies = array("d")
    errors = timeouts = done = 0
    sequence = _build_op_sequence(rng, op_mix, ops_per_connection)
    try:
        for op in sequence:
            key = _key_for(rng.randrange(key_count))
            start = time.perf_counter()
            try:
                _dispatch(client, op, key, value)
            except (socket.timeout, TimeoutError):
                timeouts += 1
                break
            except (OSError, protocols.ProtocolError):
                errors += 1
                break
            latencies.append((time.perf_counter() - start) * 1000.0)
            done += 1
    finally:
        client.close()
    return (list(latencies), errors, timeouts, done)


@dataclasses.dataclass(frozen=True)
class _WorkerSpec:
    """Picklable description of one worker process's share of the load."""

    connection_indices: tuple[int, ...]
    protocol: str
    host: str
    port: int
    ops_per_connection: int
    op_mix: str
    value_bytes: int
    key_count: int
    seed: int
    op_timeout: float


def _run_worker(spec: _WorkerSpec) -> tuple[list[float], int, int, int]:
    """Process entry point: drive this worker's connections via threads.

    Top-level and picklable so it works under both the ``fork`` (Linux) and
    ``spawn`` (Windows/macOS) multiprocessing start methods.
    """
    results: list[tuple[list[float], int, int, int]] = [None] * len(spec.connection_indices)  # type: ignore[list-item]

    def target(slot: int, connection_index: int) -> None:
        results[slot] = _run_connection(
            connection_index,
            spec.protocol,
            spec.host,
            spec.port,
            spec.ops_per_connection,
            spec.op_mix,
            spec.value_bytes,
            spec.key_count,
            spec.seed,
            spec.op_timeout,
        )

    threads = [
        threading.Thread(target=target, args=(slot, index))
        for slot, index in enumerate(spec.connection_indices)
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    latencies: list[float] = []
    errors = timeouts = done = 0
    for connection_latencies, connection_errors, connection_timeouts, connection_done in results:
        latencies.extend(connection_latencies)
        errors += connection_errors
        timeouts += connection_timeouts
        done += connection_done
    return (latencies, errors, timeouts, done)


# --- Metrics ------------------------------------------------------------------

def _percentile(sorted_values: list[float], fraction: float) -> float:
    if not sorted_values:
        return 0.0
    index = min(len(sorted_values) - 1, int(round(fraction * (len(sorted_values) - 1))))
    return sorted_values[index]


@dataclasses.dataclass
class RepMetrics:
    """Metrics from a single repetition of a scenario."""

    throughput_ops_per_sec: float
    latency_p50_ms: float
    latency_p95_ms: float
    latency_p99_ms: float
    latency_max_ms: float
    ops_completed: int
    errors: int
    timeouts: int
    connections_started: int
    connections_completed: int


def _measure(
    executor: concurrent.futures.ProcessPoolExecutor,
    pool_size: int,
    scenario: Scenario,
    host: str,
    port: int,
    seed: int,
    op_timeout: float,
) -> RepMetrics:
    """Run one timed repetition across all of the scenario's connections."""
    connections = scenario.connections
    ops_per_connection = max(1, scenario.ops_total // connections)

    # Spread connection indices across at most `pool_size` worker processes.
    worker_count = min(pool_size, connections)
    buckets: list[list[int]] = [[] for _ in range(worker_count)]
    for connection_index in range(connections):
        buckets[connection_index % worker_count].append(connection_index)

    specs = [
        _WorkerSpec(
            connection_indices=tuple(bucket),
            protocol=scenario.protocol,
            host=host,
            port=port,
            ops_per_connection=ops_per_connection,
            op_mix=scenario.op_mix,
            value_bytes=scenario.value_bytes,
            key_count=scenario.key_count,
            seed=seed,
            op_timeout=op_timeout,
        )
        for bucket in buckets
    ]

    start = time.perf_counter()
    aggregated = list(executor.map(_run_worker, specs))
    elapsed = time.perf_counter() - start

    latencies: list[float] = []
    errors = timeouts = done = completed_connections = 0
    for connection_latencies, connection_errors, connection_timeouts, connection_done in aggregated:
        latencies.extend(connection_latencies)
        errors += connection_errors
        timeouts += connection_timeouts
        done += connection_done
    # A connection "completed" if it issued all its ops without error/timeout.
    # We approximate per-connection completion via the storm's success signal in
    # report; here we record started vs ops to derive completion ratio.
    completed_connections = connections - errors - timeouts

    latencies.sort()
    throughput = done / elapsed if elapsed > 0 else 0.0
    return RepMetrics(
        throughput_ops_per_sec=throughput,
        latency_p50_ms=_percentile(latencies, 0.50),
        latency_p95_ms=_percentile(latencies, 0.95),
        latency_p99_ms=_percentile(latencies, 0.99),
        latency_max_ms=(latencies[-1] if latencies else 0.0),
        ops_completed=done,
        errors=errors,
        timeouts=timeouts,
        connections_started=connections,
        connections_completed=max(0, completed_connections),
    )


@dataclasses.dataclass
class ScenarioResult:
    """Aggregated (median across reps) metrics for one scenario."""

    name: str
    protocol: str
    op_mix: str
    storage: str
    durability: str
    connections: int
    value_bytes: int
    throughput_ops_per_sec: float
    latency_p50_ms: float
    latency_p95_ms: float
    latency_p99_ms: float
    latency_max_ms: float
    errors: int
    timeouts: int
    connections_completed: int
    reps: list[RepMetrics]

    def as_dict(self) -> dict:
        data = dataclasses.asdict(self)
        data["reps"] = [dataclasses.asdict(rep) for rep in self.reps]
        return data


def _aggregate(scenario: Scenario, reps: list[RepMetrics]) -> ScenarioResult:
    median = statistics.median
    return ScenarioResult(
        name=scenario.name,
        protocol=scenario.protocol,
        op_mix=scenario.op_mix,
        storage=scenario.storage,
        durability=scenario.durability,
        connections=scenario.connections,
        value_bytes=scenario.value_bytes,
        throughput_ops_per_sec=median(r.throughput_ops_per_sec for r in reps),
        latency_p50_ms=median(r.latency_p50_ms for r in reps),
        latency_p95_ms=median(r.latency_p95_ms for r in reps),
        latency_p99_ms=median(r.latency_p99_ms for r in reps),
        latency_max_ms=max(r.latency_max_ms for r in reps),
        errors=max(r.errors for r in reps),
        timeouts=max(r.timeouts for r in reps),
        connections_completed=min(r.connections_completed for r in reps),
        reps=reps,
    )


# --- Daemon lifecycle ---------------------------------------------------------

class Daemon:
    """Launches and stops a fastcached process for one scenario."""

    def __init__(self, binary: Path, scenario: Scenario, host: str, port: int, storage_dir: Path) -> None:
        self._binary = binary
        self._scenario = scenario
        self._host = host
        self._port = port
        self._storage_dir = storage_dir
        self._process: subprocess.Popen | None = None
        self._stderr_thread: threading.Thread | None = None
        self._ready = threading.Event()

    def _argv(self) -> list[str]:
        # The CoW page size is derived from --storage-max-value, and every
        # write shuffles whole pages, so an oversized cap makes disk writes
        # pay for megabyte pages (the default 16 MiB cap => ~200 ms/write).
        # Size the cap just above the scenario's value so the page is small.
        max_value = max(self._scenario.value_bytes + 8192, 16384)
        argv = [
            str(self._binary),
            f"--bind={self._host}",
            f"--port={self._port}",
            "--log-level=info",
            "--max-memory=1g",
            f"--storage-max-value={max_value}",
        ]
        if self._scenario.storage == "disk":
            # Extension-less path => a shard directory (shard-by-default), so
            # the multi-core reactors write to independent CoW files in parallel.
            storage_path = self._storage_dir / "store"
            argv.append(f"--storage={storage_path}")
            argv.append(f"--storage-durability={self._scenario.durability}")
        return argv

    def _drain_stderr(self) -> None:
        assert self._process is not None and self._process.stderr is not None
        for line in self._process.stderr:
            if READY_MARKER in line:
                self._ready.set()
        # Keep draining to EOF so the pipe never blocks the daemon.

    def start(self) -> None:
        self._storage_dir.mkdir(parents=True, exist_ok=True)
        self._process = subprocess.Popen(
            self._argv(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()
        if not self._ready.wait(READY_TIMEOUT_SECONDS):
            self.stop()
            raise RuntimeError(f"daemon did not become ready within {READY_TIMEOUT_SECONDS}s")

    def stop(self) -> None:
        if self._process is None:
            return
        # terminate() = SIGTERM on POSIX (handled gracefully) / TerminateProcess
        # on Windows. Each scenario uses a throwaway storage dir, so an abrupt
        # stop is harmless.
        self._process.terminate()
        try:
            self._process.wait(timeout=STOP_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait(timeout=STOP_TIMEOUT_SECONDS)
        self._process = None


def _prepopulate(scenario: Scenario, host: str, port: int, op_timeout: float) -> None:
    """Seed the key set with SETs (untimed) so GET/mixed scenarios hit."""
    client = protocols.make_client(scenario.protocol, host, port, op_timeout)
    try:
        value = b"v" * scenario.value_bytes
        for index in range(scenario.key_count):
            client.set(_key_for(index), value)
    finally:
        client.close()


def _flush(scenario: Scenario, host: str, port: int, op_timeout: float) -> None:
    """Drop all keys — used to reset a long-lived (real) server between scenarios."""
    client = protocols.make_client(scenario.protocol, host, port, op_timeout)
    try:
        client.flush()
    finally:
        client.close()


def measure_running_server(
    executor: concurrent.futures.ProcessPoolExecutor,
    pool_size: int,
    scenario: Scenario,
    host: str,
    port: int,
    reps: int,
    warmup: int,
    seed: int,
    op_timeout: float,
    flush_first: bool = False,
) -> ScenarioResult:
    """Measure a scenario against an ALREADY-RUNNING server at host:port.

    Used both by the fastcached path (fresh daemon, ``flush_first=False``) and by
    the real-server baselines (a shared long-lived server, ``flush_first=True``
    to reset state between scenarios).
    """
    if flush_first:
        _flush(scenario, host, port, op_timeout)
    if scenario.prepopulate:
        _prepopulate(scenario, host, port, op_timeout)
    for _ in range(warmup):
        _measure(executor, pool_size, scenario, host, port, seed, op_timeout)
    measured = [
        _measure(executor, pool_size, scenario, host, port, seed + rep, op_timeout)
        for rep in range(reps)
    ]
    return _aggregate(scenario, measured)


def run_scenario(
    executor: concurrent.futures.ProcessPoolExecutor,
    pool_size: int,
    binary: Path,
    scenario: Scenario,
    host: str,
    port: int,
    storage_dir: Path,
    reps: int,
    warmup: int,
    seed: int,
    op_timeout: float,
) -> ScenarioResult:
    """Start a fastcached daemon, run warmup + measured reps, return aggregated metrics."""
    daemon = Daemon(binary, scenario, host, port, storage_dir)
    daemon.start()
    try:
        return measure_running_server(
            executor, pool_size, scenario, host, port, reps, warmup, seed, op_timeout, flush_first=False
        )
    finally:
        daemon.stop()
