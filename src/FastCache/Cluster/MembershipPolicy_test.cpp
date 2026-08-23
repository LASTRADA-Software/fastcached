// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cluster;

namespace
{
/// A member record, spelled once so the cases vary only what they are about.
/// @param id The identity.
/// @param raft Where its consensus port answers.
/// @param scheduler Where clients reach it while it leads.
/// @return The record.
[[nodiscard]] ClusterMember Member(std::string id, std::string raft, std::string scheduler = {})
{
    return ClusterMember { .id = std::move(id), .raftEndpoint = std::move(raft), .schedulerEndpoint = std::move(scheduler) };
}

/// The state a cluster reaches after admitting each of `members`.
/// @param members What it has agreed on.
/// @return The state.
[[nodiscard]] ClusterState StateOf(std::vector<ClusterMember> members)
{
    return ClusterState { .members = std::move(members), .settings = {} };
}
} // namespace

TEST_CASE("A record the state already holds is not proposed again", "[cluster][membership]")
{
    // Proposing one costs a log entry, a replication round and a snapshot's worth of
    // growth per beacon interval, forever. Worse, `AddMember` applies wholesale, so a
    // re-proposal that dropped a field would clear it.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675", "10.0.0.1:7000") });
    std::vector const desired { Member("n1", "10.0.0.1:6675", "10.0.0.1:7000") };

    CHECK(MembershipProposals(state, std::span<ClusterMember const> { desired }).empty());
}

TEST_CASE("A member the state has never heard of is proposed", "[cluster][membership]")
{
    ClusterState const state;
    std::vector const desired { Member("n1", "10.0.0.1:6675", "10.0.0.1:7000"), Member("n2", "10.0.0.2:6675") };

    auto const proposals = MembershipProposals(state, std::span<ClusterMember const> { desired });
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
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675", "10.0.0.1:7000") });

    SECTION("its consensus endpoint moved")
    {
        std::vector const desired { Member("n1", "10.0.0.9:6675", "10.0.0.9:7000") };
        auto const proposals = MembershipProposals(state, std::span<ClusterMember const> { desired });
        REQUIRE(proposals.size() == 1);
        CHECK(proposals[0].value == "10.0.0.9:6675");
    }

    SECTION("it has just announced where clients reach it")
    {
        auto const unannounced = StateOf({ Member("n1", "10.0.0.1:6675") });
        std::vector const desired { Member("n1", "10.0.0.1:6675", "10.0.0.1:7000") };
        auto const proposals = MembershipProposals(unannounced, std::span<ClusterMember const> { desired });
        REQUIRE(proposals.size() == 1);
        CHECK(proposals[0].schedulerEndpoint == "10.0.0.1:7000");
    }
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
    std::vector const desired { Member("n1", "10.0.0.1:6675") };

    CHECK(MembershipProposals(state, std::span<ClusterMember const> { desired }).empty());
}

TEST_CASE("A half-record is dropped rather than proposed", "[cluster][membership]")
{
    // `Validate` would refuse it at the leader anyway, so proposing it would cost a
    // refusal per interval and change nothing -- and the diagnostic would name the
    // reconciler rather than whatever produced the half-record.
    ClusterState const state;
    std::vector const desired { Member("", "10.0.0.1:6675"), Member("n2", ""), Member("n3", "10.0.0.3:6675") };

    auto const proposals = MembershipProposals(state, std::span<ClusterMember const> { desired });
    REQUIRE(proposals.size() == 1);
    CHECK(proposals[0].key == "n3");
}
