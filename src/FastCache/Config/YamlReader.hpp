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
///   bind:          string         (e.g. "127.0.0.1")
///   port:          int            (1..65535)
///   max_memory:    int            (bytes, >= 0; 0 disables eviction)
///   log_level:     string         (trace|debug|info|warn|error|fatal)
///
/// CLI flags later override these values via the caller.
/// @param path Filesystem path of the YAML file.
/// @return Parsed Config or a ConfigError.
[[nodiscard]] std::expected<Config, ConfigError> ReadYamlConfig(std::filesystem::path const& path);

} // namespace FastCache
