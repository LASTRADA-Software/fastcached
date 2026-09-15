// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache
{

/// Everything the daemon's effective configuration is assembled FROM, besides
/// the configuration file itself.
///
/// **Held for the life of the process, because a reload has to rebuild the
/// candidate the way the start built the live configuration.** The daemon's
/// SIGHUP path re-read the file and nothing else, so a reloadable setting given
/// on the command line and absent from the file was reset to its built-in default
/// at every reload — `--max-memory=8g` published as the host-derived default, and
/// `InMemoryLruStorage::Resize` evicting down to it, with nothing in the logs
/// naming the flag
/// ([#622](https://github.com/LASTRADA-Software/fastcached/issues/622)).
///
/// So this is not a startup detail: it is the *other two* sources, kept so that
/// `AssembleEffectiveConfig` is the one place any of them is applied.
struct ConfigSources
{
    /// The command line as the operator typed it, program name excluded.
    ///
    /// **The TOKENS, never a parse.** The command line reaches the configuration by
    /// running its appliers again over the file-seeded result, so "the command line
    /// wins" is which loop runs second -- and only tokens can be applied a second
    /// time. A parse held beside them would be a second copy of the same argv that a
    /// caller could fill differently. Owned, because the reloader keeps these for the
    /// life of the process and a reload runs on the signal thread.
    std::vector<std::string> args {};

    /// `FASTCACHED_METRICS_PORT`, already parsed, or nothing when it is unset,
    /// empty or unusable.
    ///
    /// A resolved VALUE rather than a reader, and that is the point rather than a
    /// shortcut: a process's environment does not change under it, so replaying the
    /// value the start resolved makes the reload candidate identical to the startup
    /// assembly by construction. A second read could only ever differ if something
    /// in this process called `setenv`, which would be a source of truth nobody
    /// declared.
    std::optional<std::uint16_t> metricsPortEnv {};
};

/// The daemon's effective configuration, and which settings somebody NAMED on the
/// way to it -- in the file or on the command line, without saying which.
///
/// **A class, and it hands out no `CliResult`.** The assembly IS a `CliResult`
/// underneath -- the file and argv both ran the option table's appliers into one --
/// and its explicit bits therefore mean "named anywhere". A service registration
/// must bake in only what was TYPED (`MakeDaemonServiceSpec` takes the command-line
/// parse for exactly that reason), so a merged `CliResult` in reach of that call
/// would put a file's `requirepass:` one wrong argument away from a world-readable
/// registration. Here the type is the guard: nothing in it passes for a parse.
class EffectiveConfig
{
  public:
    /// @param assembled The file, the command line and the environment, applied.
    explicit EffectiveConfig(CliResult assembled) noexcept:
        _assembled { std::move(assembled) }
    {
    }

    /// What the daemon runs with.
    /// @return The configuration.
    [[nodiscard]] Config const& Configuration() const noexcept
    {
        return _assembled.config;
    }

    /// The configuration alone, for a caller that keeps nothing else.
    /// @return The configuration, moved out.
    [[nodiscard]] Config TakeConfiguration() && noexcept
    {
        return std::move(_assembled.config);
    }

    /// Whether a file or the command line named a setting.
    /// @param setting The row's `explicitBit`.
    /// @return True when some applier ran for it.
    [[nodiscard]] bool Named(bool CliResult::* setting) const noexcept
    {
        return _assembled.*setting;
    }

  private:
    CliResult _assembled;
};

/// Assemble the effective configuration from every source, in precedence order.
///
/// **The single place the daemon's configuration is put together.** `main()` calls
/// it at startup and `ConfigReloader` calls it again on every SIGHUP with the same
/// `ConfigSources`, so "the command line wins" stays a question of which loop ran
/// second rather than a rule two callers each re-implement — and a reload cannot
/// silently drop what the start honoured (#622).
///
/// **Every source reaches the configuration through `CliOptions()`'s own appliers.**
/// The file is applied first (`ApplyFileSettings`), then every list the command line
/// names is emptied (`ClearListsNamedOn`), then the command line is applied over it.
/// There is no per-field merge and no second parser: a file value is refused by
/// exactly the rule that refuses the same text on argv, and a key naming no row is
/// refused by the walk that applies the rest.
///
/// The environment is last, and gated on the ASSEMBLED explicit bit:
/// `FASTCACHED_METRICS_PORT` applies only when neither the command line nor the file
/// NAMED `metrics_port`, so a stray variable can never outrank a port an operator
/// wrote down — even when they wrote the compiled-in default.
///
/// @param configPath The file to read; empty means there is none, and the command
///        line then stands alone over the compiled-in defaults.
/// @param sources The command line and the environment fallback.
/// @return The effective configuration, or why it could not be assembled. A missing
///         file is `FileNotFound`, never `ParseError`; what to do about that is the
///         caller's decision, because a file the operator NAMED and one the daemon
///         merely found are not the same failure. A file that fails anywhere is
///         declined whole: nothing from it is returned.
[[nodiscard]] std::expected<EffectiveConfig, ConfigError> AssembleEffectiveConfig(std::filesystem::path const& configPath,
                                                                                  ConfigSources const& sources);

/// Reject duplicate {address, port} listener pairs. Two BindConfig entries
/// targeting the same endpoint would both bind successfully under SO_REUSEPORT
/// on POSIX, then load-balance accepted connections randomly between them — if
/// one is TLS and the other plaintext, a single client connection ends up on
/// the wrong protocol with 50/50 probability. We fail fast at startup instead.
/// @param binds The listener list to validate (typically `Config::binds`).
/// @return Empty on success; ConfigError naming the duplicated endpoint
///         otherwise.
[[nodiscard]] std::expected<void, ConfigError> ValidateBinds(std::span<BindConfig const> binds);

/// Reject a configuration naming both ways to declare endpoints, which would
/// silently drop what the operator wrote. There are two: the legacy single-bind
/// triplet (`--bind` / `--port` / `--tls`, or `bind:` / `port:` / `tls:`) and the
/// repeatable listeners (`--listen` / `--listen-tls`, or `listen:` / `listen_tls:`).
/// When both are named, the daemon serves `binds` and discards the legacy values —
/// the operator's `--bind 0.0.0.0` vanishes with no diagnostic. Fail fast at startup
/// instead, the same way duplicate {address, port} pairs are refused.
///
/// Asked of the ASSEMBLED configuration, because the two shapes can arrive from
/// different sources -- a `bind:` in the file and a `--listen` on the command line
/// lose a value exactly as two flags do.
/// @param effective The assembled configuration, with what was named anywhere.
/// @return Empty on success; ConfigError naming the offending flag otherwise.
[[nodiscard]] std::expected<void, ConfigError> ValidateBindFlagShape(EffectiveConfig const& effective);

/// Render the listener list for the startup banner. The original banner
/// formatted `bind={bindAddress}:{port}` from the legacy single-bind
/// fields and ignored `binds`, so a daemon brought up via `--listen` /
/// a file's listeners always logged the legacy single-bind fields — the defaults
/// of the unused legacy fields. This helper renders every endpoint that
/// will actually be listening, with a `[tls]` suffix per TLS bind.
/// @param binds The active listener list (typically `serverOpts.binds`).
/// @return A human-readable summary string, "<none>" when `binds` is
///         empty (defensive — `RunReactorServer` already errors on
///         empty).
[[nodiscard]] std::string FormatBindSummary(std::span<BindConfig const> binds);

} // namespace FastCache
