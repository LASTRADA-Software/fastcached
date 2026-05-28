// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/PageId.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace CowTree
{

/// Fixed-size page header preceding the entry payload on every data page.
/// Encoded in little-endian on disk.
///
/// Layout (16 bytes):
/// ```
/// [0..3]   crc32c     CRC-32C over [4..PageSize)
/// [4]      type       PageType (1=leaf, 2=internal)
/// [5]      reserved0
/// [6..7]   entryCount Number of entries in this page.
/// [8..15]  firstChild For internal pages: PageId of the leftmost child.
///                     For leaf pages: must be 0.
/// ```
inline constexpr std::size_t PageHeaderSize = 16;

/// Maximum length of a single key in bytes. Absolute ceiling; the
/// practical limit is set by the page size of the backing store (one
/// key+value must fit in one page).
inline constexpr std::size_t MaxKeyLength = 64 * 1024;

/// Maximum length of a single value in bytes. Absolute ceiling; the
/// practical limit is set by the page size of the backing store (one
/// key+value must fit in one page).
inline constexpr std::size_t MaxValueLength = 64 * 1024 * 1024;

/// Header view over the raw bytes of a page. The actual entries follow
/// the header, packed sequentially.
struct PageHeader
{
    std::uint32_t crc32c { 0 };
    PageType type { PageType::Leaf };
    std::uint16_t entryCount { 0 };
    PageId firstChild { PageId::None() };
};

/// Decode the page header from the front of `page` and verify the CRC
/// over `[4..pageSize)`.
/// @param page Page bytes (must equal pageSize).
/// @return Header on success; CowTreeError::Corrupt if the CRC fails.
[[nodiscard]] std::expected<PageHeader, CowTreeError> DecodePageHeader(BytesView page) noexcept;

/// Encode the header at the front of `page` and compute the CRC over
/// `[4..page.size())`. The caller must have populated `[PageHeaderSize..)`
/// with the entry payload first.
/// @param page Mutable page bytes (must equal pageSize).
/// @param header Header to encode (its crc32c field is ignored — recomputed).
[[nodiscard]] std::expected<void, CowTreeError> EncodePageHeader(BytesSpan page, PageHeader const& header) noexcept;

/// Logical entry inside a leaf page.
struct LeafEntry
{
    BytesView key;
    BytesView value;
};

/// Logical entry inside an internal page (the key-and-right-child pair;
/// the leftmost child lives separately in `PageHeader::firstChild`).
struct InternalEntry
{
    BytesView key;
    PageId child { PageId::None() };
};

/// Iterate the entries of a leaf page in stored order.
/// @param page    Decoded page bytes.
/// @param header  Decoded header (entryCount).
/// @return Vector of LeafEntry views into `page`. Lifetime tied to `page`.
[[nodiscard]] std::expected<std::vector<LeafEntry>, CowTreeError> DecodeLeafEntries(BytesView page,
                                                                                     PageHeader const& header) noexcept;

/// Iterate the entries of an internal page in stored order.
[[nodiscard]] std::expected<std::vector<InternalEntry>, CowTreeError> DecodeInternalEntries(BytesView page,
                                                                                             PageHeader const& header) noexcept;

/// Compute the on-page byte cost of a leaf entry (header bytes + key + value).
[[nodiscard]] constexpr std::size_t LeafEntryBytes(std::size_t keyLen, std::size_t valueLen) noexcept
{
    return sizeof(std::uint16_t) + sizeof(std::uint32_t) + keyLen + valueLen;
}

/// Compute the on-page byte cost of an internal entry (child id + key length + key).
[[nodiscard]] constexpr std::size_t InternalEntryBytes(std::size_t keyLen) noexcept
{
    return sizeof(std::uint64_t) + sizeof(std::uint16_t) + keyLen;
}

/// Write a fresh leaf page from a vector of entries.
/// @param page    Destination page (length = page size); zero-filled first.
/// @param entries Entries to encode, in lexicographic order.
/// @return CowTreeError::ValueTooLarge if the entries don't fit.
[[nodiscard]] std::expected<void, CowTreeError> EncodeLeafPage(BytesSpan page, std::span<LeafEntry const> entries) noexcept;

/// Write a fresh internal page from a leftmost child + vector of entries.
[[nodiscard]] std::expected<void, CowTreeError> EncodeInternalPage(BytesSpan page,
                                                                    PageId firstChild,
                                                                    std::span<InternalEntry const> entries) noexcept;

/// Effective payload area (bytes available for entries after the header).
[[nodiscard]] constexpr std::size_t PagePayloadCapacity(std::size_t pageSize) noexcept
{
    return pageSize - PageHeaderSize;
}

} // namespace CowTree
