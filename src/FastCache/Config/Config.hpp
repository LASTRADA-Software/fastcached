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

/// Server execution model. Controls how concurrent client connections
/// are served.
enum class ExecutionModel : std::uint8_t
{
    /// Pick the best model for the configured storage backend: reactor
    /// for in-memory (single-threaded suffices — operations are
    /// non-blocking and CPU-bound on hash lookups), threaded for CoW
    /// on-disk storage (disk I/O and page-store writes benefit from
    /// per-shard parallelism). The default.
    Auto = 0,

    /// Thread-pool-backed accept loop (fixed-size pool created at
    /// startup). One accept thread feeds a bounded queue; N workers
    /// drive Connection coroutines to completion. Required for
    /// parallel sccache builds and other multi-client workloads.
    Threaded = 1,

    /// Single-threaded reactor (IOCP / epoll / kqueue). All
    /// connections multiplexed on one thread. Useful for testing,
    /// low-resource deployments, and single-client workloads where
    /// CPU parallelism is not needed.
    Reactor = 2,
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

    /// Worker thread count for Threaded mode. 0 means "use
    /// std::thread::hardware_concurrency()". Ignored when the
    /// resolved execution model is Reactor.
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

    /// Optional path to a persistent storage file. When set, the
    /// daemon uses a CoW-tree storage backed by this file; when empty,
    /// the cache is in-memory only.
    std::string storagePath {};

    /// TCP port. memcached default is 11211; fastcached's MVP follows.
    std::uint16_t port { 11211 };

    /// Log threshold.
    LogLevel logLevel { LogLevel::Info };

    /// When true, each ConsoleLogger line is prefixed with an ISO 8601 UTC timestamp.
    bool logTimestamps { false };

    /// If true, daemonize (POSIX) or self-register as a Windows service.
    bool daemon { false };

    /// Durability mode for the persistent backend (ignored when
    /// storagePath is empty).
    StorageDurability storageDurability { StorageDurability::Batched };

    /// Server execution model. See ExecutionModel docs.
    ExecutionModel executionModel { ExecutionModel::Auto };
};

} // namespace FastCache
