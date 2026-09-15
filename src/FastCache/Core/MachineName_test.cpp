// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/MachineName.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace FastCache;

TEST_CASE("a machine name is kebab-case and nothing that merely resembles it", "[core][names]")
{
    CHECK(IsKebabName("cache-hit-rate"));
    CHECK(IsKebabName("index-ram"));
    CHECK(IsKebabName("p95"));

    // WHAT DISTINGUISHES: the snake spelling the CLI's piped keys had differs from the accepted one by one character.
    CHECK_FALSE(IsKebabName("cache_hit_rate"));
    CHECK_FALSE(IsKebabName("Cache-hit-rate"));
    CHECK_FALSE(IsKebabName("cache hit rate"));
    // Every way a hyphen can stand where no run of letters is on both sides of it.
    CHECK_FALSE(IsKebabName(""));
    CHECK_FALSE(IsKebabName("-"));
    CHECK_FALSE(IsKebabName("-cache"));
    CHECK_FALSE(IsKebabName("cache-"));
    CHECK_FALSE(IsKebabName("cache--hit"));
}

TEST_CASE("a composed machine name is judged as the one name its parts join into", "[core][names]")
{
    CHECK(IsKebabJoin({ "memory", "-", "bytes-used" }));
    // Each part kebab-case, the join not: the separator is what a table of tier names and a table of columns
    // cannot see from either side.
    CHECK_FALSE(IsKebabJoin({ "memory", "_", "bytes-used" }));
    // Where the parts are cut does not matter, only what they join into: `memory-bytes` either way, and `memory--bytes`
    // is refused though neither half holds the doubled hyphen whole.
    CHECK(IsKebabJoin({ "memory", "", "-bytes" }));
    CHECK_FALSE(IsKebabJoin({ "memory-", "-", "bytes" }));
    CHECK_FALSE(IsKebabJoin({ "", "", "" }));
}
