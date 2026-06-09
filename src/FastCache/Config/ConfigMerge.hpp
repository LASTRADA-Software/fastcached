// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <expected>
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
/// YAML `listeners:` always logged "bind=127.0.0.1:11211" — the defaults
/// of the unused legacy fields. This helper renders every endpoint that
/// will actually be listening, with a `[tls]` suffix per TLS bind.
/// @param binds The active listener list (typically `serverOpts.binds`).
/// @return A human-readable summary string, "<none>" when `binds` is
///         empty (defensive — `RunReactorServer` already errors on
///         empty).
[[nodiscard]] std::string FormatBindSummary(std::span<BindConfig const> binds);

} // namespace FastCache
