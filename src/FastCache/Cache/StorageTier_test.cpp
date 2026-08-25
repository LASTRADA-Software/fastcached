// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cache/TracingStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using FastCache::StorageTier;
using FastCache::Testing::MakeBytes;
using FastCache::Testing::Unwrap;

namespace
{

/// The entry for one tier, or nullopt when the storage reports no such tier.
/// @param tiers What `SnapshotTiers()` returned.
/// @param tier Which tier to read.
/// @return That tier's entry.
[[nodiscard]] std::optional<FastCache::StorageStats> const& At(FastCache::TieredStorageStats const& tiers, StorageTier tier)
{
    return tiers[static_cast<std::size_t>(tier)];
}

/// Open a CoW tree inside @p scratch, failing the test rather than returning null.
/// @param scratch Directory to put the store in.
/// @return The opened tree.
[[nodiscard]] std::unique_ptr<FastCache::CowTreeStorage> OpenCowTree(FastCache::Testing::ScratchDirectory const& scratch)
{
    FastCache::CowTreeStorage::Options opts;
    opts.path = (scratch / "cache.cow").string();
    auto opened = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

} // namespace

TEST_CASE("StorageTierTable names every tier distinctly", "[storage-tier]")
{
    // A tier without a row is already a build failure -- the static_assert beside
    // the table sees to that. What it cannot see is two tiers sharing a name: the
    // label is what separates the series on a scrape, so a duplicate renders as
    // one line silently overwriting the other.
    std::vector<std::string_view> names;
    for (auto const& row: FastCache::StorageTierTable)
    {
        CHECK_FALSE(row.name.empty());
        CHECK(FastCache::TraitsFor(row.tier).name == row.name);
        names.push_back(row.name);
    }
    std::ranges::sort(names);
    CHECK(std::ranges::adjacent_find(names) == names.end());
}

TEST_CASE("Every StorageStats field is summed exactly once", "[storage-tier]")
{
    // The two field tables are what replaced twenty-three hand-written `+=` lines,
    // and the realistic slip in a list that long is a copy-paste that repeats one
    // field while dropping its neighbour -- which sums correctly for the repeated
    // one and silently never for the dropped one.
    auto const noDuplicates = [](auto const& table) {
        return std::ranges::all_of(std::views::iota(std::size_t { 0 }, table.size()),
                                   [&table](std::size_t i) { return std::ranges::count(table, table[i]) == 1; });
    };
    CHECK(noDuplicates(FastCache::StorageStatsSizeFields));
    CHECK(noDuplicates(FastCache::StorageStatsCounterFields));

    // Distinct values, not a common one: a table of near-identical rows makes
    // "this row adds its neighbour's field" the likely mistake, and equal values
    // would let it through.
    FastCache::StorageStats addend {};
    std::size_t next = 1;
    for (auto const field: FastCache::StorageStatsSizeFields)
        addend.*field = next++;
    for (auto const field: FastCache::StorageStatsCounterFields)
        addend.*field = next++;

    FastCache::StorageStats total {};
    AddInto(total, addend);
    AddInto(total, addend);

    for (auto const field: FastCache::StorageStatsSizeFields)
        CHECK(total.*field == addend.*field * 2);
    for (auto const field: FastCache::StorageStatsCounterFields)
        CHECK(total.*field == addend.*field * 2);
    // The field the hand-written sum had dropped, named so a regression reads as
    // itself rather than as one of twenty-four anonymous CHECKs.
    CHECK(total.writeErrors == addend.writeErrors * 2);
}

TEST_CASE("Folding a tiered snapshot never invents a tier", "[storage-tier]")
{
    FastCache::TieredStorageStats total {};
    total[static_cast<std::size_t>(StorageTier::Memory)] = FastCache::StorageStats { .itemCount = 3 };

    FastCache::TieredStorageStats addend {};
    addend[static_cast<std::size_t>(StorageTier::Memory)] = FastCache::StorageStats { .itemCount = 4 };

    AddInto(total, addend);
    REQUIRE(At(total, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(total, StorageTier::Memory)).itemCount == 7);
    // Absent stays absent. Merging is where "this cache has no disk tier" would
    // otherwise quietly become "a disk tier holding nothing".
    CHECK_FALSE(At(total, StorageTier::Disk).has_value());

    addend[static_cast<std::size_t>(StorageTier::Disk)] = FastCache::StorageStats { .itemCount = 5 };
    AddInto(total, addend);
    REQUIRE(At(total, StorageTier::Disk).has_value());
    CHECK(Unwrap(At(total, StorageTier::Disk)).itemCount == 5);
}

TEST_CASE("An in-memory store reports exactly one memory tier", "[storage-tier]")
{
    FastCache::InMemoryLruStorage storage { 4096 };
    REQUIRE(storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    auto const tiers = storage.SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).itemCount == 1);
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == 4096);

    // Absent, not a disk tier holding nothing. A dashboard reads a zero as
    // "empty", which is a different claim from "there isn't one".
    CHECK_FALSE(At(tiers, StorageTier::Disk).has_value());
}

TEST_CASE("A CoW tree reports itself as the disk tier", "[storage-tier]")
{
    FastCache::Testing::ScratchDirectory const scratch { "storage-tier-cow" };
    auto const storage = OpenCowTree(scratch);
    REQUIRE(storage->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    auto const tiers = storage->SnapshotTiers();
    // The interface default would have called this memory. A node's whole on-disk
    // cache under the label an operator reads as RAM is worse than no label.
    CHECK_FALSE(At(tiers, StorageTier::Memory).has_value());
    REQUIRE(At(tiers, StorageTier::Disk).has_value());
    CHECK(Unwrap(At(tiers, StorageTier::Disk)).itemCount == 1);
}

TEST_CASE("LayeredStorage keeps its two tiers apart", "[storage-tier][layered]")
{
    FastCache::Testing::ScratchDirectory const scratch { "storage-tier-layered" };

    // A deliberately small L1 over an unbounded L2, so the two budgets differ and
    // a test that reported one where the other belongs cannot pass by coincidence.
    constexpr std::size_t L1Budget = 64 * 1024;
    FastCache::LayeredStorage storage { std::make_unique<FastCache::InMemoryLruStorage>(L1Budget), OpenCowTree(scratch) };
    REQUIRE(storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    auto const tiers = storage.SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    REQUIRE(At(tiers, StorageTier::Disk).has_value());

    // The whole point: `Snapshot()` reports L2's budget with the composite's own
    // hit/miss patched over it, so L1's budget does not survive it. Here it does.
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == L1Budget);
    CHECK(Unwrap(At(tiers, StorageTier::Disk)).bytesLimit == 0);
    CHECK(storage.Snapshot().bytesLimit == 0);

    CHECK(Unwrap(At(tiers, StorageTier::Memory)).itemCount == 1);
    CHECK(Unwrap(At(tiers, StorageTier::Disk)).itemCount == 1);
}

TEST_CASE("A memory-only LayeredStorage reports no disk tier", "[storage-tier][layered]")
{
    // Both halves in memory -- which is what most of this suite's fixtures build,
    // and what a double standing in for a disk tier is. Each store answers for
    // itself, so neither is mislabelled by the composite above it.
    FastCache::LayeredStorage storage { std::make_unique<FastCache::InMemoryLruStorage>(4096),
                                        std::make_unique<FastCache::InMemoryLruStorage>(0) };
    REQUIRE(storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    auto const tiers = storage.SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    CHECK_FALSE(At(tiers, StorageTier::Disk).has_value());
    // Summed, because both stores answer "memory": one entry, mirrored, twice.
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).itemCount == 2);
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == 4096);
}

TEST_CASE("ShardedStorage sums its shards tier by tier", "[storage-tier][sharded]")
{
    constexpr std::size_t ShardCount = 4;
    constexpr std::size_t PerShardBudget = 1024;
    std::vector<std::unique_ptr<FastCache::IStorage>> inners;
    inners.reserve(ShardCount);
    for ([[maybe_unused]] auto const shard: std::views::iota(std::size_t { 0 }, ShardCount))
        inners.emplace_back(std::make_unique<FastCache::InMemoryLruStorage>(PerShardBudget));
    FastCache::ShardedStorage storage { std::move(inners) };

    constexpr std::size_t Keys = 32;
    for (auto const i: std::views::iota(std::size_t { 0 }, Keys))
        REQUIRE(storage.Set(std::format("k{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    auto const tiers = storage.SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    CHECK_FALSE(At(tiers, StorageTier::Disk).has_value());
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).itemCount == Keys);
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == ShardCount * PerShardBudget);
    // Shards partition their keys, so here -- and only here -- the merged view
    // equals the tier it is made of. A LAYERED cache does not have that property,
    // because L1 mirrors L2; see `IStorage::SnapshotTiers`.
    CHECK(storage.Snapshot().itemCount == Unwrap(At(tiers, StorageTier::Memory)).itemCount);
}

TEST_CASE("A sharded layered cache reports both tiers", "[storage-tier][sharded][layered]")
{
    // The shape the node's cache tier and the daemon's `--storage` both build: a
    // lock-bearing wrapper over an in-memory tier above an on-disk one. Reading it
    // from a thread that does not own the storage is the whole reason for that
    // wrapper, and the tier split has to survive it.
    FastCache::Testing::ScratchDirectory const scratch { "storage-tier-sharded-layered" };
    std::vector<std::unique_ptr<FastCache::IStorage>> inners;
    inners.emplace_back(std::make_unique<FastCache::LayeredStorage>(std::make_unique<FastCache::InMemoryLruStorage>(2048),
                                                                    OpenCowTree(scratch)));
    FastCache::ShardedStorage storage { std::move(inners) };

    REQUIRE(storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    auto const tiers = storage.SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    REQUIRE(At(tiers, StorageTier::Disk).has_value());
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == 2048);
    CHECK(Unwrap(At(tiers, StorageTier::Disk)).itemCount == 1);
}

TEST_CASE("A decorator forwards the tiers of what it wraps", "[storage-tier]")
{
    // The failure this prevents: a decorator that inherits the interface default
    // reports the pair below it as a single memory tier, so wrapping a layered
    // cache in a tracer silently deletes its disk tier from every dashboard.
    FastCache::Testing::ScratchDirectory const scratch { "storage-tier-decorated" };
    FastCache::NullLogger logger;
    FastCache::ManualClock clock;
    FastCache::LayeredStorage layered { std::make_unique<FastCache::InMemoryLruStorage>(4096), OpenCowTree(scratch) };
    FastCache::TracingStorage traced { layered, logger, clock };

    auto const direct = layered.SnapshotTiers();
    auto const through = traced.SnapshotTiers();
    for (auto const& row: FastCache::StorageTierTable)
    {
        REQUIRE(At(direct, row.tier).has_value() == At(through, row.tier).has_value());
        if (At(direct, row.tier).has_value())
            CHECK(Unwrap(At(direct, row.tier)).bytesLimit == Unwrap(At(through, row.tier)).bytesLimit);
    }
    CHECK(At(through, StorageTier::Disk).has_value());
}
