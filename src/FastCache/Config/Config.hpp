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

/// All runtime configuration. POD-like value type; built once from CLI
/// arguments (and later, from a YAML config file). For SIGHUP reload, the
/// daemon keeps a shared_ptr<const Config> and atomically swaps.
struct Config
{
    /// Bind address, IPv4 string. Default 127.0.0.1.
    std::string bindAddress { "127.0.0.1" };

    /// TCP port. memcached default is 11211; fastcached's MVP follows.
    std::uint16_t port { 11211 };

    /// In-memory storage byte budget. 0 = unbounded (testing/dev only).
    std::size_t maxMemoryBytes { 64 * 1024 * 1024 };

    /// Log threshold.
    LogLevel logLevel { LogLevel::Info };

    /// Path of the YAML config file (if any) that produced this Config.
    /// Used by ConfigReloader on SIGHUP. Empty means no file-backed config.
    std::string configPath {};

    /// If true, daemonize (POSIX) or self-register as a Windows service.
    bool daemon { false };

    /// Optional pidfile path (POSIX daemon mode only).
    std::string pidfile {};

    /// Windows service name; defaults to FastCached.
    std::string serviceName { "FastCached" };

    /// Optional path to a persistent storage file. When set, the
    /// daemon uses a CoW-tree storage backed by this file; when empty,
    /// the cache is in-memory only.
    std::string storagePath {};

    /// Durability mode for the persistent backend (ignored when
    /// storagePath is empty).
    StorageDurability storageDurability { StorageDurability::Batched };

    /// Maximum size of a single cache value in bytes (only enforced
    /// when `storagePath` is set). Defaults to 1 MiB which fits
    /// typical sccache compile-cache values; raise it to allow larger
    /// objects. Set/Add/Replace/Append/Prepend that would exceed this
    /// return StorageErrorCode::ValueTooLarge.
    std::size_t storageMaxValueBytes { 1 * 1024 * 1024 };
};

} // namespace FastCache
