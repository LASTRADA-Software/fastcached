// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache
{

/// The binary form of a `StatsReading`: what a live-stats snapshot carries over `0xFC`
/// ([#1399](https://github.com/LASTRADA-Software/fastcached/issues/1399)).
///
/// ## Positional, and protected by a layout digest
///
/// Every field travels by POSITION -- the catalogue's row order, then each struct's fields in
/// the order the tables below list them -- with no name and no tag, because a node pushes
/// this every tick and a name per series is what made `/metrics` 50 KB. What makes position
/// safe is `StatsReadingLayout`: a digest computed at COMPILE TIME from the very tables the
/// encoder walks (every catalogue row's series name and type, every struct field's name and
/// width, the tier and role names). It is the first eight bytes of every encoding, and
/// `DecodeStatsReading` refuses a reading whose digest is not this build's by name
/// (`StatsReadingFault::ForeignLayout`) before it reads a single field.
///
/// So two builds whose catalogues differ by one row -- inserted, removed or moved -- cannot
/// misread each other's figures shifted by one; they refuse. That is the property an
/// explicit `= N` on every enumerator would otherwise be asked to carry, and it is carried
/// here instead: `IMetricsSink::Counter`'s ordinals are transmitted, the enum says so at its
/// declaration, and the digest is the enforcement that makes a mid-enum insertion a refusal
/// rather than a silent shift (the transmitted-enum bullet in
/// `.agent/rules/wire-and-protocol.md`). `= N` on a hundred rows would make every insertion a
/// renumbering and still say nothing about a struct field.
///
/// **What the digest cannot see**, stated so nobody over-reads it: a row whose NAME and member
/// POINTER disagree inside one build (`{"bytesUsed", &StorageStats::itemCount}`) encodes and
/// decodes self-consistently in that build. Name and pointer sit on one line for that reason.
///
/// ## Absent stays absent
///
/// Every optional member travels as a presence bit, and a counter this build's sink cannot
/// carry is a clear bit rather than a zero (#1353). A decoded reading therefore renders
/// through `RenderPrometheus` to the same body the node itself would have served.
///
/// ## Grammar
///
/// Big-endian, as the rest of this wire:
/// ```
/// u64 layout digest
/// counter presence bitmap, ceil(rows/8) bytes; then u64 per present counter, in row order
/// u8 presence: bit0 storage, bit1 host, bit2 upstreamConfigured, bit3 consensus, bit4 hostLoad,
///             bit5 rosterExpiresInSeconds
/// storage (if present): u64 per StorageStatsWireFields row
/// tier presence bitmap, ceil(tiers/8) bytes; then StorageStatsWireFields per present tier
/// host (if present): u64 per HostCapacityWireFields row
/// hostLoad (if present): u8 presence (bit0 cpu, bit1 availableMemoryBytes);
///                        u64 busy, u64 total (if cpu); u64 bytes (if availableMemoryBytes)
/// upstreamConfigured (if present): u8, 0 or 1
/// consensus (if present): u32 member count, then per member u32 length + bytes;
///                         u8 leader present, then u32 length + bytes; u64 term; u64 commitIndex; u8 role
/// rosterExpiresInSeconds (if present): u64
/// u64 uptime seconds
/// u32 length + bytes: the version of the build that captured the reading
/// ```
namespace StatsReadingWire
{

    /// One `u64`-valued field of a struct: its name, which the digest hashes, and where it lives.
    /// @tparam Class The struct the field belongs to.
    /// @tparam Member The field's own type.
    template <typename Class, typename Member>
    struct Field
    {
        std::string_view name;  ///< The field's name, hashed into `StatsReadingLayout`.
        Member Class::* member; ///< Where the value lives.
    };

    /// Every field of `StorageStats`, in the order a storage block travels.
    ///
    /// Two arrays because the struct mixes `std::size_t` and `std::uint64_t`; both travel as
    /// `u64`. Completeness is asserted below by size, the technique `IStorage.hpp` uses for its
    /// own field tables: a field added to the struct and to neither array fails the build.
    inline constexpr std::array StorageSizeFields {
        Field<StorageStats, std::size_t> { .name = "itemCount", .member = &StorageStats::itemCount },
        Field<StorageStats, std::size_t> { .name = "bytesUsed", .member = &StorageStats::bytesUsed },
        Field<StorageStats, std::size_t> { .name = "bytesLimit", .member = &StorageStats::bytesLimit },
        Field<StorageStats, std::size_t> { .name = "indexBytes", .member = &StorageStats::indexBytes },
        Field<StorageStats, std::size_t> { .name = "indexBytesAtCapacity", .member = &StorageStats::indexBytesAtCapacity },
    };

    /// The `std::uint64_t` half of `StorageStats`; see `StorageSizeFields`.
    inline constexpr std::array StorageCounterFields {
        Field<StorageStats, std::uint64_t> { .name = "evictions", .member = &StorageStats::evictions },
        Field<StorageStats, std::uint64_t> { .name = "cmdGet", .member = &StorageStats::cmdGet },
        Field<StorageStats, std::uint64_t> { .name = "cmdSet", .member = &StorageStats::cmdSet },
        Field<StorageStats, std::uint64_t> { .name = "cmdTouch", .member = &StorageStats::cmdTouch },
        Field<StorageStats, std::uint64_t> { .name = "cmdFlush", .member = &StorageStats::cmdFlush },
        Field<StorageStats, std::uint64_t> { .name = "getHits", .member = &StorageStats::getHits },
        Field<StorageStats, std::uint64_t> { .name = "getMisses", .member = &StorageStats::getMisses },
        Field<StorageStats, std::uint64_t> { .name = "deleteHits", .member = &StorageStats::deleteHits },
        Field<StorageStats, std::uint64_t> { .name = "deleteMisses", .member = &StorageStats::deleteMisses },
        Field<StorageStats, std::uint64_t> { .name = "incrHits", .member = &StorageStats::incrHits },
        Field<StorageStats, std::uint64_t> { .name = "incrMisses", .member = &StorageStats::incrMisses },
        Field<StorageStats, std::uint64_t> { .name = "decrHits", .member = &StorageStats::decrHits },
        Field<StorageStats, std::uint64_t> { .name = "decrMisses", .member = &StorageStats::decrMisses },
        Field<StorageStats, std::uint64_t> { .name = "touchHits", .member = &StorageStats::touchHits },
        Field<StorageStats, std::uint64_t> { .name = "touchMisses", .member = &StorageStats::touchMisses },
        Field<StorageStats, std::uint64_t> { .name = "casHits", .member = &StorageStats::casHits },
        Field<StorageStats, std::uint64_t> { .name = "casMisses", .member = &StorageStats::casMisses },
        Field<StorageStats, std::uint64_t> { .name = "casBadval", .member = &StorageStats::casBadval },
        Field<StorageStats, std::uint64_t> { .name = "evictedUnfetched", .member = &StorageStats::evictedUnfetched },
        Field<StorageStats, std::uint64_t> { .name = "expiredUnfetched", .member = &StorageStats::expiredUnfetched },
        Field<StorageStats, std::uint64_t> { .name = "expirations", .member = &StorageStats::expirations },
        Field<StorageStats, std::uint64_t> { .name = "writeErrors", .member = &StorageStats::writeErrors },
    };

    static_assert(sizeof(StorageStats)
                      == (StorageSizeFields.size() * sizeof(std::size_t))
                             + (StorageCounterFields.size() * sizeof(std::uint64_t)),
                  "every field of StorageStats must travel in a live-stats snapshot; a field in neither table is "
                  "silently missing from every dashboard");

    /// Every field of `HostCapacity`, in the order a host block travels.
    inline constexpr std::array HostSizeFields {
        Field<HostCapacity, std::size_t> { .name = "logicalCores", .member = &HostCapacity::logicalCores },
        Field<HostCapacity, std::size_t> { .name = "configuredSlots", .member = &HostCapacity::configuredSlots },
        Field<HostCapacity, std::size_t> { .name = "busySlots", .member = &HostCapacity::busySlots },
        Field<HostCapacity, std::size_t> { .name = "cordoned", .member = &HostCapacity::cordoned },
    };

    /// The `std::uint64_t` half of `HostCapacity`.
    inline constexpr std::array HostByteFields {
        Field<HostCapacity, std::uint64_t> { .name = "totalMemoryBytes", .member = &HostCapacity::totalMemoryBytes },
        Field<HostCapacity, std::uint64_t> { .name = "diskCapacityBytes", .member = &HostCapacity::diskCapacityBytes },
        Field<HostCapacity, std::uint64_t> { .name = "diskFreeBytes", .member = &HostCapacity::diskFreeBytes },
    };

    static_assert(std::has_unique_object_representations_v<HostCapacity>,
                  "HostCapacity must have no padding for the completeness check below to hold");
    static_assert(sizeof(HostCapacity)
                      == (HostSizeFields.size() * sizeof(std::size_t)) + (HostByteFields.size() * sizeof(std::uint64_t)),
                  "every field of HostCapacity must travel in a live-stats snapshot");

    /// The fields of `HostLoadReading`, by name, in the order they travel -- the CPU counters
    /// spelled as the two `u64`s the grammar writes rather than as the one optional holding them.
    ///
    /// Names rather than member pointers for the reason `ConsensusFieldNames` gives: the members
    /// are optionals of different shapes. The encoder's structured bindings hold the count.
    inline constexpr std::array HostLoadFieldNames { std::string_view { "cpuBusyTicks" },
                                                     std::string_view { "cpuTotalTicks" },
                                                     std::string_view { "availableMemoryBytes" } };

    /// The fields of `ConsensusStatus` and of `MetricsSnapshot`, by name, in the order they travel.
    ///
    /// Not member pointers, because the members are not all one width. Their COUNT is held to
    /// the struct by structured bindings in the encoder, which stop compiling the moment a field
    /// is added; their ORDER is the encoder's statement order, which sits beside this list.
    inline constexpr std::array ConsensusFieldNames { std::string_view { "configuration" },
                                                      std::string_view { "knownLeader" },
                                                      std::string_view { "term" },
                                                      std::string_view { "commitIndex" },
                                                      std::string_view { "role" } };

    /// See `ConsensusFieldNames`.
    inline constexpr std::array SnapshotFieldNames { std::string_view { "storage" },
                                                     std::string_view { "storageTiers" },
                                                     std::string_view { "host" },
                                                     std::string_view { "hostLoad" },
                                                     std::string_view { "upstreamConfigured" },
                                                     std::string_view { "consensus" },
                                                     std::string_view { "rosterExpiresInSeconds" },
                                                     std::string_view { "uptime" } };

    /// The fields of `StatsReading` itself, by name, in the order they travel. Held to the struct by the
    /// encoder's structured binding, as `SnapshotFieldNames` is.
    inline constexpr std::array ReadingFieldNames { std::string_view { "counters" },
                                                    std::string_view { "snapshot" },
                                                    std::string_view { "version" } };

    /// The grammar's own name, folded in first so a change to the grammar ABOVE with no table
    /// change still moves the digest. Bump the suffix whenever the encoder's statements change
    /// shape without a table changing.
    ///
    /// 5: the consensus block's one member list became two, voters then learners (#1449).
    /// 6: a presence bit and a `u64` after the consensus block, for the roster's lapse (#178).
    inline constexpr std::string_view Grammar = "stats-reading-grammar-6";

    /// 64-bit FNV-1a over @p text, continuing from @p hash.
    /// @param hash The running digest.
    /// @param text What to fold in.
    /// @return The new digest.
    [[nodiscard]] constexpr std::uint64_t Fold(std::uint64_t hash, std::string_view text) noexcept
    {
        constexpr std::uint64_t Prime = 0x100000001b3ULL;
        for (auto const c: text)
        {
            hash ^= static_cast<std::uint8_t>(c);
            hash *= Prime;
        }
        // A separator after every item, so `"ab" + "c"` and `"a" + "bc"` fold differently.
        hash ^= 0xFFU;
        hash *= Prime;
        return hash;
    }

    /// The digest over every table the encoder walks. See `StatsReadingLayout`.
    /// @param counters The catalogue, as a parameter so a test can hand in a shifted copy.
    /// @return The layout digest.
    template <std::size_t N>
    [[nodiscard]] constexpr std::uint64_t LayoutDigest(std::array<CounterDescriptor, N> const& counters) noexcept
    {
        auto hash = Fold(0xcbf29ce484222325ULL, Grammar);
        for (auto const& row: counters)
        {
            hash = Fold(hash, row.prometheusName);
            hash = Fold(hash, TypeName(row.type));
        }
        for (auto const& field: StorageSizeFields)
            hash = Fold(hash, field.name);
        for (auto const& field: StorageCounterFields)
            hash = Fold(hash, field.name);
        for (auto const& row: StorageTierTable)
            hash = Fold(hash, row.name);
        for (auto const& field: HostSizeFields)
            hash = Fold(hash, field.name);
        for (auto const& field: HostByteFields)
            hash = Fold(hash, field.name);
        for (auto const name: HostLoadFieldNames)
            hash = Fold(hash, name);
        for (auto const name: ConsensusFieldNames)
            hash = Fold(hash, name);
        for (auto const& row: Consensus::RoleTable)
            hash = Fold(hash, row.name);
        for (auto const name: SnapshotFieldNames)
            hash = Fold(hash, name);
        for (auto const name: ReadingFieldNames)
            hash = Fold(hash, name);
        return hash;
    }

} // namespace StatsReadingWire

/// This build's live-stats layout, computed at compile time from the tables the encoder walks.
///
/// Carried as the first eight bytes of every encoded reading, and in the subscription's first
/// push so a client can refuse a node of another layout before any snapshot arrives. Two
/// builds agree on it exactly when they agree on every row's position.
inline constexpr std::uint64_t StatsReadingLayout = StatsReadingWire::LayoutDigest(CounterTable);

/// Why a byte string is not a `StatsReading` this build can read. Private: never transmitted.
enum class StatsReadingFault : std::uint8_t
{
    /// The layout digest is another build's. Carries no guess at the fields: a reading laid out
    /// differently decodes to plausible numbers in the wrong places, which is worse than none.
    ForeignLayout,
    /// The bytes end before the grammar does.
    Truncated,
    /// A presence byte, a boolean or a role carries a value the grammar has no meaning for.
    Malformed,
    /// Bytes remain after the grammar ended.
    TrailingBytes,
};

/// Encode @p reading in this build's layout.
/// @param reading What to encode.
/// @return The bytes, beginning with `StatsReadingLayout`.
[[nodiscard]] std::vector<std::byte> EncodeStatsReading(StatsReading const& reading);

/// Decode a reading encoded by `EncodeStatsReading`.
/// @param bytes The encoded reading.
/// @return The reading, owning everything it holds, or why it could not be read.
[[nodiscard]] std::expected<StatsReading, StatsReadingFault> DecodeStatsReading(std::span<std::byte const> bytes);

/// The layout digest an encoded reading declares, without decoding the rest.
/// @param bytes The encoded reading.
/// @return Its digest, or nothing when the bytes are too short to carry one.
[[nodiscard]] std::optional<std::uint64_t> DeclaredStatsReadingLayout(std::span<std::byte const> bytes) noexcept;

} // namespace FastCache
