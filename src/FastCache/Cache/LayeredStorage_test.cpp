// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace
{

std::vector<std::byte> MakeBytes(std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (auto const c: text)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

std::string Decode(std::span<std::byte const> bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (auto const b: bytes)
        out.push_back(static_cast<char>(b));
    return out;
}

/// Build a `LayeredStorage(InMemoryLruStorage, InMemoryLruStorage)`. L1
/// has a configurable budget so tests can force eviction; L2 is
/// unbounded so it acts as the canonical store.
std::unique_ptr<FastCache::LayeredStorage> MakeLayered(std::size_t l1Budget = 0)
{
    auto l1 = std::make_unique<FastCache::InMemoryLruStorage>(l1Budget);
    auto l2 = std::make_unique<FastCache::InMemoryLruStorage>(0);
    return std::make_unique<FastCache::LayeredStorage>(std::move(l1), std::move(l2));
}

/// RAII helper for tests that use a real on-disk CowTreeStorage as L2.
struct TempFile
{
    std::filesystem::path path;
    TempFile()
    {
        std::mt19937_64 rng { std::random_device {}() };
        path = std::filesystem::temp_directory_path() / ("layered-test-" + std::to_string(rng()) + ".cow");
        std::filesystem::remove(path);
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempFile(TempFile const&) = delete;
    TempFile& operator=(TempFile const&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
};

} // namespace

TEST_CASE("LayeredStorage Set + Get write-through roundtrip", "[layered]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;

    auto const cas = storage->Set("k", MakeBytes("v"), 42, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());

    auto got = storage->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "v");
    REQUIRE(got->entry.flags == 42u);
    REQUIRE(got->entry.cas == *cas);
}

TEST_CASE("LayeredStorage write-through populates BOTH tiers", "[layered][write-through]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;
    auto const cas = storage->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());

    // Inspect L1 directly: entry must be present with L2's CAS.
    auto l1Got = storage->L1().Get("k", clock.Now());
    REQUIRE(l1Got.has_value());
    REQUIRE(l1Got->found);
    REQUIRE(l1Got->entry.cas == *cas);

    // Inspect L2 directly: same CAS.
    auto l2Got = storage->L2().Get("k", clock.Now());
    REQUIRE(l2Got.has_value());
    REQUIRE(l2Got->found);
    REQUIRE(l2Got->entry.cas == *cas);
}

TEST_CASE("LayeredStorage read-through populates L1 on an L1 miss", "[layered][read-through]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;

    // Inject an entry directly into L2 without touching L1.
    auto const directCas = storage->L2().Set("k", MakeBytes("disk-only"), 7, FastCache::TimePoint::max());
    REQUIRE(directCas.has_value());

    // First Get: L1 misses, L2 hits, mirror to L1.
    auto first = storage->Get("k", clock.Now());
    REQUIRE(first.has_value());
    REQUIRE(first->found);
    REQUIRE(first->entry.cas == *directCas);

    // L1 should now have the entry with the same CAS as L2.
    auto l1Direct = storage->L1().Get("k", clock.Now());
    REQUIRE(l1Direct.has_value());
    REQUIRE(l1Direct->found);
    REQUIRE(l1Direct->entry.cas == *directCas);
}

TEST_CASE("LayeredStorage CompareAndSwap survives L1 eviction (CAS coherency)", "[layered][cas]")
{
    // L1 budget is 8 bytes — small enough to force eviction after the
    // first Set. We then verify that a subsequent CAS against the cas
    // observed earlier still succeeds; this proves the read-through
    // path mirrors L2's canonical CAS verbatim into L1.
    auto storage = MakeLayered(8);
    FastCache::ManualClock clock;

    auto const cas = storage->Set("key", MakeBytes("12345678"), 0, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());

    // Insert other entries to evict "key" from L1.
    for (int i = 0; i < 10; ++i)
        REQUIRE(
            storage->Set(std::format("filler-{}", i), MakeBytes("XXXXXXXX"), 0, FastCache::TimePoint::max()).has_value());

    // L1 should no longer hold "key" (the small budget evicted it).
    auto l1Probe = storage->L1().Get("key", clock.Now());
    REQUIRE(l1Probe.has_value());
    REQUIRE_FALSE(l1Probe->found);

    // A Get through the layered storage repopulates L1 with L2's CAS;
    // a CAS with the original cas value still succeeds.
    auto refetched = storage->Get("key", clock.Now());
    REQUIRE(refetched.has_value());
    REQUIRE(refetched->found);
    REQUIRE(refetched->entry.cas == *cas);

    auto casResult = storage->CompareAndSwap("key", *cas, MakeBytes("post"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE(casResult.has_value());
}

TEST_CASE("LayeredStorage Delete drops from both tiers", "[layered][delete]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;
    REQUIRE(storage->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    REQUIRE(storage->Delete("k", clock.Now()).has_value());

    auto l1 = storage->L1().Get("k", clock.Now());
    REQUIRE(l1.has_value());
    REQUIRE_FALSE(l1->found);

    auto l2 = storage->L2().Get("k", clock.Now());
    REQUIRE(l2.has_value());
    REQUIRE_FALSE(l2->found);
}

TEST_CASE("LayeredStorage Append routes through L2 and mirrors to L1", "[layered][compound]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;
    REQUIRE(storage->Set("k", MakeBytes("hello"), 0, FastCache::TimePoint::max()).has_value());

    auto const suffix = MakeBytes(" world");
    auto const cas = storage->Append("k", std::span<std::byte const> { suffix.data(), suffix.size() }, clock.Now());
    REQUIRE(cas.has_value());

    // Both tiers must reflect the appended value with the new CAS.
    auto l1 = storage->L1().Get("k", clock.Now());
    REQUIRE(l1.has_value());
    REQUIRE(l1->found);
    REQUIRE(Decode(l1->entry.value) == "hello world");
    REQUIRE(l1->entry.cas == *cas);

    auto l2 = storage->L2().Get("k", clock.Now());
    REQUIRE(l2.has_value());
    REQUIRE(l2->found);
    REQUIRE(Decode(l2->entry.value) == "hello world");
    REQUIRE(l2->entry.cas == *cas);
}

TEST_CASE("LayeredStorage IncrementOrInitialize routes through L2", "[layered][compound]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;
    // InMemoryLruStorage::IncrementOrInitialize requires the key to
    // exist (returns KeyNotFound otherwise) — seed an explicit "10"
    // so this test does not depend on the more permissive
    // CowTreeStorage initialise-on-missing behaviour.
    REQUIRE(storage->Set("counter", MakeBytes("10"), 0, FastCache::TimePoint::max()).has_value());
    auto next = storage->IncrementOrInitialize("counter", 5, clock.Now());
    REQUIRE(next.has_value());
    REQUIRE(next->value == 15u);

    // Verify L2 sees "15".
    auto l2 = storage->L2().Get("counter", clock.Now());
    REQUIRE(l2.has_value());
    REQUIRE(l2->found);
    REQUIRE(Decode(l2->entry.value) == "15");
}

TEST_CASE("LayeredStorage FlushWithGeneration hides entries in both tiers", "[layered][flush]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;
    REQUIRE(storage->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    storage->FlushWithGeneration(clock.Now());

    auto via = storage->Get("k", clock.Now());
    REQUIRE(via.has_value());
    REQUIRE_FALSE(via->found);

    // Both tiers individually treat the entry as flushed.
    auto l1 = storage->L1().Get("k", clock.Now());
    REQUIRE(l1.has_value());
    REQUIRE_FALSE(l1->found);
    auto l2 = storage->L2().Get("k", clock.Now());
    REQUIRE(l2.has_value());
    REQUIRE_FALSE(l2->found);
}

TEST_CASE("LayeredStorage PurgeExpired returns the L2 (canonical) count", "[layered][purge]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;
    auto const shortExpiry = clock.Now() + 1ms;

    for (int i = 0; i < 5; ++i)
        REQUIRE(storage->Set(std::format("expire-{}", i), MakeBytes("v"), 0, shortExpiry).has_value());
    for (int i = 0; i < 3; ++i)
        REQUIRE(storage->Set(std::format("keep-{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    clock.Advance(10ms);
    auto const purged = storage->PurgeExpired(clock.Now());
    REQUIRE(purged == 5u);

    // Surviving entries still reachable.
    for (int i = 0; i < 3; ++i)
    {
        auto got = storage->Get(std::format("keep-{}", i), clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
    }
}

TEST_CASE("L1 eviction never loses data; L2 still serves every key", "[layered][eviction]")
{
    auto storage = MakeLayered(64); // tiny L1 budget so only a few entries fit
    FastCache::ManualClock clock;

    constexpr int N = 50;
    for (int i = 0; i < N; ++i)
    {
        auto const key = std::format("k-{:02d}", i);
        REQUIRE(storage->Set(key, MakeBytes(std::format("payload-{:02d}", i)), 0, FastCache::TimePoint::max()).has_value());
    }

    // Every key must still be retrievable (L1 evicted older entries
    // but L2 holds them all).
    for (int i = 0; i < N; ++i)
    {
        auto got = storage->Get(std::format("k-{:02d}", i), clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == std::format("payload-{:02d}", i));
    }
}

TEST_CASE("LayeredStorage Snapshot tracks LayeredStorage-level stats", "[layered][stats]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;

    REQUIRE(storage->Set("a", MakeBytes("1"), 0, FastCache::TimePoint::max()).has_value());
    REQUIRE(storage->Set("b", MakeBytes("2"), 0, FastCache::TimePoint::max()).has_value());

    static_cast<void>(storage->Get("a", clock.Now()));
    static_cast<void>(storage->Get("missing", clock.Now()));

    auto const stats = storage->Snapshot();
    REQUIRE(stats.cmdSet == 2u);
    REQUIRE(stats.cmdGet == 2u);
    REQUIRE(stats.getHits == 1u);
    REQUIRE(stats.getMisses == 1u);
    REQUIRE(stats.itemCount == 2u); // canonical state from L2
}

TEST_CASE("Sharded composition: ShardedStorage of LayeredStorage(InMem, CowTree) end-to-end",
          "[layered][persist][integration]")
{
    // Build the production-mode shape: outer ShardedStorage of N
    // shards, each shard is a LayeredStorage(InMemoryLruStorage,
    // CowTreeStorage). Set entries, drop the structure, rebuild it
    // against the same on-disk files, Gets through the rebuilt stack
    // must serve the canonical disk values (L1 is empty after rebuild).
    constexpr std::size_t kShards = 4;
    std::mt19937_64 rng { std::random_device {}() };
    auto const stem = std::format("layered-integration-{}", rng());

    std::vector<std::filesystem::path> paths;
    paths.reserve(kShards);
    for (std::size_t i = 0; i < kShards; ++i)
    {
        auto path = std::filesystem::temp_directory_path() / std::format("{}-shard-{:02d}.cow", stem, i);
        std::filesystem::remove(path);
        paths.push_back(std::move(path));
    }

    auto build = [&] {
        std::vector<std::unique_ptr<FastCache::IStorage>> shards;
        shards.reserve(kShards);
        for (std::size_t i = 0; i < kShards; ++i)
        {
            auto l1 = std::make_unique<FastCache::InMemoryLruStorage>(0);
            FastCache::CowTreeStorage::Options opts;
            opts.path = paths[i];
            auto l2 = FastCache::CowTreeStorage::Open(opts);
            REQUIRE(l2.has_value());
            auto layered = std::make_unique<FastCache::LayeredStorage>(std::move(l1), std::move(*l2));
            shards.push_back(std::move(layered));
        }
        return std::make_unique<FastCache::ShardedStorage>(std::move(shards));
    };

    // Phase 1: write through the layered+sharded stack.
    {
        auto storage = build();
        FastCache::ManualClock clock;
        for (int i = 0; i < 100; ++i)
        {
            REQUIRE(
                storage->Set(std::format("key-{}", i), MakeBytes(std::format("value-{}", i)), 0, FastCache::TimePoint::max())
                    .has_value());
        }
    }

    // Phase 2: rebuild against the same files; L1 starts empty.
    {
        auto storage = build();
        FastCache::ManualClock clock;
        for (int i = 0; i < 100; ++i)
        {
            auto got = storage->Get(std::format("key-{}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(Decode(got->entry.value) == std::format("value-{}", i));
        }
    }

    // Clean up.
    for (auto const& path: paths)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

TEST_CASE("LayeredStorage::Resize tunes only the L1 budget", "[layered][resize]")
{
    auto storage = MakeLayered(1024);

    constexpr int N = 50;
    for (int i = 0; i < N; ++i)
    {
        REQUIRE(storage->Set(std::format("k-{:02d}", i), MakeBytes("XXXXXXXX"), 0, FastCache::TimePoint::max()).has_value());
    }
    auto const before = storage->L1().Snapshot();
    REQUIRE(before.bytesUsed <= 1024u);

    storage->Resize(64);
    auto const after = storage->L1().Snapshot();
    REQUIRE(after.bytesUsed <= 64u);
    REQUIRE(after.bytesLimit == 64u);

    // L2 still holds every entry — Resize only shrinks L1.
    FastCache::ManualClock clock;
    for (int i = 0; i < N; ++i)
    {
        auto got = storage->Get(std::format("k-{:02d}", i), clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
    }
}

TEST_CASE("LayeredStorage Add fails when present in L2 (canonical), even if L1 evicted", "[layered][add]")
{
    auto storage = MakeLayered(8); // tiny L1
    FastCache::ManualClock clock;

    REQUIRE(storage->Set("k", MakeBytes("first"), 0, FastCache::TimePoint::max()).has_value());

    // Push out L1 with other writes.
    for (int i = 0; i < 5; ++i)
        REQUIRE(storage->Set(std::format("filler-{}", i), MakeBytes("XXXX"), 0, FastCache::TimePoint::max()).has_value());

    // L1 may have evicted "k", but L2 still has it — Add must fail.
    auto r = storage->Add("k", MakeBytes("second"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::KeyExists);
}

TEST_CASE("LayeredStorage Replace fails when absent from both tiers", "[layered][replace]")
{
    auto storage = MakeLayered();
    FastCache::ManualClock clock;
    auto r = storage->Replace("missing", MakeBytes("nope"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::KeyNotFound);
}
