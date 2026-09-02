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
#include <utility>
#include <vector>

namespace FastCache
{

/// Live-reload pipeline, over whatever configuration type an executable has.
///
/// Holds an atomic `shared_ptr<ConfigT const>` snapshot; `Reload()` re-reads the file
/// at the original path, asks whether the candidate may replace the live one, then
/// publishes the new snapshot and notifies subscribers.
///
/// ## Why this is a template, and what is deliberately NOT in it
///
/// **Two executables have configuration files and there must be ONE reload
/// mechanism.** `fastcached` reads `Config`; `fastcache-compile-node` reads
/// `NodeConfig` through the option table's `yamlKey` column
/// ([#291](https://github.com/LASTRADA-Software/fastcached/issues/291)). Writing a
/// second reloader for the second one is what this codebase keeps a list about — the
/// two would drift on the questions that matter, which are the ordering of the swap
/// and what happens on a partial failure, not on the parsing.
///
/// So the type varies and the *pipeline* does not. What varies with it arrives as two
/// injected steps rather than as branches:
///
/// - **`Reparse`** — how a file becomes a candidate configuration. The daemon reads
///   YAML straight into `Config`; the node applies the same appliers argv reaches, in
///   the same order, so "the command line wins" stays a question of which loop ran
///   second rather than a per-field merge.
/// - **`ImmutabilityCheck`** — whether the candidate may replace the live snapshot.
///   The node derives this from a **column of its option table**; the daemon still
///   uses a hand-written ladder, which is
///   [#406](https://github.com/LASTRADA-Software/fastcached/issues/406) and is
///   deliberately untouched here. One reloader with two strategies is honest and
///   temporary; two reloaders would not be.
///
/// ## Declined, never half-applied
///
/// A candidate that cannot be read, or that changes something immutable, leaves the
/// live snapshot **exactly as it was** and no subscriber is called. That is one rule
/// for both failures on purpose: an operator saved a file once and expects one
/// outcome, and a process whose configuration is half the file and half the previous
/// one has no single artefact describing what is in force.
///
/// Subscribers are called synchronously, on the thread that invoked `Reload()` — the
/// signal-handling thread, or the SCM control-handler thread on Windows. They must
/// keep their work bounded and defer anything heavy onto the affected subsystem's own
/// threads.
/// @tparam ConfigT The configuration this executable reloads.
template <typename ConfigT>
class ConfigReloaderOf
{
  public:
    using Snapshot = std::shared_ptr<ConfigT const>;
    using Subscriber = std::function<void(Snapshot const& previous, Snapshot const& current)>;

    /// Reads @p path into a candidate configuration, or says why it could not.
    using Reparse = std::function<std::expected<ConfigT, ConfigError>(std::filesystem::path const& path)>;

    /// Answers whether @p candidate may replace @p previous.
    using ImmutabilityCheck =
        std::function<std::expected<void, ConfigError>(ConfigT const& previous, ConfigT const& candidate)>;

    /// @param initial Initial configuration; copied into the first snapshot.
    /// @param configPath File to re-read. Empty disables file-based reload; the live
    ///        snapshot is still observable.
    /// @param reparse How to read that file. Must not be null when `configPath` is not.
    /// @param check Whether a candidate may be published. Must not be null.
    ConfigReloaderOf(ConfigT initial, std::filesystem::path configPath, Reparse reparse, ImmutabilityCheck check):
        _configPath { std::move(configPath) },
        _reparse { std::move(reparse) },
        _check { std::move(check) },
        _current { std::make_shared<ConfigT>(std::move(initial)) }
    {
    }

    /// @return The current snapshot. The pointer may be swapped concurrently by
    ///         `Reload()`, but each snapshot is immutable, so an existing reader stays
    ///         consistent for as long as it holds one.
    [[nodiscard]] Snapshot Current() const noexcept
    {
        std::scoped_lock const lock { _swapMutex };
        return _current;
    }

    /// Register an observer invoked on every successful reload.
    /// @param subscriber Called with the old and new snapshots.
    void Subscribe(Subscriber subscriber)
    {
        std::scoped_lock const lock { _swapMutex };
        _subscribers.push_back(std::move(subscriber));
    }

    /// Re-read the file and publish it when nothing immutable changed.
    /// @return Nothing, or why the live snapshot was left alone.
    [[nodiscard]] std::expected<void, ConfigError> Reload()
    {
        if (_configPath.empty())
            return std::unexpected(ConfigError {
                .code = ConfigErrorCode::FileNotFound,
                .source = {},
                .line = 0,
                .field = {},
                .context = "no config path",
            });

        auto reloaded = _reparse(_configPath);
        if (!reloaded.has_value())
            return std::unexpected(reloaded.error());

        Snapshot previous;
        Snapshot next;
        std::vector<Subscriber> observers;
        {
            std::scoped_lock const lock { _swapMutex };
            previous = _current;

            // Asked BEFORE the swap, so a refusal leaves the live snapshot untouched
            // rather than needing to be undone.
            auto const validation = _check(*previous, *reloaded);
            if (!validation.has_value())
                return std::unexpected(validation.error());

            next = std::make_shared<ConfigT>(std::move(*reloaded));
            _current = next;
            observers = _subscribers;
        }

        // Outside the lock: a subscriber that reads `Current()` would otherwise
        // deadlock on the mutex this call still held.
        for (auto const& obs: observers)
            obs(previous, next);
        return {};
    }

  private:
    std::filesystem::path _configPath;
    Reparse _reparse;
    ImmutabilityCheck _check;
    mutable std::mutex _swapMutex;
    Snapshot _current;
    std::vector<Subscriber> _subscribers;
};

/// The daemon's reloader: `Config`, read from YAML, guarded by its own ladder.
///
/// A named type rather than an alias so every existing construction site keeps its
/// two-argument shape, and so the daemon's immutability rule stays where it is —
/// converting that ladder to a column is #406, not this.
class ConfigReloader final: public ConfigReloaderOf<Config>
{
  public:
    /// @param initial Initial Config; copied into a snapshot.
    /// @param configPath YAML file path (may be empty).
    ConfigReloader(Config initial, std::filesystem::path configPath);

    /// The daemon's immutability rule. Exposed so its tests can drive it directly.
    /// @param previous The live configuration.
    /// @param candidate What the file now says.
    /// @return Nothing, or which field may not change at runtime.
    [[nodiscard]] static std::expected<void, ConfigError> ValidateImmutable(Config const& previous, Config const& candidate);
};

} // namespace FastCache
