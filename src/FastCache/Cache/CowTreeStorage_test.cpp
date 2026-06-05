// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <CowTree/InMemoryPageStore.hpp>

using namespace std::chrono_literals;
using FastCache::Testing::Decode;
using FastCache::Testing::MakeBytes;
using FastCache::Testing::TempFile;

namespace
{

/// Generate `size` deterministic-pseudo-random bytes from `seed`. Used
/// by the binary-blob roundtrip tests so they can compare bytewise
/// after a reopen without baking the values into the test source.
std::vector<std::byte> RandomBytes(std::size_t size, std::uint64_t seed)
{
    std::mt19937_64 rng { seed };
    std::vector<std::byte> out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
        out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(rng() & 0xFFU)));
    return out;
}

/// Open + drop helper: returns the result of a callable that takes a
/// CowTreeStorage&, having opened the storage at `path` first and closed
/// it on return. Lets tests express roundtrip-across-reopen as two
/// successive calls.
template <class F>
auto WithOpenStorage(std::filesystem::path const& path, F&& fn)
{
    FastCache::CowTreeStorage::Options opts;
    opts.path = path;
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    return std::forward<F>(fn)(**storage);
}

} // namespace

// ============================================================================
// Single-session roundtrip
// ============================================================================

TEST_CASE("CowTreeStorage Set + Get round-trips", "[cowstorage]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());

    FastCache::ManualClock clock;
    auto const cas = (*storage)->Set("k", MakeBytes("hello"), 7, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());

    auto got = (*storage)->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "hello");
    REQUIRE(got->entry.flags == 7U);
    REQUIRE(got->entry.cas == *cas);
}

TEST_CASE("CowTreeStorage entries survive close + reopen", "[cowstorage][persist]")
{
    TempFile tmp;

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("k", MakeBytes("persisted"), 0, FastCache::TimePoint::max()).has_value());
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "persisted");
    });
}

// ============================================================================
// Byte-level roundtrip
// ============================================================================

TEST_CASE("Empty value roundtrips, including across reopen", "[cowstorage][roundtrip]")
{
    TempFile tmp;
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("empty", {}, 0, FastCache::TimePoint::max()).has_value());
        auto got = storage.Get("empty", FastCache::ManualClock {}.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(got->entry.value.empty());
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("empty", FastCache::ManualClock {}.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(got->entry.value.empty());
    });
}

TEST_CASE("Every single byte 0x00..0xFF roundtrips across reopen", "[cowstorage][roundtrip]")
{
    TempFile tmp;
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        for (int b = 0; b < 256; ++b)
        {
            auto const key = std::format("k-{:02x}", b);
            std::vector<std::byte> value { static_cast<std::byte>(static_cast<std::uint8_t>(b)) };
            // Evaluate Set() outside REQUIRE: the macro expands its argument
            // twice (eval + stringify), which trips bugprone-use-after-move on
            // the std::move(value).
            auto const stored = storage.Set(key, std::move(value), 0, FastCache::TimePoint::max());
            REQUIRE(stored.has_value());
        }
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        for (int b = 0; b < 256; ++b)
        {
            auto const key = std::format("k-{:02x}", b);
            auto got = storage.Get(key, clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(got->entry.value.size() == 1U);
            REQUIRE(static_cast<std::uint8_t>(got->entry.value[0]) == static_cast<std::uint8_t>(b));
        }
    });
}

TEST_CASE("All-byte-values blob roundtrips across reopen", "[cowstorage][roundtrip]")
{
    TempFile tmp;
    std::vector<std::byte> blob;
    blob.reserve(256);
    for (int i = 0; i < 256; ++i)
        blob.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(i)));

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("blob", blob, 0, FastCache::TimePoint::max()).has_value());
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("blob", FastCache::ManualClock {}.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(got->entry.value == blob);
    });
}

TEST_CASE("1 KiB / 64 KiB random binary roundtrips across reopen", "[cowstorage][roundtrip]")
{
    TempFile tmp;
    auto const small = RandomBytes(1024, 0xAAAA1111ULL);
    auto const medium = RandomBytes(64 * 1024, 0xBBBB2222ULL);

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("small", small, 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Set("medium", medium, 0, FastCache::TimePoint::max()).has_value());
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        auto a = storage.Get("small", clock.Now());
        REQUIRE(a.has_value());
        REQUIRE(a->found);
        REQUIRE(a->entry.value == small);

        auto b = storage.Get("medium", clock.Now());
        REQUIRE(b.has_value());
        REQUIRE(b->found);
        REQUIRE(b->entry.value == medium);
    });
}

TEST_CASE("Value exactly at maxValueBytes roundtrips; one over returns ValueTooLarge", "[cowstorage][roundtrip][boundary]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;
    opts.maxValueBytes = 4096;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());

    auto const fits = RandomBytes(4096, 0xCCCC3333ULL);
    auto const oversized = RandomBytes(4097, 0xDDDD4444ULL);

    REQUIRE((*storage)->Set("fits", fits, 0, FastCache::TimePoint::max()).has_value());
    auto const oversizedResult = (*storage)->Set("oversized", oversized, 0, FastCache::TimePoint::max());
    REQUIRE_FALSE(oversizedResult.has_value());
    REQUIRE(oversizedResult.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    auto got = (*storage)->Get("fits", FastCache::ManualClock {}.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(got->entry.value == fits);
}

TEST_CASE("Keys with embedded NULs and non-ASCII bytes roundtrip", "[cowstorage][roundtrip]")
{
    TempFile tmp;
    std::string const keyA { 'a', '\0', 'b' };
    std::string const keyB { '\xC3', '\xA9' };
    std::string const keyC { '\x00', '\x01', '\xFF' };

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set(keyA, MakeBytes("A"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Set(keyB, MakeBytes("B"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Set(keyC, MakeBytes("C"), 0, FastCache::TimePoint::max()).has_value());
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        auto a = storage.Get(keyA, clock.Now());
        REQUIRE(a.has_value());
        REQUIRE(a->found);
        REQUIRE(Decode(a->entry.value) == "A");

        auto b = storage.Get(keyB, clock.Now());
        REQUIRE(b.has_value());
        REQUIRE(b->found);
        REQUIRE(Decode(b->entry.value) == "B");

        auto c = storage.Get(keyC, clock.Now());
        REQUIRE(c.has_value());
        REQUIRE(c->found);
        REQUIRE(Decode(c->entry.value) == "C");
    });
}

// ============================================================================
// Metadata roundtrip
// ============================================================================

TEST_CASE("Flags roundtrip across reopen", "[cowstorage][roundtrip][metadata]")
{
    TempFile tmp;
    constexpr std::uint32_t kFlags[] { 0U, 1U, 0xDEADBEEFU, std::numeric_limits<std::uint32_t>::max() };

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        for (std::size_t i = 0; i < std::size(kFlags); ++i)
        {
            auto const key = std::format("flag-{}", i);
            REQUIRE(storage.Set(key, MakeBytes("v"), kFlags[i], FastCache::TimePoint::max()).has_value());
        }
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        for (std::size_t i = 0; i < std::size(kFlags); ++i)
        {
            auto got = storage.Get(std::format("flag-{}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(got->entry.flags == kFlags[i]);
        }
    });
}

TEST_CASE("Expiry roundtrip across reopen", "[cowstorage][roundtrip][metadata]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    auto const farFuture = clock.Now() + 24h;

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("forever", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Set("future", MakeBytes("v"), 0, farFuture).has_value());
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto forever = storage.Get("forever", clock.Now());
        REQUIRE(forever.has_value());
        REQUIRE(forever->found);
        REQUIRE(forever->entry.expiry == FastCache::TimePoint::max());

        auto future = storage.Get("future", clock.Now());
        REQUIRE(future.has_value());
        REQUIRE(future->found);
        REQUIRE(future->entry.expiry == farFuture);
    });
}

TEST_CASE("CAS tokens are strictly monotonic within a session", "[cowstorage][cas]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());

    FastCache::CasToken last { 0 };
    for (int i = 0; i < 10; ++i)
    {
        auto const cas = (*storage)->Set(std::format("k-{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max());
        REQUIRE(cas.has_value());
        REQUIRE(*cas > last);
        last = *cas;
    }
}

// ============================================================================
// B+tree shape coverage
// ============================================================================

TEST_CASE("Many small entries fit and read back across reopen", "[cowstorage][shape]")
{
    constexpr int N = 1000;
    TempFile tmp;
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        for (int i = 0; i < N; ++i)
        {
            auto const key = std::format("key-{:05d}", i);
            auto const value = std::format("value-{:05d}", i);
            REQUIRE(storage.Set(key, MakeBytes(value), 0, FastCache::TimePoint::max()).has_value());
        }
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        // Shuffle the iteration order so the test exercises non-trivial tree paths.
        std::vector<int> order(N);
        std::ranges::iota(order, 0);
        std::mt19937_64 rng { 0x12345678ULL };
        std::ranges::shuffle(order, rng);

        for (int const i: order)
        {
            auto got = storage.Get(std::format("key-{:05d}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(Decode(got->entry.value) == std::format("value-{:05d}", i));
        }
    });
}

TEST_CASE("Sort-key prefixes do not leak across entries", "[cowstorage][shape]")
{
    TempFile tmp;
    std::vector<std::pair<std::string, std::string>> const items {
        { "a", "VA" },   { "aa", "VAA" }, { "aaa", "VAAA" }, { "aab", "VAAB" },
        { "ab", "VAB" }, { "b", "VB" },   { "ba", "VBA" },   { "z", "VZ" },
    };
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        for (auto const& [k, v]: items)
            REQUIRE(storage.Set(k, MakeBytes(v), 0, FastCache::TimePoint::max()).has_value());
    });
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        for (auto const& [k, v]: items)
        {
            auto got = storage.Get(k, clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(Decode(got->entry.value) == v);
        }
    });
}

TEST_CASE("Update replaces in place; only the latest value persists", "[cowstorage][shape]")
{
    TempFile tmp;
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("k", MakeBytes("v1"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Set("k", MakeBytes("v2"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Set("k", MakeBytes("v3"), 0, FastCache::TimePoint::max()).has_value());
    });
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("k", FastCache::ManualClock {}.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "v3");
    });
}

TEST_CASE("Delete + reinsert with different value persists the new value", "[cowstorage][shape]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("k", MakeBytes("old"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Delete("k", clock.Now()).has_value());
        REQUIRE(storage.Set("k", MakeBytes("new"), 0, FastCache::TimePoint::max()).has_value());
    });
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "new");
    });
}

// ============================================================================
// Compound-op roundtrip
// ============================================================================

TEST_CASE("CowTreeStorage Add fails on existing key", "[cowstorage]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->Set("k", MakeBytes("first"), 0, FastCache::TimePoint::max()).has_value());
    auto r = (*storage)->Add("k", MakeBytes("second"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::KeyExists);
}

TEST_CASE("CowTreeStorage Replace fails when missing", "[cowstorage]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;
    auto r = (*storage)->Replace("k", MakeBytes("nope"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::KeyNotFound);
}

TEST_CASE("Replace overwrites and persists across reopen", "[cowstorage][roundtrip]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("k", MakeBytes("v1"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Replace("k", MakeBytes("v2"), 0, FastCache::TimePoint::max(), clock.Now()).has_value());
    });
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "v2");
    });
}

TEST_CASE("Append + Prepend round-trip and persist", "[cowstorage][roundtrip]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    auto const suffix = MakeBytes(" end");
    auto const prefix = MakeBytes("start ");

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("k", MakeBytes("middle"), 0, FastCache::TimePoint::max()).has_value());
        REQUIRE(
            storage.Append("k", std::span<std::byte const> { suffix.data(), suffix.size() }, 0, clock.Now()).has_value());
        REQUIRE(
            storage.Prepend("k", std::span<std::byte const> { prefix.data(), prefix.size() }, 0, clock.Now()).has_value());
        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "start middle end");
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "start middle end");
    });
}

TEST_CASE("Append exceeding maxValueBytes returns ValueTooLarge and leaves value untouched",
          "[cowstorage][roundtrip][boundary]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;
    opts.maxValueBytes = 16;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->Set("k", MakeBytes("0123456789ABCDE"), 0, FastCache::TimePoint::max()).has_value());
    auto const overflow = MakeBytes("XX");
    auto const result =
        (*storage)->Append("k", std::span<std::byte const> { overflow.data(), overflow.size() }, 0, clock.Now());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::StorageErrorCode::ValueTooLarge);

    auto got = (*storage)->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "0123456789ABCDE");
}

TEST_CASE("CompareAndSwap success path persists across reopen", "[cowstorage][cas][roundtrip]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    FastCache::CasToken originalCas { 0 };
    FastCache::CasToken newCas { 0 };

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto const setCas = storage.Set("k", MakeBytes("one"), 0, FastCache::TimePoint::max());
        REQUIRE(setCas.has_value());
        originalCas = *setCas;
        auto const casResult =
            storage.CompareAndSwap("k", originalCas, MakeBytes("two"), 0, FastCache::TimePoint::max(), clock.Now());
        REQUIRE(casResult.has_value());
        newCas = *casResult;
        REQUIRE(newCas != originalCas);
    });
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "two");
    });
}

TEST_CASE("CompareAndSwap mismatch leaves entry untouched", "[cowstorage][cas]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    auto const setCas = (*storage)->Set("k", MakeBytes("one"), 7, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());
    auto const wrong = (*storage)->CompareAndSwap("k", 999, MakeBytes("two"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().code == FastCache::StorageErrorCode::CasMismatch);

    auto got = (*storage)->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "one");
    REQUIRE(got->entry.flags == 7U);
    REQUIRE(got->entry.cas == *setCas);
}

TEST_CASE("IncrementOrInitialize returns KeyNotFound on a miss; increments an existing key", "[cowstorage][incr][roundtrip]")
{
    TempFile tmp;
    FastCache::ManualClock clock;

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        // Contract: a missing key is a miss, NOT an auto-create. The
        // protocol layer owns the "initialize" semantics (binary
        // initial/expiration, meta J/N) and re-issues a Set on KeyNotFound.
        auto miss = storage.IncrementOrInitialize("counter", 10, /*decrement=*/false, clock.Now());
        REQUIRE_FALSE(miss.has_value());
        REQUIRE(miss.error().code == FastCache::StorageErrorCode::KeyNotFound);

        // Seed the key, then increment the existing value.
        REQUIRE(storage.Set("counter", MakeBytes("10"), 0, FastCache::TimePoint::max()).has_value());
        auto b = storage.IncrementOrInitialize("counter", 5, /*decrement=*/false, clock.Now());
        REQUIRE(b.has_value());
        REQUIRE(b->value == 15U);
    });
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("counter", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "15");
    });
}

TEST_CASE("IncrementOrInitialize floors at 0 on saturating decrement", "[cowstorage][incr]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->Set("k", MakeBytes("5"), 0, FastCache::TimePoint::max()).has_value());
    auto r = (*storage)->IncrementOrInitialize("k", 10, /*decrement=*/true, clock.Now());
    REQUIRE(r.has_value());
    REQUIRE(r->value == 0U);
}

TEST_CASE("IncrementOrInitialize handles full-uint64 magnitudes without signed-overflow UB",
          "[cowstorage][incr][regression]")
{
    // The magnitude is a full std::uint64_t, so a decrement by 2^63 saturates
    // cleanly at 0 and an increment by 2^63 adds — magnitudes the old signed
    // delta could not carry (decr by 2^63 was negation-of-INT64_MIN UB; incr
    // by 2^63 aliased to a decrement). UBSan would abort on a regression.
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;
    constexpr std::uint64_t Huge = 1ULL << 63;

    REQUIRE((*storage)->Set("k", MakeBytes("0"), 0, FastCache::TimePoint::max()).has_value());
    auto const up = (*storage)->IncrementOrInitialize("k", Huge, /*decrement=*/false, clock.Now());
    REQUIRE(up.has_value());
    REQUIRE(up->value == Huge); // 0 + 2^63, not aliased to a decrement

    auto const down = (*storage)->IncrementOrInitialize("k", Huge, /*decrement=*/true, clock.Now());
    REQUIRE(down.has_value());
    REQUIRE(down->value == 0U); // 2^63 - 2^63
}

TEST_CASE("CowTreeStorage Touch preserves the fetched bit (a touch is not a read)", "[cowstorage][stats][unfetched]")
{
    // Regression: Touch used the default AccessKind::Write, clearing `fetched`
    // and diverging from InMemoryLruStorage. A previously-read entry that is
    // later purged must NOT be counted expired_unfetched.
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, clock.Now() + 1s).has_value());
    REQUIRE((*storage)->Get("k", clock.Now())->found);                          // fetched = true
    REQUIRE((*storage)->Touch("k", clock.Now() + 1s, clock.Now()).has_value()); // must keep fetched

    clock.Advance(2s);
    REQUIRE((*storage)->PurgeExpired(clock.Now()) == 1U);
    REQUIRE((*storage)->Snapshot().expiredUnfetched == 0U);
}

TEST_CASE("CowTreeStorage MarkStale preserves the fetched bit", "[cowstorage][stats][unfetched]")
{
    // MarkStale shares TouchOrInsert(AccessKind::Preserve) with Touch, so a
    // previously-read entry that is later purged must not be counted unfetched.
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, clock.Now() + 1s).has_value());
    REQUIRE((*storage)->Get("k", clock.Now())->found);                          // fetched = true
    REQUIRE((*storage)->MarkStale("k", std::nullopt, clock.Now()).has_value()); // must keep fetched

    clock.Advance(2s);
    REQUIRE((*storage)->PurgeExpired(clock.Now()) == 1U);
    REQUIRE((*storage)->Snapshot().expiredUnfetched == 0U);
}

TEST_CASE("CowTreeStorage GetAndTouch refreshes the expiry and returns the entry", "[cowstorage][gat]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, clock.Now() + 1s).has_value());
    auto const newExpiry = clock.Now() + 60s;
    auto const gat = (*storage)->GetAndTouch("k", newExpiry, clock.Now());
    REQUIRE(gat.has_value());
    REQUIRE(gat->found);
    REQUIRE(Decode(gat->entry.value) == "v");
    REQUIRE(gat->entry.expiry == newExpiry);

    auto const miss = (*storage)->GetAndTouch("absent", newExpiry, clock.Now());
    REQUIRE_FALSE(miss.has_value());
    REQUIRE(miss.error().code == FastCache::StorageErrorCode::KeyNotFound);
}

TEST_CASE("CowTreeStorage CompareAndDelete honours the CAS precondition", "[cowstorage][cad]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    auto const setCas = (*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());

    // Wrong CAS -> mismatch, entry survives.
    auto const wrong = (*storage)->CompareAndDelete("k", *setCas + 1, clock.Now());
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().code == FastCache::StorageErrorCode::CasMismatch);
    REQUIRE((*storage)->Get("k", clock.Now())->found);

    // Right CAS -> deleted.
    REQUIRE((*storage)->CompareAndDelete("k", *setCas, clock.Now()).has_value());
    REQUIRE_FALSE((*storage)->Get("k", clock.Now())->found);

    // Absent key -> KeyNotFound.
    auto const absent = (*storage)->CompareAndDelete("absent", 1, clock.Now());
    REQUIRE_FALSE(absent.has_value());
    REQUIRE(absent.error().code == FastCache::StorageErrorCode::KeyNotFound);
}

// ============================================================================
// Delete and expiry coverage
// ============================================================================

TEST_CASE("CowTreeStorage CAS mismatch path", "[cowstorage]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    auto cas = (*storage)->Set("k", MakeBytes("one"), 0, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());

    auto wrong = (*storage)->CompareAndSwap("k", 999, MakeBytes("two"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().code == FastCache::StorageErrorCode::CasMismatch);

    auto right = (*storage)->CompareAndSwap("k", *cas, MakeBytes("two"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE(right.has_value());
}

TEST_CASE("CowTreeStorage Delete + TTL expiry path", "[cowstorage]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    auto const expiry = clock.Now() + 100ms;
    REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, expiry).has_value());
    auto const before = (*storage)->Get("k", clock.Now());
    REQUIRE(before->found);

    clock.Advance(200ms);
    auto const after = (*storage)->Get("k", clock.Now());
    REQUIRE(after.has_value());
    REQUIRE_FALSE(after->found);
}

TEST_CASE("Delete on expired entry erases the on-disk record across reopen", "[cowstorage][regression]")
{
    // Regression for finding #12 — Delete on an expired entry used to
    // return KeyNotFound without erasing the disk record, so a
    // subsequent reopen would have the entry still occupying space.
    TempFile tmp;
    FastCache::ManualClock clock;
    auto const shortExpiry = clock.Now() + 1ms;

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("k", MakeBytes("v"), 0, shortExpiry).has_value());
        FastCache::ManualClock laterClock;
        laterClock.Advance(10ms);
        auto deleted = storage.Delete("k", laterClock.Now());
        REQUIRE_FALSE(deleted.has_value());
        REQUIRE(deleted.error().code == FastCache::StorageErrorCode::KeyNotFound);
    });

    // After reopen the entry must not resurface even if the clock is
    // rewound to before the original expiry.
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock freshClock;
        auto got = storage.Get("k", freshClock.Now());
        REQUIRE(got.has_value());
        REQUIRE_FALSE(got->found);
    });
}

TEST_CASE("Get on an expired entry does NOT mutate the tree (no BeginWrite from a read path)", "[cowstorage][regression]")
{
    // Regression for finding #1 — Get used to open a write transaction
    // to erase expired entries, which under ShardedStorage's
    // shared_lock would race concurrent same-shard Gets and corrupt
    // the CowTree's free list. The fix defers cleanup to
    // PurgeExpired. The test here just verifies Get is idempotent on
    // expired entries (no error, value stays accessible to
    // PurgeExpired).
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;
    auto const shortExpiry = clock.Now() + 1ms;
    REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, shortExpiry).has_value());
    clock.Advance(10ms);

    // Issue several Gets back-to-back; none should return found, none
    // should mutate the tree visibly to subsequent Gets.
    for (int i = 0; i < 5; ++i)
    {
        auto got = (*storage)->Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE_FALSE(got->found);
    }

    // PurgeExpired should now report 1 victim and remove it.
    auto const purged = (*storage)->PurgeExpired(clock.Now());
    REQUIRE(purged == 1U);

    auto after = (*storage)->Get("k", clock.Now());
    REQUIRE(after.has_value());
    REQUIRE_FALSE(after->found);
}

TEST_CASE("PurgeExpired clears all expired entries and reports the count", "[cowstorage][purge]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;
    auto const shortExpiry = clock.Now() + 1ms;

    for (int i = 0; i < 6; ++i)
        REQUIRE((*storage)->Set(std::format("expire-{}", i), MakeBytes("v"), 0, shortExpiry).has_value());
    for (int i = 0; i < 4; ++i)
        REQUIRE((*storage)->Set(std::format("keep-{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    clock.Advance(10ms);
    auto const purged = (*storage)->PurgeExpired(clock.Now());
    REQUIRE(purged == 6U);

    for (int i = 0; i < 4; ++i)
    {
        auto got = (*storage)->Get(std::format("keep-{}", i), clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
    }
}

// ============================================================================
// Eviction & accounting
// ============================================================================

TEST_CASE("EvictToFit drops LRU tail when over maxBytes", "[cowstorage][eviction]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;
    opts.maxBytes = 256;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    // Insert until total bytes exceeds the cap; the cap+eviction model
    // is best-effort soft.
    for (int i = 0; i < 50; ++i)
    {
        auto const value = std::format("v-{:08d}", i); // 10 bytes each
        REQUIRE((*storage)->Set(std::format("k-{:03d}", i), MakeBytes(value), 0, FastCache::TimePoint::max()).has_value());
    }
    auto const stats = (*storage)->Snapshot();
    REQUIRE(stats.bytesUsed <= opts.maxBytes);
    REQUIRE(stats.evictions > 0U);

    // The most recently inserted keys should still be readable.
    auto last = (*storage)->Get("k-049", clock.Now());
    REQUIRE(last.has_value());
    REQUIRE(last->found);
}

TEST_CASE("Resize shrinks budget and triggers immediate eviction", "[cowstorage][eviction]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;
    opts.maxBytes = 1024 * 1024;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());

    for (int i = 0; i < 100; ++i)
        REQUIRE((*storage)
                    ->Set(std::format("k-{:03d}", i),
                          RandomBytes(64, static_cast<std::uint64_t>(i)),
                          0,
                          FastCache::TimePoint::max())
                    .has_value());
    auto const before = (*storage)->Snapshot();
    REQUIRE(before.itemCount == 100U);

    (*storage)->Resize(1024);
    auto const after = (*storage)->Snapshot();
    REQUIRE(after.bytesUsed <= 1024U);
    REQUIRE(after.evictions > 0U);
}

// ============================================================================
// Persistence and reopen
// ============================================================================

TEST_CASE("Three Open/Close cycles preserve every entry", "[cowstorage][persist]")
{
    constexpr int N = 100;
    TempFile tmp;

    auto write = [&] {
        WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
            for (int i = 0; i < N; ++i)
                REQUIRE(storage
                            .Set(std::format("k-{:04d}", i),
                                 MakeBytes(std::format("v-{:04d}", i)),
                                 0,
                                 FastCache::TimePoint::max())
                            .has_value());
        });
    };

    auto verify = [&] {
        WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
            FastCache::ManualClock clock;
            for (int i = 0; i < N; ++i)
            {
                auto got = storage.Get(std::format("k-{:04d}", i), clock.Now());
                REQUIRE(got.has_value());
                REQUIRE(got->found);
                REQUIRE(Decode(got->entry.value) == std::format("v-{:04d}", i));
            }
        });
    };

    write();
    verify();
    verify();
    verify();
}

TEST_CASE("Mixed Set/Update/Delete script replays identically across a mid-script reopen", "[cowstorage][persist]")
{
    TempFile tmp;
    FastCache::ManualClock clock;

    // Phase 1: Set 20 keys, update 10 of them, delete 5.
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        for (int i = 0; i < 20; ++i)
            REQUIRE(storage.Set(std::format("k-{}", i), MakeBytes(std::format("v0-{}", i)), 0, FastCache::TimePoint::max())
                        .has_value());
        for (int i = 0; i < 10; ++i)
            REQUIRE(storage.Set(std::format("k-{}", i), MakeBytes(std::format("v1-{}", i)), 0, FastCache::TimePoint::max())
                        .has_value());
        for (int i = 15; i < 20; ++i)
            REQUIRE(storage.Delete(std::format("k-{}", i), clock.Now()).has_value());
    });

    // Phase 2: After reopen, verify the expected state.
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        for (int i = 0; i < 10; ++i)
        {
            auto got = storage.Get(std::format("k-{}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(Decode(got->entry.value) == std::format("v1-{}", i));
        }
        for (int i = 10; i < 15; ++i)
        {
            auto got = storage.Get(std::format("k-{}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(Decode(got->entry.value) == std::format("v0-{}", i));
        }
        for (int i = 15; i < 20; ++i)
        {
            auto got = storage.Get(std::format("k-{}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE_FALSE(got->found);
        }
    });
}

// ============================================================================
// Failure modes
// ============================================================================

TEST_CASE("Open with a non-existent path under an existing directory creates the file", "[cowstorage][open]")
{
    auto const dir = std::filesystem::temp_directory_path();
    auto const path = dir / std::format("cowstorage-mktest-{}.cow", std::mt19937_64 { std::random_device {}() }());
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        FastCache::CowTreeStorage::Options opts;
        opts.path = path;
        auto storage = FastCache::CowTreeStorage::Open(opts);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    }
    REQUIRE(std::filesystem::exists(path));
    std::filesystem::remove(path, ec);
}

TEST_CASE("Open on a path holding random non-CowTree bytes returns Corrupt or IoError", "[cowstorage][open]")
{
    TempFile tmp;
    {
        // Fill the file with random bytes so neither meta slot validates.
        auto const garbage = RandomBytes(8192, 0xCAFEBABEULL);
        std::ofstream f { tmp.path, std::ios::binary };
        REQUIRE(f.good());
        f.write(reinterpret_cast<char const*>(garbage.data()), static_cast<std::streamsize>(garbage.size()));
    }
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE_FALSE(storage.has_value());
    // The exact error code depends on which level rejects the bytes
    // first; we accept either Corrupt or IoError, both are sane.
    auto const code = storage.error().code;
    REQUIRE((code == FastCache::StorageErrorCode::Corrupt || code == FastCache::StorageErrorCode::IoError));
}

TEST_CASE("Set above maxValueBytes returns ValueTooLarge", "[cowstorage][boundary]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;
    opts.maxValueBytes = 32;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());

    auto const too_big = RandomBytes(33, 0x1ULL);
    auto const r = (*storage)->Set("k", too_big, 0, FastCache::TimePoint::max());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::ValueTooLarge);
}

TEST_CASE("Touch refreshes expiry and bumps CAS, persists across reopen", "[cowstorage][touch]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;

    auto firstCas = FastCache::CasToken { 0 };
    auto extendedExpiry = clock.Now() + 60s;
    {
        auto storage = FastCache::CowTreeStorage::Open(opts);
        REQUIRE(storage.has_value());

        auto const setCas = (*storage)->Set("k", MakeBytes("payload"), 0xBEEF, clock.Now() + 1s);
        REQUIRE(setCas.has_value());
        firstCas = *setCas;

        auto const touched = (*storage)->Touch("k", extendedExpiry, clock.Now());
        REQUIRE(touched.has_value());
        REQUIRE(*touched != firstCas);

        auto const stats = (*storage)->Snapshot();
        REQUIRE(stats.touchHits == 1U);
        REQUIRE(stats.cmdTouch == 1U);
    }
    // Reopen: the touch should have persisted (extended expiry + new CAS).
    auto reopened = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(reopened.has_value());
    auto const got = (*reopened)->Get("k", clock.Now() + 30s);
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "payload");
    REQUIRE(got->entry.flags == 0xBEEF);
    REQUIRE(got->entry.expiry == extendedExpiry);
}

TEST_CASE("Touch on absent key returns KeyNotFound + bumps touchMisses", "[cowstorage][touch]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());

    auto const r = (*storage)->Touch("nope", FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == FastCache::StorageErrorCode::KeyNotFound);
    REQUIRE((*storage)->Snapshot().touchMisses == 1U);
}

TEST_CASE("CowTree v2 trailer round-trips lastAccess and stale across reopen", "[cowstorage][v2]")
{
    TempFile tmp;
    FastCache::ManualClock clock;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;
    clock.Advance(7s);
    auto const t = clock.Now();

    {
        auto storage = FastCache::CowTreeStorage::Open(opts);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
        // Touch persists lastAccess; MarkStale persists the stale flag.
        // (A read no longer writes — see the "no write on read" test — so
        // we exercise the trailer via the write paths that legitimately
        // own those fields.)
        REQUIRE((*storage)->Touch("k", FastCache::TimePoint::max(), t).has_value());
        REQUIRE((*storage)->MarkStale("k", std::nullopt, t).has_value());
    }

    auto reopened = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(reopened.has_value());
    // Peek so the read itself does not advance lastAccess in the copy.
    auto const got = (*reopened)->Peek("k", t);
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(got->entry.lastAccess == t); // round-tripped through the v2 trailer
    REQUIRE(got->entry.stale);           // stale flag round-tripped too
}

TEST_CASE("CowTreeStorage Get does not persist lastAccess (no write on a read path)", "[cowstorage][get][regression]")
{
    // Regression: Get used to open a full write transaction on every hit to
    // persist lastAccess — crippling read-heavy workloads. The returned
    // copy still carries a fresh lastAccess, but nothing is written back, so
    // the persisted value (set by Set, which never reads) stays the unset
    // sentinel across a reopen.
    TempFile tmp;
    FastCache::ManualClock clock;
    clock.Advance(100s);
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
        auto const got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(got->entry.lastAccess == clock.Now()); // fresh in the returned copy
    });
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto const peeked = storage.Peek("k", clock.Now());
        REQUIRE(peeked.has_value());
        REQUIRE(peeked->found);
        REQUIRE(peeked->entry.lastAccess == FastCache::TimePoint::min()); // never written by the read
    });
}

// ============================================================================
// Overflow pages (values larger than the inline limit spill to a page chain)
// ============================================================================

namespace
{
/// Options with a small fixed page so values above ~1 KiB exercise the
/// overflow chain, and a generous value cap so multi-page values are allowed.
FastCache::CowTreeStorage::Options OverflowOptions(
    std::filesystem::path const& path,
    CowTree::FilePageStore::Durability durability = CowTree::FilePageStore::Durability::Batched)
{
    FastCache::CowTreeStorage::Options opts;
    opts.path = path;
    opts.pageSize = 4096;                 // InlineValueLimit() = 1024
    opts.maxValueBytes = 4 * 1024 * 1024; // allow multi-page values
    opts.durability = durability;
    return opts;
}
} // namespace

TEST_CASE("Overflow chains round-trip across many sizes and survive reopen", "[cowstorage][overflow]")
{
    TempFile tmp;
    std::vector<std::size_t> const sizes { 0, 1, 1024, 1025, 4079, 4080, 4081, 8192, 65536, 262144, 1024 * 1024 };

    {
        auto storage = FastCache::CowTreeStorage::Open(OverflowOptions(tmp.path));
        REQUIRE(storage.has_value());
        for (auto const size: sizes)
        {
            auto const value = RandomBytes(size, 0xA000ULL + size);
            REQUIRE((*storage)->Set(std::format("k{}", size), value, 0, FastCache::TimePoint::max()).has_value());
        }
    }

    auto reopened = FastCache::CowTreeStorage::Open(OverflowOptions(tmp.path));
    REQUIRE(reopened.has_value());
    FastCache::ManualClock clock;
    for (auto const size: sizes)
    {
        auto const value = RandomBytes(size, 0xA000ULL + size);
        auto const got = (*reopened)->Get(std::format("k{}", size), clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(got->entry.value == value); // exact bytewise round-trip via the chain
    }
}

TEST_CASE("Overwriting a large value reclaims the old overflow chain", "[cowstorage][overflow]")
{
    TempFile tmp;
    // Fsync durability frees pages immediately (Batched defers reuse to the
    // group-commit flush boundary), so reuse is observable within this session.
    auto storage = FastCache::CowTreeStorage::Open(OverflowOptions(tmp.path, CowTree::FilePageStore::Durability::Fsync));
    REQUIRE(storage.has_value());

    REQUIRE((*storage)->Set("k", RandomBytes(256 * 1024, 1), 0, FastCache::TimePoint::max()).has_value());
    auto const afterFirst = std::filesystem::file_size(tmp.path);

    for (auto const i: std::views::iota(0U, 12U))
        REQUIRE((*storage)->Set("k", RandomBytes(256 * 1024, 100U + i), 0, FastCache::TimePoint::max()).has_value());
    auto const afterMany = std::filesystem::file_size(tmp.path);

    // Each overwrite frees the previous chain, so the file reuses pages instead
    // of growing ~12x. (A leak would push this well past 3x.)
    REQUIRE(afterMany < afterFirst * 3);

    auto const got = (*storage)->Get("k", FastCache::ManualClock {}.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(got->entry.value == RandomBytes(256 * 1024, 111)); // last write wins
}

TEST_CASE("Deleting a large value frees its overflow chain for reuse", "[cowstorage][overflow]")
{
    TempFile tmp;
    // Fsync durability frees pages immediately so reuse is observable in-session
    // (Batched would defer the freed-chain reuse to the group-commit boundary).
    auto storage = FastCache::CowTreeStorage::Open(OverflowOptions(tmp.path, CowTree::FilePageStore::Durability::Fsync));
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->Set("k", RandomBytes(256 * 1024, 5), 0, FastCache::TimePoint::max()).has_value());
    auto const afterFirst = std::filesystem::file_size(tmp.path);
    for (auto const i: std::views::iota(0U, 8U))
    {
        REQUIRE((*storage)->Delete("k", clock.Now()).has_value());
        REQUIRE((*storage)->Set("k", RandomBytes(256 * 1024, 200U + i), 0, FastCache::TimePoint::max()).has_value());
    }
    REQUIRE(std::filesystem::file_size(tmp.path) < afterFirst * 3);

    REQUIRE((*storage)->Delete("k", clock.Now()).has_value());
    auto const got = (*storage)->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->found);
}

TEST_CASE("Crash during overflow-chain write leaves the previous value intact", "[cowstorage][overflow][crash]")
{
    CowTree::InMemoryPageStore store; // default 4 KiB page -> 1 KiB inline limit
    FastCache::CowTreeStorage::Options opts;
    opts.maxValueBytes = 1024 * 1024;

    {
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("initial"), 0, FastCache::TimePoint::max()).has_value());
    }

    {
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.failNthWrite = 1; // fail the first overflow-chunk write
        store.SetFaultPlan(plan);
        store.ResetCounters();
        auto const r = (*storage)->Set("k", RandomBytes(50000, 9), 0, FastCache::TimePoint::max());
        REQUIRE_FALSE(r.has_value());
    }

    store.SetFaultPlan({});
    store.ResetCounters();
    auto reopened = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(reopened.has_value());
    auto const got = (*reopened)->Get("k", FastCache::ManualClock {}.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "initial"); // rolled back, never a hybrid
}

TEST_CASE("Crash during overflow SyncData leaves the previous value intact", "[cowstorage][overflow][crash]")
{
    CowTree::InMemoryPageStore store;
    FastCache::CowTreeStorage::Options opts;
    opts.maxValueBytes = 1024 * 1024;

    {
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("initial"), 0, FastCache::TimePoint::max()).has_value());
    }

    {
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.failNthSyncData = 1; // fail the durability barrier before the meta flip
        store.SetFaultPlan(plan);
        store.ResetCounters();
        auto const r = (*storage)->Set("k", RandomBytes(50000, 11), 0, FastCache::TimePoint::max());
        REQUIRE_FALSE(r.has_value());
    }

    store.SetFaultPlan({});
    store.ResetCounters();
    auto reopened = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(reopened.has_value());
    auto const got = (*reopened)->Get("k", FastCache::ManualClock {}.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "initial");
}
