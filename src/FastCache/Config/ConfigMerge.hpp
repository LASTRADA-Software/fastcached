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

} // namespace FastCache
