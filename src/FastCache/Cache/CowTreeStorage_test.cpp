// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/Crc32c.hpp>
#include <CowTree/FilePageStore.hpp>
#include <CowTree/IPageStore.hpp>
#include <CowTree/InMemoryPageStore.hpp>
#include <tests/Unwrap.hpp>

using namespace std::chrono_literals;
using FastCache::Testing::Decode;
using FastCache::Testing::MakeBytes;
using FastCache::Testing::TempFile;
using FastCache::Testing::Unwrap;
using FastCache::Testing::ValueOf;

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

/// Read a little-endian u32 from `bytes` at `offset` (host-endian aware).
std::uint32_t LoadLeU32(std::span<std::byte const> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    if constexpr (std::endian::native != std::endian::little)
        value = std::byteswap(value);
    return value;
}

/// Store a little-endian u64 into `bytes` at `offset` (host-endian aware).
void StoreLeU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
{
    if constexpr (std::endian::native != std::endian::little)
        value = std::byteswap(value);
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// True iff `page` parses as a valid overflow page under the on-disk layout
/// `[u32 crc][u64 next][u32 chunkLen][chunk]`, CRC over `[4 .. 16 + chunkLen)`.
/// Lets a test pick overflow pages out of a store without touching internals.
constexpr std::size_t OverflowHeader = 16;
bool LooksLikeOverflowPage(std::span<std::byte const> page, std::size_t pageSize)
{
    if (page.size() < OverflowHeader)
        return false;
    auto const crc = LoadLeU32(page, 0);
    auto const chunkLen = LoadLeU32(page, 12);
    if (chunkLen > pageSize - OverflowHeader)
        return false;
    auto const region = std::span<std::byte const> { page.data() + 4, (OverflowHeader - 4) + chunkLen };
    return CowTree::Crc32c::Compute(region) == crc;
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

/// Like WithOpenStorage but opens the store with the given compression codec,
/// so a test can drive the compress/decompress path across a reopen.
template <class F>
auto WithOpenStorageCompressed(std::filesystem::path const& path, FastCache::CompressionCodec codec, F&& fn)
{
    FastCache::CowTreeStorage::Options opts;
    opts.path = path;
    opts.compression = codec;
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
    REQUIRE(Decode(got->entry.ValueBytes()) == "hello");
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
        REQUIRE(Decode(got->entry.ValueBytes()) == "persisted");
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
        REQUIRE(ValueOf(got->entry).empty());
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("empty", FastCache::ManualClock {}.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(ValueOf(got->entry).empty());
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
            REQUIRE(got->entry.ValueSize() == 1U);
            REQUIRE(static_cast<std::uint8_t>(ValueOf(got->entry)[0]) == static_cast<std::uint8_t>(b));
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
        REQUIRE(ValueOf(got->entry) == blob);
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
        REQUIRE(ValueOf(a->entry) == small);

        auto b = storage.Get("medium", clock.Now());
        REQUIRE(b.has_value());
        REQUIRE(b->found);
        REQUIRE(ValueOf(b->entry) == medium);
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
    REQUIRE(ValueOf(got->entry) == fits);
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
        REQUIRE(Decode(a->entry.ValueBytes()) == "A");

        auto b = storage.Get(keyB, clock.Now());
        REQUIRE(b.has_value());
        REQUIRE(b->found);
        REQUIRE(Decode(b->entry.ValueBytes()) == "B");

        auto c = storage.Get(keyC, clock.Now());
        REQUIRE(c.has_value());
        REQUIRE(c->found);
        REQUIRE(Decode(c->entry.ValueBytes()) == "C");
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
        // Deterministic seed via seed_seq for reproducibility;
        // bugprone-random-generator-seed only flags direct literal seeding.
        std::seed_seq seed { 0x12345678U, 0x9ABCDEF0U, 0x0FEDCBA9U, 0x87654321U };
        std::mt19937_64 rng { seed };
        std::ranges::shuffle(order, rng);

        for (int const i: order)
        {
            auto got = storage.Get(std::format("key-{:05d}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(Decode(got->entry.ValueBytes()) == std::format("value-{:05d}", i));
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
            REQUIRE(Decode(got->entry.ValueBytes()) == v);
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
        REQUIRE(Decode(got->entry.ValueBytes()) == "v3");
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
        REQUIRE(Decode(got->entry.ValueBytes()) == "new");
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
        REQUIRE(Decode(got->entry.ValueBytes()) == "v2");
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
        REQUIRE(Decode(got->entry.ValueBytes()) == "start middle end");
    });

    WithOpenStorage(tmp.path, [&](FastCache::CowTreeStorage& storage) {
        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.ValueBytes()) == "start middle end");
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
    REQUIRE(Decode(got->entry.ValueBytes()) == "0123456789ABCDE");
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
        REQUIRE(Decode(got->entry.ValueBytes()) == "two");
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
    REQUIRE(Decode(got->entry.ValueBytes()) == "one");
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
        REQUIRE(Decode(got->entry.ValueBytes()) == "15");
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
    REQUIRE((*storage)->PurgeExpired(clock.Now(), FastCache::PurgeBudget::Unbounded()).purged == 1U);
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
    REQUIRE((*storage)->PurgeExpired(clock.Now(), FastCache::PurgeBudget::Unbounded()).purged == 1U);
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
    REQUIRE(Decode(gat->entry.ValueBytes()) == "v");
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
    auto const purged = (*storage)->PurgeExpired(clock.Now(), FastCache::PurgeBudget::Unbounded()).purged;
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
    auto const purged = (*storage)->PurgeExpired(clock.Now(), FastCache::PurgeBudget::Unbounded()).purged;
    REQUIRE(purged == 6U);

    for (int i = 0; i < 4; ++i)
    {
        auto got = (*storage)->Get(std::format("keep-{}", i), clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
    }
}

TEST_CASE("PurgeExpired on the disk tier is bounded and resumes", "[cowstorage][purge]")
{
    // Every step here additionally costs a `LoadEntry`, so an unbounded sweep
    // over a large store is a disk read per key while the tier's lock is held.
    // The ceiling makes that a fixed cost per cycle, and the cursor is what
    // stops a fixed cost per cycle from meaning "only ever the same keys".
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts { .path = tmp.path };
    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    // Written first, so the newest-first mirror leaves them BEHIND the live
    // ones -- out of reach of a sweep that restarted at the front each cycle.
    for (auto const i: std::views::iota(0, 3))
        REQUIRE((*storage)->Set(std::format("expire-{}", i), MakeBytes("v"), 0, clock.Now() + 1ms).has_value());
    for (auto const i: std::views::iota(0, 4))
        REQUIRE((*storage)->Set(std::format("keep-{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    clock.Advance(10ms);

    auto const first = (*storage)->PurgeExpired(clock.Now(), FastCache::PurgeBudget { .maxScanned = 4 });
    CHECK(first.scanned == 4U);
    CHECK(first.purged == 0U);
    CHECK_FALSE(first.completedPass);

    auto const second = (*storage)->PurgeExpired(clock.Now(), FastCache::PurgeBudget { .maxScanned = 3 });
    CHECK(second.purged == 3U);

    for (auto const i: std::views::iota(0, 4))
    {
        auto const got = (*storage)->Get(std::format("keep-{}", i), clock.Now());
        REQUIRE(got.has_value());
        CHECK(got->found);
    }
    CHECK((*storage)->PurgeExpired(clock.Now(), FastCache::PurgeBudget::Unbounded()).completedPass);
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
                REQUIRE(Decode(got->entry.ValueBytes()) == std::format("v-{:04d}", i));
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
            REQUIRE(Decode(got->entry.ValueBytes()) == std::format("v1-{}", i));
        }
        for (int i = 10; i < 15; ++i)
        {
            auto got = storage.Get(std::format("k-{}", i), clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(Decode(got->entry.ValueBytes()) == std::format("v0-{}", i));
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

// ============================================================================
// On-disk format vintage (issue #131)
//
// A store written under another record layout is refused — that part was always
// right. What these pin down is that the refusal is not spelled `Corrupt`: the
// code is what monitoring and every programmatic caller sees, and an operator
// reading "Corrupt" about an intact cache deletes it.
// ============================================================================

namespace
{

/// The format-marker key as tree-key bytes.
///
/// Taken from `CowTreeStorage::FormatMarkerKey` rather than a second copy of
/// the magic, so these tests cannot keep passing after the real key moves.
/// @return A view over the reserved sentinel key.
[[nodiscard]] CowTree::BytesView FormatMarkerKeyView() noexcept
{
    auto const key = FastCache::CowTreeStorage::FormatMarkerKey;
    return CowTree::BytesView { reinterpret_cast<std::byte const*>(key.data()), key.size() };
}

/// Commit one key/value into a store, over a raw tree that stamps no marker of
/// its own — the only way to produce a store this build refuses.
/// @param store Page store to write into (opened/initialised here).
/// @param key   Tree key.
/// @param value Tree value.
void PutRaw(CowTree::IPageStore& store, CowTree::BytesView key, CowTree::BytesView value)
{
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());
    auto txn = tree.BeginWrite();
    REQUIRE(txn.Put(key, value).has_value());
    REQUIRE(txn.Commit().has_value());
}

/// Stamp `version` into a store's format marker.
///
/// The four bytes are spelled out least-significant first rather than memcpy'd
/// from the integer, so the marker this writes is little-endian on a big-endian
/// host too — the test would otherwise agree with a bug on exactly the hosts the
/// on-disk format exists to be portable across.
/// @param store   Page store to write into.
/// @param version The version to record.
void StampFormatVersion(CowTree::IPageStore& store, std::uint32_t version)
{
    std::array<std::byte, sizeof(std::uint32_t)> const value {
        static_cast<std::byte>(version & 0xFFU),
        static_cast<std::byte>((version >> 8) & 0xFFU),
        static_cast<std::byte>((version >> 16) & 0xFFU),
        static_cast<std::byte>((version >> 24) & 0xFFU),
    };
    PutRaw(store, FormatMarkerKeyView(), CowTree::BytesView { value.data(), value.size() });
}

} // namespace

TEST_CASE("A store stamped with an unknown format version is refused as a vintage, not as damage",
          "[cowstorage][open][format]")
{
    constexpr auto Future = FastCache::CowTreeStorage::CurrentFormatVersion + 1;
    CowTree::InMemoryPageStore store;
    StampFormatVersion(store, Future);

    FastCache::CowTreeStorage::Options opts;
    auto const opened = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == FastCache::StorageErrorCode::UnsupportedFormatVersion);

    // Both versions named, so an operator can tell which way round it is
    // without going to the source.
    auto const& context = opened.error().context;
    REQUIRE(context.contains(std::format("version {}", Future)));
    // "writes N", not a bare "N": the digit alone would match anywhere in the
    // sentence, including the store's own version, so it would keep passing
    // with the two the wrong way round.
    REQUIRE(context.contains(std::format("writes {}", FastCache::CowTreeStorage::CurrentFormatVersion)));
    // A newer store is not something this build can convert; say so instead.
    REQUIRE(context.contains("upgrade"));
}

TEST_CASE("A non-empty store carrying no format marker is refused as a vintage, not as damage", "[cowstorage][open][format]")
{
    // The marker arrived WITH v4, so its absence over a store that already
    // holds records IS the version stamp: this is a v3 store.
    CowTree::InMemoryPageStore store;
    auto const key = std::string_view { "legacy-key" };
    std::array<std::byte, 4> const record { std::byte { 0 }, std::byte { 1 }, std::byte { 2 }, std::byte { 3 } };
    PutRaw(store,
           CowTree::BytesView { reinterpret_cast<std::byte const*>(key.data()), key.size() },
           CowTree::BytesView { record.data(), record.size() });

    FastCache::CowTreeStorage::Options opts;
    auto const opened = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == FastCache::StorageErrorCode::UnsupportedFormatVersion);
    REQUIRE(opened.error().context.contains("version 3"));
    // The whole point of the code: nothing here tells the operator to delete it.
    REQUIRE(opened.error().context.contains("does not need to be deleted"));
}

TEST_CASE("A format marker too short to hold a version is damage, and keeps saying Corrupt", "[cowstorage][open][format]")
{
    // The one case in this area that really IS corruption: there is no version
    // to report and nothing to convert, so the vintage code would be a lie in
    // the other direction.
    CowTree::InMemoryPageStore store;
    std::array<std::byte, 2> const truncated { std::byte { 4 }, std::byte { 0 } };
    PutRaw(store, FormatMarkerKeyView(), CowTree::BytesView { truncated.data(), truncated.size() });

    FastCache::CowTreeStorage::Options opts;
    auto const opened = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == FastCache::StorageErrorCode::Corrupt);
}

// ============================================================================
// Converting a store of an older vintage (issue #131)
// ============================================================================

namespace
{

/// Read a little-endian integer of `Width` bytes at `offset`.
/// @param bytes Source.
/// @param offset Byte offset.
/// @return The value, host order.
template <std::size_t Width>
[[nodiscard]] std::uint64_t LoadLe(CowTree::BytesView bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (auto const i: std::views::iota(std::size_t { 0 }, Width))
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8U * i);
    return value;
}

/// Append a little-endian integer of `Width` bytes.
/// @param out Destination.
/// @param value The value, host order.
template <std::size_t Width>
void AppendLe(std::vector<std::byte>& out, std::uint64_t value)
{
    for (auto const i: std::views::iota(std::size_t { 0 }, Width))
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
}

/// Bytes of the leaf-record header both v3 and v4 carry, between the kind tag
/// (plus v4's codec byte) and the length fields: flags, cas, expiry,
/// generation, lastAccess, stale.
constexpr std::size_t CommonHeaderBytes = 4 + 8 + 8 + 8 + 8 + 1;

/// Rewrite one current-format leaf record as the v3 layout it would have had.
///
/// v3 is v4 minus the codec byte and minus every original-length field. The
/// test only reverses THAT, and never touches the overflow page format — which
/// v3 and v4 share — so the chains a downgraded store points at are the ones
/// the production writer laid down rather than a second implementation of them.
///
/// Only meaningful for an uncompressed record, which is why the callers open
/// with `CompressionCodec::Identity`: v3 had no codec field because it never
/// compressed anything, so a compressed record has no v3 spelling at all.
/// @param v4 The record as the current encoder wrote it.
/// @return The same record under format v3.
[[nodiscard]] std::vector<std::byte> DowngradeRecordToV3(CowTree::BytesView v4)
{
    REQUIRE(v4.size() > 2 + CommonHeaderBytes);
    auto const kind = std::to_integer<std::uint8_t>(v4[0]);
    REQUIRE(std::to_integer<std::uint8_t>(v4[1]) == static_cast<std::uint8_t>(FastCache::CompressionCodec::Identity));

    std::vector<std::byte> out;
    out.push_back(v4[0]);
    // The common header is copied verbatim: identical fields, identical order.
    auto const common = v4.subspan(2, CommonHeaderBytes);
    out.insert(out.end(), common.begin(), common.end());

    auto const tail = 2 + CommonHeaderBytes;
    if (kind == 0) // inline
    {
        auto const storedLen = LoadLe<4>(v4, tail);
        REQUIRE(LoadLe<4>(v4, tail + 4) == storedLen); // Identity: original == stored
        AppendLe<4>(out, storedLen);
        auto const value = v4.subspan(tail + 8, static_cast<std::size_t>(storedLen));
        out.insert(out.end(), value.begin(), value.end());
        return out;
    }

    // overflow: v4 is [u64 stored][u64 original][u64 root], v3 [u64 len][u64 root].
    auto const storedLen = LoadLe<8>(v4, tail);
    REQUIRE(LoadLe<8>(v4, tail + 8) == storedLen);
    AppendLe<8>(out, storedLen);
    AppendLe<8>(out, LoadLe<8>(v4, tail + 16));
    return out;
}

/// Turn a current-format store into a genuine v3 one: every leaf record
/// rewritten under the old layout, and the version marker removed — which is
/// exactly the shape a build from before the marker existed left behind.
/// @param store The store to rewrite in place.
void DowngradeStoreToV3(CowTree::IPageStore& store,
                        std::size_t leaveConverted = 0,
                        std::uint32_t targetVersion = FastCache::CowTreeStorage::CurrentFormatVersion)
{
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    std::vector<std::vector<std::byte>> keys;
    std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> rewritten;
    {
        auto const reader = tree.BeginRead();
        REQUIRE(reader
                    .ForEach([&](CowTree::BytesView key, CowTree::BytesView value) {
                        if (std::ranges::equal(key, FormatMarkerKeyView()))
                            return true;
                        keys.emplace_back(key.begin(), key.end());
                        // The first `leaveConverted` keys stay in the current
                        // layout: together with the progress marker the caller
                        // plants, that is precisely the shape an interrupted
                        // conversion leaves behind.
                        if (keys.size() > leaveConverted)
                            rewritten.emplace_back(std::vector<std::byte> { key.begin(), key.end() },
                                                   DowngradeRecordToV3(value));
                        return true;
                    })
                    .has_value());
    }
    REQUIRE_FALSE(rewritten.empty());

    auto txn = tree.BeginWrite();
    for (auto const& [key, record]: rewritten)
        REQUIRE(txn.Put(CowTree::BytesView { key.data(), key.size() }, CowTree::BytesView { record.data(), record.size() })
                    .has_value());
    auto const erased = txn.Erase(FormatMarkerKeyView());
    REQUIRE(erased.has_value());
    REQUIRE(*erased);
    REQUIRE(txn.Commit().has_value());

    if (leaveConverted != 0)
    {
        REQUIRE(leaveConverted <= keys.size());
        std::vector<std::byte> value;
        AppendLe<4>(value, 3);             // fromVersion
        AppendLe<4>(value, targetVersion); // toVersion
        auto const& lastConverted = keys[leaveConverted - 1];
        value.insert(value.end(), lastConverted.begin(), lastConverted.end());

        auto marker = tree.BeginWrite();
        auto const key = FastCache::CowTreeStorage::MigrationMarkerKey;
        REQUIRE(marker
                    .Put(CowTree::BytesView { reinterpret_cast<std::byte const*>(key.data()), key.size() },
                         CowTree::BytesView { value.data(), value.size() })
                    .has_value());
        REQUIRE(marker.Commit().has_value());
    }
}

/// The overflow-chain head the current-format record under `key` names, or
/// nullopt when that record stores its value inline.
///
/// Read straight off the leaf record rather than inferred from page counts,
/// because "the chain is reused in place" is the specific claim under test and
/// a page total can be right for the wrong reason — freed leaf pages get
/// recycled, so a rebuilt chain need not grow the file at all.
/// @param store The store to read.
/// @param key   The cache key.
/// @return The chain head page id, or nullopt for an inline record.
[[nodiscard]] std::optional<std::uint64_t> OverflowRootOf(CowTree::IPageStore& store, std::string_view key)
{
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());
    auto const reader = tree.BeginRead();
    auto const got = reader.Get(CowTree::AsBytes(key));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());

    // Through Unwrap: clang-tidy cannot see the REQUIRE above as a guard, and
    // the dereference it DOES understand lives in there.
    auto const& stored = FastCache::Testing::Unwrap(*got);
    auto const raw = CowTree::BytesView { stored.data(), stored.size() };
    if (std::to_integer<std::uint8_t>(raw[0]) != 1) // RecordKindOverflow
        return std::nullopt;
    return LoadLe<8>(raw, 2 + CommonHeaderBytes + 16);
}

} // namespace

TEST_CASE("A v3 store converts in place, and every entry survives it", "[cowstorage][format][migrate]")
{
    // 4 KiB pages put the inline limit at 1 KiB, so the samples straddle it on
    // purpose: the small ones exercise the inline record rewrite and the large
    // ones the overflow descriptor, whose whole claim is that the chain it
    // names is reused rather than rebuilt.
    CowTree::InMemoryPageStore store { 4096 };

    struct Sample
    {
        std::string key;
        std::size_t size;
        std::uint32_t flags;
    };
    auto const samples = std::vector<Sample> {
        { .key = "inline-tiny", .size = 8, .flags = 1 },
        { .key = "inline-near-limit", .size = 900, .flags = 7 },
        { .key = "overflow-one-page", .size = 5000, .flags = 3 },
        { .key = "overflow-many-pages", .size = 40000, .flags = 42 },
    };

    std::map<std::string, std::vector<std::byte>> written;
    {
        FastCache::CowTreeStorage::Options opts;
        // v3 never compressed anything, so a store that is to be downgraded
        // must not have.
        opts.compression = FastCache::CompressionCodec::Identity;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        // Indexed by hand rather than with `std::views::enumerate`, which Apple
        // clang's libc++ does not carry -- and which nothing else in this tree
        // uses, so the one caller is not worth the portability.
        for (auto const index: std::views::iota(std::size_t { 0 }, samples.size()))
        {
            auto const& sample = samples[index];
            auto bytes = RandomBytes(sample.size, 0xA5A5ULL + index);
            REQUIRE((*storage)->Set(sample.key, bytes, sample.flags, FastCache::TimePoint::max()).has_value());
            written.emplace(sample.key, std::move(bytes));
        }
    }

    // Captured before anything is rewritten, so the assertion at the end can
    // name the very pages the chains started on.
    std::map<std::string, std::optional<std::uint64_t>> rootsBefore;
    for (auto const& sample: samples)
        rootsBefore.emplace(sample.key, OverflowRootOf(store, sample.key));
    REQUIRE(rootsBefore.at("overflow-many-pages").has_value());

    DowngradeStoreToV3(store);

    // Precondition for the whole test: it really is refused now.
    {
        FastCache::CowTreeStorage::Options opts;
        auto const refused = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error().code == FastCache::StorageErrorCode::UnsupportedFormatVersion);
    }

    auto const report = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE(report.has_value());
    REQUIRE(report->fromVersion == 3);
    REQUIRE(report->toVersion == FastCache::CowTreeStorage::CurrentFormatVersion);
    REQUIRE(report->recordsConverted == samples.size());

    FastCache::CowTreeStorage::Options opts;
    auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(storage.has_value());

    FastCache::ManualClock clock;
    for (auto const& sample: samples)
    {
        auto const got = (*storage)->Get(sample.key, clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(FastCache::Testing::ValueOf(got->entry) == written.at(sample.key));
        REQUIRE(got->entry.flags == sample.flags);
    }

    // The conversion rewrites leaf records, never value pages. Every chain must
    // still start on the page it started on — a rebuilt chain would round-trip
    // the data just as well and quietly cost a second copy of every large
    // value, which is the difference between converting a 50 GB cache and
    // needing 100 GB to do it.
    for (auto const& sample: samples)
        REQUIRE(OverflowRootOf(store, sample.key) == rootsBefore.at(sample.key));
}

TEST_CASE("An interrupted conversion is refused by name rather than mis-parsed", "[cowstorage][format][migrate]")
{
    // A part-converted store has no single format version: some records are the
    // new layout and the marker still names the old one. Reading it under
    // either would be wrong, so it says what actually happened instead.
    CowTree::InMemoryPageStore store { 4096 };
    {
        FastCache::CowTreeStorage::Options opts;
        opts.compression = FastCache::CompressionCodec::Identity;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        for (auto const i: std::views::iota(0, 6))
            REQUIRE((*storage)->Set(std::format("key-{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    }
    DowngradeStoreToV3(store, 2);

    FastCache::CowTreeStorage::Options opts;
    auto const refused = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().code == FastCache::StorageErrorCode::UnsupportedFormatVersion);
    REQUIRE(refused.error().context.contains("interrupted"));
    // And it must not read as advice to delete the store, which is the whole
    // point of the code.
    REQUIRE(refused.error().context.contains("does not need to be deleted"));
}

TEST_CASE("A conversion another build started is never picked up", "[cowstorage][format][migrate]")
{
    // The failure this marker records a target version to prevent. A newer build
    // interrupted part-way from 4 to 5 leaves a store whose FORMAT marker still
    // says 4 -- so an older build reading only the source version would find a
    // vintage it can read, "resume", re-encode the tail 4-to-4, and stamp the
    // whole store as 4 over a prefix that is really 5. That is precisely the
    // silent mis-parse the version marker exists to prevent, reached through the
    // machinery meant to prevent it.
    constexpr auto Future = FastCache::CowTreeStorage::CurrentFormatVersion + 1;

    CowTree::InMemoryPageStore store { 4096 };
    {
        FastCache::CowTreeStorage::Options opts;
        opts.compression = FastCache::CompressionCodec::Identity;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        for (auto const i: std::views::iota(0, 6))
            REQUIRE((*storage)->Set(std::format("key-{}", i), MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    }
    DowngradeStoreToV3(store, 2, Future);

    auto const pagesBefore = store.PageCount();
    auto const report = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE_FALSE(report.has_value());
    REQUIRE(report.error().code == FastCache::StorageErrorCode::UnsupportedFormatVersion);
    REQUIRE(report.error().context.contains("finish it with the build that started it"));
    // Refused without writing: the check happens before the first slice opens a
    // transaction.
    REQUIRE(store.PageCount() == pagesBefore);

    // And Open says the same thing rather than the resumable-here message.
    FastCache::CowTreeStorage::Options opts;
    auto const refused = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().context.contains("finish it with the build that started it"));
}

TEST_CASE("Resuming converts only what is left, and never the same record twice", "[cowstorage][format][migrate]")
{
    // Converting an already-converted record would read a v4 record with the v3
    // reader: it would parse (the layouts overlap) and silently produce wrong
    // flags and a wrong value length. The value assertions below are what catch
    // that, so this test fails loudly if the resume point is off by one.
    constexpr int Total = 6;
    constexpr std::size_t AlreadyDone = 2;

    CowTree::InMemoryPageStore store { 4096 };
    std::map<std::string, std::vector<std::byte>> written;
    {
        FastCache::CowTreeStorage::Options opts;
        opts.compression = FastCache::CompressionCodec::Identity;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        for (auto const i: std::views::iota(0, Total))
        {
            auto const key = std::format("key-{}", i);
            auto bytes = RandomBytes(64 + static_cast<std::size_t>(i), 0xBEEFULL + static_cast<std::uint64_t>(i));
            REQUIRE((*storage)->Set(key, bytes, static_cast<std::uint32_t>(i) + 1, FastCache::TimePoint::max()).has_value());
            written.emplace(key, std::move(bytes));
        }
    }
    DowngradeStoreToV3(store, AlreadyDone);

    auto const report = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE(report.has_value());
    REQUIRE(report->resumed);
    REQUIRE(report->fromVersion == 3);
    REQUIRE(report->recordsConverted == Total - AlreadyDone);

    FastCache::CowTreeStorage::Options opts;
    auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;
    for (auto const i: std::views::iota(0, Total))
    {
        auto const key = std::format("key-{}", i);
        auto const got = (*storage)->Get(key, clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(FastCache::Testing::ValueOf(got->entry) == written.at(key));
        REQUIRE(got->entry.flags == static_cast<std::uint32_t>(i) + 1);
    }
}

TEST_CASE("A conversion killed part-way finishes correctly on a re-run", "[cowstorage][format][migrate]")
{
    // The end-to-end property the slicing exists to preserve: whatever write
    // fails and wherever it fails, running the conversion again produces a
    // store in which every entry is intact. Driven through the page store's
    // fault injection rather than by hand, so the interrupted state is one the
    // real code path actually produces.
    constexpr int Total = 2500; // more than MigrationChunkRecords, so it slices

    for (auto const failAt: { std::size_t { 300 }, std::size_t { 3000 }, std::size_t { 9000 } })
    {
        CowTree::InMemoryPageStore store { 4096 };
        std::map<std::string, std::vector<std::byte>> written;
        {
            FastCache::CowTreeStorage::Options opts;
            opts.compression = FastCache::CompressionCodec::Identity;
            auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
            REQUIRE(storage.has_value());
            for (auto const i: std::views::iota(0, Total))
            {
                auto const key = std::format("key-{:05d}", i);
                auto bytes = MakeBytes(std::format("value-{}", i));
                REQUIRE((*storage)->Set(key, bytes, static_cast<std::uint32_t>(i), FastCache::TimePoint::max()).has_value());
                written.emplace(key, std::move(bytes));
            }
        }
        DowngradeStoreToV3(store);

        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.failNthWrite = failAt;
        store.SetFaultPlan(plan);
        // It may fail or (if the store needed fewer writes than that) succeed;
        // either way the store must end up correct after the retry below.
        std::ignore = FastCache::CowTreeStorage::MigrateStore(store);

        store.SetFaultPlan(CowTree::InMemoryPageStore::FaultPlan {});
        auto const retry = FastCache::CowTreeStorage::MigrateStore(store);
        INFO("failAt=" << failAt);
        REQUIRE(retry.has_value());

        FastCache::CowTreeStorage::Options opts;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        FastCache::ManualClock clock;
        for (auto const i: std::views::iota(0, Total))
        {
            auto const key = std::format("key-{:05d}", i);
            auto const got = (*storage)->Get(key, clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(FastCache::Testing::ValueOf(got->entry) == written.at(key));
            REQUIRE(std::cmp_equal(got->entry.flags, i));
        }
    }
}

TEST_CASE("Converting a store costs a slice of headroom, not a multiple of the store", "[cowstorage][format][migrate]")
{
    // The reason the conversion commits in slices at all. One transaction over
    // the whole store allocates a page per record per tree level and can reuse
    // none of them until it commits -- and since `CommitTxn` writes
    // `freeRoot = PageId::None()`, that growth is not reclaimable by the next
    // process either. Slicing bounds it: each slice returns its replaced pages
    // to the free list and the next one allocates out of that.
    //
    // Asserted as a SHAPE rather than a number, because that is the actual
    // claim. Quadrupling the records must not quadruple the growth: whatever
    // the conversion costs, it costs about a slice, and a slice is a constant.
    // Measured on real files, opened fresh for the conversion exactly as the
    // tool does -- an in-memory store carries a warm free list that hides the
    // whole effect.
    auto const growthFor = [](int total) {
        TempFile tmp;
        FastCache::CowTreeStorage::Options opts;
        opts.path = tmp.path;
        opts.pageSize = 512; // small pages keep the fixture's files small
        opts.compression = FastCache::CompressionCodec::Identity;

        std::map<std::string, std::vector<std::byte>> written;
        {
            auto storage = FastCache::CowTreeStorage::Open(opts);
            REQUIRE(storage.has_value());
            for (auto const i: std::views::iota(0, total))
            {
                auto const key = std::format("key-{:05d}", i);
                auto bytes = MakeBytes(std::format("value-{}", i));
                REQUIRE((*storage)->Set(key, bytes, 0, FastCache::TimePoint::max()).has_value());
                written.emplace(key, std::move(bytes));
            }
        }
        {
            auto pageStore = CowTree::FilePageStore::Open(CowTree::FilePageStore::Options { .path = tmp.path });
            REQUIRE(pageStore.has_value());
            DowngradeStoreToV3(**pageStore);
        }

        auto const before = std::filesystem::file_size(tmp.path);
        auto const report = FastCache::CowTreeStorage::Migrate(opts);
        REQUIRE(report.has_value());
        REQUIRE(std::cmp_equal(report->recordsConverted, total));
        auto const after = std::filesystem::file_size(tmp.path);
        REQUIRE(after >= before);

        // ...and it converted everything, which is the other half of the claim:
        // a conversion that grew by nothing because it did nothing is not the
        // property under test.
        auto storage = FastCache::CowTreeStorage::Open(opts);
        REQUIRE(storage.has_value());
        FastCache::ManualClock clock;
        for (auto const& [key, value]: written)
        {
            auto const got = (*storage)->Get(key, clock.Now());
            REQUIRE(got.has_value());
            REQUIRE(got->found);
            REQUIRE(FastCache::Testing::ValueOf(got->entry) == value);
        }
        return after - before;
    };

    auto const small = growthFor(2000);
    auto const large = growthFor(8000);

    // Four times the records. A conversion that allocated per record would
    // grow four times as much; one bounded by a slice grows about the same.
    INFO("growth: 2000 records -> " << small << " bytes, 8000 records -> " << large << " bytes");
    REQUIRE(large < small * 2);
}

TEST_CASE("A record whose expiry does not fit the clock is decoded, not undefined", "[cowstorage][format]")
{
    // The timestamps come off DISK, and the clock counts in nanoseconds, so
    // building a time point from a microsecond count multiplies by a thousand.
    // A damaged field near the type's range therefore overflows a signed
    // integer -- undefined behaviour rather than a wrong timestamp, on a path
    // any corrupt record can reach. Only a sanitizer sees it, which is why this
    // pins the value rather than the absence of a crash.
    CowTree::InMemoryPageStore store { 4096 };
    {
        FastCache::CowTreeStorage::Options opts;
        opts.compression = FastCache::CompressionCodec::Identity;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    }

    // Rewrite the record's expiry field to a microsecond count that is huge but
    // not the "never expires" sentinel, so the conversion below is a real one.
    constexpr std::int64_t Overflowing = (std::numeric_limits<std::int64_t>::max() / 16) + 1;
    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());
        std::vector<std::byte> record;
        {
            auto const reader = tree.BeginRead();
            auto const got = reader.Get(CowTree::AsBytes("k"));
            REQUIRE(got.has_value());
            REQUIRE(got->has_value());
            record = FastCache::Testing::Unwrap(*got);
        }
        // kind(1) + codec(1) + flags(4) + cas(8) puts expiry at offset 14.
        constexpr std::size_t ExpiryOffset = 1 + 1 + 4 + 8;
        REQUIRE(record.size() > ExpiryOffset + sizeof(std::int64_t));
        auto const raw = static_cast<std::uint64_t>(Overflowing);
        for (auto const i: std::views::iota(std::size_t { 0 }, sizeof(std::uint64_t)))
            record[ExpiryOffset + i] = static_cast<std::byte>((raw >> (8U * i)) & 0xFFU);

        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(CowTree::AsBytes("k"), CowTree::BytesView { record.data(), record.size() }).has_value());
        REQUIRE(txn.Commit().has_value());
    }

    FastCache::CowTreeStorage::Options opts;
    auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(storage.has_value());

    // Clamped to "never", which is the nearest representable instant, and the
    // entry is still readable rather than the process being in undefined
    // behaviour.
    auto const got = (*storage)->Peek("k", FastCache::ManualClock {}.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(got->entry.expiry == FastCache::TimePoint::max());
}

TEST_CASE("A store older than any reader is refused, not rewritten in place", "[cowstorage][format][migrate]")
{
    // "No marker" is an inference, not a statement: it means v3, because that
    // is the last layout that did not stamp one -- but a store from before v3
    // is equally unmarked, and its records begin with a flags field rather than
    // a kind tag, so a zero-flags record starts with the same byte as an inline
    // one. Accepting it would be the worst outcome this feature can have: a
    // store the daemon merely refused to open, destroyed in place by the thing
    // meant to rescue it.
    CowTree::InMemoryPageStore store { 4096 };

    // A record in the pre-v3 layout -- no kind byte -- and specifically one
    // that a LENIENT reader accepts. Every field lands one byte early, so the
    // four bytes the reader takes for the inline length are the top half of the
    // real length field followed by the first two value bytes. A value that
    // begins with two zero bytes therefore reads as length zero, which a reader
    // that only checks "are there at least that many bytes left" is happy with:
    // it returns an empty value, and the conversion writes that empty value
    // over the real one. Requiring the record to end exactly where its value
    // does is what rejects it.
    std::vector<std::byte> ancient;
    AppendLe<4>(ancient, 0);  // flags
    AppendLe<8>(ancient, 11); // cas
    AppendLe<8>(ancient, 0);  // expiry
    AppendLe<8>(ancient, 0);  // generation
    AppendLe<8>(ancient, 0);  // lastAccess
    AppendLe<4>(ancient, 8);  // value length
    for (auto const b: { 0x00, 0x00, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x21 })
        ancient.push_back(static_cast<std::byte>(b));

    auto const key = std::string_view { "ancient-key" };
    PutRaw(store,
           CowTree::BytesView { reinterpret_cast<std::byte const*>(key.data()), key.size() },
           CowTree::BytesView { ancient.data(), ancient.size() });

    auto const before = store.PageCount();
    auto const report = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE_FALSE(report.has_value());
    REQUIRE(report.error().code == FastCache::StorageErrorCode::UnsupportedFormatVersion);
    REQUIRE(report.error().context.contains("NOT been modified"));

    // Refused before writing anything: the validation pass runs to completion
    // before the first slice opens a transaction.
    REQUIRE(store.PageCount() == before);
    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());
        auto const reader = tree.BeginRead();
        auto const got = reader.Get(CowTree::AsBytes(key));
        REQUIRE(got.has_value());
        REQUIRE(got->has_value());
        REQUIRE(FastCache::Testing::Unwrap(*got) == ancient);
    }
}

TEST_CASE("Converting a store already at the current version changes nothing", "[cowstorage][format][migrate]")
{
    CowTree::InMemoryPageStore store;
    {
        FastCache::CowTreeStorage::Options opts;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    }

    auto const report = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE(report.has_value());
    REQUIRE(report->fromVersion == FastCache::CowTreeStorage::CurrentFormatVersion);
    REQUIRE(report->toVersion == report->fromVersion);
    REQUIRE(report->recordsConverted == 0);
}

TEST_CASE("Converting a store nothing has written yet is a no-op, not a failure", "[cowstorage][format][migrate]")
{
    // An operator pointing the conversion at a path the daemon has not created
    // yet should read "nothing to do", not a diagnostic they have to interpret.
    CowTree::InMemoryPageStore store;
    auto const report = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE(report.has_value());
    REQUIRE(report->fromVersion == FastCache::CowTreeStorage::CurrentFormatVersion);
    REQUIRE(report->recordsConverted == 0);
}

TEST_CASE("A store newer than this build cannot be converted forwards", "[cowstorage][format][migrate]")
{
    // There is no reader for a layout that does not exist yet, so the honest
    // answer is the same refusal Open gives — not a conversion that guesses.
    CowTree::InMemoryPageStore store;
    StampFormatVersion(store, FastCache::CowTreeStorage::CurrentFormatVersion + 1);

    auto const report = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE_FALSE(report.has_value());
    REQUIRE(report.error().code == FastCache::StorageErrorCode::UnsupportedFormatVersion);
    REQUIRE(report.error().context.contains("upgrade"));
}

TEST_CASE("A converted store keeps working as a cache", "[cowstorage][format][migrate]")
{
    // Converting must leave a store that is writable, not merely readable: the
    // marker has to be committed alongside the records, or the next Open stamps
    // nothing and the LRU accounting starts from a tree it never replayed.
    CowTree::InMemoryPageStore store { 4096 };
    {
        FastCache::CowTreeStorage::Options opts;
        opts.compression = FastCache::CompressionCodec::Identity;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("old", MakeBytes("value"), 0, FastCache::TimePoint::max()).has_value());
    }
    DowngradeStoreToV3(store);
    REQUIRE(FastCache::CowTreeStorage::MigrateStore(store).has_value());

    FastCache::CowTreeStorage::Options opts;
    auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(storage.has_value());

    FastCache::ManualClock clock;
    REQUIRE((*storage)->Set("new", MakeBytes("written-after"), 0, FastCache::TimePoint::max()).has_value());
    REQUIRE((*storage)->Delete("old", clock.Now()).has_value());

    auto const fresh = (*storage)->Get("new", clock.Now());
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->found);
    REQUIRE(Decode(fresh->entry.ValueBytes()) == "written-after");

    auto const gone = (*storage)->Get("old", clock.Now());
    REQUIRE(gone.has_value());
    REQUIRE_FALSE(gone->found);
}

TEST_CASE("Converting a second time reports nothing left to do", "[cowstorage][format][migrate]")
{
    CowTree::InMemoryPageStore store { 4096 };
    {
        FastCache::CowTreeStorage::Options opts;
        opts.compression = FastCache::CompressionCodec::Identity;
        auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    }
    DowngradeStoreToV3(store);

    auto const first = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE(first.has_value());
    REQUIRE(first->recordsConverted == 1);

    // Idempotent, which is what makes it safe to put in a start-up script.
    auto const second = FastCache::CowTreeStorage::MigrateStore(store);
    REQUIRE(second.has_value());
    REQUIRE(second->fromVersion == second->toVersion);
    REQUIRE(second->recordsConverted == 0);
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
    REQUIRE(Decode(got->entry.ValueBytes()) == "payload");
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
        REQUIRE(ValueOf(got->entry) == value); // exact bytewise round-trip via the chain
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
    REQUIRE(ValueOf(got->entry) == RandomBytes(256 * 1024, 111)); // last write wins
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
    REQUIRE(Decode(got->entry.ValueBytes()) == "initial"); // rolled back, never a hybrid
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
    REQUIRE(Decode(got->entry.ValueBytes()) == "initial");
}

TEST_CASE("Reclaiming a corrupted overflow chain never frees another key's pages", "[cowstorage][overflow][crash]")
{
    // Regression for the FreeChain cross-key-corruption finding: FreeChain must
    // validate each overflow page's CRC before trusting its `next` link. Here we
    // point target's chain links at a victim page and break their CRC; if the
    // reclaim path followed the corrupt link it would free the victim's pages,
    // which a later allocation would overwrite.
    CowTree::InMemoryPageStore store { 4096 };
    FastCache::CowTreeStorage::Options opts;
    opts.maxValueBytes = 1024 * 1024;
    opts.durability = CowTree::FilePageStore::Durability::Fsync; // free pages reusable immediately

    auto const victimBytes = RandomBytes(50000, 0x5151ULL);
    auto const targetBytes = RandomBytes(50000, 0x7272ULL);

    auto liveOverflowPages = [&](std::uint64_t maxScan) {
        std::vector<std::uint64_t> ids;
        for (std::uint64_t i = 1; i <= maxScan; ++i)
        {
            auto const view = store.Read(CowTree::PageId { i });
            if (view.has_value() && LooksLikeOverflowPage(*view, store.PageSize()))
                ids.push_back(i);
        }
        return ids;
    };

    auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(storage.has_value());

    REQUIRE((*storage)->Set("victim", victimBytes, 0, FastCache::TimePoint::max()).has_value());
    auto const victimPages = liveOverflowPages(2048);
    REQUIRE_FALSE(victimPages.empty());
    auto const victimPage = victimPages.front();

    REQUIRE((*storage)->Set("target", targetBytes, 0, FastCache::TimePoint::max()).has_value());
    std::vector<std::uint64_t> targetPages;
    for (auto const id: liveOverflowPages(2048))
        if (std::ranges::find(victimPages, id) == victimPages.end())
            targetPages.push_back(id);
    REQUIRE_FALSE(targetPages.empty());

    // Point every target overflow page's `next` at a victim page. Because the
    // CRC now covers `next`, this also breaks each page's checksum.
    for (auto const id: targetPages)
    {
        auto const view = store.Read(CowTree::PageId { id });
        REQUIRE(view.has_value());
        std::vector<std::byte> page(view->begin(), view->end());
        StoreLeU64(page, sizeof(std::uint32_t), victimPage); // next := a victim page id
        REQUIRE(store.Write(CowTree::PageId { id }, CowTree::BytesView { page.data(), page.size() }).has_value());
    }

    // Overwrite target with a tiny inline value: this reclaims the (now corrupt)
    // overflow chain. The CRC guard must stop the walk at the first bad page.
    REQUIRE((*storage)->Set("target", MakeBytes("small"), 0, FastCache::TimePoint::max()).has_value());
    // Churn allocations so a wrongly-freed victim page would be reused + clobbered.
    for (auto const i: std::views::iota(0U, 8U))
        REQUIRE((*storage)
                    ->Set(std::format("filler-{}", i), RandomBytes(50000, 900U + i), 0, FastCache::TimePoint::max())
                    .has_value());

    // The victim's bytes survive intact: its pages were never freed.
    auto const got = (*storage)->Get("victim", FastCache::ManualClock {}.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(ValueOf(got->entry) == victimBytes);
}

TEST_CASE("Touch on a large value reuses the overflow chain without an O(value) rewrite", "[cowstorage][overflow][touch]")
{
    CowTree::InMemoryPageStore store { 4096 }; // inline limit 1024
    FastCache::CowTreeStorage::Options opts;
    opts.maxValueBytes = 1024 * 1024;
    opts.durability = CowTree::FilePageStore::Durability::Fsync;

    auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(storage.has_value());
    FastCache::ManualClock clock;

    auto const big = RandomBytes(50000, 0xABCDULL); // ~13 overflow pages at 4 KiB
    REQUIRE((*storage)->Set("k", big, 0, clock.Now() + 1s).has_value());

    auto const writesBefore = store.WriteCount();
    auto const newExpiry = clock.Now() + 3600s;
    auto const touched = (*storage)->Touch("k", newExpiry, clock.Now());
    REQUIRE(touched.has_value());
    auto const writesByTouch = store.WriteCount() - writesBefore;

    // Reusing the chain rewrites only the small descriptor leaf; a chain rewrite
    // would write the whole ~13-page chain. Guard well below that.
    REQUIRE(writesByTouch < 8);

    // The value is intact and the expiry was refreshed without touching the chain.
    auto const got = (*storage)->Get("k", clock.Now() + 1800s);
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(ValueOf(got->entry) == big);
    REQUIRE(got->entry.expiry == newExpiry);
}

// ============================================================================
// On-disk value compression (per-entry codec tag, v4 record format)
// ============================================================================

namespace
{

/// A highly compressible payload of `size` bytes: a short repeated pattern so
/// zstd/lz4 shrink it dramatically. Deterministic (no baked-in literal).
std::vector<std::byte> CompressibleBytes(std::size_t size)
{
    static constexpr std::string_view pattern = "fastcached-compresses-this-well-0123456789 ";
    std::vector<std::byte> out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(pattern[i % pattern.size()])));
    return out;
}

/// Current on-disk size of the store file at `path`.
std::uintmax_t StoreFileSize(std::filesystem::path const& path)
{
    std::error_code ec;
    auto const size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

} // namespace

TEST_CASE("Compression: values round-trip under every codec, inline and overflow", "[cowstorage][compression]")
{
    auto const codec =
        GENERATE(FastCache::CompressionCodec::Identity, FastCache::CompressionCodec::Lz4, FastCache::CompressionCodec::Zstd);
    if (!FastCache::Compression::IsAvailable(codec))
        return;

    TempFile tmp;
    // Small value stays inline; large value spills to an overflow chain.
    auto const small = CompressibleBytes(300);
    auto const large = CompressibleBytes(64 * 1024);

    WithOpenStorageCompressed(tmp.path, codec, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("small", small, 1, FastCache::TimePoint::max()).has_value());
        REQUIRE(storage.Set("large", large, 2, FastCache::TimePoint::max()).has_value());
    });

    // Reopen (fresh LRU mirror, values materialised from disk) and verify the
    // plaintext survives a decompress round-trip.
    WithOpenStorageCompressed(tmp.path, codec, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        auto gotSmall = storage.Get("small", clock.Now());
        REQUIRE(gotSmall.has_value());
        REQUIRE(gotSmall->found);
        REQUIRE(ValueOf(gotSmall->entry) == small);
        REQUIRE(gotSmall->entry.flags == 1U);

        auto gotLarge = storage.Get("large", clock.Now());
        REQUIRE(gotLarge.has_value());
        REQUIRE(gotLarge->found);
        REQUIRE(ValueOf(gotLarge->entry) == large);
        REQUIRE(gotLarge->entry.flags == 2U);
    });
}

TEST_CASE("Compression: a compressible value shrinks the on-disk file", "[cowstorage][compression]")
{
    if (!FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Zstd))
        return;

    auto const value = CompressibleBytes(128 * 1024);

    TempFile rawFile;
    WithOpenStorageCompressed(rawFile.path, FastCache::CompressionCodec::Identity, [&](FastCache::CowTreeStorage& s) {
        REQUIRE(s.Set("k", value, 0, FastCache::TimePoint::max()).has_value());
    });

    TempFile zstdFile;
    WithOpenStorageCompressed(zstdFile.path, FastCache::CompressionCodec::Zstd, [&](FastCache::CowTreeStorage& s) {
        REQUIRE(s.Set("k", value, 0, FastCache::TimePoint::max()).has_value());
    });

    CHECK(StoreFileSize(zstdFile.path) < StoreFileSize(rawFile.path));
}

TEST_CASE("Compression: an incompressible value falls back to Identity (shrink-check)", "[cowstorage][compression]")
{
    if (!FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Zstd))
        return;

    // Random bytes do not compress; the shrink-check must store them verbatim,
    // so the zstd store is not meaningfully larger than the identity store.
    auto const value = RandomBytes(64 * 1024, 0xFEEDFACEULL);

    TempFile rawFile;
    WithOpenStorageCompressed(rawFile.path, FastCache::CompressionCodec::Identity, [&](FastCache::CowTreeStorage& s) {
        REQUIRE(s.Set("k", value, 0, FastCache::TimePoint::max()).has_value());
    });
    TempFile zstdFile;
    WithOpenStorageCompressed(zstdFile.path, FastCache::CompressionCodec::Zstd, [&](FastCache::CowTreeStorage& s) {
        REQUIRE(s.Set("k", value, 0, FastCache::TimePoint::max()).has_value());
        // Value still reads back correctly despite the Identity fallback.
        auto got = s.Get("k", FastCache::ManualClock {}.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(ValueOf(got->entry) == value);
    });

    // The compressed store must not be dramatically larger than the raw one
    // (a naive "always compress" would inflate incompressible data).
    CHECK(StoreFileSize(zstdFile.path) <= StoreFileSize(rawFile.path) + 4096);
}

TEST_CASE("Compression: Append/Prepend on a compressed entry yield correct plaintext", "[cowstorage][compression]")
{
    if (!FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Zstd))
        return;

    TempFile tmp;
    WithOpenStorageCompressed(tmp.path, FastCache::CompressionCodec::Zstd, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        auto const base = CompressibleBytes(2000);
        REQUIRE(storage.Set("k", base, 0, FastCache::TimePoint::max()).has_value());

        auto const suffix = MakeBytes("-SUFFIX");
        REQUIRE(
            storage.Append("k", std::span<std::byte const> { suffix.data(), suffix.size() }, 0, clock.Now()).has_value());
        auto const prefix = MakeBytes("PREFIX-");
        REQUIRE(
            storage.Prepend("k", std::span<std::byte const> { prefix.data(), prefix.size() }, 0, clock.Now()).has_value());

        std::vector<std::byte> expected;
        expected.insert(expected.end(), prefix.begin(), prefix.end());
        expected.insert(expected.end(), base.begin(), base.end());
        expected.insert(expected.end(), suffix.begin(), suffix.end());

        auto got = storage.Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(ValueOf(got->entry) == expected);
    });
}

TEST_CASE("Compression: Touch preserves the compressed value unchanged", "[cowstorage][compression]")
{
    if (!FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Zstd))
        return;

    TempFile tmp;
    auto const value = CompressibleBytes(64 * 1024);
    WithOpenStorageCompressed(tmp.path, FastCache::CompressionCodec::Zstd, [&](FastCache::CowTreeStorage& storage) {
        using namespace std::chrono_literals;
        FastCache::ManualClock clock;
        REQUIRE(storage.Set("k", value, 5, clock.Now() + 1s).has_value());

        auto const newExpiry = clock.Now() + 3600s;
        REQUIRE(storage.Touch("k", newExpiry, clock.Now()).has_value());

        auto got = storage.Get("k", clock.Now() + 60s);
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(ValueOf(got->entry) == value); // value survived the metadata rewrite
        REQUIRE(got->entry.expiry == newExpiry);
        REQUIRE(got->entry.flags == 5U);
    });
}

TEST_CASE("Compression: a store mixes codecs and reads each by its own tag", "[cowstorage][compression]")
{
    if (!FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Lz4)
        || !FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Zstd))
        return;

    TempFile tmp;
    auto const lz4Value = CompressibleBytes(8000);
    auto const zstdValue = CompressibleBytes(9000);

    // First session writes an lz4-tagged entry.
    WithOpenStorageCompressed(tmp.path, FastCache::CompressionCodec::Lz4, [&](FastCache::CowTreeStorage& storage) {
        REQUIRE(storage.Set("lz4key", lz4Value, 0, FastCache::TimePoint::max()).has_value());
    });

    // Second session is configured for zstd. The old lz4 entry must still read
    // (decoded by its own per-entry tag), and a new entry is written as zstd.
    WithOpenStorageCompressed(tmp.path, FastCache::CompressionCodec::Zstd, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        auto oldGot = storage.Get("lz4key", clock.Now());
        REQUIRE(oldGot.has_value());
        REQUIRE(oldGot->found);
        REQUIRE(ValueOf(oldGot->entry) == lz4Value);

        REQUIRE(storage.Set("zstdkey", zstdValue, 0, FastCache::TimePoint::max()).has_value());
    });

    // Reopen once more: both entries survive and decode correctly.
    WithOpenStorageCompressed(tmp.path, FastCache::CompressionCodec::Zstd, [&](FastCache::CowTreeStorage& storage) {
        FastCache::ManualClock clock;
        auto a = storage.Get("lz4key", clock.Now());
        REQUIRE(a.has_value());
        REQUIRE(a->found);
        REQUIRE(ValueOf(a->entry) == lz4Value);
        auto b = storage.Get("zstdkey", clock.Now());
        REQUIRE(b.has_value());
        REQUIRE(b->found);
        REQUIRE(ValueOf(b->entry) == zstdValue);
    });
}

// ============================================================================
// The exclusive claim, as the storage layer reports it
// ============================================================================

TEST_CASE("A second CowTreeStorage on one path reports InUse, not IoError", "[cowstorage][open][lock]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;

    auto first = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(first.has_value());

    // The distinction IS the fix. `IoError` sends an operator to look at the
    // disk, the permissions and the file; `InUse` tells them the truth, which
    // is that a second process is already serving this path.
    auto second = FastCache::CowTreeStorage::Open(opts);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().code == FastCache::StorageErrorCode::InUse);
    REQUIRE(FastCache::ToStringView(second.error().code) == "InUse");
}

TEST_CASE("An opened CowTreeStorage reports holding its file", "[cowstorage][open][lock]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;

    auto storage = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(storage.has_value());
    auto const state = (*storage)->StoreLockState();
    REQUIRE(state.has_value());
    REQUIRE(Unwrap(state) == CowTree::FilePageStore::LockState::Held);
}

TEST_CASE("A CowTreeStorage over a borrowed page store reports no claim at all", "[cowstorage][open][lock]")
{
    // Absent is not zero: there is no file here, so "not claimed" would be a
    // false statement about a store that never had one to take.
    CowTree::InMemoryPageStore store;
    FastCache::CowTreeStorage::Options opts;
    auto storage = FastCache::CowTreeStorage::OpenBorrowing(opts, store);
    REQUIRE(storage.has_value());
    REQUIRE_FALSE((*storage)->StoreLockState().has_value());
}

TEST_CASE("Closing a CowTreeStorage releases the path for the next one", "[cowstorage][open][lock]")
{
    TempFile tmp;
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;

    {
        auto first = FastCache::CowTreeStorage::Open(opts);
        REQUIRE(first.has_value());
        REQUIRE((*first)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    }

    // Every restart goes through here, so the claim must not outlive the object
    // that took it -- and the data written under it must still be there.
    auto second = FastCache::CowTreeStorage::Open(opts);
    REQUIRE(second.has_value());
    FastCache::ManualClock clock;
    auto const got = (*second)->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
}
