// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/FleetVersions.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace FastCache::Distributed;

namespace
{
/// A machine as `NodeReports()` lists it, saying only what the spread reads.
[[nodiscard]] NodeReport Machine(std::string endpoint, std::string version, std::string displayName = {})
{
    return NodeReport { .endpoint = std::move(endpoint),
                        .fingerprints = {},
                        .capacity = {},
                        .load = {},
                        .registeredSlots = std::nullopt,
                        .fleetJobsInFlight = 0,
                        .heartbeatAge = {},
                        .version = std::move(version),
                        .displayName = std::move(displayName),
                        .conditions = std::nullopt };
}
} // namespace

TEST_CASE("One build across the fleet is not a mix", "[distributed][fleet][conditions]")
{
    auto const nodes = std::vector { Machine("n2:6674", "1.2.0"), Machine("n3:6674", "1.2.0") };
    auto const spread = SpreadOfVersions(nodes, "1.2.0", "n1:6674");
    CHECK_FALSE(spread.Mixed());
    REQUIRE(spread.groups.size() == 1);
    CHECK(spread.groups.front().machines == std::vector<std::string> { "n1:6674", "n2:6674", "n3:6674" });
}

TEST_CASE("Two builds of one wire are a mix, the build most machines run listed first", "[distributed][fleet][conditions]")
{
    auto const nodes = std::vector { Machine("n2:6674", "1.2.0"), Machine("n3:6674", "1.1.9", "buildnode-3") };
    auto const spread = SpreadOfVersions(nodes, "1.2.0", "n1:6674");
    REQUIRE(spread.Mixed());
    REQUIRE(spread.groups.size() == 2);
    CHECK(spread.groups[0].version == "1.2.0");
    CHECK(spread.groups[0].machines == std::vector<std::string> { "n1:6674", "n2:6674" });
    CHECK(spread.groups[1].version == "1.1.9");
    // The name an operator walks to, when the machine said one; the endpoint otherwise.
    CHECK(spread.groups[1].machines == std::vector<std::string> { "buildnode-3" });
}

TEST_CASE("The leader is counted once, whether or not it announced itself", "[distributed][fleet][conditions]")
{
    // Its own build is always in the spread -- a leader on the odd build with every worker
    // agreeing is still a mix -- and its announcement to itself, when it makes one, is not a second machine.
    auto const nodes = std::vector { Machine("n1:6674", "1.2.0"), Machine("n2:6674", "1.2.0") };
    auto const spread = SpreadOfVersions(nodes, "1.2.0", "n1:6674");
    REQUIRE(spread.groups.size() == 1);
    CHECK(spread.groups.front().machines == std::vector<std::string> { "n1:6674", "n2:6674" });

    auto const oddLeader = SpreadOfVersions(nodes, "1.3.0", "n9:6674");
    CHECK(oddLeader.Mixed());
}

TEST_CASE("A machine that states no version is counted apart, never as a build of its own",
          "[distributed][fleet][conditions]")
{
    // Absent is not a value: "did not say" is not a second build, and a fleet of one build plus a
    // silent machine is not a mix.
    auto const nodes = std::vector { Machine("n2:6674", "") };
    auto const spread = SpreadOfVersions(nodes, "1.2.0", "n1:6674");
    CHECK_FALSE(spread.Mixed());
    CHECK(spread.unstated == 1);
    REQUIRE(spread.groups.size() == 1);
    CHECK(spread.groups.front().machines == std::vector<std::string> { "n1:6674" });
}
