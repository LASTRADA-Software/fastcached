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
    // The second site of issue #267's class: this reserved from a `u32` count read out
    // of the stored value with no check that the buffer could hold that many keys. A
    // key costs its four-byte length prefix at minimum, so the four-byte value below
    // declared four billion of them -- and a `std::string` is thirty-two bytes, so
    // that is over 130 GB asked for by four bytes.
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    // The manifest's own storage key, as `ManifestKey` builds it: a 0x01 control byte
    // that keeps it out of the user keyspace, then `cohort:`, then the group id.
    std::string const manifestKey = std::string { '' } + "cohort:" + "envHostile";

    // `[u32 count = 0xFFFFFFFF]` and not one byte more.
    std::vector<std::byte> const hostile(4, std::byte { 0xFF });
    REQUIRE(storage.Set(manifestKey, hostile, /*flags=*/0, /*expiry=*/TimePoint::max()).has_value());

    // Refused by NAME rather than read as an empty group. `Corrupt` is the enumerator
    // for "this store holds bytes this build did not write", and it has to be distinct
    // from the empty list an unknown group returns -- otherwise the refusal is
    // indistinguishable from a cold prefetch group.
    auto const keys = manifest.Keys("envHostile", now);
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error().code == StorageErrorCode::Corrupt);
}

TEST_CASE("An undecodable prefetch manifest is never silently overwritten")
{
    // The half that matters more, and the one the first version of this fix got
    // wrong: `DecodeKeyList` is on the WRITE path too. Returning an empty list for an
    // undecodable value made `AddKey` store a fresh one-key list -- destroying a
    // hundred thousand keys on the strength of bytes it had just decided it did not
    // understand. One value cannot be a refusal on FETCH and a fresh start on STORE.
    InMemoryLruStorage storage { 0 };
    PrefetchGroupManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    std::string const manifestKey = std::string { '' } + "cohort:" + "envHostile";
    std::vector<std::byte> const hostile(4, std::byte { 0xFF });
    REQUIRE(storage.Set(manifestKey, hostile, /*flags=*/0, /*expiry=*/TimePoint::max()).has_value());

    auto const added = manifest.AddKey("envHostile", "objkey1", now);
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().code == StorageErrorCode::Corrupt);

    // And the bytes are still there, unmodified: the refusal cost nothing.
    auto const still = storage.Peek(manifestKey, now);
    REQUIRE(still.has_value());
    REQUIRE(still->found);
    auto const bytes = still->entry.ValueBytes();
    CHECK(std::vector<std::byte>(bytes.begin(), bytes.end()) == hostile);
}
