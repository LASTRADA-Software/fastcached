// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstddef>
#include <expected>
#include <string_view>

namespace FastCache
{

/// Parse a byte-size value with an optional 1024-based unit suffix.
///
/// Grammar: `<digits>[kKmMgG]?`
///   - no suffix      → bytes
///   - `k` or `K`     → multiply by 1024
///   - `m` or `M`     → multiply by 1024²
///   - `g` or `G`     → multiply by 1024³
///
/// Examples: `"0"`, `"4096"`, `"64m"`, `"2G"`.
///
/// @param sv    Input string (e.g., `"64m"`).
/// @param field Field name carried in the returned ConfigError for diagnostics
///              (e.g., `"max-memory"` for CLI or `"max_memory"` for YAML).
/// @return Parsed value in bytes, or a ConfigError. TypeMismatch is returned
///         for empty, non-numeric, or unknown-suffix input; OutOfRange is
///         returned when the value (after multiplication) exceeds
///         `std::numeric_limits<std::size_t>::max()`.
[[nodiscard]] std::expected<std::size_t, ConfigError> ParseByteSize(std::string_view sv, std::string_view field);

} // namespace FastCache
