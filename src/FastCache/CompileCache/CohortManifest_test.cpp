// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/CompileCache/CohortManifest.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>

using namespace FastCache;

TEST_CASE("CohortManifest accumulates keys per cohort id")
{
    InMemoryLruStorage storage { 0 };
    CohortManifest manifest { storage };
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

TEST_CASE("CohortManifest AddKey is idempotent")
{
    InMemoryLruStorage storage { 0 };
    CohortManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    REQUIRE(manifest.AddKey("env", "k", now).has_value());
    REQUIRE(manifest.AddKey("env", "k", now).has_value());
    auto const keys = manifest.Keys("env", now);
    REQUIRE(keys.has_value());
    CHECK(keys->size() == 1);
}

TEST_CASE("CohortManifest CohortOf reverse-maps a key to its cohort")
{
    InMemoryLruStorage storage { 0 };
    CohortManifest manifest { storage };
    ManualClock clock;
    auto const now = clock.Now();

    REQUIRE(manifest.AddKey("envA", "k1", now).has_value());
    REQUIRE(manifest.AddKey("envB", "k2", now).has_value());

    // Compare through value_or: an absent cohort fails the comparison rather than
    // dereferencing an empty optional. Static analysis cannot see a has_value()
    // guard through Catch2's REQUIRE macro, so avoid needing one.
    auto const c1 = manifest.CohortOf("k1", now);
    REQUIRE(c1.has_value());
    CHECK(c1->value_or("") == "envA");

    auto const c2 = manifest.CohortOf("k2", now);
    REQUIRE(c2.has_value());
    CHECK(c2->value_or("") == "envB");

    auto const unknown = manifest.CohortOf("never", now);
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->has_value());
}

TEST_CASE("CohortManifest reports empty for an unknown cohort")
{
    InMemoryLruStorage storage { 0 };
    CohortManifest manifest { storage };
    ManualClock clock;

    auto const keys = manifest.Keys("never-seen", clock.Now());
    REQUIRE(keys.has_value());
    CHECK(keys->empty());
}

TEST_CASE("CohortManifest preserves insertion order across many keys")
{
    InMemoryLruStorage storage { 0 };
    CohortManifest manifest { storage };
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
