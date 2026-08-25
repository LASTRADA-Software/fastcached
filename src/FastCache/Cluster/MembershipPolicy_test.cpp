// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

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

/// `NextQuorumChange`, spelled without the span conversions at every call.
///
/// `bootstrap` defaults to this node alone, which is the discovery-formed shape and
/// the one that leaves every other member removable -- the cases about a typed
/// cluster pass their own.
/// @param state What the cluster holds.
/// @param active What consensus counts.
/// @param self This node's id.
/// @param bootstrap What this node was started with; itself by default.
/// @return The proposed member set, or nullopt.
[[nodiscard]] std::optional<std::vector<Consensus::NodeId>> QuorumChange(
    ClusterState const& state,
    std::vector<Consensus::NodeId> const& active,
    Consensus::NodeId const& self = "n1",
    std::optional<std::vector<Consensus::NodeId>> const& bootstrap = std::nullopt)
{
    auto const started = bootstrap.value_or(std::vector<Consensus::NodeId> { self });
    return NextQuorumChange(state, std::span<Consensus::NodeId const> { active }, self, started);
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

TEST_CASE("A quorum that already matches the state proposes nothing", "[cluster][membership][quorum]")
{
    // The ordinary case, on every pass of the reconciler's loop for the whole life
    // of a healthy fleet. A change proposed here costs a configuration entry and a
    // replication round, so "nothing to do" has to be the cheap answer and the
    // common one.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:6675") });

    CHECK_FALSE(QuorumChange(state, { "n1", "n2" }).has_value());
}

TEST_CASE("A member the cluster admitted is added to the quorum", "[cluster][membership][quorum]")
{
    // The whole point: until this existed a node admitted at runtime was served by
    // every surface and voted in none, so growing a cluster's consensus meant
    // restarting its members with a longer bootstrap list.
    auto const state =
        StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:6675"), Member("n3", "10.0.0.3:6675") });

    auto const change = QuorumChange(state, { "n1", "n2" });
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == std::vector<Consensus::NodeId> { "n1", "n2", "n3" });
}

TEST_CASE("Only one member is added at a time", "[cluster][membership][quorum]")
{
    // §4.3, and the reason it is a rule rather than a convenience: going from three
    // members to five in one step makes {n1,n2} a majority of the old and {n3,n4,n5}
    // a majority of the new, with nobody in common to stop both electing.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"),
                                 Member("n2", "10.0.0.2:6675"),
                                 Member("n3", "10.0.0.3:6675"),
                                 Member("n4", "10.0.0.4:6675") });

    auto const change = QuorumChange(state, { "n1" });
    REQUIRE(change.has_value());
    CHECK(Unwrap(change).size() == 2);
}

TEST_CASE("A member the cluster forgot is removed from the quorum", "[cluster][membership][quorum]")
{
    // `--cluster-forget` takes a machine out of the state; without this it stayed in
    // the quorum, so a fleet that lost a node permanently kept counting it -- and a
    // three-member cluster reduced to two by an operator still needed two votes.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:6675") });

    // n3 was admitted at runtime rather than typed, so it can be un-admitted at
    // runtime -- which is exactly the asymmetry the case below is about.
    auto const change = QuorumChange(state, { "n1", "n2", "n3" });
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == std::vector<Consensus::NodeId> { "n1", "n2" });
}

TEST_CASE("A member an operator typed is never proposed for removal", "[cluster][membership][quorum]")
{
    // The defect this parameter exists for, and it took a running cluster to find:
    // `--raft-peer` puts a member in the CONFIGURATION and nothing puts it in the
    // STATE, so on a cluster whose peers were typed rather than discovered the
    // leader's own record is all the state holds. Read as "everybody else was
    // forgotten", that proposed removing every peer, one per commit, until a healthy
    // three-node cluster was one node counting only itself -- with the other two
    // then refused as strangers and the fleet permanently undecided.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675") });
    auto const typed = std::vector<Consensus::NodeId> { "n1", "n2", "n3" };

    CHECK_FALSE(QuorumChange(state, { "n1", "n2", "n3" }, "n1", typed).has_value());

    // And a member admitted at runtime alongside them still is: the rule is about
    // where the member came from, not about how big the cluster is.
    auto const grown = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n4", "10.0.0.4:6675") });
    CHECK_FALSE(QuorumChange(grown, { "n1", "n2", "n3", "n4" }, "n1", typed).has_value());

    auto const shrunk = QuorumChange(state, { "n1", "n2", "n3", "n4" }, "n1", typed);
    REQUIRE(shrunk.has_value());
    CHECK(Unwrap(shrunk) == std::vector<Consensus::NodeId> { "n1", "n2", "n3" });
}

TEST_CASE("Growing comes before shrinking", "[cluster][membership][quorum]")
{
    // A replacement -- one machine out, one in -- is two steps, and this is the
    // order that keeps the quorum reachable throughout. The other one passes through
    // a configuration smaller than either endpoint.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n3", "10.0.0.3:6675") });

    auto const change = QuorumChange(state, { "n1", "n2" });
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == std::vector<Consensus::NodeId> { "n1", "n2", "n3" });
}

TEST_CASE("A member with no dialable address is not counted", "[cluster][membership][quorum]")
{
    // Counting a node the transport cannot reach is the failure this whole change
    // exists to avoid, reached from the other side: the quorum grows and the votes
    // to satisfy it can never arrive.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "not-an-endpoint") });

    CHECK_FALSE(QuorumChange(state, { "n1" }).has_value());
}

TEST_CASE("A counted member is not dropped for an unreadable address", "[cluster][membership][quorum]")
{
    // The asymmetry is the point. Refusing to ADD an undialable member costs
    // nothing; removing one already counted turns a typo in a record into a smaller
    // quorum, and in a two-member cluster into one that cannot elect at all.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "not-an-endpoint") });

    CHECK_FALSE(QuorumChange(state, { "n1", "n2" }).has_value());
}

TEST_CASE("A node given no bootstrap set proposes no removal", "[cluster][membership][quorum]")
{
    // A `--raft-join` node was told nothing about the cluster's shape, so every
    // member is equally unexplained to it -- and once elected it would otherwise
    // remove all of them, one per commit, which is the identical failure the
    // bootstrap comparison exists to prevent reached through the one path that has
    // nothing to compare against.
    auto const state = StateOf({ Member("n4", "10.0.0.4:6675") });

    CHECK_FALSE(QuorumChange(state, { "n1", "n2", "n3", "n4" }, "n4", std::vector<Consensus::NodeId> {}).has_value());

    // It still ADDS, which is the half a joiner can decide safely: a member the
    // cluster has agreed on is one the state names, whatever this node was told.
    auto const grown = StateOf({ Member("n4", "10.0.0.4:6675"), Member("n5", "10.0.0.5:6675") });
    auto const change = QuorumChange(grown, { "n4" }, "n4", std::vector<Consensus::NodeId> {});
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == std::vector<Consensus::NodeId> { "n4", "n5" });
}

TEST_CASE("A member whose port nobody can connect to is not counted", "[cluster][membership][quorum]")
{
    // A split alone is not the question a dialer asks: `10.0.0.5:0` splits cleanly
    // and names no port anybody can connect to, so a member accepted on that basis
    // is counted towards the quorum here and silently never dialled.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n5", "10.0.0.5:0") });

    CHECK_FALSE(QuorumChange(state, { "n1" }).has_value());
}

TEST_CASE("This node never proposes its own removal", "[cluster][membership][quorum]")
{
    // It would not stick: a node always desires its own record, so the next pass
    // would propose putting it back, and a configuration flapping on a timer is
    // worse than one that is merely wrong.
    auto const state = StateOf({ Member("n2", "10.0.0.2:6675") });

    CHECK_FALSE(QuorumChange(state, { "n1", "n2" }, "n1").has_value());
}

TEST_CASE("A node with no cluster proposes no change", "[cluster][membership][quorum]")
{
    // It counts nobody and cannot lead, so this is a guard against being asked
    // rather than a case that arises -- but a member set built out of an empty one
    // would be a cluster this node invented for itself.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675") });

    CHECK_FALSE(QuorumChange(state, {}).has_value());
}
