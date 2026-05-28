// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/PageId.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace CowTree
{

/// Magic bytes identifying a CowTree file: ASCII "FCOW" little-endian.
inline constexpr std::uint32_t MetaMagic = 0x574F'4346U;

/// On-disk meta-page layout version. Bump on any incompatible change.
inline constexpr std::uint32_t MetaVersion = 1U;

/// Default data page size used by FilePageStore unless overridden.
inline constexpr std::size_t DefaultPageSize = 4096;

/// Smallest page size we accept. Smaller would not fit the header +
/// useful payload reliably.
inline constexpr std::size_t MinPageSize = 512;

/// Largest page size we accept. The tree allows single-page entries
/// only, so the page size is the absolute hard cap on key+value bytes
/// for a single entry.
inline constexpr std::size_t MaxPageSize = 128 * 1024 * 1024;

/// In-memory representation of a meta page.
///
/// Two of these live at fixed offsets in the file (slot A at offset 0,
/// slot B at offset PageSize). Commit alternates between them; the slot
/// matching `txnId mod 2` always holds the most recent commit attempt.
/// Recovery picks the slot with the higher `txnId` whose CRC validates.
struct Meta
{
    /// Magic bytes (must equal `MetaMagic`).
    std::uint32_t magic { MetaMagic };

    /// Layout version (must equal `MetaVersion`).
    std::uint32_t version { MetaVersion };

    /// Data page size in bytes. Each non-meta page is exactly this big.
    std::uint32_t pageSize { 0 };

    /// Reserved; must be zero on disk.
    std::uint32_t reserved0 { 0 };

    /// Monotonically increasing transaction id. The valid meta with the
    /// higher txnId is the live one.
    TxnId txnId { 0 };

    /// Root page id of the B+tree. `PageId::None()` for an empty tree.
    PageId root { PageId::None() };

    /// Page id of the free-list head (linked list of pages reusable by
    /// future write transactions). `PageId::None()` if no free pages.
    PageId freeRoot { PageId::None() };

    /// Advisory cached entry count. Updated by writers; consumers may
    /// use it for stats but should not rely on it for correctness.
    std::uint64_t itemCount { 0 };

    /// Reserved; must be zero on disk.
    std::uint64_t reserved1 { 0 };

    /// CRC-32C over all preceding bytes when encoded on disk. Not used
    /// for in-memory comparisons.
    std::uint32_t crc32c { 0 };
};

/// Number of bytes the encoded form of `Meta` occupies. Fits well within
/// any supported page size; the rest of the meta page is zero-padded.
inline constexpr std::size_t MetaEncodedSize = 4 + 4 + 4 + 4 + 8 + 8 + 8 + 8 + 8 + 4;

/// Encode a Meta into the front of the given byte span. The remainder of
/// the span is zero-filled. The CRC is computed over the encoded payload
/// (everything except the CRC field itself) and stored as the trailing
/// 4 bytes of the encoded prefix.
///
/// @param dst    Destination span; must be at least `pageSize` bytes
///               long. The span is zero-filled in full before encoding.
/// @param meta   Source meta record. `meta.pageSize` must equal the
///               page size of the destination.
/// @return       Empty on success, CowTreeError::InvalidArg if `dst` is
///               smaller than `MetaEncodedSize`.
[[nodiscard]] std::expected<void, CowTreeError> EncodeMeta(BytesSpan dst, Meta const& meta) noexcept;

/// Decode a Meta from the front of the given byte span, verifying the
/// CRC and the magic+version fields.
///
/// @param src    Source span; must start with an encoded meta page.
/// @return       Decoded meta on success; CowTreeError::Corrupt if the
///               CRC mismatches; CowTreeError::InvalidArg if magic or
///               version is wrong; CowTreeError::OutOfRange if `src` is
///               too small to hold an encoded meta.
[[nodiscard]] std::expected<Meta, CowTreeError> DecodeMeta(BytesView src) noexcept;

} // namespace CowTree
