// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/DiskStorage.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{

std::filesystem::path TempLogPath(std::string_view stem)
{
    auto path = std::filesystem::temp_directory_path() / "fastcached-test";
    std::filesystem::create_directories(path);
    path /= std::string { stem } + ".log";
    std::filesystem::remove(path);
    return path;
}

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

TEST_CASE("DiskStorage: set, close, reopen reads value back", "[diskstorage]")
{
    auto const path = TempLogPath("reopen");
    FastCache::ManualClock clock;

    {
        auto storage = FastCache::DiskStorage::Open({ .logPath = path });
        REQUIRE(storage.has_value());
        auto const cas = (*storage)->Set("k", MakeBytes("hello"), 7, FastCache::TimePoint::max());
        REQUIRE(cas.has_value());
    }

    auto storage = FastCache::DiskStorage::Open({ .logPath = path });
    REQUIRE(storage.has_value());
    auto const got = (*storage)->Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "hello");
    REQUIRE(got->entry.flags == 7);
}

TEST_CASE("DiskStorage: delete after reopen takes effect", "[diskstorage]")
{
    auto const path = TempLogPath("delete-after-reopen");
    FastCache::ManualClock clock;

    {
        auto storage = FastCache::DiskStorage::Open({ .logPath = path });
        REQUIRE(storage.has_value());
        std::ignore = (*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
        REQUIRE((*storage)->Delete("k", clock.Now()).has_value());
    }
    auto storage = FastCache::DiskStorage::Open({ .logPath = path });
    REQUIRE(storage.has_value());
    auto const got = (*storage)->Get("k", clock.Now());
    REQUIRE_FALSE(got->found);
}

TEST_CASE("DiskStorage: crash recovery truncates a torn final record", "[diskstorage]")
{
    auto const path = TempLogPath("torn");
    FastCache::ManualClock clock;

    {
        auto storage = FastCache::DiskStorage::Open({ .logPath = path });
        REQUIRE(storage.has_value());
        std::ignore = (*storage)->Set("good", MakeBytes("alive"), 0, FastCache::TimePoint::max());
        std::ignore = (*storage)->Set("alsogood", MakeBytes("also-alive"), 0, FastCache::TimePoint::max());
    }

    // Corrupt the file by appending a bogus record header followed by
    // partial payload — i.e. simulate a crash mid-write.
    {
        std::ofstream out { path, std::ios::binary | std::ios::app };
        std::uint32_t const totalLen = 1024; // claims a big record
        std::uint32_t const crc = 0xDEAD'BEEFU;
        out.write(reinterpret_cast<char const*>(&totalLen), sizeof(totalLen));
        out.write(reinterpret_cast<char const*>(&crc), sizeof(crc));
        // Write only a few bytes of payload (truncated).
        std::array<char, 4> garbage { '\1', '\2', '\3', '\4' };
        out.write(garbage.data(), garbage.size());
    }

    auto storage = FastCache::DiskStorage::Open({ .logPath = path });
    REQUIRE(storage.has_value());
    auto const good = (*storage)->Get("good", clock.Now());
    REQUIRE(good->found);
    REQUIRE(Decode(good->entry.value) == "alive");
    auto const also = (*storage)->Get("alsogood", clock.Now());
    REQUIRE(also->found);
}

TEST_CASE("DiskStorage: Compact drops deleted/expired entries", "[diskstorage]")
{
    auto const path = TempLogPath("compact");
    FastCache::ManualClock clock;

    auto storage = FastCache::DiskStorage::Open({ .logPath = path });
    REQUIRE(storage.has_value());
    std::ignore = (*storage)->Set("keep", MakeBytes("yes"), 0, FastCache::TimePoint::max());
    std::ignore = (*storage)->Set("gone", MakeBytes("bye"), 0, FastCache::TimePoint::max());
    REQUIRE((*storage)->Delete("gone", clock.Now()).has_value());

    auto const sizeBefore = std::filesystem::file_size(path);
    auto const written = (*storage)->Compact();
    REQUIRE(written.has_value());
    REQUIRE(*written == 1);
    auto const sizeAfter = std::filesystem::file_size(path);
    REQUIRE(sizeAfter < sizeBefore);

    // Sanity: the surviving entry is still reachable.
    auto const got = (*storage)->Get("keep", clock.Now());
    REQUIRE(got->found);
    REQUIRE(Decode(got->entry.value) == "yes");
}

TEST_CASE("DiskStorage: CAS round-trips across reopen", "[diskstorage]")
{
    auto const path = TempLogPath("cas-restart");
    FastCache::ManualClock clock;

    FastCache::CasToken originalCas = 0;
    {
        auto storage = FastCache::DiskStorage::Open({ .logPath = path });
        REQUIRE(storage.has_value());
        auto const cas = (*storage)->Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
        REQUIRE(cas.has_value());
        originalCas = *cas;
    }

    auto storage = FastCache::DiskStorage::Open({ .logPath = path });
    REQUIRE(storage.has_value());
    auto const got = (*storage)->Get("k", clock.Now());
    REQUIRE(got->found);
    REQUIRE(got->entry.cas == originalCas);

    // CAS mismatch should fire against the persisted token.
    auto const bad =
        (*storage)->CompareAndSwap("k", originalCas + 99, MakeBytes("nope"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().code == FastCache::StorageErrorCode::CasMismatch);

    auto const good =
        (*storage)->CompareAndSwap("k", originalCas, MakeBytes("new"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE(good.has_value());
}
