// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/SplitEvidence.hpp>
#include <FastCache/Consensus/RaftClusterHarness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;

// A fleet split in two, healed, over the whole cluster harness. The formation decision is outside
// consensus, so each case drives the PURE functions a leader decides with -- `SplitEvidenceFor` and
// `ClassifyEncounter` -- over the states the harness's logs hold, replicates the loser's
// `DissolveInto` through the real log, and plays what every member does once it applies the order:
// set its store aside (`Dissolve`) and be admitted by the survivor. Every step runs the harness's
// invariants, cluster isolation among them.

namespace
{

/// Every machine a case starts, each under its own key.
/// @return A fresh roster.
[[nodiscard]] std::shared_ptr<Testing::SharedRoster> Roster()
{
    return Testing::SharedRoster::Of({ "n1", "n2", "n3", "n4", "n5" });
}

/// Who each member is: itself, under its own key, over @p roster.
/// @param roster What every member believes about everybody's keys.
/// @return The factory the harness requires.
[[nodiscard]] RaftClusterHarness::IdentityFactory MembersOf(std::shared_ptr<Testing::SharedRoster const> roster)
{
    return [roster = std::move(roster)](NodeId const& who) -> std::unique_ptr<IRaftPeerIdentity const> {
        return Testing::TestPeerIdentity::Honest(who, roster);
    };
}

/// Every safety property, reported by name.
/// @param cluster The cluster to read.
void RequireNoViolations(RaftClusterHarness const& cluster)
{
    for (auto const& violation: cluster.Violations())
        FAIL_CHECK(violation);
    REQUIRE(cluster.Violations().empty());
}

/// The cluster state @p member's log holds: every applied entry that is a cluster command, applied.
/// @param cluster The cluster.
/// @param member Whose log.
/// @return The state.
[[nodiscard]] Cluster::ClusterState StateOf(RaftClusterHarness const& cluster, NodeId const& member)
{
    auto state = Cluster::ClusterState {};
    for (auto const& entry: cluster.At(member).application)
        if (auto const command = Cluster::DecodeCommand(entry.payload); command.has_value())
            Cluster::Apply(state, *command);
    return state;
}

/// Record @p id in the fleet @p leader leads, under its own key -- in the STATE, the way an admission
/// replicates it.
/// @param cluster The cluster.
/// @param leader The fleet's leader.
/// @param id The member recorded.
/// @param seat Its seat.
void Record(RaftClusterHarness& cluster, NodeId const& leader, NodeId const& id, Cluster::MemberSeat seat)
{
    auto const command = Cluster::Command { .kind = seat == Cluster::MemberSeat::Voter ? Cluster::CommandKind::AddMember
                                                                                       : Cluster::CommandKind::AddLearner,
                                            .key = id,
                                            .value = seat == Cluster::MemberSeat::Voter ? id + ":6680" : std::string {},
                                            .schedulerEndpoint = {},
                                            .publicKey = Testing::TestKeyPair(id).PublicKey(),
                                            .role = std::nullopt,
                                            .createdAtUnixSeconds = std::nullopt };
    REQUIRE(cluster.ProposeOnLeaderOf(leader, Cluster::Encode(command)).has_value());
    cluster.Run(50);
}

/// A fleet of two: @p voter leading it alone, @p learner admitted as a learner -- in the
/// configuration and in the state.
/// @param cluster The cluster.
/// @param voter Its founder.
/// @param learner The machine it admitted.
void FormFleet(RaftClusterHarness& cluster, NodeId const& voter, NodeId const& learner)
{
    cluster.Solitary(voter);
    cluster.Join(learner);
    cluster.Run(100);
    REQUIRE(cluster.ProposeMembershipOnLeaderOf(voter, Configuration { .voters = { voter }, .learners = { learner } })
                .has_value());
    cluster.Run(200);
    Record(cluster, voter, voter, Cluster::MemberSeat::Voter);
    Record(cluster, voter, learner, Cluster::MemberSeat::Learner);
}

/// What the fleet @p speaker belongs to says, spoken by @p speaker under its own key: its cluster
/// tag, established since @p created, its leader answering at `<leader>:6674`, listing @p members.
/// @param cluster The cluster.
/// @param leader The fleet's leader.
/// @param speaker Who speaks.
/// @param created When the fleet was created.
/// @param members Whom it lists.
/// @return The summary, proven.
[[nodiscard]] Cluster::ProvenFleetSummary Speaking(RaftClusterHarness const& cluster,
                                                   NodeId const& leader,
                                                   NodeId const& speaker,
                                                   std::uint64_t created,
                                                   std::vector<std::string> members)
{
    auto const total = members.size();
    return Testing::ProvenBy(CompileCacheWire::FleetSummary { .clusterId = cluster.At(leader).cluster,
                                                              .state = CompileCacheWire::FleetState::Established,
                                                              .createdAtUnixSeconds = created,
                                                              .leaderId = leader,
                                                              .leaderNodeEndpoint = leader + ":6674",
                                                              .nodeId = speaker,
                                                              .raftEndpoint = speaker + ":6680",
                                                              .members = std::move(members),
                                                              .memberTotal = total,
                                                              .nodeEndpoint = speaker + ":6675",
                                                              .leaderKey = Testing::TestKeyPair(leader).PublicKey(),
                                                              .pointsAt = {} },
                             speaker);
}

/// What a fleet's own summary says about itself, as its leader announces it.
/// @param cluster The cluster.
/// @param leader The fleet's leader.
/// @param created When it was created.
/// @return The summary.
[[nodiscard]] CompileCacheWire::FleetSummary Own(RaftClusterHarness const& cluster,
                                                 NodeId const& leader,
                                                 std::uint64_t created)
{
    return CompileCacheWire::FleetSummary { .clusterId = cluster.At(leader).cluster,
                                            .state = CompileCacheWire::FleetState::Established,
                                            .createdAtUnixSeconds = created,
                                            .leaderId = leader,
                                            .leaderNodeEndpoint = leader + ":6674",
                                            .nodeId = leader };
}

/// The leader's decision, exactly as `FormationController::TickMember` takes it: the dissolve the
/// fleet @p leader leads would propose into @p seen, or nothing -- only on evidence that heals by
/// itself.
/// @param cluster The cluster.
/// @param leader The deciding fleet's leader.
/// @param created When its fleet was created.
/// @param record Its formation record, whose memos count.
/// @param announced What its members announced.
/// @param seen The other fleet, as proven.
/// @return The command, or nothing.
[[nodiscard]] std::optional<Cluster::Command> Decide(RaftClusterHarness const& cluster,
                                                     NodeId const& leader,
                                                     std::uint64_t created,
                                                     Cluster::FormationRecord const& record,
                                                     std::vector<Cluster::AskedJoinBy> const& announced,
                                                     Cluster::ProvenFleetSummary const& seen)
{
    auto const evidence = Cluster::SplitEvidenceFor(StateOf(cluster, leader), leader, record, announced, seen);
    if (Cluster::HealingOf(evidence) != Cluster::SplitHealing::Automatically
        || Cluster::ClassifyEncounter(Own(cluster, leader, created), seen, evidence, Cluster::FleetPin {})
               != Cluster::Encounter::Yield)
        return std::nullopt;
    auto const& summary = seen.Summary();
    // The survivor's leader's key, reached from the key that proved it, as the controller reaches it.
    auto const speakerLeads = !summary.nodeId.empty() && summary.leaderId == summary.nodeId;
    auto const leaderKey = speakerLeads ? std::optional { seen.Key() } : summary.leaderKey;
    if (!leaderKey.has_value())
        return std::nullopt;
    return Cluster::Command { .kind = Cluster::CommandKind::DissolveInto,
                              .key = summary.clusterId,
                              .value = summary.leaderNodeEndpoint,
                              .schedulerEndpoint = {},
                              .publicKey = seen.Key(),
                              .role = std::nullopt,
                              .createdAtUnixSeconds = summary.createdAtUnixSeconds,
                              .leaderKey = leaderKey };
}

/// Replicate @p order through @p leader's fleet, then play every member's part: each that APPLIED it
/// sets its store aside and is admitted by @p survivor as a learner, one change at a time.
/// @param cluster The cluster.
/// @param leader The losing fleet's leader.
/// @param losers Every member of the losing fleet.
/// @param survivor The surviving fleet's leader.
/// @param learners The survivor's learners so far; each loser joins them.
/// @param order The dissolve.
void Heal(RaftClusterHarness& cluster,
          NodeId const& leader,
          std::vector<NodeId> const& losers,
          NodeId const& survivor,
          std::vector<NodeId> learners,
          Cluster::Command const& order)
{
    REQUIRE(Cluster::Validate(order).has_value());
    REQUIRE(cluster.ProposeOnLeaderOf(leader, Cluster::Encode(order)).has_value());
    cluster.Run(100);
    for (auto const& member: losers)
    {
        // A member leaves on the ORDER its log applied, never on evidence of its own.
        auto const state = StateOf(cluster, member);
        REQUIRE(state.dissolveOrder.has_value());
        REQUIRE(Testing::Unwrap(state.dissolveOrder).clusterId == cluster.At(survivor).cluster);
        cluster.Dissolve(member);
        learners.push_back(member);
        REQUIRE(cluster.ProposeMembershipOnLeaderOf(survivor, Configuration { .voters = { survivor }, .learners = learners })
                    .has_value());
        cluster.Run(200);
    }
}

/// Require that every one of @p members runs @p survivor's cluster, having set its own store aside.
/// @param cluster The cluster.
/// @param members The machines that left.
/// @param survivor The surviving fleet's leader.
void RequireConverged(RaftClusterHarness const& cluster, std::vector<NodeId> const& members, NodeId const& survivor)
{
    for (auto const& member: members)
    {
        INFO(member);
        CHECK(cluster.At(member).cluster == cluster.At(survivor).cluster);
        CHECK(cluster.LeaderOf(member) == std::optional<NodeId> { survivor });
        CHECK(cluster.At(member).archived.size() == 1); // its fleet's log, kept aside, never merged
        CHECK(cluster.At(member).driver->Node().CommitIndex() == cluster.At(survivor).driver->Node().CommitIndex());
    }
}

} // namespace

TEST_CASE("A split whose recorder loses heals: the recorder's fleet dissolves on one order and rejoins",
          "[consensus][raft][formation][harness][split]")
{
    // Fleet a = {n1 voter, n2 learner}, created at 200; fleet b = {n3 voter, n4 learner}, created at
    // 100. The split: a's state records n3 as a VOTER under its key (a one-sided admission an operator
    // promoted), and n3 speaks for b -- evidence (A) at a's leader. b is older, so a loses and
    // dissolves.
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    FormFleet(cluster, "n1", "n2");
    FormFleet(cluster, "n3", "n4");
    Record(cluster, "n1", "n3", Cluster::MemberSeat::Voter);

    auto const seen = Speaking(cluster, "n3", "n3", 100, { "n3", "n4" });
    auto const order = Decide(cluster, "n1", 200, Cluster::FormationRecord {}, {}, seen);
    REQUIRE(order.has_value());
    Heal(cluster, "n1", { "n1", "n2" }, "n3", { "n4" }, Testing::Unwrap(order));

    RequireConverged(cluster, { "n1", "n2" }, "n3");
    RequireNoViolations(cluster);
}

TEST_CASE("A split the recorded fleet sees only through its asker's memo moves nothing on its own",
          "[consensus][raft][formation][harness][split][security]")
{
    // The same one-sided admission, the ages reversed: a (the recorder) is older and stays. b has no
    // speaker of a recorded, but n3 once asked a to admit it, under n1's key, and a lists n3 --
    // evidence (C) at b's leader. That key is one n3 trusted on FIRST use, which a fleet minted to be
    // asked would hold as well, so (C) is an operator's decision: b proposes nothing, and both fleets
    // keep running apart until one is told.
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    FormFleet(cluster, "n1", "n2");
    FormFleet(cluster, "n3", "n4");
    Record(cluster, "n1", "n3", Cluster::MemberSeat::Voter);

    // What a decides: b's speaker is recorded here, but a is older -- it stays.
    CHECK_FALSE(
        Decide(cluster, "n1", 100, Cluster::FormationRecord {}, {}, Speaking(cluster, "n3", "n3", 200, { "n3", "n4" }))
            .has_value());

    auto record = Cluster::FormationRecord {};
    Cluster::RememberAsked(record,
                           Cluster::AskedJoin { .clusterId = cluster.At("n1").cluster,
                                                .provenKey = Testing::TestKeyPair("n1").PublicKey(),
                                                .askedAtUnixSeconds = 1 });
    auto const seen = Speaking(cluster, "n1", "n1", 100, { "n1", "n2", "n3" });
    CHECK(Cluster::SplitEvidenceFor(StateOf(cluster, "n3"), "n3", record, {}, seen)
          == Cluster::SplitEvidence::WeAskedAndTheyListUs);
    CHECK_FALSE(Decide(cluster, "n3", 200, record, {}, seen).has_value());
    CHECK(cluster.At("n3").cluster != cluster.At("n1").cluster);
    CHECK(cluster.At("n3").archived.empty());
    RequireNoViolations(cluster);
}

TEST_CASE("A mutual admission heals too, and exactly one side yields", "[consensus][raft][formation][harness][split]")
{
    // Each fleet records the other's leader as a voter: both hold evidence (A) that heals by itself.
    // The tiebreak is antisymmetric, so one side yields and the other stays -- never both, never neither.
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    FormFleet(cluster, "n1", "n2");
    FormFleet(cluster, "n3", "n4");
    Record(cluster, "n1", "n3", Cluster::MemberSeat::Voter);
    Record(cluster, "n3", "n1", Cluster::MemberSeat::Voter);

    auto const aDecides = Decide(cluster, "n1", 150, {}, {}, Speaking(cluster, "n3", "n3", 150, { "n3", "n4", "n1" }));
    auto const bDecides = Decide(cluster, "n3", 150, {}, {}, Speaking(cluster, "n1", "n1", 150, { "n1", "n2", "n3" }));
    REQUIRE(aDecides.has_value() != bDecides.has_value());

    if (aDecides.has_value())
    {
        Heal(cluster, "n1", { "n1", "n2" }, "n3", { "n4" }, Testing::Unwrap(aDecides));
        RequireConverged(cluster, { "n1", "n2" }, "n3");
    }
    else
    {
        Heal(cluster, "n3", { "n3", "n4" }, "n1", { "n2" }, Testing::Unwrap(bDecides));
        RequireConverged(cluster, { "n3", "n4" }, "n1");
    }
    RequireNoViolations(cluster);
}

TEST_CASE("A loser member that restarts after the order applied still leaves on it",
          "[consensus][raft][formation][harness][split]")
{
    // The order is in the member's own log: a restart re-applies it, so the member follows it all the
    // same -- no evidence of its own, and no decision made twice.
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    FormFleet(cluster, "n1", "n2");
    FormFleet(cluster, "n3", "n4");
    Record(cluster, "n1", "n3", Cluster::MemberSeat::Voter);
    auto const order = Decide(cluster, "n1", 200, {}, {}, Speaking(cluster, "n3", "n3", 100, { "n3", "n4" }));
    REQUIRE(order.has_value());
    REQUIRE(cluster.ProposeOnLeaderOf("n1", Cluster::Encode(Testing::Unwrap(order))).has_value());
    cluster.Run(100);

    REQUIRE(cluster.Restart("n2").has_value());
    cluster.Run(100);
    REQUIRE(StateOf(cluster, "n2").dissolveOrder.has_value());

    // The order is already applied: `Heal` replays the part every member plays from here.
    auto learners = std::vector<NodeId> { "n4" };
    for (auto const& member: { NodeId { "n1" }, NodeId { "n2" } })
    {
        cluster.Dissolve(member);
        learners.push_back(member);
        REQUIRE(cluster.ProposeMembershipOnLeaderOf("n3", Configuration { .voters = { "n3" }, .learners = learners })
                    .has_value());
        cluster.Run(200);
    }
    RequireConverged(cluster, { "n1", "n2" }, "n3");
    RequireNoViolations(cluster);
}

TEST_CASE("An older fleet listing this fleet's leader, proven by a key this fleet never held, is never yielded to",
          "[consensus][raft][formation][harness][split]")
{
    // The attack this design exists to refuse: ids are public, so any machine can mint a fleet created
    // at 1 listing our leader. Its speaker is recorded nowhere here and nobody here asked it -- no key
    // this fleet holds verifies the claim -- so it is foreign, and nothing is proposed.
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    FormFleet(cluster, "n1", "n2");
    cluster.Solitary("n5");
    cluster.Run(100);

    auto const claim = Speaking(cluster, "n5", "n5", 1, { "n5", "n1", "n2" });
    CHECK(Cluster::SplitEvidenceFor(StateOf(cluster, "n1"), "n1", {}, {}, claim) == Cluster::SplitEvidence::None);
    CHECK(Cluster::ClassifyEncounter(Own(cluster, "n1", 200), claim, Cluster::SplitEvidence::None, Cluster::FleetPin {})
          == Cluster::Encounter::ForeignFleet);
    CHECK_FALSE(Decide(cluster, "n1", 200, {}, {}, claim).has_value());

    // The control: the same claim spoken by a machine this fleet DOES record as a voter under its key
    // is a split that heals.
    Record(cluster, "n1", "n5", Cluster::MemberSeat::Voter);
    CHECK(Decide(cluster, "n1", 200, {}, {}, claim).has_value());
    RequireNoViolations(cluster);
}
