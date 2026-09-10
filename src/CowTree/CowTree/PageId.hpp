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
/// or written. Which slot a commit takes is the page store's choice -- the one
/// not holding the last durable meta -- and NOT `txnId mod 2`, which this
/// comment claimed until #726; see `Meta`.
///
/// **ORDINALS ARE AN ON-DISK CONTRACT. Never insert or reorder, and there is no room
/// to append.** (#308) The value is not decoded from a byte, it is MULTIPLIED into a
/// file offset: `FilePageStore::MetaSlotOffset` is `slot * pageSize`, so these two
/// numbers name the first two pages of every store this project has ever written.
/// A third slot inserted mid-enum would put `B` where the first DATA page lives.
enum class MetaSlot : std::uint8_t
{
    A = 0, ///< First meta page (file offset 0).
    B = 1, ///< Second meta page (file offset PageSize).
};

/// The meta slot that is not `slot`.
///
/// One spelling, because "write the slot we did not last make durable" is the
/// commit-point rule for BOTH the batched flush and the strict path, and the two
/// wrote it out separately as inline ternaries. It also existed as a private
/// helper in two test files, which is how a rulebook entry came to record that
/// it "exists in no production file".
/// @param slot One slot.
/// @return The one it alternates with.
[[nodiscard]] constexpr MetaSlot OtherSlot(MetaSlot slot) noexcept
{
    return slot == MetaSlot::A ? MetaSlot::B : MetaSlot::A;
}

/// Discriminator for data page kinds.
///
/// **ORDINALS ARE AN ON-DISK CONTRACT. Append only; never insert, reorder or reuse.**
/// (#308) The enumerator's numeric value IS the page header's type byte:
/// `PageLayout` writes it and reads it back with a `static_cast<PageType>`, so these
/// two numbers describe every store this project has ever written.
///
/// Renumbering them does not fail to open a store -- `RecordFormats()` and the meta
/// CRC both still agree -- it makes a walk read a leaf's entries under the internal
/// page's layout, which is the `Corrupt` an operator is told means the bytes are
/// damaged. `DecodePageHeader` refuses a byte that is neither value, so a zeroed or
/// torn page is caught -- but SWAPPING two live values passes that guard perfectly,
/// which is why the direction that matters is renumbering rather than an unknown byte.
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
