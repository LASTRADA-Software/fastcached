// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>

namespace FastCache
{

/// Merge CLI flags into a YAML-loaded Config. A CLI value overrides the file
/// value only when the corresponding flag was explicitly passed — driven by
/// the per-flag "explicit" booleans on `CliResult`, not by value comparison
/// against the default. The latter would silently drop `--threads=0`,
/// `--storage-shards=0`, `--storage-durability=batched`, and any other typed
/// value that matches the field's default.
/// @param fileCfg The YAML-loaded baseline.
/// @param cli     The parsed CLI result (config + explicit-flag bits).
/// @return A merged Config where each field is the CLI value if explicit,
///         otherwise the YAML value.
[[nodiscard]] Config Merge(Config fileCfg, CliResult const& cli);

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
    /// The command line, with the provenance bits the parse recorded.
    ///
    /// **The whole parse, never `parsed->config`.** Which flags outrank a file is
    /// decided by what the operator NAMED, and only `CliResult`'s explicit bits
    /// record that — a comparison against the default cannot see an operator who
    /// typed the default, which is the input that matters.
    CliResult cli {};

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

/// The daemon's effective configuration, and what the FILE said about it.
struct EffectiveConfig
{
    /// What the daemon runs with: the file, then the command line over it, then
    /// the environment fallback.
    Config config {};

    /// What the file carried, with its per-key presence bits.
    ///
    /// Returned rather than swallowed because a START asks questions of the file
    /// alone that a reload does not: whether the legacy single-bind triplet was
    /// declared there as well as on the command line is one, and it decides a
    /// refusal `main()` makes before any tier exists.
    YamlConfigWithPresence file {};
};

/// Assemble the effective configuration from every source, in precedence order.
///
/// **The single place the daemon's configuration is put together.** `main()` calls
/// it at startup and `ConfigReloader` calls it again on every SIGHUP with the same
/// `ConfigSources`, so "the command line wins" stays a question of which loop ran
/// second rather than a rule two callers each re-implement — and a reload cannot
/// silently drop what the start honoured (#622).
///
/// The order is file, then command line, then environment, and the last step is
/// gated on both of the first two: `FASTCACHED_METRICS_PORT` applies only when
/// neither the command line nor the file NAMED `metrics_port`, so a stray variable
/// can never outrank a port an operator wrote down — even when they wrote the
/// compiled-in default.
///
/// @param configPath The file to read; empty means there is none, and the command
///        line then stands alone over the compiled-in defaults.
/// @param sources The command line and the environment fallback.
/// @return The effective configuration and the file's presence bits, or why the
///         file could not be read. A missing file is `FileNotFound`, never
///         `ParseError`; what to do about that is the caller's decision, because a
///         file the operator NAMED and one the daemon merely found are not the same
///         failure.
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

/// Reject CLI flag combinations that would silently drop user-typed values.
/// The dual-listener commit introduced two ways to declare endpoints: the
/// legacy single-bind triplet (`--bind` / `--port` / `--tls`) and the
/// repeatable `--listen` / `--listen-tls` (which also reads YAML
/// `listeners:`). When BOTH shapes are given on one invocation, main.cpp
/// silently picks `binds` and discards the legacy values — the operator's
/// `--bind 0.0.0.0` vanishes with no diagnostic. Fail fast at startup
/// instead, the same way we reject duplicate {address, port} pairs.
/// @param cli   The parsed CLI result (carries the per-flag explicit bits).
/// @param binds The merged listener list — `effective.binds` after `Merge`.
/// @return Empty on success; ConfigError naming the offending flag
///         otherwise.
[[nodiscard]] std::expected<void, ConfigError> ValidateBindFlagShape(CliResult const& cli,
                                                                     std::span<BindConfig const> binds);

/// Render the listener list for the startup banner. The original banner
/// formatted `bind={bindAddress}:{port}` from the legacy single-bind
/// fields and ignored `binds`, so a daemon brought up via `--listen` /
/// YAML `listeners:` always logged the legacy single-bind fields — the defaults
/// of the unused legacy fields. This helper renders every endpoint that
/// will actually be listening, with a `[tls]` suffix per TLS bind.
/// @param binds The active listener list (typically `serverOpts.binds`).
/// @return A human-readable summary string, "<none>" when `binds` is
///         empty (defensive — `RunReactorServer` already errors on
///         empty).
[[nodiscard]] std::string FormatBindSummary(std::span<BindConfig const> binds);

} // namespace FastCache
