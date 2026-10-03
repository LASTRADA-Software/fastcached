// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/SeedSources.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cluster;

TEST_CASE("Seeds are tried remembered first then flags then DNS SRV", "[cluster][formation][seeds]")
{
    auto const remembered = std::vector<std::string> { "office-a.corp:6674" };
    auto const flags = std::vector<std::string> { "office-a.corp", "office-b.corp:7000" };
    auto const srv = std::vector<SrvTarget> { { .host = "office-c.corp", .port = 6674, .priority = 20, .weight = 5 },
                                              { .host = "office-d.corp", .port = 6674, .priority = 10, .weight = 1 },
                                              { .host = "office-e.corp", .port = 6674, .priority = 10, .weight = 9 } };
    auto const seeds = OrderSeeds(remembered, flags, srv);
    auto const expected = std::vector<SeedCandidate> {
        { .endpoint = "office-a.corp:6674", .source = SeedSource::Remembered },
        // "office-a.corp" normalizes to the remembered one and keeps the earlier source.
        { .endpoint = "office-b.corp:7000", .source = SeedSource::FleetSeedFlag },
        { .endpoint = "office-e.corp:6674", .source = SeedSource::DnsSrv },
        { .endpoint = "office-d.corp:6674", .source = SeedSource::DnsSrv },
        { .endpoint = "office-c.corp:6674", .source = SeedSource::DnsSrv },
    };
    CHECK(seeds == expected);
}

TEST_CASE("A seed names a machine and takes the node port by default", "[cluster][formation][seeds]")
{
    CHECK(NormalizeSeed("office-a") == std::optional<std::string> { "office-a:6674" });
    CHECK(NormalizeSeed("office-a:7000") == std::optional<std::string> { "office-a:7000" });
    CHECK(NormalizeSeed("[fe80::1]:7000") == std::optional<std::string> { "[fe80::1]:7000" });
    CHECK_FALSE(NormalizeSeed("").has_value());
    CHECK_FALSE(NormalizeSeed(":6674").has_value()); // a bare port names no machine
    CHECK_FALSE(NormalizeSeed("office-a:0").has_value());
}

TEST_CASE("The SRV name is the primary DNS suffix under _fastcache._tcp and absent without one",
          "[cluster][formation][seeds]")
{
    CHECK(SrvQueryName("corp.example") == "_fastcache._tcp.corp.example");
    CHECK(SrvQueryName("").empty());
}
