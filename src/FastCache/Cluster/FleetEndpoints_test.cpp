// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Consensus/DurableFile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

TEST_CASE("Fleet endpoints round-trip and absent is not unreadable", "[cluster][formation][endpoints]")
{
    Testing::ScratchDirectory scratch { "fleet-endpoints" };
    FleetEndpointsFile file { scratch.Path() };
    CHECK(file.Load().outcome == FleetEndpointsLoad::Absent);

    auto const endpoints =
        FleetEndpoints { .clusterId = "c1",
                         .voters = { { .id = "n-a", .raftEndpoint = "office-a:6680", .nodeEndpoint = "office-a:6674" } } };
    REQUIRE(file.Save(endpoints).has_value());
    auto const loaded = FleetEndpointsFile { scratch.Path() }.Load();
    CHECK(loaded.outcome == FleetEndpointsLoad::Loaded);
    CHECK(loaded.endpoints == endpoints);
    CHECK(RememberedSeeds(endpoints) == std::vector<std::string> { "office-a:6674" });
}

TEST_CASE("A fleet endpoints file a later build wrote is kept and never overwritten", "[cluster][formation][endpoints]")
{
    Testing::ScratchDirectory scratch { "fleet-endpoints-later" };
    auto const path = scratch.Path() / std::string { FleetEndpointsFileName };
    // The envelope: magic, then the format byte. A later format with bytes this build cannot read.
    auto later = std::vector<std::byte> {
        std::byte { 'F' }, std::byte { 'C' }, std::byte { 'F' }, std::byte { 'E' }, std::byte { FleetEndpointsFormat + 1 },
        std::byte { 0xEE }
    };
    REQUIRE(Consensus::ReplaceFileAtomically(path, later, StateFile::FleetEndpoints).has_value());

    FleetEndpointsFile file { scratch.Path() };
    CHECK(file.Load().outcome == FleetEndpointsLoad::LaterBuild);
    CHECK(file.ReadOnly());
    CHECK_FALSE(file.Save(FleetEndpoints { .clusterId = "c1", .voters = {} }).has_value());
    CHECK(Unwrap(Consensus::ReadFileIfPresent(path)) == std::optional { later });
}

TEST_CASE("A damaged fleet endpoints file is unreadable and never keeps a node from starting",
          "[cluster][formation][endpoints]")
{
    Testing::ScratchDirectory scratch { "fleet-endpoints-damaged" };
    auto const path = scratch.Path() / std::string { FleetEndpointsFileName };
    REQUIRE(Consensus::ReplaceFileAtomically(path, std::vector<std::byte> { std::byte { 0x00 } }, StateFile::FleetEndpoints)
                .has_value());
    FleetEndpointsFile file { scratch.Path() };
    CHECK(file.Load().outcome == FleetEndpointsLoad::Unreadable);
    // Damage is not a later build: the next save replaces it.
    CHECK_FALSE(file.ReadOnly());
    CHECK(file.Save(FleetEndpoints { .clusterId = "c1", .voters = {} }).has_value());
}
