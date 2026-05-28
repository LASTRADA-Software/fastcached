// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace FastCache
{

/// Parse a byte-size value with an optional unit suffix.
///
/// Grammar: `<digits>[kKmMgG%]?`
///   - no suffix      → bytes
///   - `k` or `K`     → multiply by 1024
///   - `m` or `M`     → multiply by 1024²
///   - `g` or `G`     → multiply by 1024³
///   - `%`            → percentage of `hostTotalBytes` (digits must be 0..100)
///
/// Examples: `"0"`, `"4096"`, `"64m"`, `"2G"`, `"50%"`.
///
/// @param sv              Input string (e.g., `"64m"` or `"50%"`).
/// @param field           Field name carried in the returned ConfigError for
///                        diagnostics (e.g., `"max-memory"` for CLI).
/// @param hostTotalBytes  Total physical RAM in bytes. Required for `%`
///                        inputs; ignored otherwise. When 0, `%` inputs are
///                        rejected as TypeMismatch.
/// @return Parsed value in bytes, or a ConfigError. TypeMismatch is returned
///         for empty, non-numeric, or unknown-suffix input (or `%` without a
///         host total); OutOfRange is returned when the value exceeds
///         `std::numeric_limits<std::size_t>::max()` or when the percentage
///         is outside 0..100.
[[nodiscard]] std::expected<std::size_t, ConfigError> ParseByteSize(std::string_view sv,
                                                                    std::string_view field,
                                                                    std::size_t hostTotalBytes = 0);

/// Format a byte count as a short human-readable string.
///
/// The largest unit (`G`, `M`, `K`) for which the byte count is an exact
/// integer multiple is chosen; otherwise the raw byte count with a `B`
/// suffix is returned. Examples: `8G`, `64M`, `4096B`, `0B`.
///
/// 1024-based units, mirroring `ParseByteSize`.
///
/// @param bytes Byte count to format.
/// @return Short human-readable string (e.g. `"30M"`, `"4096B"`).
[[nodiscard]] std::string FormatByteSize(std::size_t bytes);

} // namespace FastCache
