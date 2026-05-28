// SPDX-License-Identifier: Apache-2.0
#include <CowTree/Crc32c.hpp>
#include <CowTree/Meta.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <ranges>

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

    /// Write the meta record (everything except the trailing CRC) into
    /// `dst`. Returns the number of bytes written.
    std::size_t EncodePayload(BytesSpan dst, Meta const& meta) noexcept
    {
        auto cursor = dst;
        WriteLe<std::uint32_t>(cursor, meta.magic);
        WriteLe<std::uint32_t>(cursor, meta.version);
        WriteLe<std::uint32_t>(cursor, meta.pageSize);
        WriteLe<std::uint32_t>(cursor, meta.reserved0);
        WriteLe<std::uint64_t>(cursor, meta.txnId);
        WriteLe<std::uint64_t>(cursor, meta.root.value);
        WriteLe<std::uint64_t>(cursor, meta.freeRoot.value);
        WriteLe<std::uint64_t>(cursor, meta.itemCount);
        WriteLe<std::uint64_t>(cursor, meta.reserved1);
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
    if (src.size() < MetaEncodedSize)
        return std::unexpected(CowTreeError::OutOfRange);

    auto const payloadSize = MetaEncodedSize - sizeof(std::uint32_t);
    auto const expectedCrc = Crc32c::Compute(src.subspan(0, payloadSize));

    auto cursor = src;
    Meta meta;
    meta.magic = ReadLe<std::uint32_t>(cursor);
    meta.version = ReadLe<std::uint32_t>(cursor);
    meta.pageSize = ReadLe<std::uint32_t>(cursor);
    meta.reserved0 = ReadLe<std::uint32_t>(cursor);
    meta.txnId = ReadLe<std::uint64_t>(cursor);
    meta.root = PageId { ReadLe<std::uint64_t>(cursor) };
    meta.freeRoot = PageId { ReadLe<std::uint64_t>(cursor) };
    meta.itemCount = ReadLe<std::uint64_t>(cursor);
    meta.reserved1 = ReadLe<std::uint64_t>(cursor);
    meta.crc32c = ReadLe<std::uint32_t>(cursor);

    if (meta.crc32c != expectedCrc)
        return std::unexpected(CowTreeError::Corrupt);
    if (meta.magic != MetaMagic)
        return std::unexpected(CowTreeError::InvalidArg);
    if (meta.version != MetaVersion)
        return std::unexpected(CowTreeError::InvalidArg);
    return meta;
}

} // namespace CowTree
