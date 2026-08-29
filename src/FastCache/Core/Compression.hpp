// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache
{

/// On-disk compression codec identifier.
///
/// The numeric value is written verbatim into every persisted cache record
/// (see CowTreeStorage's leaf-record format), so the ids are a stable part of
/// the on-disk contract: **never renumber or reuse an id**. Adding a codec
/// means appending a new id and a matching descriptor row in Compression.cpp.
///
/// **And one place outside this file**: `Cc::AvailableCodecs` in
/// `apps/fastcache-cc/CodecEnvelope.cpp` keeps its own list of the non-Identity
/// codecs, because this class exposes no way to enumerate the table — a codec
/// missing from it is never advertised and never negotiated by the compile fleet,
/// with the build green and every test passing. `AvailableCodecsCoverEveryCodec`
/// in `WorkerProtocol_test.cpp` fails when that happens; a public enumeration here
/// would remove the need for it.
///
/// `Identity` is always available — even in a build configured with
/// `FASTCACHED_ENABLE_COMPRESSION=OFF` — and stores the value verbatim. It is
/// also the shrink-check fallback: a value that does not get smaller when
/// compressed is stored under `Identity` so a read never pays a pointless
/// decompress.
enum class CompressionCodec : std::uint8_t
{
    Identity = 0, ///< No compression; bytes stored verbatim. Always available.
    Lz4 = 1,      ///< LZ4 block format. Fastest; modest ratio.
    Zstd = 2,     ///< Zstandard. Best general-purpose ratio; the runtime default.
};

/// Compression algorithms and their metadata, exposed as data so config
/// parsing, the startup banner, and the encode/decode path all iterate one
/// source of truth instead of hand-rolling a switch each.
class Compression
{
  public:
    /// Whether `codec` can actually be used by this build. `Identity` is
    /// always usable; `Lz4`/`Zstd` are usable only when the library was built
    /// with `FASTCACHED_ENABLE_COMPRESSION` (the `FC_COMPRESSION_ENABLED`
    /// compile definition).
    /// @param codec Codec to test.
    /// @return True if the codec is compiled in and selectable.
    [[nodiscard]] static bool IsAvailable(CompressionCodec codec) noexcept;

    /// Lower-case textual name of a codec (`"none"`, `"lz4"`, `"zstd"`). Used
    /// by the CLI/YAML/banner. `Identity` maps to `"none"`.
    /// @param codec Codec to name.
    /// @return Stable lower-case name.
    [[nodiscard]] static std::string_view NameOf(CompressionCodec codec) noexcept;

    /// Parse a codec name (case-sensitive, lower-case: `"none"`, `"lz4"`,
    /// `"zstd"`). Recognises the name even when the codec is not compiled in,
    /// so config validation can report "unavailable in this build" distinctly
    /// from "unknown codec".
    /// @param name Codec name.
    /// @return The codec, or std::nullopt if the name is not recognised.
    [[nodiscard]] static std::optional<CompressionCodec> CodecFromName(std::string_view name) noexcept;

    /// The set of codec names, in id order, for help text / diagnostics.
    /// @return Comma-separated list, e.g. `"none, lz4, zstd"`.
    [[nodiscard]] static std::string_view NameList() noexcept;

    /// Compress `input` with `codec` at `level` (level is ignored by codecs
    /// that have no notion of one, e.g. `Identity` and `Lz4` fast mode).
    ///
    /// `Identity` returns a verbatim copy. Requesting a codec that is not
    /// available in this build is a programmer error the caller must avoid
    /// (guarded by config validation); such a call falls back to a verbatim
    /// copy rather than crashing, but the record must then be tagged
    /// `Identity` by the caller.
    /// @param codec Codec to use.
    /// @param input Bytes to compress.
    /// @param level Codec-specific effort level (higher = smaller/slower).
    /// @return Newly allocated compressed (or copied) bytes.
    [[nodiscard]] static std::vector<std::byte> Compress(CompressionCodec codec,
                                                         std::span<std::byte const> input,
                                                         int level);

    /// Decompress `input` (produced by `Compress` with the same `codec`) back
    /// to exactly `originalLen` bytes.
    ///
    /// `originalLen` is the trusted expected size taken from the record header;
    /// it bounds the output allocation so a corrupt/oversized length can never
    /// drive an unbounded reserve (mirroring ReadOverflowChain's defence).
    /// @param codec       Codec the input was compressed with.
    /// @param input       Compressed bytes.
    /// @param originalLen Expected decompressed length.
    /// @return The decompressed bytes, or StorageErrorCode::Corrupt if the
    ///         input is malformed or does not expand to exactly `originalLen`.
    [[nodiscard]] static std::expected<std::vector<std::byte>, StorageError> Decompress(CompressionCodec codec,
                                                                                        std::span<std::byte const> input,
                                                                                        std::size_t originalLen);
};

} // namespace FastCache
