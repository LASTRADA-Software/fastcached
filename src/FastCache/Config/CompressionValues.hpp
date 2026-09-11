// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstddef>
#include <expected>
#include <string_view>

namespace FastCache
{

/// The three value parsers behind every compression setting, for both binaries.
///
/// There are six such settings on the daemon and six on the worker -- an on-disk
/// trio and an in-memory one each -- and all twelve reduce to these three
/// questions: which codec, how hard, and below which size not to bother. They
/// live here because the alternative is a copy per table: `CliParser.cpp` and
/// `YamlReader.cpp` already carried one each, differing only in how they dressed
/// the error, and `fastcache-compile-node` would have made a third
/// ([#623](https://github.com/LASTRADA-Software/fastcached/issues/623) is what a
/// setting reachable from one table and not another costs).
///
/// **They name no field.** `ApplyOneOption` stamps the row's own spelling onto an
/// error whose `field` is empty, which is the only attribution that cannot drift
/// when a flag is renamed -- and it is what the copies got wrong: the daemon
/// stamped `compression` on a bad `--memory-compression` value, sending an
/// operator to the wrong flag. A caller reading a FILE re-stamps `source`, `line`
/// and the key, because none of those is knowable from the value text.
///
/// `source` is left empty for the same reason.

/// Parse a compression codec by name.
///
/// Refuses a name no codec answers to, and -- separately -- a codec this build
/// was compiled without, because those are different mistakes with different
/// remedies: the first is a typo, the second wants `FASTCACHED_ENABLE_COMPRESSION`.
/// @param sv The codec name: `none`, `lz4` or `zstd`.
/// @return The codec, or why it is not one.
[[nodiscard]] std::expected<CompressionCodec, ConfigError> ParseCompressionCodec(std::string_view sv);

/// Parse a codec effort level.
///
/// Range-checked to 1..22 -- zstd's range, applied codec-agnostically so an
/// absurd value is refused where it was typed rather than reinterpreted by
/// whichever codec receives it.
/// @param sv The level text.
/// @return The level, or why it is not one.
[[nodiscard]] std::expected<int, ConfigError> ParseCompressionLevel(std::string_view sv);

/// Parse the size below which values are stored uncompressed.
///
/// Takes the `k`/`m`/`g` suffixes every other byte-valued setting takes. No host
/// total is passed, so `N%` is refused: a threshold denominated in a fraction of
/// RAM would describe nothing about the value being weighed against it.
/// @param sv The size text.
/// @return The size in bytes, or why it is not one.
[[nodiscard]] std::expected<std::size_t, ConfigError> ParseCompressionMinBytes(std::string_view sv);

} // namespace FastCache
