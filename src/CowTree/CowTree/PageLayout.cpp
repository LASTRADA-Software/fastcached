// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <ranges>

#include <CowTree/Crc32c.hpp>
#include <CowTree/PageLayout.hpp>

namespace CowTree
{

namespace
{

    template <typename T>
    void WriteLe(BytesSpan& dst, T value) noexcept
    {
        if constexpr (std::endian::native == std::endian::little)
            std::memcpy(dst.data(), &value, sizeof(T));
        else
        {
            T const swapped = std::byteswap(value);
            std::memcpy(dst.data(), &swapped, sizeof(T));
        }
        dst = dst.subspan(sizeof(T));
    }

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

    template <typename T>
    T PeekLe(BytesView src, std::size_t offset) noexcept
    {
        T raw {};
        std::memcpy(&raw, src.data() + offset, sizeof(T));
        if constexpr (std::endian::native == std::endian::little)
            return raw;
        else
            return std::byteswap(raw);
    }

} // namespace

std::expected<PageHeader, CowTreeError> DecodePageHeader(BytesView page) noexcept
{
    if (page.size() < PageHeaderSize)
        return std::unexpected(CowTreeError::OutOfRange);

    auto cursor = page;
    auto const storedCrc = ReadLe<std::uint32_t>(cursor);
    auto const typeByte = ReadLe<std::uint8_t>(cursor);
    (void) ReadLe<std::uint8_t>(cursor); // reserved0
    auto const entryCount = ReadLe<std::uint16_t>(cursor);
    auto const firstChild = ReadLe<std::uint64_t>(cursor);

    auto const expectedCrc = Crc32c::Compute(page.subspan(4));
    if (storedCrc != expectedCrc)
        return std::unexpected(CowTreeError::Corrupt);

    PageHeader header;
    header.crc32c = storedCrc;
    if (typeByte != static_cast<std::uint8_t>(PageType::Leaf) && typeByte != static_cast<std::uint8_t>(PageType::Internal))
        return std::unexpected(CowTreeError::Corrupt);
    header.type = static_cast<PageType>(typeByte);
    header.entryCount = entryCount;
    header.firstChild = PageId { firstChild };
    return header;
}

std::expected<void, CowTreeError> EncodePageHeader(BytesSpan page, PageHeader const& header) noexcept
{
    if (page.size() < PageHeaderSize)
        return std::unexpected(CowTreeError::OutOfRange);

    auto cursor = page;
    // Skip the CRC slot for now; fill it in last once the payload is known.
    auto const crcDst = cursor.subspan(0, 4);
    cursor = cursor.subspan(4);
    WriteLe<std::uint8_t>(cursor, static_cast<std::uint8_t>(header.type));
    WriteLe<std::uint8_t>(cursor, 0);
    WriteLe<std::uint16_t>(cursor, header.entryCount);
    WriteLe<std::uint64_t>(cursor, header.firstChild.value);

    auto const crc = Crc32c::Compute(page.subspan(4));
    auto crcSpan = crcDst;
    WriteLe<std::uint32_t>(crcSpan, crc);
    return {};
}

std::expected<std::vector<LeafEntry>, CowTreeError> DecodeLeafEntries(BytesView page, PageHeader const& header) noexcept
{
    if (header.type != PageType::Leaf)
        return std::unexpected(CowTreeError::Corrupt);

    auto cursor = page.subspan(PageHeaderSize);
    std::vector<LeafEntry> entries;
    entries.reserve(header.entryCount);

    for (std::uint16_t i = 0; i < header.entryCount; ++i)
    {
        if (cursor.size() < sizeof(std::uint16_t) + sizeof(std::uint32_t))
            return std::unexpected(CowTreeError::Corrupt);
        auto const keyLen = ReadLe<std::uint16_t>(cursor);
        auto const valLen = ReadLe<std::uint32_t>(cursor);
        if (cursor.size() < keyLen + valLen)
            return std::unexpected(CowTreeError::Corrupt);
        LeafEntry entry;
        entry.key = cursor.subspan(0, keyLen);
        entry.value = cursor.subspan(keyLen, valLen);
        cursor = cursor.subspan(keyLen + valLen);
        entries.push_back(entry);
    }
    return entries;
}

std::expected<std::vector<InternalEntry>, CowTreeError> DecodeInternalEntries(BytesView page,
                                                                              PageHeader const& header) noexcept
{
    if (header.type != PageType::Internal)
        return std::unexpected(CowTreeError::Corrupt);

    auto cursor = page.subspan(PageHeaderSize);
    std::vector<InternalEntry> entries;
    entries.reserve(header.entryCount);

    for (std::uint16_t i = 0; i < header.entryCount; ++i)
    {
        if (cursor.size() < sizeof(std::uint64_t) + sizeof(std::uint16_t))
            return std::unexpected(CowTreeError::Corrupt);
        auto const child = ReadLe<std::uint64_t>(cursor);
        auto const keyLen = ReadLe<std::uint16_t>(cursor);
        if (cursor.size() < keyLen)
            return std::unexpected(CowTreeError::Corrupt);
        InternalEntry entry;
        entry.child = PageId { child };
        entry.key = cursor.subspan(0, keyLen);
        cursor = cursor.subspan(keyLen);
        entries.push_back(entry);
    }
    return entries;
}

std::expected<void, CowTreeError> EncodeLeafPage(BytesSpan page, std::span<LeafEntry const> entries) noexcept
{
    std::ranges::fill(page, std::byte { 0 });

    if (entries.size() > MaxEntriesPerPage)
        return std::unexpected(CowTreeError::ValueTooLarge);

    // Compute total byte cost; reject if it overflows the page.
    std::size_t needed = 0;
    for (auto const& e: entries)
    {
        if (e.key.size() > MaxKeyLength || e.value.size() > MaxValueLength)
            return std::unexpected(CowTreeError::ValueTooLarge);
        needed += LeafEntryBytes(e.key.size(), e.value.size());
    }
    if (needed > PagePayloadCapacity(page.size()))
        return std::unexpected(CowTreeError::ValueTooLarge);

    auto cursor = page.subspan(PageHeaderSize);
    for (auto const& e: entries)
    {
        WriteLe<std::uint16_t>(cursor, static_cast<std::uint16_t>(e.key.size()));
        WriteLe<std::uint32_t>(cursor, static_cast<std::uint32_t>(e.value.size()));
        std::memcpy(cursor.data(), e.key.data(), e.key.size());
        cursor = cursor.subspan(e.key.size());
        std::memcpy(cursor.data(), e.value.data(), e.value.size());
        cursor = cursor.subspan(e.value.size());
    }

    PageHeader header;
    header.type = PageType::Leaf;
    header.entryCount = static_cast<std::uint16_t>(entries.size());
    header.firstChild = PageId::None();
    return EncodePageHeader(page, header);
}

std::expected<void, CowTreeError> EncodeInternalPage(BytesSpan page,
                                                     PageId firstChild,
                                                     std::span<InternalEntry const> entries) noexcept
{
    std::ranges::fill(page, std::byte { 0 });

    if (entries.size() > MaxEntriesPerPage)
        return std::unexpected(CowTreeError::ValueTooLarge);

    std::size_t needed = 0;
    for (auto const& e: entries)
    {
        if (e.key.size() > MaxKeyLength)
            return std::unexpected(CowTreeError::ValueTooLarge);
        needed += InternalEntryBytes(e.key.size());
    }
    if (needed > PagePayloadCapacity(page.size()))
        return std::unexpected(CowTreeError::ValueTooLarge);

    auto cursor = page.subspan(PageHeaderSize);
    for (auto const& e: entries)
    {
        WriteLe<std::uint64_t>(cursor, e.child.value);
        WriteLe<std::uint16_t>(cursor, static_cast<std::uint16_t>(e.key.size()));
        std::memcpy(cursor.data(), e.key.data(), e.key.size());
        cursor = cursor.subspan(e.key.size());
    }

    PageHeader header;
    header.type = PageType::Internal;
    header.entryCount = static_cast<std::uint16_t>(entries.size());
    header.firstChild = firstChild;
    return EncodePageHeader(page, header);
}

} // namespace CowTree
