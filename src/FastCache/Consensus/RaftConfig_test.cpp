// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftConfig.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>

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
                        .voters = { "n1", "n2", "n3" },
                        .learners = {},
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
    config.voters.clear();
    CHECK(config.Validate().has_value());
}

TEST_CASE("A node with no cluster is still held to its timings", "[consensus][raft][config]")
{
    // The member-set rules do not apply to a set that does not exist; everything
    // else still does. A joiner arms an election timer from the moment it starts
    // -- it declines to act on it, but the moment it is admitted it will -- so an
    // inverted range accepted here would be one nobody discovers until then.
    auto config = Sound();
    config.voters.clear();
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
    config.voters = { "n1", "n2", "n2" };

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

TEST_CASE("Quorum is a strict majority of the voters", "[consensus][raft][config]")
{
    // Two overlapping majorities always share a voter, which is the whole
    // mechanism behind Election Safety -- so this is floor(v/2)+1, never v/2.
    auto const quorumOf = [](std::vector<NodeId> voters) {
        return Membership::QuorumOf(Configuration { .voters = std::move(voters), .learners = {} });
    };

    CHECK(quorumOf({ "a" }) == 1);
    CHECK(quorumOf({ "a", "b" }) == 2);
    CHECK(quorumOf({ "a", "b", "c" }) == 2);
    CHECK(quorumOf({ "a", "b", "c", "d" }) == 3);
    CHECK(quorumOf({ "a", "b", "c", "d", "e" }) == 3);
}

TEST_CASE("Learners do not move the quorum", "[consensus][raft][config][learner]")
{
    // The whole of what a learner is (#1449): one voter and any number of learners
    // is a quorum of one, which is what lets the always-on machine of a pair lead
    // while the other is off its VPN. The voter-only arithmetic above is the
    // control -- one voter is a quorum of one there too.
    auto const one = Configuration { .voters = { "a" }, .learners = { "b", "c", "d" } };
    CHECK(Membership::QuorumOf(one) == 1);

    auto const three = Configuration { .voters = { "a", "b", "c" }, .learners = { "d" } };
    CHECK(Membership::QuorumOf(three) == 2);
}

TEST_CASE("A node may be bootstrapped as a learner", "[consensus][raft][config][learner]")
{
    // A usually-absent machine is a learner on its own command line as much as in
    // the leader's log: `self` is found in either set.
    auto config = Sound();
    config.self = "laptop";
    config.learners = { "laptop" };
    CHECK(config.Validate().has_value());
}

TEST_CASE("A member that is both a voter and a learner is refused", "[consensus][raft][config][learner]")
{
    // Counted by one rule and excused by the other -- one member meaning two things.
    // The sets are disjoint because the duplicate check runs over both at once.
    auto config = Sound();
    config.learners = { "n3" };

    auto const result = config.Validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ConsensusErrorCode::InvalidConfiguration);
    CHECK(result.error().context.contains("n3"));
}

TEST_CASE("A configuration of learners alone is refused", "[consensus][raft][config][learner]")
{
    // Nobody may lead a cluster of learners. Not the empty configuration either --
    // that one is legal and means no cluster yet -- so this is asserted beside it.
    auto config = Sound();
    config.voters.clear();
    config.learners = { "n1", "n2" };

    auto const result = config.Validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().context.contains("voter"));

    config.learners.clear();
    CHECK(config.Validate().has_value());
}
