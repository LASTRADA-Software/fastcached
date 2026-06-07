# SPDX-License-Identifier: Apache-2.0
"""Data-driven catalog of benchmark scenarios.

Everything the suite runs is declared here as data — no magic literals scattered
through the runner. A :class:`Scenario` fully describes one measurement: which
protocol, which mix of operations, value size, key space, connection count,
storage backend, durability, and how many operations to issue. :data:`PROFILES`
selects subsets for quick / standard / full runs.
"""

from __future__ import annotations

import dataclasses

from protocols import SUPPORTED_OPS

# Operation mixes expressed as (op, weight) pairs. The runner draws ops in these
# proportions from a fixed RNG so every repetition issues the identical sequence.
OP_MIXES: dict[str, tuple[tuple[str, int], ...]] = {
    "set": (("set", 1),),
    "get": (("get", 1),),
    "mixed": (("set", 1), ("get", 9)),  # write-light, read-heavy (cache-like)
    "delete": (("delete", 1),),
    "incr": (("incr", 1),),
}


@dataclasses.dataclass(frozen=True)
class Scenario:
    """One fully-specified benchmark measurement.

    :param name: Stable identifier used as the results key and chart label.
    :param protocol: One of :data:`protocols.PROTOCOLS`.
    :param op_mix: Key into :data:`OP_MIXES`.
    :param value_bytes: Size of each value written.
    :param key_count: Size of the working key set (keys are reused round-robin).
    :param connections: Number of concurrent keep-alive connections.
    :param storage: ``"memory"`` or ``"disk"`` (adds ``--storage``).
    :param durability: ``fsync`` | ``batched`` | ``none`` (disk only).
    :param ops_total: Total operations across all connections per repetition.
    :param prepopulate: Seed the key set with SETs before the (timed) phase so
        reads hit. Done untimed.
    :param keepalive_storm: All connections issue concurrently and the metric of
        interest is how many get a timely reply — the connection-ceiling test.
    """

    name: str
    protocol: str
    op_mix: str
    value_bytes: int
    key_count: int
    connections: int
    storage: str
    durability: str
    ops_total: int
    prepopulate: bool
    keepalive_storm: bool = False

    def __post_init__(self) -> None:
        ops = {op for op, _ in OP_MIXES[self.op_mix]}
        unsupported = ops - SUPPORTED_OPS[self.protocol]
        if unsupported:
            raise ValueError(f"{self.protocol} cannot run ops {sorted(unsupported)} in {self.name!r}")
        if self.storage not in ("memory", "disk"):
            raise ValueError(f"bad storage {self.storage!r}")


# --- Catalog -------------------------------------------------------------------

_SMALL_VALUE = 64
_LARGE_VALUE = 64 * 1024
_KEYSPACE = 10_000

# Disk writes go through the CoW tree (a few ms each) and serialise on the
# single shard's lock, so disk scenarios use far fewer ops and a smaller key
# set than the in-memory ones — otherwise a single scenario would run for
# minutes. Reads still come from the in-memory L1 mirror, so they stay cheap.
_DISK_KEYSPACE = 2_000
_DISK_WRITE_OPS = 2_000   # set-heavy / mixed disk scenarios
_DISK_READ_OPS = 20_000   # get-only disk scenarios hit the L1 mirror


def _disk_ops(mix: str) -> int:
    return _DISK_READ_OPS if mix == "get" else _DISK_WRITE_OPS


def _matrix(
    protocols: tuple[str, ...],
    mixes: tuple[str, ...],
    storages: tuple[str, ...],
    connection_levels: tuple[int, ...],
    ops_total: int,
    value_bytes: int = _SMALL_VALUE,
) -> list[Scenario]:
    """Expand the cartesian product into Scenario objects (storage-aware sizing)."""
    scenarios: list[Scenario] = []
    for protocol in protocols:
        for mix in mixes:
            if {op for op, _ in OP_MIXES[mix]} - SUPPORTED_OPS[protocol]:
                continue  # silently skip combos a protocol cannot serve
            for storage in storages:
                disk = storage == "disk"
                # Disk writes serialise on the single shard lock, so very high
                # concurrency just thrashes that lock and adds noise. Cap disk
                # to the low end of the connection sweep; in-memory uses the
                # full sweep.
                levels = tuple(c for c in connection_levels if c <= 16) or (1,) if disk else connection_levels
                for connections in levels:
                    scenarios.append(
                        Scenario(
                            name=f"{protocol}/{mix}/{storage}/c{connections}",
                            protocol=protocol,
                            op_mix=mix,
                            value_bytes=value_bytes,
                            key_count=_DISK_KEYSPACE if disk else _KEYSPACE,
                            connections=connections,
                            storage=storage,
                            durability="batched",
                            ops_total=_disk_ops(mix) if disk else ops_total,
                            prepopulate=(mix in ("get", "mixed")),
                        )
                    )
    return scenarios


def _keepalive_storm(connections: int = 256, ops_per_connection: int = 40) -> Scenario:
    """The headline connection-ceiling demonstration (persistent storage)."""
    return Scenario(
        name=f"keepalive-storm/redis/disk/c{connections}",
        protocol="redis",
        op_mix="mixed",
        value_bytes=_SMALL_VALUE,
        key_count=_DISK_KEYSPACE,
        connections=connections,
        storage="disk",
        durability="batched",
        ops_total=connections * ops_per_connection,
        prepopulate=True,
        keepalive_storm=True,
    )


def _quick() -> list[Scenario]:
    return [
        *_matrix(("redis", "memcached-text"), ("set", "get"), ("memory", "disk"), (64,), ops_total=20_000),
        _keepalive_storm(connections=128, ops_per_connection=40),
    ]


def _standard() -> list[Scenario]:
    return [
        *_matrix(
            ("memcached-text", "memcached-binary", "redis"),
            ("set", "get", "mixed"),
            ("memory", "disk"),
            (1, 16, 64, 256),
            ops_total=50_000,
        ),
        _keepalive_storm(),
    ]


def _full() -> list[Scenario]:
    scenarios = _standard()
    # Larger values (object-file-chunk sized).
    scenarios += _matrix(
        ("memcached-text", "memcached-binary", "redis"),
        ("set", "get"),
        ("memory", "disk"),
        (16, 64),
        ops_total=10_000,
        value_bytes=_LARGE_VALUE,
    )
    # Counters and deletes where supported.
    scenarios += _matrix(
        ("memcached-text", "memcached-binary"),
        ("incr",),
        ("memory", "disk"),
        (16, 64),
        ops_total=50_000,
    )
    scenarios += _matrix(
        ("redis", "memcached-text"),
        ("delete",),
        ("disk",),
        (16, 64),
        ops_total=50_000,
    )
    # Durability sweep on a write-only disk workload.
    for durability in ("fsync", "batched", "none"):
        scenarios.append(
            Scenario(
                name=f"redis/set/disk-{durability}/c64",
                protocol="redis",
                op_mix="set",
                value_bytes=_SMALL_VALUE,
                key_count=_KEYSPACE,
                connections=64,
                storage="disk",
                durability=durability,
                ops_total=50_000,
                prepopulate=False,
            )
        )
    return scenarios


PROFILES = {
    "quick": _quick,
    "standard": _standard,
    "full": _full,
}


def scenarios_for(profile: str) -> list[Scenario]:
    """Return the scenario list for a profile name."""
    return PROFILES[profile]()
