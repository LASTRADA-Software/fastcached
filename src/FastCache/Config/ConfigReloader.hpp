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
#include <span>
#include <string_view>
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
///   Both executables now derive this from a **column of their own option table**
///   ([#406](https://github.com/LASTRADA-Software/fastcached/issues/406) closed the
///   daemon's hand-written ladder), so the strategy varies only in which table it
///   reads. One reloader with two tables is one mechanism; two reloaders would not
///   be.
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

/// The refusal a reload answers with when something immutable changed.
///
/// Shared rather than written per executable: the join and the `ConfigError` it
/// builds take no table and no configuration type, so there was nothing left to
/// vary — and two independently written copies had already drifted on the sentence
/// they say. `fastcache-compile-node` still builds its own
/// (`NodeConfig.cpp`'s `ValidateNodeReloadable`) and should adopt this.
///
/// @param changed The settings that may not change, in the order they were found.
/// @return An `ImmutableChanged` error naming the first and listing them all.
[[nodiscard]] ConfigError ImmutableChangedError(std::span<std::string_view const> changed);

/// Every setting a candidate configuration changes that cannot take effect live.
///
/// **EVERY one, never the first.** A reload that reports one unreloadable setting
/// and stops sends the operator round the same loop per setting: they fix it, save,
/// and are refused again for the next. The whole answer in one refusal is the
/// difference between a diagnosis and a guessing game.
///
/// Driven by `ConfigFileSettings()`, which is the option table's own `yamlKey`,
/// `reloadable` and `same` columns plus the settings that table cannot express — so
/// a row added tomorrow is covered without touching this, and a row that forgets its
/// comparator does not compile.
///
/// **The domain is what a FILE can express, and that is not an approximation of
/// "every field".** The daemon's reload candidate is `ReadYamlConfig(path)` and is
/// never re-merged with argv, so a field no key can set — `configPath`, `daemon`,
/// `pidfile`, `serviceName` — sits at its default in every candidate. Guarding those
/// would refuse *every* reload: `--daemon`, which the systemd unit passes, would
/// alone be enough. Do not "complete" this by walking rows with no key.
/// **What widening the guarded set costs, stated rather than discovered.** The
/// candidate is built from the file alone, so a setting that is in force because
/// somebody passed a flag — or set `FASTCACHED_METRICS_PORT`, which `main.cpp`
/// applies after the merge — and that the file does not mention arrives here as a
/// setting being changed back to its default. The reload is then refused, by name,
/// until the file carries that value too. That was already true of the ten settings
/// the old ladder guarded (`--port` with no `port:` has always refused); this makes
/// it true of every setting a file can carry, which is a wider surface for one
/// underlying defect: **the candidate is not built the way the live configuration
/// was built.** Fixing that is
/// [#622](https://github.com/LASTRADA-Software/fastcached/issues/622) — argv and the
/// environment fallback are the same half of it — and it is what would change this
/// domain. Refusing is the right answer until then: publishing instead is the
/// split-brain the whole function exists to prevent.
///
/// @param previous What the daemon is running with.
/// @param candidate What the file now says.
/// @return The YAML keys that changed and may not, table order first. Empty means
///         the candidate may be published.
[[nodiscard]] std::vector<std::string_view> UnreloadableChanges(Config const& previous, Config const& candidate);

/// The daemon's reloader: `Config`, read from YAML, guarded by its option table.
///
/// A named type rather than an alias so every existing construction site keeps its
/// two-argument shape.
class ConfigReloader final: public ConfigReloaderOf<Config>
{
  public:
    /// @param initial Initial Config; copied into a snapshot.
    /// @param configPath YAML file path (may be empty).
    ConfigReloader(Config initial, std::filesystem::path configPath);

    /// The daemon's immutability rule. Exposed so its tests can drive it directly.
    ///
    /// A thin shape over `UnreloadableChanges` because the reloader speaks errors and
    /// the table speaks keys; the list is joined into one sentence naming all of them.
    /// @param previous The live configuration.
    /// @param candidate What the file now says.
    /// @return Nothing, or which settings may not change at runtime.
    [[nodiscard]] static std::expected<void, ConfigError> ValidateImmutable(Config const& previous, Config const& candidate);
};

} // namespace FastCache
