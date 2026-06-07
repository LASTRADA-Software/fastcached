// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace FastCache
{

/// Durability policy for the persistent storage backend. Decoupled from
/// the storage subsystem so the Config layer does not depend on Cache
/// internals; main.cpp translates this into the backend's enum.
enum class StorageDurability : std::uint8_t
{
    Fsync = 0,   ///< fsync after every write. Slowest, safest.
    Batched = 1, ///< Buffer writes, fsync at commit boundaries (default).
    None = 2,    ///< OS page cache only; no fsync.
};

/// CPU-affinity policy for the reactor worker threads.
enum class CpuAffinity : std::uint8_t
{
    /// Let the OS scheduler place reactor threads (default for a lone reactor).
    None = 0,
    /// Pin each reactor thread to its own core (default with >1 reactor): keeps
    /// per-worker state cache-resident and avoids cross-core migration churn.
    PerCore = 1,
};

/// In-memory LRU recency policy. Decoupled from the Cache layer (like
/// StorageDurability); main.cpp translates this into the backend's `LruMode`.
enum class LruRecency : std::uint8_t
{
    /// Sampled/deferred promotion: reads run concurrently under a shared lock
    /// and eviction stays approximately-LRU. Favours read throughput. Default.
    Approximate = 0,
    /// Promote on every read for exact LRU order; reads on one shard serialise.
    Strict = 1,
};

/// All runtime configuration. POD-like value type; built once from CLI
/// arguments (and later, from a YAML config file). For SIGHUP reload, the
/// daemon keeps a shared_ptr<const Config> and atomically swaps.
struct Config
{
    /// In-memory storage byte budget. 0 = unbounded (testing/dev only).
    std::size_t maxMemoryBytes { 64 * 1024 * 1024 };

    /// Maximum size of a single cache value in bytes, enforced by every
    /// storage backend (in-memory LRU and on-disk COW tree). Defaults to
    /// 16 MiB, matching the protocol payload cap (`MaxPayloadBytes`), so
    /// the default imposes no extra limit beyond what the wire layer
    /// already rejects; lower it to enforce a stricter per-value cap.
    /// Set/Add/Replace/CompareAndSwap/Append/Prepend that would exceed
    /// this return StorageErrorCode::ValueTooLarge.
    std::size_t storageMaxValueBytes { 16 * 1024 * 1024 };

    /// Number of independent pinned reactors to run (the `--threads`
    /// flag). Each reactor is a single-threaded event loop; connections
    /// are pinned to one for their lifetime, so this is the server's
    /// across-core parallelism. 0 means "use
    /// std::thread::hardware_concurrency()".
    std::size_t workerThreads { 0 };

    /// Number of storage shards. 1 means "do not shard" (preserves
    /// PR #10 single-file storage behaviour). When >1 and `storagePath`
    /// is set, the path is treated as a directory containing
    /// `shard-NN.cow` files. 0 means "auto" — defaults to a sensible
    /// value at runtime (min(16, hardware_concurrency)).
    std::size_t storageShards { 0 };

    /// Bind address: IPv4/IPv6 literal or hostname. Default 127.0.0.1
    /// (IPv4 loopback). An IPv6 wildcard (`::`) binds dual-stack and
    /// serves both IPv4 and IPv6 on every platform.
    std::string bindAddress { "127.0.0.1" };

    /// Path of the YAML config file (if any) that produced this Config.
    /// Used by ConfigReloader on SIGHUP. Empty means no file-backed config.
    std::string configPath {};

    /// Optional pidfile path (POSIX daemon mode only).
    std::string pidfile {};

    /// Windows service name; defaults to FastCached.
    std::string serviceName { "FastCached" };

    /// Shared authentication secret (redis `requirepass` style). Empty (the
    /// default) means authentication is disabled and every client is served
    /// without a credential check. When set, clients must authenticate before
    /// any data command: redis via `AUTH`, the memcached binary protocol via
    /// SASL PLAIN. The memcached text protocol has no auth handshake, so it
    /// then rejects data commands.
    std::string requirePass {};

    /// Expected username for the two-argument auth form (redis
    /// `AUTH <user> <pass>` and SASL PLAIN authcid). Defaults to "default",
    /// mirroring redis's default ACL user. Only consulted when `requirePass`
    /// is non-empty.
    ///
    /// Note: the one-argument redis form `AUTH <pass>` authenticates the default
    /// user against `requirePass` alone and does NOT enforce this username
    /// (standard redis semantics). The username only constrains the two-argument
    /// `AUTH <user> <pass>` form and SASL PLAIN, where the supplied authcid must
    /// match it.
    std::string authUsername { "default" };

    /// Optional path to a persistent storage file. When set, the
    /// daemon uses a CoW-tree storage backed by this file; when empty,
    /// the cache is in-memory only.
    std::string storagePath {};

    /// TCP port. memcached default is 11211; fastcached's MVP follows.
    std::uint16_t port { 11211 };

    /// Bind address for the admin HTTP endpoint (`/metrics`, `/healthz`).
    /// Defaults to loopback so metrics are not exposed to the world unless the
    /// operator opts in. Only used when `metricsEnabled` is true.
    std::string metricsBindAddress { "127.0.0.1" };

    /// TCP port for the admin HTTP endpoint. Served on a dedicated port so it
    /// never collides with the cache protocols. Only used when `metricsEnabled`.
    std::uint16_t metricsPort { 9259 };

    /// Whether to start the admin HTTP endpoint (Prometheus `/metrics` plus a
    /// `/healthz` liveness probe). Off by default.
    bool metricsEnabled { false };

    /// ::listen() backlog — the depth of the kernel's queue of accepted-
    /// but-not-yet-handed-off connections. Bursts of parallel clients (a
    /// `make -jN` driving sccache opens many sockets at once) overflow a
    /// small backlog and get ECONNREFUSED / timeouts at the OS layer before
    /// the daemon ever sees them. Defaults to 511 (the value redis uses);
    /// the kernel silently clamps to its own SOMAXCONN ceiling.
    int listenBacklog { 511 };

    /// Log threshold.
    LogLevel logLevel { LogLevel::Info };

    /// When true, each ConsoleLogger line is prefixed with an ISO 8601 UTC timestamp.
    bool logTimestamps { false };

    /// If true, daemonize (POSIX) or self-register as a Windows service.
    bool daemon { false };

    /// Durability mode for the persistent backend (ignored when
    /// storagePath is empty).
    StorageDurability storageDurability { StorageDurability::Batched };

    /// In-memory LRU recency policy. Approximate (default) favours read
    /// throughput by letting same-shard reads run concurrently; Strict gives
    /// exact LRU order at the cost of serialising reads per shard.
    LruRecency lruRecency { LruRecency::Approximate };

    /// CPU-affinity policy for reactor threads. PerCore (default) pins each
    /// reactor to its own core when running more than one; with a single
    /// reactor it is a no-op regardless. None lets the scheduler place threads.
    CpuAffinity cpuAffinity { CpuAffinity::PerCore };
};

} // namespace FastCache
