// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using FastCache::Testing::Decode;
using FastCache::Testing::MakeBytes;
using FastCache::Testing::ValueOf;

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
    REQUIRE(Decode(got->entry.ValueBytes()) == "v");
    REQUIRE(got->entry.cas == *cas);
}

TEST_CASE("InMemoryLruStorage a held GET value survives a concurrent overwrite (copy-on-write)", "[cache]")
{
    // Models the zero-copy GET lifetime invariant: a reader holds the value's
    // reference-counted handle (as a suspended socket write would) while a
    // writer overwrites the same key. Because mutation rebinds to a fresh
    // buffer rather than editing in place, the reader's bytes must stay intact.
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;

    REQUIRE(storage.Set("k", MakeBytes("original"), 0, FastCache::TimePoint::max()).has_value());

    auto reader = storage.Get("k", clock.Now());
    REQUIRE(reader.has_value());
    REQUIRE(reader->found);
    auto const heldHandle = reader->entry.value; // refcounted handle to the original buffer
    auto const heldView = reader->entry.ValueBytes();
    REQUIRE(Decode(heldView) == "original");

    // Writer replaces the value (and a second write to force the old node out
    // of any internal reuse). The reader's handle keeps the old buffer alive.
    REQUIRE(storage.Set("k", MakeBytes("rewritten-and-longer"), 0, FastCache::TimePoint::max()).has_value());
    REQUIRE(storage.Set("k", MakeBytes("third"), 0, FastCache::TimePoint::max()).has_value());

    // The bytes the reader is still "streaming" are unchanged and valid.
    REQUIRE(Decode(heldView) == "original");
    REQUIRE(static_cast<bool>(heldHandle));
    REQUIRE(Decode(heldHandle.Bytes()) == "original");

    // A fresh GET observes the latest value.
    auto const latest = storage.Get("k", clock.Now());
    REQUIRE(latest.has_value());
    REQUIRE(Decode(latest->entry.ValueBytes()) == "third");
}

TEST_CASE("Add fails when the key already exists", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("k", MakeBytes("first"), 0, FastCache::TimePoint::max());
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
    REQUIRE(Decode(got->entry.ValueBytes()) == "two");
}

TEST_CASE("Increment treats numeric values and saturates", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("counter", MakeBytes("10"), 0, FastCache::TimePoint::max());

    auto const up = storage.IncrementOrInitialize("counter", 5, /*decrement=*/false, clock.Now());
    REQUIRE(up.has_value());
    REQUIRE(up->value == 15);

    auto const down = storage.IncrementOrInitialize("counter", 100, /*decrement=*/true, clock.Now());
    REQUIRE(down.has_value());
    REQUIRE(down->value == 0);
}

TEST_CASE("TTL expiry hides entries past their deadline", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const expiry = clock.Now() + 100ms;
    std::ignore = storage.Set("k", MakeBytes("v"), 0, expiry);

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
    std::ignore = storage.Set("a", MakeBytes("xx"), 0, FastCache::TimePoint::max());
    std::ignore = storage.Set("b", MakeBytes("yy"), 0, FastCache::TimePoint::max());
    REQUIRE(storage.Snapshot().bytesUsed == 4);

    // Inserting one more byte should evict the LRU tail ("a").
    std::ignore = storage.Set("c", MakeBytes("z"), 0, FastCache::TimePoint::max());
    auto const a = storage.Get("a", clock.Now());
    REQUIRE_FALSE(a->found);
    REQUIRE(storage.Snapshot().evictions == 1);
}

TEST_CASE("FlushWithGeneration hides existing entries immediately", "[cache]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());

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
    auto const appendCas = storage.Append("k", std::span<std::byte const> { bar.data(), bar.size() }, 0, clock.Now());
    REQUIRE(appendCas.has_value());
    REQUIRE(*appendCas != *setCas);

    auto const got = storage.Get("k", clock.Now());
    REQUIRE(Decode(got->entry.ValueBytes()) == "foobar");
}

TEST_CASE("Touch on miss returns KeyNotFound and bumps touchMisses", "[cache][touch]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const r = storage.Touch("nope", FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::KeyNotFound);
    REQUIRE(storage.Snapshot().touchMisses == 1U);
    REQUIRE(storage.Snapshot().touchHits == 0U);
    REQUIRE(storage.Snapshot().cmdTouch == 1U);
}

TEST_CASE("Touch on hit refreshes expiry without rewriting the value, bumps CAS", "[cache][touch]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const startExpiry = clock.Now() + 1s;
    auto const setCas = storage.Set("k", MakeBytes("payload"), 0xCAFE, startExpiry);
    REQUIRE(setCas.has_value());

    auto const newExpiry = clock.Now() + 60s;
    auto const touchCas = storage.Touch("k", newExpiry, clock.Now());
    REQUIRE(touchCas.has_value());
    REQUIRE(*touchCas != *setCas);

    auto const got = storage.Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.ValueBytes()) == "payload");
    REQUIRE(got->entry.flags == 0xCAFE);
    REQUIRE(got->entry.expiry == newExpiry);
    REQUIRE(got->entry.cas == *touchCas);

    auto const stats = storage.Snapshot();
    REQUIRE(stats.touchHits == 1U);
    REQUIRE(stats.touchMisses == 0U);
}

TEST_CASE("Touch on expired entry treats it as a miss", "[cache][touch]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const expiry = clock.Now() + 1s;
    std::ignore = storage.Set("k", MakeBytes("v"), 0, expiry);

    clock.Advance(2s);
    auto const r = storage.Touch("k", clock.Now() + 60s, clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::KeyNotFound);
    REQUIRE(storage.Snapshot().touchMisses == 1U);
}

TEST_CASE("Get reports the previous lastAccess and advances the stored one", "[cache][last-access]")
{
    // The returned entry carries the access *before* this read so the meta
    // `l` flag reports seconds since the prior access (never a constant 0),
    // while the stored lastAccess advances for the next reader.
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());

    clock.Advance(5s);
    auto const firstReadAt = clock.Now();
    auto const first = storage.Get("k", clock.Now());
    REQUIRE(first.has_value());
    REQUIRE(first->found);
    // Never read since insertion -> the sentinel, not `now`.
    REQUIRE(first->entry.lastAccess == FastCache::TimePoint::min());

    clock.Advance(10s);
    auto const second = storage.Get("k", clock.Now());
    REQUIRE(second.has_value());
    // Reports the first read's timestamp (10s ago), not 0.
    REQUIRE(second->entry.lastAccess == firstReadAt);
}

TEST_CASE("Delete hits and misses are counted separately", "[cache][stats]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());

    auto const hit = storage.Delete("k", clock.Now());
    REQUIRE(hit.has_value());
    auto const miss = storage.Delete("nope", clock.Now());
    REQUIRE_FALSE(miss.has_value());

    auto const stats = storage.Snapshot();
    REQUIRE(stats.deleteHits == 1U);
    REQUIRE(stats.deleteMisses == 1U);
}

TEST_CASE("Incr and decr maintain their own hit/miss counters", "[cache][stats]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("k", MakeBytes("10"), 0, FastCache::TimePoint::max());

    std::ignore = storage.IncrementOrInitialize("k", 5, /*decrement=*/false, clock.Now());
    std::ignore = storage.IncrementOrInitialize("nope", 5, /*decrement=*/false, clock.Now());
    std::ignore = storage.IncrementOrInitialize("k", 3, /*decrement=*/true, clock.Now());
    std::ignore = storage.IncrementOrInitialize("nope", 3, /*decrement=*/true, clock.Now());

    auto const stats = storage.Snapshot();
    REQUIRE(stats.incrHits == 1U);
    REQUIRE(stats.incrMisses == 1U);
    REQUIRE(stats.decrHits == 1U);
    REQUIRE(stats.decrMisses == 1U);
}

TEST_CASE("CAS hits, misses, and badval are counted distinctly", "[cache][stats]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const setCas = storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());

    auto const badval = storage.CompareAndSwap("k", 9999, MakeBytes("x"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(badval.has_value());
    auto const hit = storage.CompareAndSwap("k", *setCas, MakeBytes("y"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE(hit.has_value());
    auto const miss = storage.CompareAndSwap("nope", 1, MakeBytes("z"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(miss.has_value());

    auto const stats = storage.Snapshot();
    REQUIRE(stats.casHits == 1U);
    REQUIRE(stats.casMisses == 1U);
    REQUIRE(stats.casBadval == 1U);
}

TEST_CASE("InMemoryLruStorage counts evicted_unfetched only for never-read victims", "[cache][stats][unfetched]")
{
    // Budget fits exactly one 100-byte value, so each insert evicts the
    // prior one.
    FastCache::InMemoryLruStorage storage { 150 };
    FastCache::ManualClock clock;
    auto const big = MakeBytes(std::string(100, 'x'));

    REQUIRE(storage.Set("k1", big, 0, FastCache::TimePoint::max()).has_value());
    REQUIRE(storage.Set("k2", big, 0, FastCache::TimePoint::max()).has_value()); // evicts unread k1
    REQUIRE(storage.Get("k2", clock.Now())->found);                              // k2 is now fetched
    REQUIRE(storage.Set("k3", big, 0, FastCache::TimePoint::max()).has_value()); // evicts fetched k2

    auto const stats = storage.Snapshot();
    REQUIRE(stats.evictions == 2U);
    REQUIRE(stats.evictedUnfetched == 1U); // only k1 was discarded before any read
}

TEST_CASE("InMemoryLruStorage counts expired_unfetched only for never-read victims", "[cache][stats][unfetched]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const expiry = clock.Now() + 1s;
    REQUIRE(storage.Set("k1", MakeBytes("a"), 0, expiry).has_value());
    REQUIRE(storage.Set("k2", MakeBytes("b"), 0, expiry).has_value());
    REQUIRE(storage.Get("k1", clock.Now())->found); // k1 fetched before it expires

    clock.Advance(2s);
    auto const purged = storage.PurgeExpired(clock.Now());
    REQUIRE(purged == 2U);

    auto const stats = storage.Snapshot();
    REQUIRE(stats.expiredUnfetched == 1U); // only the unread k2
}

TEST_CASE("InMemoryLruStorage Peek is non-mutating", "[cache][peek]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    REQUIRE(storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    auto const peeked = storage.Peek("k", clock.Now());
    REQUIRE(peeked.has_value());
    REQUIRE(peeked->found);
    REQUIRE(Decode(peeked->entry.ValueBytes()) == "v");

    // Peek must not register a get hit, and must not mark the entry fetched
    // (so a later eviction still counts it unfetched).
    auto const stats = storage.Snapshot();
    REQUIRE(stats.cmdGet == 0U);
    REQUIRE(stats.getHits == 0U);

    // A miss / expired key reads as not-found without erasing.
    REQUIRE_FALSE(storage.Peek("absent", clock.Now())->found);
}

TEST_CASE("InMemoryLruStorage MarkStale flags the entry and optionally refreshes TTL", "[cache][stale]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const setCas = storage.Set("k", MakeBytes("v"), 0, clock.Now() + 5s);
    REQUIRE(setCas.has_value());

    auto const staled = storage.MarkStale("k", std::nullopt, clock.Now());
    REQUIRE(staled.has_value());
    REQUIRE(*staled != *setCas); // CAS bumped

    auto const got = storage.Get("k", clock.Now());
    REQUIRE(got->found);
    REQUIRE(got->entry.stale);

    // Missing key is a miss.
    REQUIRE_FALSE(storage.MarkStale("absent", std::nullopt, clock.Now()).has_value());

    // A value-rewriting Set clears the stale flag again.
    REQUIRE(storage.Set("k", MakeBytes("w"), 0, FastCache::TimePoint::max()).has_value());
    REQUIRE_FALSE(storage.Get("k", clock.Now())->entry.stale);
}

TEST_CASE("Increment by a delta >= 2^63 adds rather than aliasing to a decrement", "[cache][incr][overflow]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("c", MakeBytes("0"), 0, FastCache::TimePoint::max());

    constexpr std::uint64_t Huge = 1ULL << 63; // 9223372036854775808
    auto const r = storage.IncrementOrInitialize("c", Huge, /*decrement=*/false, clock.Now());
    REQUIRE(r.has_value());
    REQUIRE(r->value == Huge); // 0 + 2^63, NOT a saturating decrement to 0
}

TEST_CASE("Decrement by 2^63 saturates to zero without signed-overflow UB", "[cache][decr][overflow]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("c", MakeBytes("5"), 0, FastCache::TimePoint::max());

    constexpr std::uint64_t Huge = 1ULL << 63;
    auto const r = storage.IncrementOrInitialize("c", Huge, /*decrement=*/true, clock.Now());
    REQUIRE(r.has_value());
    REQUIRE(r->value == 0);
}

TEST_CASE("Set/Add/Replace/CAS at the value cap roundtrip; one byte over returns ValueTooLarge",
          "[cache][max-value][boundary]")
{
    // maxBytes == 0 -> no eviction so the byte budget never masks the
    // per-value check; maxValueBytes == 16 is the per-value cap.
    FastCache::InMemoryLruStorage storage { 0, 16 };
    FastCache::ManualClock clock;

    auto const fits = MakeBytes(std::string(16, 'x'));
    auto const oversized = MakeBytes(std::string(17, 'x'));

    REQUIRE(storage.Set("k", fits, 0, FastCache::TimePoint::max()).has_value());

    auto const setOver = storage.Set("over", oversized, 0, FastCache::TimePoint::max());
    REQUIRE_FALSE(setOver.has_value());
    REQUIRE(setOver.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    auto const addOver = storage.Add("over", oversized, 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(addOver.has_value());
    REQUIRE(addOver.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    auto const replaceOver = storage.Replace("k", oversized, 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(replaceOver.has_value());
    REQUIRE(replaceOver.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    auto const setCas = storage.Set("k", fits, 0, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());
    auto const casOver = storage.CompareAndSwap("k", *setCas, oversized, 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(casOver.has_value());
    REQUIRE(casOver.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    // The oversized rejections never mutated the stored value.
    auto const got = storage.Get("k", clock.Now());
    REQUIRE(got->found);
    REQUIRE(ValueOf(got->entry) == fits);
}

TEST_CASE("Append/Prepend that push the combined value over the cap return ValueTooLarge", "[cache][max-value][boundary]")
{
    FastCache::InMemoryLruStorage storage { 0, 8 };
    FastCache::ManualClock clock;
    REQUIRE(storage.Set("k", MakeBytes("12345"), 0, FastCache::TimePoint::max()).has_value()); // 5 bytes, fits

    auto const suffix = MakeBytes("six!"); // 5 + 4 = 9 > 8
    auto const appended = storage.Append("k", std::span<std::byte const> { suffix.data(), suffix.size() }, 0, clock.Now());
    REQUIRE_FALSE(appended.has_value());
    REQUIRE(appended.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    auto const prefix = MakeBytes("pre!"); // 5 + 4 = 9 > 8
    auto const prepended = storage.Prepend("k", std::span<std::byte const> { prefix.data(), prefix.size() }, 0, clock.Now());
    REQUIRE_FALSE(prepended.has_value());
    REQUIRE(prepended.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    // A growth that still fits the cap succeeds (5 + 3 = 8 == cap).
    auto const fitting = MakeBytes("678");
    REQUIRE(storage.Append("k", std::span<std::byte const> { fitting.data(), fitting.size() }, 0, clock.Now()).has_value());
    REQUIRE(Decode(storage.Get("k", clock.Now())->entry.ValueBytes()) == "12345678");
}

TEST_CASE("maxValueBytes == 0 disables the per-value cap entirely", "[cache][max-value]")
{
    FastCache::InMemoryLruStorage storage { 0, 0 };        // no budget cap, no value cap
    auto const big = MakeBytes(std::string(1 << 20, 'x')); // 1 MiB
    REQUIRE(storage.Set("k", big, 0, FastCache::TimePoint::max()).has_value());
}

TEST_CASE("Decrement by zero is booked under decr stats, not incr", "[cache][decr][stats]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("c", MakeBytes("7"), 0, FastCache::TimePoint::max());

    auto const r = storage.IncrementOrInitialize("c", 0, /*decrement=*/true, clock.Now());
    REQUIRE(r.has_value());
    REQUIRE(r->value == 7);

    auto const stats = storage.Snapshot();
    REQUIRE(stats.decrHits == 1U);
    REQUIRE(stats.incrHits == 0U);
}

TEST_CASE("Increment on a non-numeric value errors without booking a hit", "[cache][incr][stats]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    std::ignore = storage.Set("k", MakeBytes("abc"), 0, FastCache::TimePoint::max());

    auto const r = storage.IncrementOrInitialize("k", 1, /*decrement=*/false, clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::InvalidArgument);

    // The failed incr must not be booked as a hit (it performed no increment) —
    // matching CowTreeStorage, which only counts the hit after a successful Set.
    auto const stats = storage.Snapshot();
    REQUIRE(stats.incrHits == 0U);
    REQUIRE(stats.incrMisses == 0U);
}

TEST_CASE("Append honours an optional CAS precondition", "[cache][append][cas]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const setCas = storage.Set("k", MakeBytes("foo"), 0, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());

    auto const bar = MakeBytes("bar");
    auto const span = std::span<std::byte const> { bar.data(), bar.size() };

    // Wrong CAS -> mismatch, value untouched.
    auto const wrong = storage.Append("k", span, *setCas + 999, clock.Now());
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().code == FastCache::StorageErrorCode::CasMismatch);
    REQUIRE(Decode(storage.Get("k", clock.Now())->entry.ValueBytes()) == "foo");

    // Right CAS -> appends.
    auto const right = storage.Append("k", span, *setCas, clock.Now());
    REQUIRE(right.has_value());
    REQUIRE(Decode(storage.Get("k", clock.Now())->entry.ValueBytes()) == "foobar");

    // expected == 0 means unconditional.
    auto const baz = MakeBytes("baz");
    REQUIRE(storage.Append("k", std::span<std::byte const> { baz.data(), baz.size() }, 0, clock.Now()).has_value());
    REQUIRE(Decode(storage.Get("k", clock.Now())->entry.ValueBytes()) == "foobarbaz");
}

TEST_CASE("Prepend honours an optional CAS precondition", "[cache][prepend][cas]")
{
    FastCache::InMemoryLruStorage storage;
    FastCache::ManualClock clock;
    auto const setCas = storage.Set("k", MakeBytes("bar"), 0, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());

    auto const foo = MakeBytes("foo");
    auto const span = std::span<std::byte const> { foo.data(), foo.size() };

    // Wrong CAS -> mismatch, value untouched.
    auto const wrong = storage.Prepend("k", span, *setCas + 7, clock.Now());
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().code == FastCache::StorageErrorCode::CasMismatch);
    REQUIRE(Decode(storage.Get("k", clock.Now())->entry.ValueBytes()) == "bar");

    // Right CAS -> prepends.
    auto const right = storage.Prepend("k", span, *setCas, clock.Now());
    REQUIRE(right.has_value());
    REQUIRE(Decode(storage.Get("k", clock.Now())->entry.ValueBytes()) == "foobar");
}
