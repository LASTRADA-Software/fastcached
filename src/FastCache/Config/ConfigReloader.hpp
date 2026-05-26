// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace FastCache
{

/// Live-reload pipeline. Holds an atomic shared_ptr<const Config> snapshot;
/// Reload() re-parses the YAML at the original path, validates that
/// immutable fields (bind/port) match the live snapshot, then atomically
/// publishes the new snapshot and notifies subscribers.
///
/// Subscribers are called synchronously, on the thread that invoked
/// Reload(). For the MVP that thread is the signal-handling thread (or
/// the SCM control-handler thread on Windows). Subscribers must therefore
/// keep their work bounded — defer heavy lifting onto the affected
/// subsystems' own worker threads.
class ConfigReloader
{
  public:
    using Snapshot = std::shared_ptr<Config const>;
    using Subscriber = std::function<void(Snapshot const& previous, Snapshot const& current)>;

    /// Construct over the initial in-memory config and the on-disk file we
    /// should re-parse when Reload() is called. Pass an empty path to
    /// disable file-based reload (the live snapshot is still observable).
    /// @param initial Initial Config; copied into a snapshot.
    /// @param configPath YAML file path (may be empty).
    ConfigReloader(Config initial, std::filesystem::path configPath);

    /// @return The current snapshot. Stable across the caller's frame; the
    /// pointer may be swapped concurrently by Reload(), but each snapshot
    /// is immutable so existing readers stay consistent.
    [[nodiscard]] Snapshot Current() const noexcept;

    /// Register an observer invoked on every successful reload (with old
    /// and new snapshots). Returns a handle that can be passed to
    /// Unsubscribe; for the MVP the handle is opaque (subscribers stay
    /// for the life of the daemon).
    void Subscribe(Subscriber subscriber);

    /// Re-parse the config file and, if it differs only in reloadable
    /// fields, swap in the new snapshot. Returns ConfigError on parse or
    /// validation failure; the live snapshot is left unchanged on error.
    [[nodiscard]] std::expected<void, ConfigError> Reload();

  private:
    [[nodiscard]] static std::expected<void, ConfigError> ValidateImmutable(Config const& previous, Config const& candidate);

    std::filesystem::path _configPath;
    mutable std::mutex _swapMutex;
    Snapshot _current;
    std::vector<Subscriber> _subscribers;
};

} // namespace FastCache
