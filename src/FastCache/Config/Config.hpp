// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace FastCache
{

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
};

} // namespace FastCache
