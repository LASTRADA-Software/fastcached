// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Which kind of storage a set of statistics describes.
///
/// A cache in this project is frequently more than one store — the daemon's
/// `--storage` and the node's `--cache-dir` both build an in-memory LRU over an
/// on-disk B+tree — and a merged view of the pair loses precisely the numbers an
/// operator needs. `LayeredStorage::Snapshot()` reports L2's item count, bytes and
/// budget with the composite's hit/miss patched over them, so the in-memory half
/// is invisible: a node whose RAM tier is full and whose disk tier is nearly empty
/// reads identically to one whose RAM tier was never populated.
///
/// A taxonomy rather than a bool because the third case is foreseeable — a tier
/// held on a peer, a tier on a second filesystem — and each would otherwise be
/// another field somebody adds to every struct that carries these numbers.
///
/// **The enumerator values are an index, so they are appended and never
/// reordered.** `TieredStorageStats` is a table indexed by them, and every
/// consumer that carries a tier apart from its label -- a scrape, a record on a
/// wire, anything positional -- inherits that indexing. Renumbering would read
/// one tier's numbers under another's name with nothing anywhere reporting a
/// fault, which is the shape of failure this whole file exists to prevent.
enum class StorageTier : std::uint8_t
{
    /// Held in this process's memory. Lost when it exits.
    Memory = 0,
    /// Held on a filesystem. Survives a restart.
    Disk,
    Last, ///< Not a tier, and has no row: `StorageTierTable`'s length.
};

/// One row per tier: what it is, and what it is called.
struct StorageTierTraits
{
    StorageTier tier;      ///< The tier this row describes.
    std::string_view name; ///< Spelling used as a metric label and in diagnostics.
};

/// Every tier, in enumerator order.
inline constexpr EnumTable<StorageTier, StorageTierTraits> StorageTierTable { {
    { .tier = StorageTier::Memory, .name = "memory" },
    { .tier = StorageTier::Disk, .name = "disk" },
} };

// A tier added to the enum without a row is a build failure rather than a series
// an operator was told to scrape and that renders as a gap.
static_assert(RowsInEnumeratorOrder(StorageTierTable, &StorageTierTraits::tier),
              "StorageTierTable must hold one row per StorageTier, in enumerator order");

/// The traits for a tier.
/// @param tier The tier.
/// @return Its row.
[[nodiscard]] constexpr StorageTierTraits const& TraitsFor(StorageTier tier) noexcept
{
    return StorageTierTable[static_cast<std::size_t>(tier)];
}

} // namespace FastCache
