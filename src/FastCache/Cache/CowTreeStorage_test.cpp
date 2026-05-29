// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
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
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

/// Generate `size` deterministic-pseudo-random bytes from `seed`. Used
/// by the binary-blob roundtrip tests so they can compare bytewise
/// after a reopen without baking the values into the test source.
std::vector<std::byte> RandomBytes(std::size_t size, std::uint64_t seed)
{
    std::mt19937_64 rng { seed };
    std::vector<std::byte> out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
        out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(rng() & 0xFFu)));
    return out;
}

struct TempFile
{
    std::filesystem::path path;
    TempFile()
    {
        std::mt19937_64 rng { std::random_device {}() };
        path = std::filesystem::temp_directory_path() / ("cowstorage-test-" + std::to_string(rng()) + ".cow");
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
    REQUIRE(got->entry.flags == 7u);
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
            REQUIRE(storage.Set(key, std::move(value), 0, FastCache::TimePoint::max()).has_value());
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
            REQUIRE(got->entry.value.size() == 1u);
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

TEST_CASE("Value exactly at maxValueBytes roundtrips; one over returns ValueTooLarge",
          "[cowstorage][roundtrip][boundary]")
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
    constexpr std::uint32_t kFlags[] { 0u, 1u, 0xDEADBEEFu, std::numeric_limits<std::uint32_t>::max() };

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
        std::iota(order.begin(), order.end(), 0);
        std::mt19937_64 rng { 0x12345678ULL };
        std::shuffle(order.begin(), order.end(), rng);

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
        { "a", "VA" }, { "aa", "VAA" }, { "aaa", "VAAA" }, { "aab", "VAAB" },
        { "ab", "VAB" }, { "b", "VB" }, { "ba", "VBA" }, { "z", "VZ" },
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
        REQUIRE(storage.Append("k", std::span<std::byte const> { suffix.data(), suffix.size() }, clock.Now())
                    .has_value());
        REQUIRE(storage.Prepend("k", std::span<std::byte const> { prefix.data(), prefix.size() }, clock.Now())
                    .has_value());
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
    auto const result = (*storage)->Append("k", std::span<std::byte const> { overflow.data(), overflow.size() },
                                            clock.Now());
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
        auto const casResult = storage.CompareAndSwap("k", originalCas, MakeBytes("two"), 0,
                                                       FastCache::TimePoint::max(), clock.Now());
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
    auto const wrong = (*storage)->CompareAndSwap("k", 999, MakeBytes("two"), 0, FastCache::TimePoint::max(),
                                                   clock.Now());
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().code == FastCache::StorageErrorCode::CasMismatch);

    auto got = (*storage)->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "one");
    REQUIRE(got->entry.flags == 7u);
    REQUIRE(got->entry.cas == *setCas);
}

TEST_CASE("IncrementOrInitialize creates and increments; survives reopen", "[cowstorage][incr][roundtrip]")
{
    TempFile tmp;
    FastCache::ManualClock clock;

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto a = storage.IncrementOrInitialize("counter", 10, clock.Now());
        REQUIRE(a.has_value());
        REQUIRE(a->value == 10u);

        auto b = storage.IncrementOrInitialize("counter", 5, clock.Now());
        REQUIRE(b.has_value());
        REQUIRE(b->value == 15u);
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

    REQUIRE((*storage)->IncrementOrInitialize("k", 5, clock.Now()).has_value());
    auto r = (*storage)->IncrementOrInitialize("k", -10, clock.Now());
    REQUIRE(r.has_value());
    REQUIRE(r->value == 0u);
}

TEST_CASE("IncrementOrInitialize DECR with INT64_MIN does not invoke UB and floors at 0",
          "[cowstorage][incr][regression]")
{
    // Regression for finding #10 — negating INT64_MIN as a signed int
    // is UB. ASan/UBSan would abort. The fix uses the
    // `static_cast<uint64_t>(-(delta + 1)) + 1` idiom.
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    REQUIRE((*storage)->IncrementOrInitialize("k", 0, clock.Now()).has_value());
    auto r = (*storage)->IncrementOrInitialize("k", std::numeric_limits<std::int64_t>::min(), clock.Now());
    REQUIRE(r.has_value());
    REQUIRE(r->value == 0u);
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

TEST_CASE("Get on an expired entry does NOT mutate the tree (no BeginWrite from a read path)",
          "[cowstorage][regression]")
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
    REQUIRE(purged == 1u);

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
        REQUIRE((*storage)->Set(std::format("keep-{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max())
                    .has_value());

    clock.Advance(10ms);
    auto const purged = (*storage)->PurgeExpired(clock.Now());
    REQUIRE(purged == 6u);

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
        REQUIRE((*storage)->Set(std::format("k-{:03d}", i), MakeBytes(value), 0,
                                 FastCache::TimePoint::max())
                    .has_value());
    }
    auto const stats = (*storage)->Snapshot();
    REQUIRE(stats.bytesUsed <= opts.maxBytes);
    REQUIRE(stats.evictions > 0u);

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
        REQUIRE((*storage)->Set(std::format("k-{:03d}", i), RandomBytes(64, static_cast<std::uint64_t>(i)), 0,
                                 FastCache::TimePoint::max())
                    .has_value());
    auto const before = (*storage)->Snapshot();
    REQUIRE(before.itemCount == 100u);

    (*storage)->Resize(1024);
    auto const after = (*storage)->Snapshot();
    REQUIRE(after.bytesUsed <= 1024u);
    REQUIRE(after.evictions > 0u);
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
                REQUIRE(
                    storage
                        .Set(std::format("k-{:04d}", i), MakeBytes(std::format("v-{:04d}", i)), 0,
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

TEST_CASE("Mixed Set/Update/Delete script replays identically across a mid-script reopen",
          "[cowstorage][persist]")
{
    TempFile tmp;
    FastCache::ManualClock clock;

    // Phase 1: Set 20 keys, update 10 of them, delete 5.
    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        for (int i = 0; i < 20; ++i)
            REQUIRE(
                storage.Set(std::format("k-{}", i), MakeBytes(std::format("v0-{}", i)), 0, FastCache::TimePoint::max())
                    .has_value());
        for (int i = 0; i < 10; ++i)
            REQUIRE(
                storage.Set(std::format("k-{}", i), MakeBytes(std::format("v1-{}", i)), 0, FastCache::TimePoint::max())
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

TEST_CASE("Open with a non-existent path under an existing directory creates the file",
          "[cowstorage][open]")
{
    auto const dir = std::filesystem::temp_directory_path();
    auto const path = dir / std::format("cowstorage-mktest-{}.cow",
                                         std::mt19937_64 { std::random_device {}() }());
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

TEST_CASE("Open on a path holding random non-CowTree bytes returns Corrupt or IoError",
          "[cowstorage][open]")
{
    TempFile tmp;
    {
        // Fill the file with random bytes so neither meta slot validates.
        auto const garbage = RandomBytes(8192, 0xCAFEBABEULL);
        std::ofstream f { tmp.path, std::ios::binary };
        REQUIRE(f.good());
        f.write(reinterpret_cast<char const*>(garbage.data()),
                static_cast<std::streamsize>(garbage.size()));
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
