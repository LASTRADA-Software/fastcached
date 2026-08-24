// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftConfig.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

/// A configuration that passes validation, for a test to then break one field of.
/// @return A valid three-node configuration.
[[nodiscard]] RaftConfig Sound()
{
    return RaftConfig { .self = "n1",
                        .members = { "n1", "n2", "n3" },
                        .electionTimeoutMin = 150ms,
                        .electionTimeoutMax = 300ms,
                        .heartbeatInterval = 50ms };
}

} // namespace

TEST_CASE("A sound configuration validates", "[consensus][raft][config]")
{
    CHECK(Sound().Validate().has_value());
}

TEST_CASE("A configuration must name this node among its members", "[consensus][raft][config]")
{
    auto config = Sound();
    config.self = "n9";

    auto const result = config.Validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ConsensusErrorCode::InvalidConfiguration);
    // The message names the field, because "invalid configuration" tells somebody
    // editing a file nothing about where to look.
    CHECK(result.error().context.contains("n9"));
}

TEST_CASE("An empty identity is refused", "[consensus][raft][config]")
{
    auto config = Sound();
    config.self.clear();
    CHECK_FALSE(config.Validate().has_value());
}

TEST_CASE("An empty member set is a node with no cluster, not a broken one", "[consensus][raft][config]")
{
    // The shape a machine has while it waits to be admitted to a running fleet.
    // Refusing it -- which this used to do -- is what made "add a node" mean
    // "restart every node with a longer --raft-peer list", because the only
    // startable alternative bootstraps a one-member cluster of its own, and a node
    // that has elected itself can never be admitted to somebody else's.
    auto config = Sound();
    config.members.clear();
    CHECK(config.Validate().has_value());
}

TEST_CASE("A node with no cluster is still held to its timings", "[consensus][raft][config]")
{
    // The member-set rules do not apply to a set that does not exist; everything
    // else still does. A joiner arms an election timer from the moment it starts
    // -- it declines to act on it, but the moment it is admitted it will -- so an
    // inverted range accepted here would be one nobody discovers until then.
    auto config = Sound();
    config.members.clear();
    config.electionTimeoutMin = 300ms;
    config.electionTimeoutMax = 150ms;

    auto const result = config.Validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ConsensusErrorCode::InvalidConfiguration);
}

TEST_CASE("A duplicated member is refused", "[consensus][raft][config]")
{
    // A duplicate is counted twice toward a quorum, so a "majority" could be one
    // physical node agreeing with itself -- Election Safety lost to a typo.
    auto config = Sound();
    config.members = { "n1", "n2", "n2" };

    auto const result = config.Validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().context.contains("n2"));
}

TEST_CASE("An inverted election-timeout range is refused here", "[consensus][raft][config]")
{
    // This is the layer that diagnoses it. IRandomSource defines the inverted
    // case rather than rejecting it, precisely because only here is it known that
    // the two bounds came from a configuration file and can be named back.
    auto config = Sound();
    config.electionTimeoutMin = 400ms;
    config.electionTimeoutMax = 300ms;

    auto const result = config.Validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().context.contains("400"));
    CHECK(result.error().context.contains("300"));
}

TEST_CASE("A heartbeat no faster than the election timeout is refused", "[consensus][raft][config]")
{
    // Equality is already too slow: a heartbeat arriving exactly as followers time
    // out deposes a healthy leader about half the time, and the cluster spends its
    // life electing rather than working.
    auto config = Sound();
    config.heartbeatInterval = 150ms;

    CHECK_FALSE(config.Validate().has_value());

    config.heartbeatInterval = 149ms;
    CHECK(config.Validate().has_value());
}

TEST_CASE("Non-positive timeouts are refused", "[consensus][raft][config]")
{
    SECTION("election timeout")
    {
        auto config = Sound();
        config.electionTimeoutMin = 0ms;
        CHECK_FALSE(config.Validate().has_value());
    }

    SECTION("heartbeat interval")
    {
        auto config = Sound();
        config.heartbeatInterval = 0ms;
        CHECK_FALSE(config.Validate().has_value());
    }
}

TEST_CASE("Quorum is a strict majority", "[consensus][raft][config]")
{
    // Two overlapping majorities always share a member, which is the whole
    // mechanism behind Election Safety -- so this is floor(n/2)+1, never n/2.
    auto config = Sound();

    auto const quorumOf = [&config](std::vector<NodeId> members) {
        config.members = std::move(members);
        return config.Quorum();
    };

    CHECK(quorumOf({ "a" }) == 1);
    CHECK(quorumOf({ "a", "b" }) == 2);
    CHECK(quorumOf({ "a", "b", "c" }) == 2);
    CHECK(quorumOf({ "a", "b", "c", "d" }) == 3);
    CHECK(quorumOf({ "a", "b", "c", "d", "e" }) == 3);
}

TEST_CASE("Peers are the members other than this node", "[consensus][raft][config]")
{
    auto const config = Sound();
    auto const peers = config.Peers();

    REQUIRE(peers.size() == 2);
    CHECK(peers[0] == "n2");
    CHECK(peers[1] == "n3");
}

TEST_CASE("A single-node cluster has no peers", "[consensus][raft][config]")
{
    auto config = Sound();
    config.members = { "n1" };

    CHECK(config.Peers().empty());
    CHECK(config.Quorum() == 1);
    CHECK(config.Validate().has_value());
}
