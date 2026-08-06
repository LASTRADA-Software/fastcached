// SPDX-License-Identifier: Apache-2.0
//
// In-memory (L1) compression: values are held compressed and the byte budget counts
// the compressed size, so the tier holds far more entries per byte of RAM. Every
// read must still hand back the original plaintext.

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;

namespace
{

TimePoint Never()
{
    return TimePoint::max();
}

TimePoint Now()
{
    return TimePoint {} + std::chrono::seconds { 1000 };
}

/// A payload that compresses well, mimicking the repetitive symbol and debug-info
/// text that dominates a real compiled object.
std::vector<std::byte> CompressiblePayload(std::size_t bytes)
{
    std::string text;
    while (text.size() < bytes)
        text += "void __cdecl FastCache::Repeated::Symbol(int, char const*, double); ";
    text.resize(bytes);
    std::vector<std::byte> out(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        out[i] = static_cast<std::byte>(text[i]);
    return out;
}

/// Random-ish payload that will not compress, to exercise the "keep plaintext" path.
std::vector<std::byte> IncompressiblePayload(std::size_t bytes)
{
    std::vector<std::byte> out(bytes);
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (auto& b: out)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        b = static_cast<std::byte>(state & 0xFFULL);
    }
    return out;
}

InMemoryLruStorage::CompressionOptions ZstdOptions()
{
    return { .codec = CompressionCodec::Zstd, .level = 3, .minBytes = 1024 };
}

bool CodecUsable()
{
    return Compression::IsAvailable(CompressionCodec::Zstd);
}

} // namespace

TEST_CASE("L1 compression returns the original plaintext on read", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return; // zstd not built in; the tier stores verbatim and other tests cover that

    InMemoryLruStorage storage { 0, 0, LruMode::Strict };
    storage.SetCompression(ZstdOptions());

    auto const payload = CompressiblePayload(64 * 1024);
    REQUIRE(storage.Set("k", payload, 0, Never()).has_value());

    auto const got = storage.Get("k", Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    auto const bytes = got->entry.ValueBytes();
    REQUIRE(bytes.size() == payload.size());
    CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == payload);
}

TEST_CASE("L1 compression makes the byte budget hold more entries", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return;

    // A budget far below the plaintext total: uncompressed, only a couple of values
    // survive; compressed, all of them should.
    constexpr std::size_t ValueBytes = 64 * 1024;
    constexpr std::size_t Values = 16;
    constexpr std::size_t Budget = 6 * ValueBytes;

    auto const payload = CompressiblePayload(ValueBytes);

    InMemoryLruStorage plain { Budget, 0, LruMode::Strict };
    InMemoryLruStorage packed { Budget, 0, LruMode::Strict };
    packed.SetCompression(ZstdOptions());

    for (std::size_t i = 0; i < Values; ++i)
    {
        auto const key = "key" + std::to_string(i);
        REQUIRE(plain.Set(key, payload, 0, Never()).has_value());
        REQUIRE(packed.Set(key, payload, 0, Never()).has_value());
    }

    auto survivors = [&](InMemoryLruStorage& s) {
        std::size_t alive = 0;
        for (std::size_t i = 0; i < Values; ++i)
        {
            auto const got = s.Get("key" + std::to_string(i), Now());
            if (got.has_value() && got->found)
                ++alive;
        }
        return alive;
    };

    auto const alivePlain = survivors(plain);
    auto const alivePacked = survivors(packed);

    // The whole point: the same RAM budget retains strictly more entries.
    CHECK(alivePacked > alivePlain);
    CHECK(alivePacked == Values);
}

TEST_CASE("L1 compression keeps plaintext when a value does not shrink", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return;

    // Compressing random data yields something larger; storing that would waste both
    // bytes and a decompress on every read, so the plaintext must be kept instead.
    InMemoryLruStorage storage { 0, 0, LruMode::Strict };
    storage.SetCompression(ZstdOptions());

    auto const payload = IncompressiblePayload(32 * 1024);
    REQUIRE(storage.Set("k", payload, 0, Never()).has_value());

    auto const got = storage.Get("k", Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    auto const bytes = got->entry.ValueBytes();
    CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == payload);
    CHECK(storage.Snapshot().bytesUsed <= payload.size());
}

TEST_CASE("L1 compression leaves values below the minimum uncompressed", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return;

    InMemoryLruStorage storage { 0, 0, LruMode::Strict };
    storage.SetCompression({ .codec = CompressionCodec::Zstd, .level = 3, .minBytes = 1 << 20 });

    auto const payload = CompressiblePayload(4096); // below minBytes
    REQUIRE(storage.Set("k", payload, 0, Never()).has_value());
    CHECK(storage.Snapshot().bytesUsed == payload.size());

    auto const got = storage.Get("k", Now());
    REQUIRE(got.has_value());
    auto const bytes = got->entry.ValueBytes();
    CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == payload);
}

TEST_CASE("L1 compression round-trips through Append and Prepend", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return;

    // Append/Prepend read the current value before rewriting it. On a compressed tier
    // that read must decompress first, or the concatenation would splice plaintext
    // onto compressed bytes and produce an undecodable value.
    InMemoryLruStorage storage { 0, 0, LruMode::Strict };
    storage.SetCompression(ZstdOptions());

    auto const base = CompressiblePayload(8192);
    REQUIRE(storage.Set("k", base, 0, Never()).has_value());

    std::vector<std::byte> const suffix { std::byte { 'X' }, std::byte { 'Y' } };
    REQUIRE(storage.Append("k", suffix, 0, Now()).has_value());

    std::vector<std::byte> const prefix { std::byte { 'A' } };
    REQUIRE(storage.Prepend("k", prefix, 0, Now()).has_value());

    std::vector<std::byte> expected;
    expected.insert(expected.end(), prefix.begin(), prefix.end());
    expected.insert(expected.end(), base.begin(), base.end());
    expected.insert(expected.end(), suffix.begin(), suffix.end());

    auto const got = storage.Get("k", Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    auto const bytes = got->entry.ValueBytes();
    CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == expected);
}

TEST_CASE("L1 compression re-encodes on overwrite", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return;

    // A compressible value replaced by an incompressible one (and vice versa) must
    // both read back correctly: the codec tag has to follow the new bytes, not the old.
    InMemoryLruStorage storage { 0, 0, LruMode::Strict };
    storage.SetCompression(ZstdOptions());

    auto const compressible = CompressiblePayload(16384);
    auto const incompressible = IncompressiblePayload(16384);

    REQUIRE(storage.Set("k", compressible, 0, Never()).has_value());
    REQUIRE(storage.Set("k", incompressible, 0, Never()).has_value());
    {
        auto const got = storage.Get("k", Now());
        REQUIRE(got.has_value());
        auto const bytes = got->entry.ValueBytes();
        CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == incompressible);
    }

    REQUIRE(storage.Set("k", compressible, 0, Never()).has_value());
    {
        auto const got = storage.Get("k", Now());
        REQUIRE(got.has_value());
        auto const bytes = got->entry.ValueBytes();
        CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == compressible);
    }
}

TEST_CASE("L1 compression applies to InsertVerbatim mirrors", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return;

    // InsertVerbatim is how LayeredStorage mirrors every L2 read into L1 — the
    // dominant write path for a persistent cache. It once bypassed the codec, which
    // left the in-memory tier fully uncompressed in production while the unit tests
    // (which use Set) all passed. Mirrored values must compress too, and must still
    // read back as plaintext.
    InMemoryLruStorage storage { 0, 0, LruMode::Strict };
    storage.SetCompression(ZstdOptions());

    auto const payload = CompressiblePayload(64 * 1024);
    CacheEntry mirrored;
    mirrored.value = MakeSharedValue(payload);
    mirrored.expiry = Never();
    // InsertVerbatim preserves the caller's identity fields, so the entry must carry
    // a live generation (a fresh storage starts at 1) or it reads as flushed.
    mirrored.generation = 1;
    storage.InsertVerbatim("mirror", std::move(mirrored));

    // The budget must reflect the COMPRESSED size, or the tier gains no capacity.
    CHECK(storage.Snapshot().bytesUsed < payload.size());

    auto const got = storage.Get("mirror", Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    auto const bytes = got->entry.ValueBytes();
    REQUIRE(bytes.size() == payload.size());
    CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == payload);
}

TEST_CASE("L1 compression works in Approximate (shared-read) mode", "[cache][lru][compression]")
{
    if (!CodecUsable())
        return;

    // The shared-read path returns a decompressed copy; it must never hand back the
    // stored compressed bytes, and must not mutate the node.
    InMemoryLruStorage storage { 0, 0, LruMode::Approximate };
    storage.SetCompression(ZstdOptions());

    auto const payload = CompressiblePayload(32768);
    REQUIRE(storage.Set("k", payload, 0, Never()).has_value());

    for (int i = 0; i < 3; ++i)
    {
        auto const got = storage.Get("k", Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        auto const bytes = got->entry.ValueBytes();
        CHECK(std::vector<std::byte> { bytes.begin(), bytes.end() } == payload);
    }
}
