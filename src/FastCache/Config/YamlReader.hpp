// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <expected>
#include <filesystem>
#include <string_view>

namespace FastCache
{

/// Parse a YAML file into a Config. Unknown keys yield ConfigError(UnknownKey);
/// wrong types yield ConfigError(TypeMismatch); out-of-range numbers yield
/// ConfigError(OutOfRange). Missing file is ConfigError(FileNotFound).
///
/// Recognised top-level keys (all optional):
///
///   bind:        string   interface to bind on (e.g. "127.0.0.1", "0.0.0.0").
///
///   port:        int      TCP listen port; 1..65535.
///
///   max_memory:  size     in-memory cache budget. Integer with optional unit
///                         suffix: k/K = 1024, m/M = 1024², g/G = 1024³
///                         (1024-based). Plain integer means bytes. A trailing
///                         "%" sets the budget to that percentage of host total
///                         RAM (e.g., 50%). 0 disables eviction.
///                         Examples: 67108864, 64m, 1g, 50%.
///
///   log_level:   string   one of: trace | debug | info | warn | error | fatal.
///
/// CLI flags later override these values via the caller.
/// @param path Filesystem path of the YAML file.
/// @return Parsed Config or a ConfigError.
[[nodiscard]] std::expected<Config, ConfigError> ReadYamlConfig(std::filesystem::path const& path);

} // namespace FastCache
