// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Compression.hpp>

#include <array>
#include <cstddef>
#include <string>

#if defined(FC_COMPRESSION_ENABLED)
    #include <limits>

    #include <lz4.h>
    #include <zstd.h>
#endif

namespace FastCache
{

namespace
{

    /// Build a Corrupt StorageError with an explanatory context string.
    /// @param context Human-readable reason.
    /// @return StorageError tagged Corrupt.
    [[nodiscard]] StorageError CorruptError(std::string context)
    {
        return StorageError { .code = StorageErrorCode::Corrupt, .systemCode = 0, .context = std::move(context) };
    }

    /// Verbatim copy of `input`; the Identity codec's compress path and the
    /// fallback for codecs that are not compiled in.
    [[nodiscard]] std::vector<std::byte> CopyBytes(std::span<std::byte const> input)
    {
        return std::vector<std::byte> { input.begin(), input.end() };
    }

    /// Identity decode: the stored bytes must already be exactly `originalLen`.
    [[nodiscard]] std::expected<std::vector<std::byte>, StorageError> IdentityDecompress(std::span<std::byte const> input,
                                                                                         std::size_t originalLen)
    {
        if (input.size() != originalLen)
            return std::unexpected(CorruptError("identity length mismatch"));
        return CopyBytes(input);
    }

#if defined(FC_COMPRESSION_ENABLED)

    // --- zstd -------------------------------------------------------------

    [[nodiscard]] std::vector<std::byte> ZstdCompress(std::span<std::byte const> input, int level)
    {
        auto const bound = ZSTD_compressBound(input.size());
        std::vector<std::byte> out(bound);
        auto const written = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), level);
        // A compress failure is not fatal: the caller shrink-checks the result
        // and falls back to Identity when compression does not help, and an
        // empty output never beats the raw size — so it is simply rejected.
        if (ZSTD_isError(written) != 0U)
            return {};
        out.resize(written);
        return out;
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, StorageError> ZstdDecompress(std::span<std::byte const> input,
                                                                                     std::size_t originalLen)
    {
        std::vector<std::byte> out(originalLen);
        auto const produced = ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
        if (ZSTD_isError(produced) != 0U)
            return std::unexpected(CorruptError("zstd decode failed"));
        if (produced != originalLen)
            return std::unexpected(CorruptError("zstd length mismatch"));
        return out;
    }

    // --- lz4 --------------------------------------------------------------

    [[nodiscard]] std::vector<std::byte> Lz4Compress(std::span<std::byte const> input, int /*level*/)
    {
        // LZ4 block API takes int-sized lengths; a value larger than INT_MAX
        // cannot be block-compressed, so decline and let the shrink-check keep
        // it as Identity. (maxValueBytes is far below this in practice.)
        if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return {};
        auto const srcLen = static_cast<int>(input.size());
        auto const bound = LZ4_compressBound(srcLen);
        if (bound <= 0)
            return {};
        std::vector<std::byte> out(static_cast<std::size_t>(bound));
        auto const written = LZ4_compress_default(
            reinterpret_cast<char const*>(input.data()), reinterpret_cast<char*>(out.data()), srcLen, bound);
        if (written <= 0)
            return {};
        out.resize(static_cast<std::size_t>(written));
        return out;
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, StorageError> Lz4Decompress(std::span<std::byte const> input,
                                                                                    std::size_t originalLen)
    {
        // LZ4_decompress_safe caps output at `originalLen`, so a corrupt input
        // can never overrun the buffer; a wrong length is caught by the
        // produced-count check below.
        if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
            || originalLen > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return std::unexpected(CorruptError("lz4 length out of range"));
        std::vector<std::byte> out(originalLen);
        // originalLen is already range-checked to fit an int above, so comparing
        // the produced count in int space avoids a signed/unsigned comparison.
        auto const expected = static_cast<int>(originalLen);
        auto const produced = LZ4_decompress_safe(reinterpret_cast<char const*>(input.data()),
                                                  reinterpret_cast<char*>(out.data()),
                                                  static_cast<int>(input.size()),
                                                  expected);
        if (produced != expected)
            return std::unexpected(CorruptError("lz4 decode failed"));
        return out;
    }

#endif // FC_COMPRESSION_ENABLED

    /// One row of the codec dispatch table. Keeping the id/name/availability
    /// and the two function pointers together makes adding a codec a one-row
    /// change and lets every consumer iterate the same source of truth.
    struct CodecDescriptor
    {
        CompressionCodec codec;                                              ///< On-disk id.
        std::string_view name;                                               ///< Lower-case config/banner name.
        bool available;                                                      ///< Compiled into this build.
        std::vector<std::byte> (*compress)(std::span<std::byte const>, int); ///< Encode.
        std::expected<std::vector<std::byte>, StorageError> (*decompress)(std::span<std::byte const>,
                                                                          std::size_t); ///< Decode.
    };

    /// The codec table — the single source of truth. Rows are in id order.
    constexpr std::size_t CodecCount = 3;
    std::array<CodecDescriptor, CodecCount> const& CodecTable() noexcept
    {
        static std::array<CodecDescriptor, CodecCount> const table {
            CodecDescriptor { .codec = CompressionCodec::Identity,
                              .name = "none",
                              .available = true,
                              .compress = [](std::span<std::byte const> in, int) { return CopyBytes(in); },
                              .decompress = &IdentityDecompress },
#if defined(FC_COMPRESSION_ENABLED)
            CodecDescriptor { .codec = CompressionCodec::Lz4,
                              .name = "lz4",
                              .available = true,
                              .compress = &Lz4Compress,
                              .decompress = &Lz4Decompress },
            CodecDescriptor { .codec = CompressionCodec::Zstd,
                              .name = "zstd",
                              .available = true,
                              .compress = &ZstdCompress,
                              .decompress = &ZstdDecompress },
#else
            // Names stay recognised so config validation can distinguish
            // "unavailable in this build" from "unknown codec". The function
            // pointers fall back to verbatim copy / a Corrupt decode (never
            // reached: config rejects selecting an unavailable codec).
            CodecDescriptor { .codec = CompressionCodec::Lz4,
                              .name = "lz4",
                              .available = false,
                              .compress = [](std::span<std::byte const> in, int) { return CopyBytes(in); },
                              .decompress = &IdentityDecompress },
            CodecDescriptor { .codec = CompressionCodec::Zstd,
                              .name = "zstd",
                              .available = false,
                              .compress = [](std::span<std::byte const> in, int) { return CopyBytes(in); },
                              .decompress = &IdentityDecompress },
#endif
        };
        return table;
    }

    /// Look up the descriptor for a codec id, or nullptr if the id is unknown
    /// (a corrupt on-disk record).
    [[nodiscard]] CodecDescriptor const* Find(CompressionCodec codec) noexcept
    {
        for (auto const& row: CodecTable())
            if (row.codec == codec)
                return &row;
        return nullptr;
    }

} // namespace

bool Compression::IsAvailable(CompressionCodec codec) noexcept
{
    auto const* row = Find(codec);
    return row != nullptr && row->available;
}

std::string_view Compression::NameOf(CompressionCodec codec) noexcept
{
    auto const* row = Find(codec);
    return row != nullptr ? row->name : std::string_view { "unknown" };
}

std::optional<CompressionCodec> Compression::CodecFromName(std::string_view name) noexcept
{
    for (auto const& row: CodecTable())
        if (row.name == name)
            return row.codec;
    return std::nullopt;
}

std::string_view Compression::NameList() noexcept
{
    static std::string const list = [] {
        std::string acc;
        for (auto const& row: CodecTable())
        {
            if (!acc.empty())
                acc += ", ";
            acc += row.name;
        }
        return acc;
    }();
    return list;
}

std::vector<std::byte> Compression::Compress(CompressionCodec codec, std::span<std::byte const> input, int level)
{
    auto const* row = Find(codec);
    if (row == nullptr || !row->available)
        return CopyBytes(input);
    return row->compress(input, level);
}

std::expected<std::vector<std::byte>, StorageError> Compression::Decompress(CompressionCodec codec,
                                                                            std::span<std::byte const> input,
                                                                            std::size_t originalLen)
{
    auto const* row = Find(codec);
    if (row == nullptr)
        return std::unexpected(CorruptError("unknown compression codec id"));
    if (!row->available)
        return std::unexpected(CorruptError("record uses a codec not compiled into this build"));
    return row->decompress(input, originalLen);
}

} // namespace FastCache
