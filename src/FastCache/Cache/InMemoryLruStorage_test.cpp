// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <span>
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

} // namespace

TEST_CASE("InMemoryLruStorage Get miss returns found=false", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const result = storage.Get("missing", clock.Now());
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->found);
}

TEST_CASE("InMemoryLruStorage Set + Get round-trips", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;

    auto const cas = storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());

    auto const got = storage.Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "v");
    REQUIRE(got->entry.cas == *cas);
}

TEST_CASE("Add fails when the key already exists", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    (void) storage.Set("k", MakeBytes("first"), 0, FastCache::TimePoint::max());
    auto const result = storage.Add("k", MakeBytes("second"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::StorageErrorCode::KeyExists);
}

TEST_CASE("Replace fails when the key is absent", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const result = storage.Replace("k", MakeBytes("nope"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::StorageErrorCode::KeyNotFound);
}

TEST_CASE("CompareAndSwap matches the CAS token", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const setCas = storage.Set("k", MakeBytes("one"), 0, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());

    auto const wrongResult =
        storage.CompareAndSwap("k", 9999, MakeBytes("two"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(wrongResult.has_value());
    REQUIRE(wrongResult.error().code == FastCache::StorageErrorCode::CasMismatch);

    auto const rightResult =
        storage.CompareAndSwap("k", *setCas, MakeBytes("two"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE(rightResult.has_value());

    auto const got = storage.Get("k", clock.Now());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "two");
}

TEST_CASE("Increment treats numeric values and saturates", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    (void) storage.Set("counter", MakeBytes("10"), 0, FastCache::TimePoint::max());

    auto const up = storage.IncrementOrInitialize("counter", 5, clock.Now());
    REQUIRE(up.has_value());
    REQUIRE(up->value == 15);

    auto const down = storage.IncrementOrInitialize("counter", -100, clock.Now());
    REQUIRE(down.has_value());
    REQUIRE(down->value == 0);
}

TEST_CASE("TTL expiry hides entries past their deadline", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const expiry = clock.Now() + 100ms;
    (void) storage.Set("k", MakeBytes("v"), 0, expiry);

    auto const before = storage.Get("k", clock.Now());
    REQUIRE(before->found);

    clock.Advance(200ms);
    auto const after = storage.Get("k", clock.Now());
    REQUIRE_FALSE(after->found);
}

TEST_CASE("LRU eviction kicks in when byte budget exceeded", "[cache]")
{
    FastCache::InMemoryLruStorage storage { 4 }; // 4 bytes total
    FastCache::ManualClock clock;
    (void) storage.Set("a", MakeBytes("xx"), 0, FastCache::TimePoint::max());
    (void) storage.Set("b", MakeBytes("yy"), 0, FastCache::TimePoint::max());
    REQUIRE(storage.Snapshot().bytesUsed == 4);

    // Inserting one more byte should evict the LRU tail ("a").
    (void) storage.Set("c", MakeBytes("z"), 0, FastCache::TimePoint::max());
    auto const a = storage.Get("a", clock.Now());
    REQUIRE_FALSE(a->found);
    REQUIRE(storage.Snapshot().evictions == 1);
}

TEST_CASE("FlushWithGeneration hides existing entries immediately", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    (void) storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());

    storage.FlushWithGeneration(clock.Now());
    auto const got = storage.Get("k", clock.Now());
    REQUIRE_FALSE(got->found);
}

TEST_CASE("Append concatenates and bumps CAS", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const setCas = storage.Set("k", MakeBytes("foo"), 0, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());

    auto const bar = MakeBytes("bar");
    auto const appendCas = storage.Append("k", std::span<std::byte const> { bar.data(), bar.size() }, clock.Now());
    REQUIRE(appendCas.has_value());
    REQUIRE(*appendCas != *setCas);

    auto const got = storage.Get("k", clock.Now());
    REQUIRE(Decode(got->entry.value) == "foobar");
}
