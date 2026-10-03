// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/RosterKeys.hpp>
#include <FastCache/Cluster/SelfForgotten.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Core/Ed25519.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
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
/// @param seat Which set the operator admitted it into.
/// @return The member.
[[nodiscard]] ClusterMember Member(std::string id,
                                   std::string raft,
                                   std::string scheduler = {},
                                   MemberSeat seat = MemberSeat::Voter)
{
    // The key before the id is moved from, which the initializer below would do first.
    auto const key = Testing::TestKeyPair(id).PublicKey();
    return ClusterMember { .id = std::move(id),
                           .raftEndpoint = std::move(raft),
                           .schedulerEndpoint = std::move(scheduler),
                           .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                           .seat = seat,
                           .publicKey = key };
}

/// A learner the cluster has agreed on (#1449).
/// @param id The identity.
/// @param raft Where its consensus port answers.
/// @return The member.
[[nodiscard]] ClusterMember Learner(std::string id, std::string raft)
{
    return Member(std::move(id), std::move(raft), {}, MemberSeat::Learner);
}

/// A record this node believes should be present.
/// @param id The identity.
/// @param raft Where its consensus port answers.
/// @param scheduler What this node knows about its scheduler port; absent for "no
///        opinion", which is what discovery has about a peer.
/// @return The desire.
[[nodiscard]] DesiredMember Desire(std::string id, std::string raft, std::optional<std::string> scheduler = std::nullopt)
{
    return DesiredMember { .id = std::move(id),
                           .raftEndpoint = std::move(raft),
                           .schedulerEndpoint = std::move(scheduler),
                           .publicKey = std::nullopt };
}

/// The state a cluster reaches after admitting each of `members`.
/// @param members What it has agreed on.
/// @return The state.
[[nodiscard]] ClusterState StateOf(std::vector<ClusterMember> members)
{
    return ClusterState { .members = std::move(members), .settings = {}, .principals = {}, .revokedKeys = {} };
}

/// The identity key @p id holds: the shared test key, so two ids never hold one key.
/// @param id The machine.
/// @return Its public key.
[[nodiscard]] Ed25519PublicKey Key(std::string const& id)
{
    return Testing::TestKeyPair(id).PublicKey();
}

/// `MembershipProposals`, spelled without the span conversion at every call.
/// @param state What the cluster holds.
/// @param desired What this node believes.
/// @param active What consensus counts; nobody by default, so every member the state
///        does not record is a newcomer.
/// @return The whole plan: what to propose, and what was refused.
[[nodiscard]] MembershipPlan Plan(ClusterState const& state,
                                  std::vector<DesiredMember> const& desired,
                                  Consensus::Configuration const& active = {})
{
    return MembershipProposals(state, active, std::span<DesiredMember const> { desired });
}

/// The proposals alone, which is all most cases are about.
/// @param state What the cluster holds.
/// @param desired What this node believes.
/// @param active What consensus counts; nobody by default.
/// @return The proposals.
[[nodiscard]] std::vector<Command> Proposals(ClusterState const& state,
                                             std::vector<DesiredMember> const& desired,
                                             Consensus::Configuration const& active = {})
{
    return Plan(state, desired, active).proposals;
}

/// A configuration of both sets.
/// @param voters Counted by every quorum.
/// @param learners Counted by none.
/// @return The configuration.
[[nodiscard]] Consensus::Configuration Configured(std::vector<Consensus::NodeId> voters,
                                                  std::vector<Consensus::NodeId> learners)
{
    return Consensus::Configuration { .voters = std::move(voters), .learners = std::move(learners) };
}

/// A configuration of voters alone: every configuration before #1449.
/// @param voters The voters.
/// @return The configuration.
[[nodiscard]] Consensus::Configuration Voters(std::vector<Consensus::NodeId> voters)
{
    return Consensus::Configuration { .voters = std::move(voters), .learners = {} };
}

/// What the leader knows when every member it counts or replicates to has caught up.
///
/// The default of every case that is not ABOUT catching up (#1537): a promotion then
/// waits for nothing but a dialable address, which is what those cases pin.
/// @param active The configuration.
/// @return Every member of it at the commit index.
[[nodiscard]] Replication EveryoneCaughtUp(Consensus::Configuration const& active)
{
    constexpr auto Committed = Consensus::LogIndex { .value = 7 };
    auto replication = Replication { .commitIndex = Committed, .matchIndex = {} };
    for (auto const& id: active.voters)
        replication.matchIndex.emplace(id, Committed);
    for (auto const& id: active.learners)
        replication.matchIndex.emplace(id, Committed);
    return replication;
}

/// `NextQuorumChange`'s change alone, with everybody caught up.
/// @param state What the cluster holds.
/// @param active What consensus holds, both sets.
/// @param self This node's own record.
/// @param bootstrap What this node was started with.
/// @return The proposed configuration, or nullopt.
[[nodiscard]] std::optional<Consensus::Configuration> Step(ClusterState const& state,
                                                           Consensus::Configuration const& active,
                                                           ClusterMember const& self,
                                                           std::vector<Consensus::NodeId> const& bootstrap)
{
    return NextQuorumChange(state, active, self, bootstrap, EveryoneCaughtUp(active)).change;
}

/// `NextQuorumChange`, spelled without the span conversions at every call.
///
/// `bootstrap` defaults to this node alone, which is the discovery-formed shape and
/// the one that leaves every other member removable -- the cases about a typed
/// cluster pass their own.
/// @param state What the cluster holds.
/// @param active What consensus holds, both sets.
/// @param self This node's id. Its record is given an endpoint on a host of its own,
///        which no case here forgets -- the cases about a forgotten leader call
///        `Step` with the record they mean.
/// @param bootstrap What this node was started with; itself by default.
/// @return The proposed configuration, or nullopt.
[[nodiscard]] std::optional<Consensus::Configuration> QuorumChange(
    ClusterState const& state,
    Consensus::Configuration const& active,
    Consensus::NodeId const& self = "n1",
    std::optional<std::vector<Consensus::NodeId>> const& bootstrap = std::nullopt)
{
    auto const started = bootstrap.value_or(std::vector<Consensus::NodeId> { self });
    return Step(state, active, Member(self, self + ".self:6675"), started);
}

/// The same, for a configuration of voters alone -- which every case before #1449 was.
/// @param state What the cluster holds.
/// @param voters What consensus counts.
/// @param self This node's id.
/// @param bootstrap What this node was started with; itself by default.
/// @return The proposed configuration, or nullopt.
[[nodiscard]] std::optional<Consensus::Configuration> QuorumChange(
    ClusterState const& state,
    std::vector<Consensus::NodeId> const& voters,
    Consensus::NodeId const& self = "n1",
    std::optional<std::vector<Consensus::NodeId>> const& bootstrap = std::nullopt)
{
    return QuorumChange(state, Voters(voters), self, bootstrap);
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
    // `n1` is this node, counted since it bootstrapped and recorded by nobody yet, so it
    // is recorded where consensus counts it; `n2` is placed nowhere and joins as the
    // learner a newcomer is (#1535).
    ClusterState const state;
    auto const proposals = Proposals(
        state, { Desire("n1", "10.0.0.1:6675", "10.0.0.1:7000"), Desire("n2", "10.0.0.2:6675") }, Voters({ "n1" }));

    REQUIRE(proposals.size() == 2);
    CHECK(proposals[0]
          == Command { .kind = CommandKind::AddMember,
                       .key = "n1",
                       .value = "10.0.0.1:6675",
                       .schedulerEndpoint = "10.0.0.1:7000",
                       .publicKey = std::nullopt,
                       .role = std::nullopt });
    CHECK(proposals[1]
          == Command { .kind = CommandKind::AddLearner,
                       .key = "n2",
                       .value = "10.0.0.2:6675",
                       .schedulerEndpoint = {},
                       .publicKey = std::nullopt,
                       .role = std::nullopt });
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
    //
    // A record with no consensus endpoint is half a record only where its seat is DIALLED:
    // `n2` is recorded a voter, so the desire that would re-admit it without an address is
    // dropped. A learner's empty endpoint is a whole record, and has a case of its own below.
    auto const state = StateOf({ Member("n2", "10.0.0.2:6675") });
    auto const proposals =
        Proposals(state, { Desire("", "10.0.0.1:6675"), Desire("n2", ""), Desire("n3", "10.0.0.3:6675") });

    REQUIRE(proposals.size() == 1);
    CHECK(proposals[0].key == "n3");
}

TEST_CASE("A learner's own empty endpoint is desired as it is and not skipped", "[cluster][membership][learner][formation]")
{
    // A learner dials in, so its record carries no consensus endpoint -- and the desire
    // that moves its scheduler endpoint must not be mistaken for a half-record and dropped,
    // or the learner's announcement never lands.
    auto const state = StateOf({ Learner("laptop", "") });
    auto const plan = Plan(state, { Desire("laptop", "", "laptop:6674") }, Voters({ "office" }));

    REQUIRE(plan.proposals.size() == 1);
    CHECK(plan.proposals[0].kind == CommandKind::AddLearner);
    CHECK(plan.proposals[0].value.empty());
    CHECK(plan.proposals[0].schedulerEndpoint == "laptop:6674");
    CHECK(plan.forgotten.empty());
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

    // Two changes (#1537): into the learners, where it is replicated to and counted by
    // nothing, and then promoted -- once it has caught up, which everybody has here.
    auto const added = QuorumChange(state, { "n1", "n2" });
    REQUIRE(added.has_value());
    CHECK(Unwrap(added) == Configured({ "n1", "n2" }, { "n3" }));

    auto const counted = QuorumChange(state, Unwrap(added));
    REQUIRE(counted.has_value());
    CHECK(Unwrap(counted) == Voters({ "n1", "n2", "n3" }));
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
    CHECK(Unwrap(change).voters.size() == 1);
    CHECK(Unwrap(change).learners.size() == 1);
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
    CHECK(Unwrap(change) == Voters({ "n1", "n2" }));
}

TEST_CASE("A member an operator typed is never proposed for removal", "[cluster][membership][quorum]")
{
    // The defect this parameter exists for, and it took a running cluster to find:
    // the bootstrap set puts a member in the CONFIGURATION and nothing puts it in the
    // STATE, so on a cluster whose members were bootstrapped rather than admitted the
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
    CHECK(Unwrap(shrunk) == Voters({ "n1", "n2", "n3" }));
}

TEST_CASE("Growing comes before shrinking", "[cluster][membership][quorum]")
{
    // A replacement -- one machine out, one in -- is two steps, and this is the
    // order that keeps the quorum reachable throughout. The other one passes through
    // a configuration smaller than either endpoint.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n3", "10.0.0.3:6675") });

    auto const change = QuorumChange(state, { "n1", "n2" });
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Configured({ "n1", "n2" }, { "n3" }));
}

TEST_CASE("A member with no dialable address is not counted", "[cluster][membership][quorum]")
{
    // Counting a node the transport cannot reach is the failure this whole change
    // exists to avoid, reached from the other side: the quorum grows and the votes
    // to satisfy it can never arrive.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "not-an-endpoint") });

    CHECK_FALSE(QuorumChange(state, { "n1" }).has_value());
}

TEST_CASE("A learner with no endpoint is added to the configuration and a voter with none is not",
          "[cluster][membership][quorum][learner][formation]")
{
    // Whether an addition waits for an address is the SEAT's link: a learner dials in, so
    // replication reaches it over the session it opens and it needs none; a member recorded
    // a voter is dialled, and one with no address would join and never catch up -- so it is
    // never added, and so never promoted.
    auto const learner = StateOf({ Member("office", "10.0.0.1:6675"), Learner("laptop", "") });
    auto const added = QuorumChange(learner, Voters({ "office" }), "office");
    REQUIRE(added.has_value());
    CHECK(Unwrap(added) == Configured({ "office" }, { "laptop" }));

    auto const voter = StateOf({ Member("office", "10.0.0.1:6675"), Member("desk", "") });
    CHECK_FALSE(QuorumChange(voter, Voters({ "office" }), "office").has_value());
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
    // A node that joined a fleet was told nothing about the cluster's shape, so every
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
    CHECK(Unwrap(change) == Configured({ "n4" }, { "n5" }));
}

namespace
{
/// What a forget of `id` leaves in `state` besides the missing record: the key it was admitted
/// under, revoked under its id (#1555).
/// @param state The state to add it to.
/// @param id The forgotten member.
/// @param fill The key's byte, so two forgotten members hold two keys.
void RevokeUnder(ClusterState& state, Consensus::NodeId const& id, std::uint8_t fill)
{
    auto key = Ed25519PublicKey {};
    key.fill(std::byte { fill });
    state.revokedKeys.push_back(RevokedKey { .id = id, .publicKey = key });
}
} // namespace

TEST_CASE("A member the cluster forgot leaves the quorum whoever typed it, because its key is revoked",
          "[cluster][membership][quorum][forget]")
{
    // #1555. The forget revoked n3's key, which a member the configuration counts keeps for
    // itself -- so kept here, n3 would go on voting for as long as it runs. The rules above
    // that keep a TYPED member, and keep everything on a node given no bootstrap set, are
    // about ABSENCE, which this is not.
    auto state = StateOf({ Member("n1", "10.0.0.1:6675") });
    RevokeUnder(state, "n3", 0x33);
    auto const typed = std::vector<Consensus::NodeId> { "n1", "n2", "n3" };

    // In this node's own bootstrap set, and removed anyway. n2 -- typed, absent and
    // never forgotten -- is the control beside it, and stays.
    auto const change = QuorumChange(state, { "n1", "n2", "n3" }, "n1", typed);
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Voters({ "n1", "n2" }));

    // On a joined node, which has no bootstrap set to compare against: removed too, and
    // this is the successor that has to do it when the forgotten member was the LEADER -- cut
    // off by its revoked key before the removal it proposes for itself could commit.
    auto const joined = QuorumChange(state, { "n1", "n2", "n3" }, "n1", std::vector<Consensus::NodeId> {});
    REQUIRE(joined.has_value());
    CHECK(Unwrap(joined) == Voters({ "n1", "n2" }));

    // A learner is removed on exactly the terms a voter is.
    auto const learner = QuorumChange(state, Configured({ "n1", "n2" }, { "n3" }), "n1", typed);
    REQUIRE(learner.has_value());
    CHECK(Unwrap(learner) == Voters({ "n1", "n2" }));
}

TEST_CASE("A member admitted again after a forget is counted, whatever its old key's entry says",
          "[cluster][membership][quorum][forget]")
{
    // The revocation outlives the record it came from, so the question is asked of the id
    // RECORDED NOW: n3 re-admitted under a new identity is a member, and the entry naming its
    // old key is history.
    auto state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n3", "10.0.0.3:6675") });
    RevokeUnder(state, "n3", 0x33);

    CHECK_FALSE(
        QuorumChange(state, { "n1", "n2", "n3" }, "n1", std::vector<Consensus::NodeId> { "n1", "n2", "n3" }).has_value());
    CHECK_FALSE(QuorumChange(state, { "n1", "n3" }, "n1", std::vector<Consensus::NodeId> {}).has_value());
}

TEST_CASE("A forgotten leader that shares its machine over loopback is forgotten by its revoked key",
          "[cluster][membership][quorum][forget]")
{
    // On a rig whose members share one machine an address names no machine at all, which is
    // no loss: a forget is a key revocation, and the key names the id.
    auto state = StateOf({ Member("n2", "127.0.0.1:6681"), Member("n3", "127.0.0.1:6682") });
    RevokeUnder(state, "n1", 0x11);

    auto const change = Step(state,
                             Voters({ "n1", "n2", "n3" }),
                             Member("n1", "127.0.0.1:6680"),
                             std::vector<Consensus::NodeId> { "n1", "n2", "n3" });
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Voters({ "n2", "n3" }));

    // The control: another id's revoked key forgets nobody else.
    auto other = StateOf({ Member("n2", "127.0.0.1:6681"), Member("n3", "127.0.0.1:6682") });
    RevokeUnder(other, "n9", 0x99);
    CHECK_FALSE(Step(other,
                     Voters({ "n1", "n2", "n3" }),
                     Member("n1", "127.0.0.1:6680"),
                     std::vector<Consensus::NodeId> { "n1", "n2", "n3" })
                    .has_value());
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

    CHECK_FALSE(QuorumChange(state, Consensus::Configuration {}).has_value());
}

// --------------------------------------------------------------------------
// Learners (#1449): which SET a member is in is part of its record, and consensus is
// moved towards it one change at a time.

TEST_CASE("A member admitted as a learner is added to the learners", "[cluster][membership][quorum][learner]")
{
    // Into the set its record names, and nowhere else: a learner addition changes no
    // quorum, which is what makes it safe at any size -- and a reconciler that put it
    // among the voters would grow the very majority the operator admitted a learner to
    // keep small.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Learner("laptop", "10.0.0.9:6675") });

    auto const change = QuorumChange(state, Voters({ "n1" }));
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Configured({ "n1" }, { "laptop" }));
}

TEST_CASE("A learner is never proposed for removal because it is absent", "[cluster][membership][quorum][learner]")
{
    // The ticket's own clause: absence from the network is not removal, and absence
    // from the STATE is removal only for a member admitted at runtime and then
    // forgotten. Nothing here asks whether a member answers, so a learner on a laptop
    // that has been shut for a week is as safe as one that answered a moment ago -- and
    // so is one whose recorded address has since stopped reading as an address.
    SECTION("recorded, and unreachable")
    {
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Learner("laptop", "not-an-endpoint") });
        CHECK_FALSE(QuorumChange(state, Configured({ "n1" }, { "laptop" })).has_value());
    }

    SECTION("typed into the bootstrap set and never recorded")
    {
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675") });
        CHECK_FALSE(
            QuorumChange(state, Configured({ "n1" }, { "laptop" }), "n1", std::vector<Consensus::NodeId> { "n1", "laptop" })
                .has_value());
    }

    SECTION("on a node given no bootstrap set, which removes nobody")
    {
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675") });
        CHECK_FALSE(
            QuorumChange(state, Configured({ "n1" }, { "laptop" }), "n1", std::vector<Consensus::NodeId> {}).has_value());
    }
}

TEST_CASE("A learner the operator forgot is removed, like any member", "[cluster][membership][quorum][learner]")
{
    // The control the case above needs: a reconciler that never removed a learner at
    // all would pass it. `--cluster-forget` takes the record out of the state, and a
    // learner admitted at runtime is un-admitted at runtime.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675") });

    auto const change = QuorumChange(state, Configured({ "n1" }, { "laptop" }));
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Configured({ "n1" }, {}));
}

TEST_CASE("A learner the record names a voter is promoted, once it can be dialled", "[cluster][membership][quorum][learner]")
{
    // A promotion is a voter ADDITION and follows its rule: never counted before every
    // node can dial it. One change -- the member moves, and nobody else does.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:6675") });

    auto const change = QuorumChange(state, Configured({ "n1" }, { "n2" }));
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Configured({ "n1", "n2" }, {}));

    auto const undialable = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:0") });
    CHECK_FALSE(QuorumChange(undialable, Configured({ "n1" }, { "n2" })).has_value());
}

TEST_CASE("A voter the record names a learner is demoted, and never the last one", "[cluster][membership][quorum][learner]")
{
    // A demotion counts nobody new, so it needs no address -- but a configuration with
    // no voter can commit nothing, including the change that would undo it. So the last
    // voter stays one, and the record saying otherwise is the fail-closed direction.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Learner("n2", "not-an-endpoint") });

    auto const change = QuorumChange(state, Voters({ "n1", "n2" }));
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Configured({ "n1" }, { "n2" }));

    auto const alone = StateOf({ Learner("n1", "10.0.0.1:6675") });
    CHECK_FALSE(QuorumChange(alone, Voters({ "n1" })).has_value());
}

TEST_CASE("Additions come first, then promotions, then demotions, then removals", "[cluster][membership][quorum][learner]")
{
    // Growing the voter set before shrinking it, for the reason additions have always
    // preceded removals: the other order passes through a configuration smaller than
    // either end. Walked to the end one step at a time, as the reconciler does, so each
    // step is asserted rather than only the first.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"),
                                 Member("n2", "10.0.0.2:6675"),
                                 Learner("n3", "10.0.0.3:6675"),
                                 Learner("n4", "10.0.0.4:6675") });
    auto active = Configured({ "n1", "n3", "n5" }, { "n2" });

    auto const step = [&state, &active] {
        auto const change = QuorumChange(state, active);
        REQUIRE(change.has_value());
        active = Unwrap(change);
        return active;
    };

    CHECK(step() == Configured({ "n1", "n3", "n5" }, { "n2", "n4" })); // n4 added, as a learner
    CHECK(step() == Configured({ "n1", "n3", "n5", "n2" }, { "n4" })); // n2 promoted
    CHECK(step() == Configured({ "n1", "n5", "n2" }, { "n4", "n3" })); // n3 demoted
    CHECK(step() == Configured({ "n1", "n2" }, { "n4", "n3" }));       // n5, forgotten, removed
    CHECK_FALSE(QuorumChange(state, active).has_value());
}

TEST_CASE("A seat something already placed is never the desire's to change", "[cluster][membership][learner]")
{
    // A node desires ITSELF on every pass and discovery desires every peer it proves, and
    // neither says anything about a seat (#1449, #1535). Were an unrecorded seat read as
    // either one, an operator's decision would be undone one interval after it committed.
    SECTION("this node's own record keeps the operator's demotion")
    {
        auto const state = StateOf({ Learner("n1", "10.0.0.1:6675") });
        CHECK(Proposals(state, { Desire("n1", "10.0.0.1:6675", std::string {}) }, Voters({ "n1" })).empty());
    }

    SECTION("a rediscovered learner the operator promoted stays promoted")
    {
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:6675") });
        CHECK(Proposals(state, { Desire("n2", "10.0.0.2:6675") }, Configured({ "n1" }, { "n2" })).empty());
    }

    SECTION("the record outranks a configuration that has not caught up with it")
    {
        // A demotion in flight: recorded a learner, still counted a voter. Reading the
        // configuration first would re-record a voter and undo it.
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Learner("n2", "10.0.0.2:6675") });
        CHECK(Proposals(state, { Desire("n2", "10.0.0.2:6675") }, Voters({ "n1", "n2" })).empty());
    }

    SECTION("a member counted and recorded nowhere else takes the set it is counted in")
    {
        auto const typed = Proposals(ClusterState {}, { Desire("n2", "10.0.0.2:6675") }, Voters({ "n1", "n2" }));
        REQUIRE(typed.size() == 1);
        CHECK(typed[0].kind == CommandKind::AddMember);

        auto const learner = Proposals(ClusterState {}, { Desire("n3", "10.0.0.3:6675") }, Configured({ "n1" }, { "n3" }));
        REQUIRE(learner.size() == 1);
        CHECK(learner[0].kind == CommandKind::AddLearner);
    }

    SECTION("and only a member placed nowhere is a newcomer, recorded as a learner")
    {
        auto const fresh = Proposals(ClusterState {}, { Desire("n4", "10.0.0.4:6675") }, Voters({ "n1" }));
        REQUIRE(fresh.size() == 1);
        CHECK(fresh[0].kind == CommandKind::AddLearner);
    }
}

// --------------------------------------------------------------------------
// Forgetting (#1528): a forget is a positive act, and what a leader merely OBSERVES --
// a peer proving the key, its own record -- must not undo it.

namespace
{
/// A leader's reconcile passes, as `ConsensusTier::Reconcile` runs them.
///
/// Both halves decide from the state read at the TOP of a pass, as the tier does -- the
/// proposals and `ReconcileQuorum` see one snapshot -- and everything a pass proposes
/// has committed by the next one, which is the tier's pending-change wait letting one
/// configuration change through per commit. Composed from the two functions the tier
/// calls rather than restated, so a case here exercises the decisions production makes.
struct Leader
{
    ClusterState state;                  ///< What the cluster has applied.
    Consensus::Configuration active;     ///< What consensus counts.
    Consensus::NodeId self;              ///< This node.
    std::vector<Consensus::NodeId> boot; ///< What it was started with.
    std::vector<DesiredMember> desired;  ///< Itself, and whatever discovery has proved.

    /// The keys this node holds live: every machine of these cases, typed with its own key, as
    /// a bootstrap member names it.
    std::shared_ptr<Testing::SharedRoster const> typed = Testing::SharedRoster::Of({ "n1", "n2", "n3", "n4" });

    /// One pass: propose what the state does not yet say, and move the quorum one step. A
    /// desire that states no key takes the one this node holds live, as the tier's does
    /// (`WithLiveKeys`) -- a member is never admitted without a key.
    void Pass()
    {
        auto const top = state;
        auto const keys = Testing::RosterPeerKeys { Testing::TestKeyPair(self), typed };
        for (auto const& command: Proposals(top, WithLiveKeys(top, desired, keys), active))
            Apply(state, command);
        if (auto const change = Step(top, active, Self(), boot); change.has_value())
            active = Unwrap(change);
    }

    /// This node's own record as it announces it: its id, where its desire says it answers.
    /// @return The record.
    [[nodiscard]] ClusterMember Self() const
    {
        auto const it = std::ranges::find(desired, self, &DesiredMember::id);
        return Member(self, it != desired.end() ? it->raftEndpoint : std::string {});
    }

    /// Whether the state records `id`.
    /// @param id The member.
    /// @return True when a member record names it.
    [[nodiscard]] bool Records(std::string_view id) const
    {
        return std::ranges::find(state.members, id, &ClusterMember::id) != state.members.end();
    }
};

/// A leader that bootstrapped alone and admitted `n2` and `n3` because discovery proved them.
///
/// The shape formation builds: one machine bootstraps only itself, so
/// every other member was admitted at runtime -- which is what makes it removable.
/// @return The leader, settled: a pass proposes nothing.
[[nodiscard]] Leader DiscoveredCluster()
{
    return Leader { .state = StateOf(
                        { Member("n1", "10.0.0.1:6680"), Member("n2", "10.0.0.2:6680"), Member("n3", "10.0.0.3:6680") }),
                    .active = Voters({ "n1", "n2", "n3" }),
                    .self = "n1",
                    .boot = { "n1" },
                    .desired = { Desire("n1", "10.0.0.1:6680", std::string {}),
                                 Desire("n2", "10.0.0.2:6680"),
                                 Desire("n3", "10.0.0.3:6680") } };
}

/// What `--cluster-forget=<id>` commits.
/// @param id The member.
/// @return The command.
[[nodiscard]] Command Forget(std::string id)
{
    return Command { .kind = CommandKind::Forget,
                     .key = std::move(id),
                     .value = {},
                     .schedulerEndpoint = {},
                     .publicKey = std::nullopt,
                     .role = std::nullopt };
}
} // namespace

TEST_CASE("A member the operator forgot stays forgotten while discovery still proves it", "[cluster][membership][forget]")
{
    // #1528. `n3` still holds the key, so discovery goes on proving it and this node goes
    // on desiring it; the forget is the operator saying that no longer admits it. Several
    // passes, each asserted, because the failure is a re-admission on the NEXT pass and a
    // configuration that then flaps -- removed from the quorum on one pass and added back
    // on the one after.
    auto leader = DiscoveredCluster();
    leader.Pass();
    REQUIRE(leader.active == Voters({ "n1", "n2", "n3" }));

    Apply(leader.state, Forget("n3"));
    REQUIRE(leader.state.IsRevoked(Key("n3")));

    for (auto const pass: std::views::iota(0, 4))
    {
        leader.Pass();
        INFO("reconcile pass " << pass);
        CHECK_FALSE(leader.Records("n3"));
        CHECK(leader.state.IsRevoked(Key("n3")));
    }

    // And the forget reached the quorum: `n3` was admitted at runtime, so it is removed.
    CHECK(leader.active == Voters({ "n1", "n2" }));
}

TEST_CASE("A desire for an id whose key a forget revoked is refused, wherever it now dials from",
          "[cluster][membership][forget]")
{
    // A machine is forgotten by its key, and an address is not an identity: the forgotten
    // machine dialling from somewhere new is still the forgotten machine, refused by name
    // rather than dropped -- a desire nothing proposes for and nothing reports reads exactly
    // like one the state already matches, and the tier logs this list.
    auto state = ClusterState {};
    Apply(state,
          Command { .kind = CommandKind::AddMember,
                    .key = "n2",
                    .value = "10.0.0.2:6680",
                    .schedulerEndpoint = {},
                    .publicKey = Key("n2"),
                    .role = std::nullopt });
    Apply(state, Forget("n2"));
    auto const active = Voters({ "n1" });
    auto const desire = DesiredMember {
        .id = "n2", .raftEndpoint = "10.9.9.9:6680", .schedulerEndpoint = std::nullopt, .publicKey = Key("n2")
    };
    auto const plan = MembershipProposals(state, active, std::span { &desire, 1 });
    CHECK(plan.proposals.empty());
    REQUIRE(plan.forgotten.size() == 1);
    CHECK(plan.forgotten.front().id == "n2");

    // Whatever the desire says about its key, and wherever it answers: discovery states no
    // key, and this node's own record is an observation like any other -- a leader that was
    // forgotten does not put itself back. Beside a machine nobody forgot, which is proposed.
    auto const mixed = Plan(
        state,
        { Desire("n2", "10.0.0.2:6680"), Desire("n2", "127.0.0.1:6680", std::string {}), Desire("n4", "10.0.0.4:6680") },
        active);
    REQUIRE(mixed.proposals.size() == 1);
    CHECK(mixed.proposals[0].key == "n4");
    CHECK(mixed.forgotten.size() == 2);
}

TEST_CASE("A machine re-admitted under a new key is served again, and its revoked key is not",
          "[cluster][membership][forget]")
{
    // The operator's re-admission under a NEW key is the one way back, and after it the id is
    // recorded again: a move of the machine it now is still proposes. A desire still carrying
    // the key the forget revoked is the FORGOTTEN machine, whatever the record now says --
    // refused by name here rather than proposed and refused `KeyRevoked` at every pass.
    auto state = StateOf({ Member("n1", "10.0.0.1:6680"), Member("n2", "10.0.0.2:6680") });
    Apply(state, Forget("n2"));
    auto readmitted = Key("n2-again");
    Apply(state,
          Command { .kind = CommandKind::AddMember,
                    .key = "n2",
                    .value = "10.0.0.2:6680",
                    .schedulerEndpoint = {},
                    .publicKey = readmitted,
                    .role = std::nullopt });
    REQUIRE(state.IsRevoked(Key("n2")));
    REQUIRE(std::ranges::contains(state.revokedKeys, "n2", &RevokedKey::id));

    // A desire the record already matches is no refusal, and proposes nothing.
    auto const settled = Plan(state, { Desire("n2", "10.0.0.2:6680") });
    CHECK(settled.proposals.empty());
    CHECK(settled.forgotten.empty());

    // A move of the machine it now is -- no key stated, so the recorded new one stands.
    auto const moved = Plan(state, { Desire("n2", "10.0.0.12:6680") });
    CHECK(moved.forgotten.empty());
    REQUIRE(moved.proposals.size() == 1);
    CHECK(moved.proposals[0].value == "10.0.0.12:6680");

    // The old machine, still holding the revoked key, is refused by name.
    auto stale = Desire("n2", "10.0.0.12:6680");
    stale.publicKey = Key("n2");
    auto const refused = Plan(state, { stale });
    CHECK(refused.proposals.empty());
    REQUIRE(refused.forgotten.size() == 1);
    CHECK(refused.forgotten[0].id == "n2");
}

TEST_CASE("A desire that states no key takes the key this node holds live, and never over a recorded one",
          "[cluster][membership][forget]")
{
    // Discovery states no key, and a member is never admitted without one. A bootstrap member
    // the formation record names WITH its key, recorded nowhere yet, is admitted under that key
    // -- the one this node holds live for it -- rather than proposed keyless and refused at
    // every pass.
    auto const typed = std::vector { MemberSpec { .id = "n2", .raftEndpoint = "10.0.0.2:6680", .publicKey = Key("n2") } };
    RosterKeys roster { Testing::TestKeyPair("n1"), typed };
    auto const desired = std::vector { Desire("n2", "10.0.0.2:6680"), Desire("n9", "10.0.0.9:6680") };

    auto const state = StateOf({});
    auto const keyed = WithLiveKeys(state, desired, roster);
    REQUIRE(keyed.size() == 2);
    CHECK(keyed[0].publicKey == std::optional { Key("n2") });
    // Nobody holds a key for n9, so there is nothing to fill in, and its admission stays one
    // `ValidateAgainst` refuses.
    CHECK_FALSE(keyed[1].publicKey.has_value());

    auto const plan = MembershipProposals(state, Voters({ "n1", "n2" }), keyed);
    REQUIRE(plan.proposals.size() == 2);
    CHECK(ValidateAgainst(state, plan.proposals[0]).has_value());
    CHECK_FALSE(ValidateAgainst(state, plan.proposals[1]).has_value());

    // Never kept: the desires this node holds are untouched, so a key filled in for one pass
    // cannot outlive the facts it was read from.
    CHECK_FALSE(desired[0].publicKey.has_value());

    // A recorded key wins: re-admitted under a new key, the record stands and the desire keeps
    // no opinion -- it does not bring the typed key back over the operator's decision.
    auto readmitted = StateOf({ Member("n2", "10.0.0.2:6680") });
    readmitted.members[0].publicKey = Key("n2-again");
    roster.Adopt(readmitted);
    auto const kept = WithLiveKeys(readmitted, desired, roster);
    CHECK_FALSE(kept[0].publicKey.has_value());

    // And a desire that states its own key keeps it.
    auto stated = Desire("n2", "10.0.0.2:6680");
    stated.publicKey = Key("n2-stated");
    CHECK(WithLiveKeys(state, std::span { &stated, 1 }, roster).front().publicKey == std::optional { Key("n2-stated") });
}

TEST_CASE("A member nobody forgot is still admitted when discovery proves it", "[cluster][membership][forget]")
{
    // The control the case above needs: a reconciler that stopped admitting whatever
    // discovery proved, or stopped re-proposing a record that changed, would pass it.
    // Beside a forgotten member, so the refusal is seen to be about THAT member.
    auto leader = DiscoveredCluster();
    Apply(leader.state, Forget("n3"));
    leader.desired.push_back(Desire("n4", "10.0.0.4:6680"));
    leader.desired[1] = Desire("n2", "10.0.0.12:6680");

    for ([[maybe_unused]] auto const pass: std::views::iota(0, 4))
        leader.Pass();

    CHECK(leader.Records("n4"));
    CHECK(Consensus::Membership::IsMember(leader.active, "n4"));
    CHECK(leader.state.RaftEndpointOf("n2") == "10.0.0.12:6680");
    CHECK_FALSE(leader.Records("n3"));
}

TEST_CASE("Only an operator's admit brings a forgotten member back", "[cluster][membership][forget]")
{
    // Re-admission is a positive act too, and it is the operator's: `--cluster-admit`
    // commits the record -- under a NEW key, because the one the forget revoked is never
    // admitted again. After that the machine is desired and recorded again, and discovery's
    // desire has nothing left to change.
    auto leader = DiscoveredCluster();
    Apply(leader.state, Forget("n3"));
    leader.Pass();
    leader.Pass();
    REQUIRE_FALSE(leader.Records("n3"));

    auto readmit = Command { .kind = CommandKind::AddMember,
                             .key = "n3",
                             .value = "10.0.0.3:6680",
                             .schedulerEndpoint = {},
                             .publicKey = Key("n3"),
                             .role = std::nullopt };
    // The revoked key is refused for good, and names whose it was.
    auto const refused = ValidateAgainst(leader.state, readmit);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::KeyRevoked);

    readmit.publicKey = Key("n3-reminted");
    REQUIRE(ValidateAgainst(leader.state, readmit).has_value());
    Apply(leader.state, readmit);

    for ([[maybe_unused]] auto const pass: std::views::iota(0, 4))
        leader.Pass();

    CHECK(leader.Records("n3"));
    CHECK(Consensus::Membership::IsMember(leader.active, "n3"));
}

// --------------------------------------------------------------------------
// Learner first (#1535): a machine discovery proves is replicated to and counted by
// nothing, until an operator decides it should vote.

namespace
{
/// What `--cluster-admit=<id>=<endpoint>` commits: a voter's record.
/// @param id The member.
/// @param raft Where its consensus port answers.
/// @return The command.
[[nodiscard]] Command Admit(std::string id, std::string raft)
{
    return Command { .kind = CommandKind::AddMember,
                     .key = std::move(id),
                     .value = std::move(raft),
                     .schedulerEndpoint = {},
                     .publicKey = std::nullopt,
                     .role = std::nullopt };
}
} // namespace

TEST_CASE("A machine discovery proves joins as a learner, and the quorum grows only when an operator promotes it",
          "[cluster][membership][learner][discovery]")
{
    // #1535. `n1` bootstrapped alone and `n2` proved the key. Admitted as a voter the
    // moment it could be dialled, `n2` would make every commit need both machines from
    // then on -- asserted at EVERY pass, because the failure is the quorum growing on the
    // pass after the record commits rather than at the end.
    auto leader = Leader { .state = StateOf({}),
                           .active = Voters({ "n1" }),
                           .self = "n1",
                           .boot = { "n1" },
                           .desired = { Desire("n1", "10.0.0.1:6680", std::string {}), Desire("n2", "10.0.0.2:6680") } };

    for (auto const pass: std::views::iota(0, 4))
    {
        leader.Pass();
        INFO("reconcile pass " << pass);
        CHECK(leader.active.voters == std::vector<Consensus::NodeId> { "n1" });
    }

    // Recorded as a learner, and replicated to: in the configuration, counted by nothing.
    REQUIRE(leader.Records("n2"));
    CHECK(RecordedSeatOf(leader.state, "n2") == MemberSeat::Learner);
    CHECK(leader.active == Configured({ "n1" }, { "n2" }));

    // Promotion is the operator's act, and it is one more change -- the voter set grows by
    // exactly this member, once.
    Apply(leader.state, Admit("n2", "10.0.0.2:6680"));
    for ([[maybe_unused]] auto const pass: std::views::iota(0, 4))
        leader.Pass();
    CHECK(leader.active == Voters({ "n1", "n2" }));

    // And discovery, which goes on proving `n2` on every beacon, does not demote it back.
    CHECK(RecordedSeatOf(leader.state, "n2") == MemberSeat::Voter);
}

TEST_CASE("A machine recorded as a voter and discovered again stays a voter", "[cluster][membership][learner][discovery]")
{
    // The control the case above needs: a reconciler that recorded every proven peer as a
    // learner would pass it, and would demote every voter discovery rediscovered.
    SECTION("recorded by the state")
    {
        auto leader = Leader { .state = StateOf({ Member("n1", "10.0.0.1:6680"), Member("n2", "10.0.0.2:6680") }),
                               .active = Voters({ "n1", "n2" }),
                               .self = "n1",
                               .boot = { "n1" },
                               .desired = { Desire("n1", "10.0.0.1:6680", std::string {}), Desire("n2", "10.0.0.2:6680") } };
        for ([[maybe_unused]] auto const pass: std::views::iota(0, 4))
            leader.Pass();

        CHECK(RecordedSeatOf(leader.state, "n2") == MemberSeat::Voter);
        CHECK(leader.active == Voters({ "n1", "n2" }));
    }

    SECTION("in the bootstrap set, so counted by the configuration and recorded nowhere")
    {
        // The case a desire's source cannot see: nothing puts a typed member in the state,
        // so to anything reading only the state it looks exactly like a newcomer.
        auto leader = Leader { .state = StateOf({}),
                               .active = Voters({ "n1", "n2" }),
                               .self = "n1",
                               .boot = { "n1", "n2" },
                               .desired = { Desire("n1", "10.0.0.1:6680", std::string {}), Desire("n2", "10.0.0.2:6680") } };
        for (auto const pass: std::views::iota(0, 4))
        {
            leader.Pass();
            INFO("reconcile pass " << pass);
            CHECK(leader.active == Voters({ "n1", "n2" }));
        }

        REQUIRE(leader.Records("n2"));
        CHECK(RecordedSeatOf(leader.state, "n2") == MemberSeat::Voter);
    }
}

// --------------------------------------------------------------------------
// A forgotten leader (#1539): a forget means the same thing whoever leads.

TEST_CASE("A forgotten leader proposes its own removal, after every other change", "[cluster][membership][quorum][forget]")
{
    // `n1` leads and the operator forgot it: its record gone and its key revoked -- the two
    // facts `Forget` writes, written here by the forget itself.
    auto state = StateOf({ Member("n1", "10.0.0.1:6680"), Member("n2", "10.0.0.2:6680"), Member("n3", "10.0.0.3:6680") });
    Apply(state, Forget("n1"));
    REQUIRE(state.IsRevoked(Key("n1")));
    auto const self = Member("n1", "10.0.0.1:6680");
    auto const typed = std::vector<Consensus::NodeId> { "n1", "n2", "n3" };

    // Its own bootstrap entry does not protect it: it names itself because it could not
    // start otherwise, which asserts nothing about whether it belongs.
    auto const change = Step(state, Voters({ "n1", "n2", "n3" }), self, typed);
    REQUIRE(change.has_value());
    CHECK(Unwrap(change) == Voters({ "n2", "n3" }));

    // Nor does having no bootstrap set at all, which stops a node removing any PEER.
    auto const joined = Step(state, Voters({ "n1", "n2", "n3" }), self, std::vector<Consensus::NodeId> {});
    REQUIRE(joined.has_value());
    CHECK(Unwrap(joined) == Voters({ "n2", "n3" }));

    // LAST: a change it can still make as the leader goes first, because once its own
    // removal commits it leads nothing.
    auto grown = state;
    grown.members.push_back(Learner("n4", "10.0.0.4:6680"));
    auto const first = Step(grown, Voters({ "n1", "n2", "n3" }), self, typed);
    REQUIRE(first.has_value());
    CHECK(Unwrap(first) == Configured({ "n1", "n2", "n3" }, { "n4" }));
}

TEST_CASE("A leader is forgotten only by the record gone AND its key revoked", "[cluster][membership][quorum][forget]")
{
    // Either fact alone is something else, and removing on it would be the flaw the
    // bootstrap rule exists for, reached from this node's side.
    auto const self = Member("n1", "10.0.0.1:6680");
    auto const typed = std::vector<Consensus::NodeId> { "n1", "n2", "n3" };

    SECTION("nothing recorded yet: a fresh leader's first pass")
    {
        CHECK_FALSE(Step(StateOf({}), Voters({ "n1", "n2", "n3" }), self, typed).has_value());
    }

    SECTION("a key revoked under its id while its record stands: re-admitted under a new key")
    {
        auto recorded = StateOf({ Member("n1", "10.0.0.1:6680") });
        RevokeUnder(recorded, "n1", 0x11);
        CHECK_FALSE(Step(recorded, Voters({ "n1", "n2", "n3" }), self, typed).has_value());
    }
}

TEST_CASE("The last voter is never removed, and forgetting it is refused by name", "[cluster][membership][quorum][forget]")
{
    // A configuration nobody is counted in commits nothing, including the change that
    // would undo it -- so the policy never proposes it, and the forget that would ask
    // for it is refused while the operator is still reading the answer.
    auto state = StateOf({});
    RevokeUnder(state, "n1", 0x11);
    CHECK_FALSE(
        Step(state, Voters({ "n1" }), Member("n1", "10.0.0.1:6680"), std::vector<Consensus::NodeId> { "n1" }).has_value());

    // Refused as the only voter even with a key to revoke: that refusal is asked first.
    auto const keyed = StateOf({ Member("n1", "10.0.0.1:6680"), Member("n2", "10.0.0.2:6680") });
    auto const refused = PrepareForget(keyed, Voters({ "n1" }), "n1", Key("n1"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::InvalidConfiguration);
    CHECK(refused.error().context.starts_with("cannot forget n1: it is the cluster's only voter"));

    // Every other forget leaves somebody counted.
    CHECK(PrepareForget(keyed, Voters({ "n1", "n2" }), "n1", std::nullopt).has_value());
    CHECK(PrepareForget(keyed, Configured({ "n1" }, { "n2" }), "n2", std::nullopt).has_value());
    CHECK(PrepareForget(keyed, Voters({ "n1" }), "n9", Key("n9")).has_value());

    // And the forget names the key this node holds live for the id (#1555), which is what
    // revokes a bootstrap member named with its key. None held, none named -- the record's own
    // key is what `Apply` revokes then, as it is for a principal.
    auto typed = Ed25519PublicKey {};
    typed.fill(std::byte { 0x29 });
    auto const withLive = PrepareForget(StateOf({}), Voters({ "n1", "n2" }), "n2", typed);
    REQUIRE(withLive.has_value());
    CHECK(withLive.value().kind == CommandKind::Forget);
    CHECK(withLive.value().key == "n2");
    CHECK(withLive.value().publicKey == std::optional { typed });
    auto principal = StateOf({});
    principal.principals.push_back(ClusterPrincipal { .id = "w1", .publicKey = Key("w1"), .role = PrincipalRole::Worker });
    auto const recordOnly = PrepareForget(principal, Voters({ "n1", "n2" }), "w1", std::nullopt);
    REQUIRE(recordOnly.has_value());
    CHECK_FALSE(recordOnly.value().publicKey.has_value());
}

TEST_CASE("A forget with nothing to revoke is refused by name before it is proposed", "[cluster][membership][forget]")
{
    // A machine is forgotten by its key. A forget of an id nothing records a key for, when
    // this node holds none live either, would remove a record and nothing else -- and the
    // next observation of the machine would admit it again.
    auto const state = ClusterState {};
    auto const active = Voters({ "n1", "n2" });
    auto const refused = PrepareForget(state, active, "n2", std::nullopt);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::InvalidConfiguration);
    CHECK(refused.error().context.contains("cannot forget n2"));
    CHECK(refused.error().context.contains("would revoke nothing"));

    // A typed key suffices, and so does a key the state records -- a member's or a
    // principal's.
    CHECK(PrepareForget(state, active, "n2", Key("n2")).has_value());
    CHECK(PrepareForget(StateOf({ Member("n2", "10.0.0.2:6680") }), active, "n2", std::nullopt).has_value());
}

// --------------------------------------------------------------------------
// A voter is counted only once it has caught up (#1537).

TEST_CASE("A recorded voter held as a learner is promoted only once it has caught up",
          "[cluster][membership][quorum][learner]")
{
    // The operator promoted `n2` -- its record says voter -- and consensus holds it as a
    // learner. Whether to promote was the operator's; WHEN is this rule's.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Member("n2", "10.0.0.2:6675") });
    auto const active = Configured({ "n1" }, { "n2" });
    auto const self = Member("n1", "10.0.0.1:6675");
    auto const typed = std::vector<Consensus::NodeId> { "n1" };
    constexpr auto Committed = Consensus::LogIndex { .value = 7 };

    SECTION("behind the commit index: held, and named")
    {
        auto const plan = NextQuorumChange(
            state,
            active,
            self,
            typed,
            Replication { .commitIndex = Committed, .matchIndex = { { "n2", Consensus::LogIndex { .value = 3 } } } });
        CHECK_FALSE(plan.change.has_value());
        CHECK(plan.catchingUp == std::vector<Consensus::NodeId> { "n2" });
    }

    SECTION("never heard from: held, and named")
    {
        auto const plan =
            NextQuorumChange(state, active, self, typed, Replication { .commitIndex = Committed, .matchIndex = {} });
        CHECK_FALSE(plan.change.has_value());
        CHECK(plan.catchingUp == std::vector<Consensus::NodeId> { "n2" });
    }

    SECTION("caught up: promoted, one change, and nobody waiting")
    {
        auto const plan = NextQuorumChange(
            state, active, self, typed, Replication { .commitIndex = Committed, .matchIndex = { { "n2", Committed } } });
        REQUIRE(plan.change.has_value());
        CHECK(Unwrap(plan.change) == Voters({ "n1", "n2" }));
        CHECK(plan.catchingUp.empty());
    }
}

TEST_CASE("A learner nobody promoted is never named as catching up", "[cluster][membership][quorum][learner]")
{
    // The control: the wait is a PROMOTION waiting, so an operator's learner -- the laptop
    // meant to stay one -- is not reported however far behind it is.
    auto const state = StateOf({ Member("n1", "10.0.0.1:6675"), Learner("laptop", "10.0.0.9:6675") });
    auto const plan = NextQuorumChange(state,
                                       Configured({ "n1" }, { "laptop" }),
                                       Member("n1", "10.0.0.1:6675"),
                                       std::vector<Consensus::NodeId> { "n1" },
                                       Replication { .commitIndex = Consensus::LogIndex { .value = 7 }, .matchIndex = {} });
    CHECK_FALSE(plan.change.has_value());
    CHECK(plan.catchingUp.empty());
}

TEST_CASE("A node's own key reaches the roster, and no opinion about a peer's leaves it alone",
          "[cluster][membership][identity]")
{
    // #178. A node is the authority on the key it holds, as it is on its scheduler endpoint:
    // its own record carries the key, and a record that differs from the state in that one
    // field is re-proposed. Discovery has no opinion about a peer's key, and no opinion must
    // never clear what the member announced.
    auto key = Ed25519PublicKey {};
    key.fill(std::byte { 0x4E });
    auto withKey = Desire("n1", "10.0.0.1:6675", std::string {});
    withKey.publicKey = key;

    SECTION("the key is proposed where the state records none")
    {
        auto const state = StateOf({ Member("n1", "10.0.0.1:6675") });
        auto const proposals = Proposals(state, { withKey });
        REQUIRE(proposals.size() == 1);
        CHECK(proposals[0].publicKey == std::optional { key });
    }

    SECTION("and not again once it is recorded")
    {
        auto recorded = Member("n1", "10.0.0.1:6675");
        recorded.publicKey = key;
        CHECK(Proposals(StateOf({ recorded }), { withKey }).empty());
    }

    SECTION("a peer about whose key this node knows nothing keeps the one recorded")
    {
        auto recorded = Member("n2", "10.0.0.2:6675");
        recorded.publicKey = key;
        auto const state = StateOf({ recorded });

        // Nothing differs but the opinion nobody has: nothing is proposed.
        CHECK(Proposals(state, { Desire("n2", "10.0.0.2:6675") }).empty());

        // It moved: proposed, carrying NO key, which `AddMember` reads as *keep* -- so the
        // recorded key survives the move rather than being dropped by omission.
        auto const moved = Proposals(state, { Desire("n2", "10.0.0.9:6675") });
        REQUIRE(moved.size() == 1);
        CHECK_FALSE(moved[0].publicKey.has_value());
        auto applied = state;
        Apply(applied, moved[0]);
        CHECK(applied.members[0].publicKey == key);
    }
}

TEST_CASE("A node is forgotten only when its record is gone and its key is revoked", "[cluster][membership][formation]")
{
    // #1539's reading, public: a formation controller asks it of the state it applies, and a leader
    // asks it before proposing a forgotten member's removal. One reading, so the two cannot disagree.
    auto state = ClusterState {};
    CHECK_FALSE(IsSelfForgotten(state, "n2")); // a fresh state has recorded nothing: never a forget

    auto revoked = Ed25519PublicKey {};
    revoked.fill(std::byte { 0x22 });
    state.revokedKeys.push_back(RevokedKey { .id = "n2", .publicKey = revoked });
    CHECK(IsSelfForgotten(state, "n2"));
    CHECK_FALSE(IsSelfForgotten(state, "n3")); // somebody else's revocation is not this node's

    state.members.push_back(ClusterMember { .id = "n2",
                                            .raftEndpoint = "",
                                            .schedulerEndpoint = {},
                                            .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                                            .seat = MemberSeat::Learner,
                                            .publicKey = Key("n2-new") });
    CHECK_FALSE(IsSelfForgotten(state, "n2")); // re-admitted under a new key: a member

    // No host decides it: a machine is forgotten by its key wherever it dials from, and the state
    // records no address a forget could name.
}
