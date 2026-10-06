// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <ranges>

#include <CowTree/Crc32c.hpp>
#include <CowTree/Meta.hpp>

namespace CowTree
{

namespace
{

    /// Encode a fixed-width unsigned integer in little-endian into `dst`,
    /// advancing `dst` past the written bytes.
    template <typename T>
    void WriteLe(BytesSpan& dst, T value) noexcept
    {
        if constexpr (std::endian::native == std::endian::little)
        {
            std::memcpy(dst.data(), &value, sizeof(T));
        }
        else
        {
            T const swapped = std::byteswap(value);
            std::memcpy(dst.data(), &swapped, sizeof(T));
        }
        dst = dst.subspan(sizeof(T));
    }

    /// Decode a fixed-width unsigned integer from little-endian, advancing
    /// `src` past the consumed bytes.
    template <typename T>
    T ReadLe(BytesView& src) noexcept
    {
        T raw {};
        std::memcpy(&raw, src.data(), sizeof(T));
        src = src.subspan(sizeof(T));
        if constexpr (std::endian::native == std::endian::little)
            return raw;
        else
            return std::byteswap(raw);
    }

    /// One on-disk meta layout: the version that names it, its encoded size, and whether it carries
    /// `dataPages`. Version 1 is every store written before #1624; it still opens, reclaiming no
    /// extensions, and its slot is rewritten as the current version at the next meta write.
    struct MetaLayout
    {
        std::uint32_t version;
        std::size_t encodedSize;
        bool carriesDataPages;
    };

    constexpr auto MetaLayouts = std::to_array<MetaLayout>({
        { .version = 1U, .encodedSize = MetaEncodedSizeV1, .carriesDataPages = false },
        { .version = MetaVersion, .encodedSize = MetaEncodedSize, .carriesDataPages = true },
    });
    static_assert(MetaLayouts.back().version == MetaVersion, "the current layout is the last row");

    /// The layout row that names @p version.
    /// @param version Version read, untrusted, from a meta page.
    /// @return The row, or nothing when this build knows no such layout.
    std::optional<MetaLayout> LayoutOf(std::uint32_t version) noexcept
    {
        // A range-for rather than `ranges::find`: `readability-qualified-auto` wants `auto const*`
        // for the iterator, which does not compile on MSVC, where it is not a pointer.
        for (auto const& row: MetaLayouts)
            if (row.version == version)
                return row;
        return std::nullopt;
    }

    /// Write the meta record (everything except the trailing CRC) into
    /// `dst`. Returns the number of bytes written.
    std::size_t EncodePayload(BytesSpan dst, Meta const& meta) noexcept
    {
        auto cursor = dst;
        WriteLe<std::uint32_t>(cursor, meta.magic);
        WriteLe<std::uint32_t>(cursor, MetaVersion);
        WriteLe<std::uint32_t>(cursor, meta.pageSize);
        WriteLe<std::uint32_t>(cursor, meta.keyBytes);
        WriteLe<std::uint64_t>(cursor, meta.txnId);
        WriteLe<std::uint64_t>(cursor, meta.root.value);
        WriteLe<std::uint64_t>(cursor, meta.freeRoot.value);
        WriteLe<std::uint64_t>(cursor, meta.itemCount);
        WriteLe<std::uint64_t>(cursor, meta.valueBytes);
        WriteLe<std::uint64_t>(cursor, meta.dataPages.value_or(MetaDataPagesUnknown));
        return dst.size() - cursor.size();
    }

} // namespace

std::expected<void, CowTreeError> EncodeMeta(BytesSpan dst, Meta const& meta) noexcept
{
    if (dst.size() < MetaEncodedSize)
        return std::unexpected(CowTreeError::InvalidArg);

    std::ranges::fill(dst, std::byte { 0 });

    auto const payloadSize = EncodePayload(dst, meta);
    auto const crc = Crc32c::Compute(dst.subspan(0, payloadSize));

    auto crcSpan = dst.subspan(payloadSize, sizeof(std::uint32_t));
    WriteLe<std::uint32_t>(crcSpan, crc);
    return {};
}

std::expected<Meta, CowTreeError> DecodeMeta(BytesView src) noexcept
{
    constexpr auto versionOffset = sizeof(std::uint32_t);
    if (src.size() < versionOffset + sizeof(std::uint32_t))
        return std::unexpected(CowTreeError::OutOfRange);

    // The version is read without being trusted: it only selects the layout whose CRC decides.
    auto versionCursor = src.subspan(versionOffset);
    auto const version = ReadLe<std::uint32_t>(versionCursor);

    auto const row = LayoutOf(version);
    if (!row.has_value())
    {
        // No layout names this version. Under the current layout a CRC mismatch is damage (a
        // flipped version byte); a match is a meta some other build wrote.
        if (src.size() < MetaEncodedSize)
            return std::unexpected(CowTreeError::OutOfRange);
        auto const payloadSize = MetaEncodedSize - sizeof(std::uint32_t);
        auto crcCursor = src.subspan(payloadSize);
        if (Crc32c::Compute(src.subspan(0, payloadSize)) != ReadLe<std::uint32_t>(crcCursor))
            return std::unexpected(CowTreeError::Corrupt);
        return std::unexpected(CowTreeError::InvalidArg);
    }

    if (src.size() < row->encodedSize)
        return std::unexpected(CowTreeError::OutOfRange);

    auto const payloadSize = row->encodedSize - sizeof(std::uint32_t);
    auto const expectedCrc = Crc32c::Compute(src.subspan(0, payloadSize));

    auto cursor = src;
    Meta meta;
    meta.magic = ReadLe<std::uint32_t>(cursor);
    meta.version = ReadLe<std::uint32_t>(cursor);
    meta.pageSize = ReadLe<std::uint32_t>(cursor);
    meta.keyBytes = ReadLe<std::uint32_t>(cursor);
    meta.txnId = ReadLe<std::uint64_t>(cursor);
    meta.root = PageId { ReadLe<std::uint64_t>(cursor) };
    meta.freeRoot = PageId { ReadLe<std::uint64_t>(cursor) };
    meta.itemCount = ReadLe<std::uint64_t>(cursor);
    meta.valueBytes = ReadLe<std::uint64_t>(cursor);
    if (row->carriesDataPages)
    {
        auto const pages = ReadLe<std::uint64_t>(cursor);
        if (pages != MetaDataPagesUnknown)
            meta.dataPages = pages;
    }
    meta.crc32c = ReadLe<std::uint32_t>(cursor);

    if (meta.crc32c != expectedCrc)
        return std::unexpected(CowTreeError::Corrupt);
    if (meta.magic != MetaMagic)
        return std::unexpected(CowTreeError::InvalidArg);
    return meta;
}

} // namespace CowTree
