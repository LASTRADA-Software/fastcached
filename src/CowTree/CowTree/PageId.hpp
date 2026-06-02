// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace CowTree
{

/// Strongly-typed identifier for a page within an IPageStore.
///
/// PageId(0) is reserved to mean "no page" — used to represent a null
/// root, an absent free-list head, etc. Real pages always have a non-zero
/// PageId. The two meta-page slots are addressed separately via MetaSlot
/// and do not consume PageId values.
struct PageId
{
    /// Raw page number. Zero means "none".
    std::uint64_t value { 0 };

    /// Default-constructed PageId equals None.
    constexpr PageId() noexcept = default;

    /// Wrap a raw page number.
    /// @param v Raw 64-bit page index.
    constexpr explicit PageId(std::uint64_t v) noexcept:
        value { v }
    {
    }

    /// Three-way comparison so PageId can be used in ordered containers.
    [[nodiscard]] constexpr auto operator<=>(PageId const&) const noexcept = default;

    /// True iff this is not the reserved "none" value.
    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }

    /// The reserved sentinel meaning "no page" / null root.
    static constexpr PageId None() noexcept
    {
        return PageId { 0 };
    }
};

/// Identifies which of the two alternating meta-page slots is being read
/// or written. The slot used for a given transaction is determined by
/// `txnId mod 2`.
enum class MetaSlot : std::uint8_t
{
    A = 0, ///< First meta page (file offset 0).
    B = 1, ///< Second meta page (file offset PageSize).
};

/// Discriminator for data page kinds.
enum class PageType : std::uint8_t
{
    Leaf = 1,     ///< Leaf page: holds key→value entries.
    Internal = 2, ///< Internal page: holds key→child-PageId entries.
};

/// Monotonically increasing transaction identifier. The Meta page with
/// the higher (valid-CRC) `txnId` is the live one on recovery.
using TxnId = std::uint64_t;

} // namespace CowTree

/// Hash specialisation so `PageId` can key `std::unordered_map` etc.
template <>
struct std::hash<CowTree::PageId>
{
    [[nodiscard]] std::size_t operator()(CowTree::PageId id) const noexcept
    {
        return std::hash<std::uint64_t> {}(id.value);
    }
};
