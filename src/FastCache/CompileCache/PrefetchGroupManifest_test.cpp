// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/CompileCache/PrefetchGroupManifest.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

using namespace FastCache;

TEST_CASE("PrefetchGroupManifest accumulates keys per prefetch group id")
{
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    REQUIRE(manifest.AddKey("envA", "objkey1", now).has_value());
    REQUIRE(manifest.AddKey("envA", "objkey2", now).has_value());
    REQUIRE(manifest.AddKey("envB", "objkey3", now).has_value());

    auto const a = manifest.Keys("envA", now);
    REQUIRE(a.has_value());
    REQUIRE(a->size() == 2);
    CHECK(std::ranges::find(*a, "objkey1") != a->end());
    CHECK(std::ranges::find(*a, "objkey2") != a->end());

    auto const b = manifest.Keys("envB", now);
    REQUIRE(b.has_value());
    REQUIRE(b->size() == 1);
    CHECK((*b)[0] == "objkey3");
}

TEST_CASE("PrefetchGroupManifest AddKey is idempotent")
{
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    REQUIRE(manifest.AddKey("env", "k", now).has_value());
    REQUIRE(manifest.AddKey("env", "k", now).has_value());
    auto const keys = manifest.Keys("env", now);
    REQUIRE(keys.has_value());
    CHECK(keys->size() == 1);
}

TEST_CASE("PrefetchGroupManifest GroupOf reverse-maps a key to its prefetch group")
{
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    REQUIRE(manifest.AddKey("envA", "k1", now).has_value());
    REQUIRE(manifest.AddKey("envB", "k2", now).has_value());

    // Compare through value_or: an absent prefetch group fails the comparison rather than
    // dereferencing an empty optional. Static analysis cannot see a has_value()
    // guard through Catch2's REQUIRE macro, so avoid needing one.
    auto const c1 = manifest.GroupOf("k1", now);
    REQUIRE(c1.has_value());
    CHECK(c1->value_or("") == "envA");

    auto const c2 = manifest.GroupOf("k2", now);
    REQUIRE(c2.has_value());
    CHECK(c2->value_or("") == "envB");

    auto const unknown = manifest.GroupOf("never", now);
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->has_value());
}

TEST_CASE("PrefetchGroupManifest reports empty for an unknown prefetch group")
{
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;

    auto const keys = manifest.Keys("never-seen", clock.Now());
    REQUIRE(keys.has_value());
    CHECK(keys->empty());
}

TEST_CASE("PrefetchGroupManifest preserves insertion order across many keys")
{
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    for (auto const i: std::views::iota(0, 50))
        REQUIRE(manifest.AddKey("env", "k" + std::to_string(i), now).has_value());

    auto const keys = manifest.Keys("env", now);
    REQUIRE(keys.has_value());
    REQUIRE(keys->size() == 50);
    CHECK((*keys).front() == "k0");
    CHECK((*keys).back() == "k49");
}

TEST_CASE("PrefetchGroupManifest refuses a key count the manifest bytes cannot supply")
{
    // The second site of issue #267's class, and the same defect as
    // `DecodeCompileValue`: this reserved from a `u32` count read out of the stored
    // value with no check that the buffer could hold that many keys. A key costs its
    // four-byte length prefix at minimum, so the five-byte value below declared four
    // billion of them -- and a `std::string` is thirty-two bytes, so that is over
    // 130 GB asked for by five bytes.
    //
    // Reachable because the manifest is written from the STORE path's prefetch-group
    // field, and read back on FETCH.
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    // The manifest's own storage key, as `ManifestKey` builds it: a 0x01 control byte
    // that keeps it out of the user keyspace, then `cohort:`, then the group id.
    std::string const manifestKey = std::string { '\x01' } + "cohort:" + "envHostile";

    // `[u32 count = 0xFFFFFFFF]` and not one byte more.
    std::vector<std::byte> hostile;
    for ([[maybe_unused]] auto const i: { 0, 1, 2, 3 })
        hostile.push_back(std::byte { 0xFF });
    REQUIRE(storage.Set(manifestKey, hostile, /*flags=*/0, /*expiry=*/TimePoint {}).has_value());

    // Refused whole rather than decoded best-effort: a count the bytes cannot supply
    // means this is not a manifest this build wrote, and a truncated prefix would
    // silently prefetch part of a group and read as a cold cache.
    auto const keys = manifest.Keys("envHostile", now);
    REQUIRE(keys.has_value());
    CHECK(keys->empty());
}
