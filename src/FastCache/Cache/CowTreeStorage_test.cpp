// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <random>
#include <span>
#include <string>
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

struct TempFile
{
    std::filesystem::path path;
    TempFile()
    {
        std::mt19937_64 rng { std::random_device {}() };
        path = std::filesystem::temp_directory_path()
             / ("cowstorage-test-" + std::to_string(rng()) + ".cow");
        std::filesystem::remove(path);
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempFile(TempFile const&) = delete;
    TempFile& operator=(TempFile const&) = delete;
};

} // namespace

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
    FastCache::CowTreeStorage::Options opts;
    opts.path = tmp.path;

    {
        auto storage = FastCache::CowTreeStorage::Open(opts);
        REQUIRE(storage.has_value());
        REQUIRE((*storage)->Set("k", MakeBytes("persisted"), 0, FastCache::TimePoint::max()).has_value());
    }

    {
        auto storage = FastCache::CowTreeStorage::Open(opts);
        REQUIRE(storage.has_value());
        FastCache::ManualClock clock;
        auto got = (*storage)->Get("k", clock.Now());
        REQUIRE(got.has_value());
        REQUIRE(got->found);
        REQUIRE(Decode(got->entry.value) == "persisted");
    }
}

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
