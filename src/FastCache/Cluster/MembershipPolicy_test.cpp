// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cluster;

namespace
{
/// A record the cluster has agreed on.
/// @param id The identity.
/// @param raft Where its consensus port answers.
/// @param scheduler Where clients reach it while it leads.
/// @return The member.
[[nodiscard]] ClusterMember Member(std::string id, std::string raft, std::string scheduler = {})
{
    return ClusterMember { .id = std::move(id), .raftEndpoint = std::move(raft), .schedulerEndpoint = std::move(scheduler) };
}

/// A record this node believes should be present.
/// @param id The identity.
/// @param raft Where its consensus port answers.
/// @param scheduler What this node knows about its scheduler port; absent for "no
///        opinion", which is what discovery has about a peer.
/// @return The desire.
[[nodiscard]] DesiredMember Desire(std::string id, std::string raft, std::optional<std::string> scheduler = std::nullopt)
{
    return DesiredMember { .id = std::move(id), .raftEndpoint = std::move(raft), .schedulerEndpoint = std::move(scheduler) };
}

/// The state a cluster reaches after admitting each of `members`.
/// @param members What it has agreed on.
/// @return The state.
[[nodiscard]] ClusterState StateOf(std::vector<ClusterMember> members)
{
    return ClusterState { .members = std::move(members), .settings = {} };
}

/// `MembershipProposals`, spelled without the span conversion at every call.
/// @param state What the cluster holds.
/// @param desired What this node believes.
/// @return The proposals.
[[nodiscard]] std::vector<Command> Proposals(ClusterState const& state, std::vector<DesiredMember> const& desired)
{
    return MembershipProposals(state, std::span<DesiredMember const> { desired });
}
} // namespace

TEST_CASE("A record the state already holds is not proposed again", "[cluster][membership]")
{
    // Proposing one costs a log entry, a replication round and a snapshot's worth of
    // growth per beacon interval, forever.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675", "10.0.0.1:7000") });

    CHECK(Proposals(state, { Desire("n1", "10.0.0.1:6675", "10.0.0.1:7000") }).empty());
}

TEST_CASE("A member the state has never heard of is proposed", "[cluster][membership]")
{
    ClusterState const state;
    auto const proposals =
        Proposals(state, { Desire("n1", "10.0.0.1:6675", "10.0.0.1:7000"), Desire("n2", "10.0.0.2:6675") });

    REQUIRE(proposals.size() == 2);
    CHECK(proposals[0]
          == Command {
              .kind = CommandKind::AddMember, .key = "n1", .value = "10.0.0.1:6675", .schedulerEndpoint = "10.0.0.1:7000" });
    CHECK(proposals[1].key == "n2");
    CHECK(proposals[1].schedulerEndpoint.empty());
}

TEST_CASE("A record that differs in any field is re-proposed", "[cluster][membership]")
{
    // The comparison is on the WHOLE record rather than on the id, and the second
    // half is the case that matters: a node that has just become leader differs from
    // its recorded self only by a scheduler endpoint nobody had asked it for, and
    // that is precisely the value a follower needs in order to redirect.
    SECTION("its consensus endpoint moved")
    {
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675", "10.0.0.1:7000") });
        auto const proposals = Proposals(state, { Desire("n1", "10.0.0.9:6675", "10.0.0.9:7000") });
        REQUIRE(proposals.size() == 1);
        CHECK(proposals[0].value == "10.0.0.9:6675");
        CHECK(proposals[0].schedulerEndpoint == "10.0.0.9:7000");
    }

    SECTION("it has just announced where clients reach it")
    {
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675") });
        auto const proposals = Proposals(state, { Desire("n1", "10.0.0.1:6675", "10.0.0.1:7000") });
        REQUIRE(proposals.size() == 1);
        CHECK(proposals[0].schedulerEndpoint == "10.0.0.1:7000");
    }
}

TEST_CASE("Knowing nothing about a scheduler endpoint leaves the recorded one alone", "[cluster][membership]")
{
    // The distinction `DesiredMember` exists for, and getting it wrong is a fleet
    // whose redirects break every time a follower's discovery loop notices the
    // leader. Discovery proves a peer's CONSENSUS endpoint and learns nothing about
    // the port clients speak to -- so it has no opinion, and no opinion must not
    // overwrite an assertion the peer made about itself.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675", "10.0.0.1:7000") });

    // No opinion, same consensus endpoint: nothing to say.
    CHECK(Proposals(state, { Desire("n1", "10.0.0.1:6675") }).empty());

    // No opinion, moved consensus endpoint: proposed, and the recorded scheduler
    // endpoint travels with it rather than being dropped by omission.
    auto const moved = Proposals(state, { Desire("n1", "10.0.0.9:6675") });
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].value == "10.0.0.9:6675");
    CHECK(moved[0].schedulerEndpoint == "10.0.0.1:7000");

    // An EMPTY string is an opinion -- "I know it has none" -- and does clear it.
    // That is what a node says about itself when it serves no scheduler surface.
    auto const cleared = Proposals(state, { Desire("n1", "10.0.0.1:6675", std::string {}) });
    REQUIRE(cleared.size() == 1);
    CHECK(cleared[0].schedulerEndpoint.empty());
}

TEST_CASE("A member the state holds and nobody desires is left alone", "[cluster][membership]")
{
    // Never a removal. A member vanishes from what a node can see for reasons that
    // are almost never "it left": a beacon lost on a broadcast, a switch rebooting, a
    // laptop closed for an hour. Removing on absence would take a node out of the
    // quorum the moment the network hiccupped, and a cluster that re-computes its own
    // membership from reachability can shrink itself below a majority and never come
    // back.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:6675") });

    CHECK(Proposals(state, { Desire("n1", "10.0.0.1:6675") }).empty());
}

TEST_CASE("A half-record is dropped rather than proposed", "[cluster][membership]")
{
    // `Validate` would refuse it at the leader anyway, so proposing it would cost a
    // refusal per interval and change nothing -- and the diagnostic would name the
    // reconciler rather than whatever produced the half-record.
    ClusterState const state;
    auto const proposals =
        Proposals(state, { Desire("", "10.0.0.1:6675"), Desire("n2", ""), Desire("n3", "10.0.0.3:6675") });

    REQUIRE(proposals.size() == 1);
    CHECK(proposals[0].key == "n3");
}
