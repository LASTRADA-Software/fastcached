// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/PskRaftPeerCredential.hpp>
#include <FastCache/Consensus/RaftClusterHarness.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

/// The key every member of every cluster in this file holds.
/// @param who Which member; every one gets the same key.
/// @return Its credential.
[[nodiscard]] std::unique_ptr<IRaftPeerCredential const> ClusterKey(NodeId const& who)
{
    std::ignore = who;
    return std::make_unique<Cluster::PskRaftPeerCredential const>(SecureByteBuffer(32, std::byte { 0x5A }));
}

/// A key that is not the cluster's.
/// @return Its credential.
[[nodiscard]] std::unique_ptr<IRaftPeerCredential const> StrangerKey()
{
    return std::make_unique<Cluster::PskRaftPeerCredential const>(SecureByteBuffer(32, std::byte { 0x33 }));
}

/// What a machine holding NO key can present: a tag of zeroes, and a check of every
/// tag it is shown that nothing passes.
class NoKey final: public IRaftPeerCredential
{
  public:
    [[nodiscard]] Sha256::Digest Sign(RaftPeerMac purpose, WireFields::FieldList fields) const override
    {
        std::ignore = purpose;
        std::ignore = fields;
        return {};
    }

    [[nodiscard]] bool Verify(RaftPeerMac purpose,
                              WireFields::FieldList fields,
                              Sha256::Digest const& presented) const override
    {
        std::ignore = purpose;
        std::ignore = fields;
        std::ignore = presented;
        return false;
    }
};

/// Every safety property, reported by name rather than as a bare count.
///
/// A failure here has to say *which* invariant broke: "the cluster is wrong" is
/// not something anybody can act on, and these five have entirely different
/// causes.
void RequireNoViolations(RaftClusterHarness const& cluster)
{
    for (auto const& violation: cluster.Violations())
        FAIL_CHECK(violation);

    REQUIRE(cluster.Violations().empty());
}

/// The four nodes a membership case ends up with.
constexpr std::array Everyone { "n1", "n2", "n3", "n4" };

/// A configuration of voters and no learners.
/// @param voters The voters.
/// @return The configuration.
[[nodiscard]] Configuration Voters(std::vector<NodeId> voters)
{
    return Configuration { .voters = std::move(voters), .learners = {} };
}

/// The furthest any of `who` has committed.
///
/// The commit index rather than what a node calls itself, for the reason "A
/// minority partition cannot elect or commit" gives: a partitioned leader goes on
/// reporting itself leader, so the only thing a quorum test can assert is what was
/// decided.
/// @param cluster The cluster to read.
/// @param who Which nodes to consider.
/// @return The highest commit index among them.
[[nodiscard]] LogIndex HighestCommitIndex(RaftClusterHarness const& cluster, std::span<char const* const> who)
{
    auto highest = LogIndex::BeforeFirst();
    for (auto const* const id: who)
        highest = std::max(highest, cluster.At(id).driver->Node().CommitIndex());

    return highest;
}

/// Step until a single leader exists, or give up.
/// @param cluster The cluster to drive.
/// @param steps How long to allow.
/// @return Whether one emerged.
[[nodiscard]] bool SettleOnLeader(RaftClusterHarness& cluster, std::size_t steps = 200)
{
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, steps))
    {
        cluster.Step();
        if (cluster.Leader().has_value())
            return true;
    }

    return false;
}

} // namespace

TEST_CASE("A three-node cluster elects exactly one leader", "[consensus][raft][cluster]")
{
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };

    REQUIRE(SettleOnLeader(cluster));
    CHECK(cluster.Leaders().size() == 1);
    RequireNoViolations(cluster);

    // Every message proved the key, and the positive half of that is what makes the
    // intruder cases below mean anything: members holding one key refuse nothing, and
    // each of them was heard.
    for (auto const* const id: { "n1", "n2", "n3" })
    {
        CAPTURE(id);
        CHECK(cluster.RefusedAt(id) == 0);
        CHECK(cluster.DeliveredFrom(id) > 0);
    }
}

TEST_CASE("A five-node cluster elects exactly one leader", "[consensus][raft][cluster]")
{
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, ClusterKey, 7 };

    REQUIRE(SettleOnLeader(cluster));
    CHECK(cluster.Leaders().size() == 1);
    RequireNoViolations(cluster);
}

TEST_CASE("A committed entry reaches every node", "[consensus][raft][cluster]")
{
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("hello")).has_value());
    cluster.Run(60);

    for (auto const& id: { "n1", "n2", "n3" })
    {
        auto const& applied = cluster.At(id).applied;
        REQUIRE(applied.size() == 1);
        CHECK(FastCache::AsStringView(applied.front().payload) == "hello");
    }

    RequireNoViolations(cluster);
}

TEST_CASE("Entries are applied in the same order on every node", "[consensus][raft][cluster]")
{
    // State Machine Safety stated the way an application experiences it: not
    // merely that nobody disagrees, but that everyone sees the same sequence.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    for (auto const index: std::views::iota(0, 8))
    {
        REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("v" + std::to_string(index))).has_value());
        cluster.Run(6);
    }

    cluster.Run(80);

    auto const& reference = cluster.At("n1").applied;
    REQUIRE(reference.size() == 8);
    for (auto const& id: { "n2", "n3" })
    {
        auto const& applied = cluster.At(id).applied;
        REQUIRE(applied.size() == reference.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, reference.size()))
        {
            CHECK(applied[index].index == reference[index].index);
            CHECK(applied[index].payload == reference[index].payload);
        }
    }

    RequireNoViolations(cluster);
}

TEST_CASE("A minority partition cannot elect or commit", "[consensus][raft][cluster]")
{
    // The property that makes a quorum a quorum -- and it is about *committing*,
    // not about who calls themselves leader.
    //
    // An isolated node that was already leader keeps reporting itself leader for
    // a while, and then stops: since issue #437 its own `Tick` relinquishes
    // leadership once a majority has stopped answering it, so the role this case
    // reads is a snapshot rather than a fixture -- which is exactly why the
    // assertions below are about commitment and never about who calls themselves
    // leader. (This comment used to say that acting on that answer was "a
    // separate mechanism"; it stopped being one at #437 and nobody corrected the
    // claim, which is what issue #1061 cost.) A leader lease is still a separate
    // mechanism. An isolated *follower* keeps standing for election and keeps
    // losing. Either way the guarantee is the same and it is the one asserted
    // here: nothing the minority does can be committed.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    cluster.Partition({ "n1" });
    cluster.Run(200);

    // Nothing reached n1's state machine, whatever role it believes it holds.
    CHECK(cluster.At("n1").applied.empty());

    // A proposal made on the isolated node cannot commit either, even if it
    // accepts one.
    if (cluster.At("n1").driver->Node().CurrentRole() == Role::Leader)
    {
        auto const orphan = cluster.At("n1").driver->Node().CommitIndex();
        cluster.Run(200);
        CHECK(cluster.At("n1").driver->Node().CommitIndex() == orphan);
    }

    // And the majority is unaffected: it holds the higher term.
    auto const majorityLeader = cluster.Leader();
    CHECK(majorityLeader.value_or(NodeId { "n1" }) != "n1");

    RequireNoViolations(cluster);
}

TEST_CASE("A majority keeps working when a minority is cut off", "[consensus][raft][cluster]")
{
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, ClusterKey, 3 };
    REQUIRE(SettleOnLeader(cluster));

    // Isolate two of five. The remaining three are still a quorum.
    cluster.Partition({ "n4", "n5" });
    cluster.Run(200);

    REQUIRE(SettleOnLeader(cluster, 200));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("during")).has_value());
    cluster.Run(100);

    auto applied = std::size_t { 0 };
    for (auto const& id: { "n1", "n2", "n3", "n4", "n5" })
        if (!cluster.At(id).applied.empty())
            ++applied;

    // A majority applied it; the two cut off could not have.
    CHECK(applied >= 3);
    RequireNoViolations(cluster);
}

TEST_CASE("A healed partition converges and loses nothing", "[consensus][raft][cluster]")
{
    // The case the whole design is for: a node comes back and its log is repaired
    // to match, without any committed entry being lost or duplicated.
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, ClusterKey, 11 };
    REQUIRE(SettleOnLeader(cluster));

    cluster.Partition({ "n4", "n5" });
    cluster.Run(120);
    REQUIRE(SettleOnLeader(cluster, 200));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("apart")).has_value());
    cluster.Run(120);

    cluster.Heal();
    cluster.Run(300);

    for (auto const& id: { "n1", "n2", "n3", "n4", "n5" })
    {
        auto const& applied = cluster.At(id).applied;
        REQUIRE(applied.size() == 1);
        CHECK(FastCache::AsStringView(applied.front().payload) == "apart");
    }

    RequireNoViolations(cluster);
}

TEST_CASE("A restarted node rejoins without violating anything", "[consensus][raft][cluster]")
{
    // A restart is a new node recovered from the same storage, which is what a
    // process restart looks like from the algorithm's side.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("before")).has_value());
    cluster.Run(60);

    auto const leader = cluster.Leader();
    REQUIRE(leader.has_value());
    REQUIRE(cluster.Restart(leader.value_or(NodeId {})).has_value());
    cluster.Run(300);

    REQUIRE(SettleOnLeader(cluster, 200));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("after")).has_value());
    cluster.Run(120);

    RequireNoViolations(cluster);
}

TEST_CASE("Restarting every node in turn preserves what was committed", "[consensus][raft][cluster]")
{
    // Rolling restarts, which is what upgrading a fleet looks like. Nothing that
    // was committed may go missing.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey, 5 };
    REQUIRE(SettleOnLeader(cluster));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("durable")).has_value());
    cluster.Run(80);

    for (auto const& id: { "n1", "n2", "n3" })
    {
        REQUIRE(cluster.Restart(id).has_value());
        cluster.Run(200);
    }

    REQUIRE(SettleOnLeader(cluster, 300));

    // Every node still holds the committed entry in its log.
    for (auto const& id: { "n1", "n2", "n3" })
    {
        auto const& log = cluster.At(id).driver->Node().Log();
        auto found = false;
        for (auto const index: std::views::iota(std::uint64_t { 1 }, log.LastIndex().value + 1))
        {
            auto const* const entry = log.EntryAt(LogIndex { .value = index });
            if (entry != nullptr && entry->kind == EntryKind::Command
                && FastCache::AsStringView(entry->payload) == "durable")
                found = true;
        }

        CHECK(found);
    }

    RequireNoViolations(cluster);
}

TEST_CASE("The cluster survives heavy message loss", "[consensus][raft][cluster]")
{
    // Raft is supposed to be indifferent to loss rather than merely tolerant of
    // it: progress gets slower and nothing becomes unsafe.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey, 13 };
    cluster.SetLossPercent(30);

    REQUIRE(SettleOnLeader(cluster, 600));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("lossy")).has_value());
    cluster.Run(400);

    RequireNoViolations(cluster);
}

TEST_CASE("A long adversarial run violates nothing", "[consensus][raft][cluster]")
{
    // The soak: loss, reordering, rolling partitions and restarts together, with
    // every safety property checked after every single step. This is the case
    // that exercises interleavings nobody wrote down.
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, ClusterKey, 23 };
    cluster.SetLossPercent(15);

    auto proposals = 0;
    for (auto const round: std::views::iota(0, 12))
    {
        if (round % 4 == 1)
            cluster.Partition({ "n1", "n2" });
        else if (round % 4 == 3)
            cluster.Heal();

        cluster.Run(60);

        if (cluster.ProposeOnLeader(FastCache::BytesFromString("r" + std::to_string(round))).has_value())
            ++proposals;

        if (round % 5 == 2)
            REQUIRE(cluster.Restart("n3").has_value());

        cluster.Run(60);
    }

    cluster.Heal();
    cluster.SetLossPercent(0);
    cluster.Run(400);

    // The run has to have done something, or a harness that quietly did nothing
    // would pass this while proving nothing at all.
    CHECK(proposals > 0);
    REQUIRE(SettleOnLeader(cluster, 400));
    RequireNoViolations(cluster);
}

TEST_CASE("A machine with no cluster is admitted into a running one", "[consensus][raft][cluster][membership]")
{
    // Growing a cluster's *consensus*, which used to mean restarting every member
    // with a longer bootstrap list. The joiner is brought up with no configuration
    // at all: it is reachable, it does nothing, and admission is what gives it
    // both a member set and a leader.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    auto const committed = Unwrap(cluster.ProposeOnLeader(FastCache::BytesFromString("before")));
    cluster.Run(60);

    cluster.Join("n4");
    cluster.Run(30);

    // It has done nothing on its own, which is the property that makes it
    // admissible: a node that had elected itself would hold a term and a log of
    // its own and refuse every leader its configuration does not name.
    CHECK_FALSE(cluster.At("n4").driver->Node().HasCluster());
    CHECK(cluster.At("n4").driver->Node().CurrentRole() == Role::Follower);
    CHECK(cluster.Leaders().size() == 1);

    REQUIRE(cluster.ProposeMembershipOnLeader(Voters({ "n1", "n2", "n3", "n4" })).has_value());
    cluster.Run(120);

    CHECK(cluster.At("n4").driver->Node().HasCluster());
    CHECK(cluster.At("n4").driver->Node().ActiveConfiguration().voters.size() == 4);

    // Caught up rather than merely counted. A member the cluster admits and never
    // fills in is one that would win an election holding nothing, which is
    // precisely the Leader Completeness violation the harness watches for.
    CHECK(cluster.At("n4").driver->Node().Log().LastIndex() >= committed);
    RequireNoViolations(cluster);

    SECTION("and it then counts towards the quorum")
    {
        // The leader plus exactly one of the other originals: two of the four, a
        // majority of the THREE the cluster was bootstrapped with and not of the
        // four it now has. That shape is the only one that proves anything -- were
        // n4 still uncounted, this side would commit; because it is counted, it
        // cannot. A split three against one passes identically under both member
        // sets and so says nothing about which one the arithmetic used.
        //
        // Built around whoever leads rather than named, because a side that cannot
        // elect keeps the leader it already had: a fixed pair proves the property
        // only in the runs where the leader happens to land in it.
        auto const ceiling = HighestCommitIndex(cluster, Everyone);
        auto side = std::set<NodeId> { Unwrap(cluster.Leader()) };
        for (auto const* const id: { "n1", "n2", "n3" })
            if (side.size() < 2)
                side.insert(id);

        cluster.Partition(side);

        // Proposing is what makes the assertion say something: a leader with
        // nothing to replicate commits nothing either way, so a case that only
        // waited would pass against any member set at all.
        REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("split")).has_value());
        cluster.Run(400);

        // Not "unchanged": a follower that was behind may still catch up to what
        // was committed before the split, which is progress rather than a new
        // decision. What may not happen is anything ABOVE that point.
        CHECK(HighestCommitIndex(cluster, Everyone) == ceiling);
        RequireNoViolations(cluster);

        // And the positive half, so the case cannot pass by the cluster being
        // wedged: healed, four of four commit again -- the entry stranded above
        // included.
        cluster.Heal();
        REQUIRE(SettleOnLeader(cluster, 400));
        REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("after")).has_value());
        cluster.Run(300);
        CHECK(HighestCommitIndex(cluster, Everyone) > ceiling);
        RequireNoViolations(cluster);
    }
}

namespace
{

/// Put an intruder holding @p credential beside a formed cluster, and check nothing it sent
/// reached a member while the cluster carried on.
/// @param credential What the intruder presents instead of the cluster key.
void CheckIntruderIsNeverHeard(std::unique_ptr<IRaftPeerCredential const> credential)
{
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));
    auto const term = Unwrap(cluster.TermOfLeader());

    cluster.Intrude("n9", std::move(credential));
    cluster.Run(400);

    CHECK(cluster.DeliveredFrom("n9") == 0);
    auto const refused = cluster.RefusedAt("n1") + cluster.RefusedAt("n2") + cluster.RefusedAt("n3");
    CHECK(refused > 0);

    // And the cluster it could not reach carried on as though it were not there: one
    // leader, the same term, and a proposal still commits on every member.
    CHECK(cluster.Leaders().size() == 1);
    CHECK(cluster.TermOfLeader() == term);
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("unbothered")).has_value());
    cluster.Run(60);
    for (auto const* const id: { "n1", "n2", "n3" })
    {
        CAPTURE(id);
        CHECK(cluster.At(id).applied.size() == 1);
    }
    RequireNoViolations(cluster);
}

} // namespace

TEST_CASE("A machine without the cluster key is never heard by a member", "[consensus][raft][cluster][handshake]")
{
    // #1308. Before the handshake a machine that could reach the Raft port WAS a member
    // as far as the wire could tell: nothing it sent was checked. The intruder claims
    // membership and campaigns -- its election timer expires and it asks every member
    // for a vote -- so a single message of it reaching a member is visible, and the
    // count of refusals says each attempt met the check rather than being lost.
    SECTION("a key that is not the cluster's")
    {
        CheckIntruderIsNeverHeard(StrangerKey());
    }

    SECTION("no key at all")
    {
        CheckIntruderIsNeverHeard(std::make_unique<NoKey const>());
    }
}

TEST_CASE("A joiner admitted with the wrong key receives nothing, and the cluster still commits",
          "[consensus][raft][cluster][membership][handshake]")
{
    // The operator admitted n4 by id and handed it the wrong key file. The leader's
    // configuration names it and the leader replicates to it -- and every one of those
    // messages is refused at n4, which proves nothing it is sent came from the cluster.
    // `A machine with no cluster is admitted into a running one` is the control: the
    // same steps with the cluster's key, and n4 catches up.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    cluster.Join("n4", StrangerKey());
    REQUIRE(cluster.ProposeMembershipOnLeader(Voters({ "n1", "n2", "n3", "n4" })).has_value());
    cluster.Run(120);

    CHECK(cluster.RefusedAt("n4") > 0);
    CHECK(cluster.DeliveredFrom("n4") == 0);
    CHECK_FALSE(cluster.At("n4").driver->Node().HasCluster());
    CHECK(cluster.At("n4").driver->Node().Log().LastIndex() == LogIndex::BeforeFirst());

    // Three of four is still a majority, so the members commit without it.
    REQUIRE(SettleOnLeader(cluster, 400));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("without n4")).has_value());
    cluster.Run(120);
    for (auto const* const id: { "n1", "n2", "n3" })
    {
        CAPTURE(id);
        CHECK_FALSE(cluster.At(id).applied.empty());
    }
    CHECK(cluster.At("n4").applied.empty());
    RequireNoViolations(cluster);
}

TEST_CASE("A cluster that admitted a member re-elects after losing the leader", "[consensus][raft][cluster][membership]")
{
    // What `cluster-e2e` phase 6 does, in process. Admission is what makes this
    // different from the plain "a leader dies" case: the quorum grew to four while
    // one of the four holds a configuration it was given rather than started with,
    // and that node is the one the arithmetic now depends on.
    //
    // The failure this is written against (#388) is a cluster that formed, admitted,
    // served -- and then could not re-elect, because the admitted node never fell
    // due. `NextDeadline()` answers `TimePoint::max()` while `HasCluster()` is
    // false, so a joiner that lost its configuration neither campaigns nor answers,
    // and a four-member quorum with one node dead and one silent can never reach
    // three.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    cluster.Join("n4");
    cluster.Run(30);
    REQUIRE(cluster.ProposeMembershipOnLeader(Voters({ "n1", "n2", "n3", "n4" })).has_value());
    cluster.Run(120);
    REQUIRE(cluster.At("n4").driver->Node().HasCluster());

    auto const deposed = Unwrap(cluster.Leader());

    // Isolating the leader is losing it as far as everyone else is concerned, and
    // it is what the harness can express. The other three are a majority of four,
    // so they must elect -- and n4 has to take part for them to get there.
    auto survivors = std::set<NodeId> {};
    for (auto const* const id: Everyone)
        if (id != deposed)
            survivors.insert(id);
    cluster.Partition(survivors);

    auto elected = std::optional<NodeId> {};
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, std::size_t { 600 }))
    {
        cluster.Step();
        for (auto const& who: cluster.Leaders())
            if (who != deposed)
                elected = who;
        if (elected.has_value())
            break;
    }

    // Named rather than counted, because "no leader" and "the deposed one is still
    // reported" are different failures: the first is the stall this is about, the
    // second would mean the partition never took effect and the case proved
    // nothing.
    CHECK(elected.has_value());

    // The half that says WHY, so a failure does not need the log read twice. A
    // joiner that dropped its configuration reports no cluster, and a node with no
    // cluster is excused from every deadline -- silent, and uncountable.
    CHECK(cluster.At("n4").driver->Node().HasCluster());
    CHECK(cluster.At("n4").driver->Node().ActiveConfiguration().voters.size() == 4);

    RequireNoViolations(cluster);
}

TEST_CASE("An admitted member survives its own restart", "[consensus][raft][cluster][membership]")
{
    // The whole point of putting membership in the log rather than on a command
    // line. The joiner was started with no bootstrap set and has none to fall back
    // on, so a restart that did not re-derive its configuration from its own log
    // would come back with no cluster and wait to be admitted a second time.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    cluster.Join("n4");
    cluster.Run(30);
    REQUIRE(cluster.ProposeMembershipOnLeader(Voters({ "n1", "n2", "n3", "n4" })).has_value());
    cluster.Run(120);
    REQUIRE(cluster.At("n4").driver->Node().HasCluster());

    REQUIRE(cluster.Restart("n4").has_value());
    CHECK(cluster.At("n4").driver->Node().HasCluster());
    CHECK(cluster.At("n4").driver->Node().ActiveConfiguration().voters.size() == 4);

    cluster.Run(200);
    RequireNoViolations(cluster);
}

TEST_CASE("A cluster formed on a bare quorum keeps its leader when the last member attaches",
          "[consensus][raft][cluster][prevote]")
{
    // Issue #117: a leader elected while the third node was still connecting was
    // deposed the moment that node appeared, and the report's first hypothesis was
    // that the join path skipped pre-vote -- or that a leader's `HasQuorumContact`
    // was somehow false at that instant.
    //
    // It is neither, and this case is what says so for the shape the artifact
    // showed -- a member already in the configuration whose process has not
    // finished connecting. `Tick` has exactly one election path and it is
    // `StartPreVote`, so such a node has no un-gated `RequestVote` to raise the
    // term with; pre-vote is what keeps its term from moving while it can reach
    // nobody; and it therefore arrives BEHIND, which is a node the other two
    // refuse on their logs alone. Admission through a committed configuration
    // change is a different route and has its own cases below.
    //
    // Every other election in this file happens with all peers already reachable,
    // so nothing covered the shape the failing artifact actually showed: a cluster
    // that has ELECTED but not yet FORMED.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    auto const joiner = std::string { "n3" };

    // n3 is in the configuration and unreachable -- a member whose process has not
    // finished starting, which is what the fixture's third node was.
    cluster.Partition({ "n1", "n2" });
    REQUIRE(SettleOnLeader(cluster));
    cluster.Run(60);

    REQUIRE(cluster.Leader().has_value());
    REQUIRE(cluster.TermOfLeader().has_value());
    auto const leader = Unwrap(cluster.Leader());
    auto const term = Unwrap(cluster.TermOfLeader());

    // Reaching nobody, it can have won nothing -- and saying so by name is what
    // keeps the rest of this case from silently swapping the roles of the two
    // nodes it is about.
    REQUIRE(leader != joiner);

    // It really did campaign, which everything below rests on. Without this the
    // case passes just as well against a joiner that never started a pre-vote
    // round at all -- and then it covers nothing, because the question is what
    // happens to a node that HAS been trying to elect itself.
    auto const& isolated = cluster.At(joiner).driver->Node();
    REQUIRE(isolated.CurrentRole() == Role::PreCandidate);

    // Every one of those rounds left its term alone, which is the whole of
    // pre-vote's purpose. A joiner that came back with an inflated one would
    // depose the leader on arrival whatever else this case asserted.
    CHECK(isolated.CurrentTerm() < term);

    // It attaches.
    cluster.Heal();
    cluster.Run(400);

    CHECK(cluster.Leader() == std::optional<NodeId> { leader });
    CHECK(cluster.TermOfLeader() == std::optional<Term> { term });
    CHECK(cluster.Leaders().size() == 1);

    // And it joined rather than merely failing to disturb anybody: it follows the
    // same leader, at the same term, having been caught up to what was committed
    // while it was away.
    auto const& attached = cluster.At(joiner).driver->Node();
    CHECK(attached.CurrentRole() == Role::Follower);
    CHECK(attached.CurrentTerm() == term);
    CHECK(attached.KnownLeader() == std::optional<NodeId> { leader });
    CHECK(attached.CommitIndex() == cluster.At(leader).driver->Node().CommitIndex());

    RequireNoViolations(cluster);
}

TEST_CASE("A leader that loses quorum contact stops being one", "[consensus][raft][cluster][prevote]")
{
    // #437. Raft does not need this for SAFETY -- a partitioned leader commits
    // nothing, because committing needs the quorum it has lost. It is needed by
    // everything that READS from a leader, which is most of what this daemon
    // exposes: `--cluster-status`, the fleet page, `NotLeader` redirects, and a
    // `--cluster-set` that is reported accepted and then never commits.
    //
    // What made it invisible is that `HasQuorumContact` already existed and was
    // consulted only when answering somebody else's pre-vote. The leader knew, and
    // acted on it only on another node's behalf.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    auto const isolated = Unwrap(cluster.Leader());
    auto const termWhenLeading = cluster.At(isolated).driver->Node().CurrentTerm();
    cluster.Partition({ isolated });

    auto stoodDown = false;
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, std::size_t { 600 }))
    {
        cluster.Step();
        if (cluster.At(isolated).driver->Node().CurrentRole() != Role::Leader)
        {
            stoodDown = true;
            break;
        }
    }
    CHECK(stoodDown);

    // In the SAME term, which is the half that keeps this from being a cure worse
    // than the disease: a node that bumped its term here would return from the
    // partition and depose a leader that is working perfectly well -- exactly the
    // disruption pre-vote exists to prevent.
    CHECK(cluster.At(isolated).driver->Node().CurrentTerm() == termWhenLeading);

    // And it names nobody, rather than going on redirecting clients to itself.
    CHECK_FALSE(cluster.At(isolated).driver->Node().KnownLeader().has_value());

    // The majority side is unaffected and still elects, so this cannot pass by the
    // whole cluster having stalled.
    cluster.Run(200);
    auto const others = cluster.Leaders();
    CHECK(std::ranges::find(others, isolated) == others.end());

    RequireNoViolations(cluster);
}

TEST_CASE("A healthy cluster never changes term", "[consensus][raft][cluster][prevote]")
{
    // The property issue #117 is about, stated at the cluster level: with every
    // node reachable and nothing else wrong, leadership is decided once. A term
    // that moves here is a node campaigning against a leader it can hear, which is
    // exactly what the pre-vote round exists to prevent and what the follower side
    // of `HasLiveLeader` got wrong twice.
    //
    // A guard rather than a regression test, and worth saying which: it passes
    // against the defect too, because the harness delivers every heartbeat on time
    // and a follower whose leader is punctual never reaches either version of the
    // rule. What made the real cluster re-elect was a runner slow enough to age
    // out a timestamp, and reproducing *that* here would mean racing a grant
    // against the next heartbeat -- arithmetic over the step size and the
    // per-message delay, which this rulebook already records paying for as a case
    // that reports future regressions as flakes. The exact form of the rule is
    // pinned by the `ManualClock` cases on `RaftNode`; this is the end-to-end
    // statement that nothing perturbs a cluster nobody is perturbing.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    REQUIRE(cluster.Leader().has_value());
    REQUIRE(cluster.TermOfLeader().has_value());
    auto const leader = Unwrap(cluster.Leader());
    auto const term = Unwrap(cluster.TermOfLeader());

    // Six simulated seconds -- twenty to forty election timeouts, so every node's
    // randomized draw comes round many times over.
    cluster.Run(600);

    CHECK(cluster.Leader() == std::optional<NodeId> { leader });
    CHECK(cluster.TermOfLeader() == std::optional<Term> { term });
    CHECK(cluster.Leaders().size() == 1);

    // Every node, not just the leader: a follower that campaigned and lost would
    // carry a raised term while the leader's stayed put.
    for (auto const& id: { "n1", "n2", "n3" })
        CHECK(cluster.At(id).driver->Node().CurrentTerm() == term);

    RequireNoViolations(cluster);
}

TEST_CASE("A cluster of one that admits a second member keeps leading", "[consensus][raft][cluster][membership]")
{
    // The arrangement that has no recovery, and the reason it is the discriminator
    // rather than the four-member one. Growing from one member to two doubles the
    // quorum at the instant the configuration is adopted, and the member it adds
    // has by construction never answered -- so a leader deposed for that silence
    // needs a vote from a node holding no configuration, which grants none. There
    // is no second candidate and no later term: both machines stay up, healthy and
    // leaderless.
    //
    // At three members and above somebody else can campaign, so the same defect
    // presents as an election storm that eventually settles -- which is why a case
    // asserting only that a leader exists eventually passes under it.
    RaftClusterHarness cluster { { "n1" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));
    REQUIRE(Unwrap(cluster.Leader()) == "n1");

    cluster.Join("n2");
    cluster.Run(30);
    REQUIRE_FALSE(cluster.At("n2").driver->Node().HasCluster());

    auto const term = Unwrap(cluster.TermOfLeader());
    REQUIRE(cluster.ProposeMembershipOnLeader(Voters({ "n1", "n2" })).has_value());

    // The admitted member's first answer does not arrive inside one heartbeat, and
    // the case says nothing without that. The harness delivers in one to three
    // steps, so a proposal answered by the very next step reaches the leader before
    // its own heartbeat falls due and the defect is stepped straight over -- this
    // case passed against it, measured, before the delay was added. What produces
    // it in the field is ordinary: a leader's durability write and a joiner's first
    // reply both happen before any contact can be recorded, and on a sanitizer
    // build they are not reliably done inside 50 ms.
    //
    // Four steps, not more: the answer must still land inside the window a member
    // admitted at this instant is owed, so a partition long enough to outlast that
    // window would take the leader down under the fix as well and assert nothing.
    cluster.Partition({ "n1" });
    cluster.Run(4);
    cluster.Heal();
    cluster.Run(120);

    // Named, and in the same term: "somebody leads" is what the four-member
    // arrangement answers under the defect too, after four terms of churn. What
    // distinguishes this is that the node which proposed the change is still the
    // one leading, and that admitting a member cost no election at all.
    CHECK(cluster.Leader() == std::optional<NodeId> { "n1" });
    CHECK(cluster.TermOfLeader() == std::optional<Term> { term });

    // And the member it admitted was actually replicated to, which is what makes
    // the assertion above a property of the cluster rather than of one node's
    // opinion of itself.
    CHECK(cluster.At("n2").driver->Node().HasCluster());
    CHECK(cluster.At("n2").driver->Node().ActiveConfiguration().voters.size() == 2);
    CHECK(cluster.At("n2").driver->Node().KnownLeader() == std::optional<NodeId> { "n1" });

    RequireNoViolations(cluster);
}

// --------------------------------------------------------------------------
// Learners (#1449): a machine that is usually absent, as a member of the cluster.

namespace
{

/// A two-machine cluster in which `n1` leads and `n2` has been admitted, as `seat`
/// says -- with the change committed and `n2` caught up.
///
/// Built the way a real pair is: `n1` leads a cluster of itself, `n2` joins with no
/// configuration, and the leader proposes the pair. The learner case and its voter
/// control differ in that ONE argument and in nothing else, so whatever separates
/// their outcomes is the seat.
/// @param seat The configuration the leader proposes for the pair.
/// @return The cluster, `n1` leading.
[[nodiscard]] std::unique_ptr<RaftClusterHarness> PairWith(Configuration const& seat)
{
    auto cluster = std::make_unique<RaftClusterHarness>(std::vector<NodeId> { "n1" }, ClusterKey);
    REQUIRE(SettleOnLeader(*cluster));
    REQUIRE(Unwrap(cluster->Leader()) == "n1");

    cluster->Join("n2");
    cluster->Run(30);
    REQUIRE(cluster->ProposeMembershipOnLeader(seat).has_value());
    cluster->Run(120);

    // Formed, not merely elected: n2 holds the configuration, follows n1, and has
    // been caught up to everything n1 committed.
    auto const& second = cluster->At("n2").driver->Node();
    REQUIRE(second.HasCluster());
    REQUIRE(second.KnownLeader() == std::optional<NodeId> { "n1" });
    REQUIRE(second.CommitIndex() == cluster->At("n1").driver->Node().CommitIndex());
    REQUIRE(cluster->At("n1").driver->Node().CurrentRole() == Role::Leader);
    return cluster;
}

} // namespace

TEST_CASE("A voter whose only peer is a learner leads through that learner's absence, and restarts alone to lead",
          "[consensus][raft][cluster][learner]")
{
    // #178's clause, which is #1449's reason to exist: an always-on node and a laptop
    // that drops off the VPN. With the laptop a voter, the always-on node loses its
    // quorum the moment the laptop leaves and stops leading -- the control below.
    // With the laptop a LEARNER it is replicated to and counted by nothing, so its
    // absence costs the leader nothing, and the leader restarting alone still leads.
    auto cluster = PairWith(Configuration { .voters = { "n1" }, .learners = { "n2" } });
    REQUIRE(cluster->At("n2").driver->Node().CurrentStanding() == Standing::Learner);

    auto const term = cluster->At("n1").driver->Node().CurrentTerm();
    CAPTURE(term.value);

    // The learner goes absent.
    cluster->Partition({ "n1" });

    // Checked at EVERY step, never only at the end: a lapse that recovered before the
    // last step is the defect too, and a single poll passes against leadership that
    // comes and goes. Six simulated seconds is forty `electionTimeoutMin` windows --
    // CheckQuorum would have deposed a leader counting n2 inside the first.
    for (auto const step: std::views::iota(std::size_t { 0 }, std::size_t { 600 }))
    {
        cluster->Step();
        CAPTURE(step);
        auto const& leader = cluster->At("n1").driver->Node();
        REQUIRE(leader.CurrentRole() == Role::Leader);
        REQUIRE(leader.CurrentTerm() == term);
    }

    // It commits alone, too: the learner is no part of the quorum that decides that.
    auto const alone = cluster->ProposeOnLeader(FastCache::BytesFromString("while n2 is away"));
    REQUIRE(alone.has_value());
    cluster->Run(10);
    CHECK(cluster->At("n1").driver->Node().CommitIndex() >= Unwrap(alone));

    // And it restarts alone, the learner still absent, recovering the pair's
    // configuration from its own log rather than from a list it was started with.
    REQUIRE(cluster->Restart("n1").has_value());
    REQUIRE(cluster->At("n1").driver->Node().CurrentRole() == Role::Follower);
    CHECK(cluster->At("n1").driver->Node().ActiveConfiguration()
          == Configuration { .voters = { "n1" }, .learners = { "n2" } });

    auto relead = false;
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, std::size_t { 600 }))
    {
        cluster->Step();
        if (cluster->At("n1").driver->Node().CurrentRole() == Role::Leader)
        {
            relead = true;
            break;
        }
    }
    auto const termAfterRestart = cluster->At("n1").driver->Node().CurrentTerm();
    CAPTURE(termAfterRestart.value);
    CHECK(relead);
    CHECK(termAfterRestart > term);

    RequireNoViolations(*cluster);
}

TEST_CASE("The same two machines as two voters lose their leader when one is absent", "[consensus][raft][cluster][learner]")
{
    // The CONTROL for the case above: every step the same, and n2 admitted as a VOTER.
    // Two voters are a quorum of two, so the leader stops leading once n2 has been
    // silent for a CheckQuorum window -- which is the behaviour #437 wants for a real
    // quorum, and the one a learner exists to be exempt from. Without this beside it,
    // a harness that simply never deposed anybody would pass the case above.
    auto cluster = PairWith(Configuration { .voters = { "n1", "n2" }, .learners = {} });
    REQUIRE(cluster->At("n2").driver->Node().CurrentStanding() == Standing::Voter);

    auto const term = cluster->At("n1").driver->Node().CurrentTerm();
    CAPTURE(term.value);
    cluster->Partition({ "n1" });

    auto stoodDown = false;
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, std::size_t { 600 }))
    {
        cluster->Step();
        if (cluster->At("n1").driver->Node().CurrentRole() != Role::Leader)
        {
            stoodDown = true;
            break;
        }
    }
    CHECK(stoodDown);
    CHECK(cluster->At("n1").driver->Node().CurrentTerm() == term);

    RequireNoViolations(*cluster);
}

TEST_CASE("A learner is promoted and demoted one change at a time, across the whole cluster",
          "[consensus][raft][cluster][learner]")
{
    // The node-level cases pin each step's rule; this is every message authenticated,
    // every member applying the same configurations in the same order, and the
    // invariants checked at every step while a member moves between the two sets.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey };
    REQUIRE(SettleOnLeader(cluster));

    cluster.Join("n4");
    cluster.Run(30);

    auto const agreed = [&cluster](Configuration const& expected) {
        for (auto const* const id: Everyone)
        {
            CAPTURE(id);
            CHECK(cluster.At(id).driver->Node().ActiveConfiguration() == expected);
        }
    };

    auto const learner = Configuration { .voters = { "n1", "n2", "n3" }, .learners = { "n4" } };
    REQUIRE(cluster.ProposeMembershipOnLeader(learner).has_value());
    cluster.Run(120);
    agreed(learner);
    CHECK(cluster.At("n4").driver->Node().CurrentStanding() == Standing::Learner);

    auto const promoted = Voters({ "n1", "n2", "n3", "n4" });
    REQUIRE(cluster.ProposeMembershipOnLeader(promoted).has_value());
    cluster.Run(120);
    agreed(promoted);
    CHECK(cluster.At("n4").driver->Node().CurrentStanding() == Standing::Voter);

    REQUIRE(cluster.ProposeMembershipOnLeader(learner).has_value());
    cluster.Run(120);
    agreed(learner);
    CHECK(cluster.At("n4").driver->Node().CurrentStanding() == Standing::Learner);

    // Still one leader, still committing.
    REQUIRE(SettleOnLeader(cluster));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("after")).has_value());
    cluster.Run(60);
    CHECK_FALSE(cluster.At("n4").applied.empty());

    RequireNoViolations(cluster);
}

// --------------------------------------------------------------------------
// A snapshot reaches the application, on recovery and on install (#1542).

namespace
{

/// Whether two applications hold the same entries, index and bytes alike.
/// @param lhs One.
/// @param rhs The other.
/// @return True when they are the same sequence.
[[nodiscard]] bool SameApplication(std::span<AppliedEntry const> lhs, std::span<AppliedEntry const> rhs)
{
    return std::ranges::equal(lhs, rhs, [](AppliedEntry const& left, AppliedEntry const& right) {
        return left.index == right.index && left.payload == right.payload;
    });
}

/// Trims every node's log into a snapshot once four applied entries pile up.
constexpr auto CompactOften = CompactionPolicy { .appliedEntriesBeforeCompaction = 4 };

/// Propose `count` commands on whoever leads, letting each replicate.
/// @param cluster The cluster.
/// @param count How many.
void ProposeSeveral(RaftClusterHarness& cluster, int count)
{
    for (auto const step: std::views::iota(0, count))
    {
        REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString(std::format("e{}", step))).has_value());
        cluster.Run(5);
    }
}

} // namespace

TEST_CASE("A node restarted after compacting holds in its application what its snapshot covered",
          "[consensus][raft][cluster][snapshot]")
{
    // The harness half of #1542. Its state machine used to hold nothing and restore
    // nothing, so every restart case here passed whether or not a recovered snapshot
    // reached the application. It now holds what it applied, and a restart empties it
    // the way a process's memory is emptied -- so what n2 holds after restarting is
    // exactly what recovery gave back.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey, 1, CompactOften };
    REQUIRE(SettleOnLeader(cluster));
    ProposeSeveral(cluster, 10);
    cluster.Run(60);

    auto const snapshotIndex = cluster.At("n2").driver->Node().SnapshotIndex();
    REQUIRE(snapshotIndex != LogIndex::BeforeFirst());
    auto const before = cluster.At("n2").application;
    REQUIRE(SameApplication(before, cluster.At("n1").application));

    REQUIRE(cluster.Restart("n2").has_value());

    // Before a single step: exactly what the snapshot covers, restored -- not empty, and
    // not the entries above it, which only a commit can bring back.
    //
    // The size is a REQUIRE: a missed restore fails every check below together, and
    // exactly four of them used to -- which a Catch2 exit status collides with
    // `SKIP_RETURN_CODE 4`, scoring the neutered fix SKIPPED rather than failed (#1152).
    // Measured on this case before this line was a REQUIRE.
    auto const covered = static_cast<std::size_t>(
        std::ranges::count_if(before, [snapshotIndex](AppliedEntry const& entry) { return entry.index <= snapshotIndex; }));
    REQUIRE(covered > 0);
    auto const& restored = cluster.At("n2").application;
    REQUIRE(restored.size() == covered);
    CHECK(SameApplication(restored, std::span { before }.first(std::min(covered, before.size()))));

    // And once it is caught up, all of it: the same as before the restart, and the same
    // as the leader's.
    cluster.Run(120);
    CHECK(SameApplication(cluster.At("n2").application, before));
    CHECK(SameApplication(cluster.At("n2").application, cluster.At("n1").application));

    RequireNoViolations(cluster);
}

TEST_CASE("A follower caught up by an installed snapshot holds in its application what the snapshot covered",
          "[consensus][raft][cluster][snapshot]")
{
    // The other half of `RestoreSnapshot`'s contract, which the old no-op restore let
    // through just as silently: a follower the leader can no longer replay to is sent
    // the state instead, and its application must hold that state -- not the handful
    // of entries it applied itself before it fell behind.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey, 1, CompactOften };
    REQUIRE(SettleOnLeader(cluster));

    auto const leader = Unwrap(cluster.Leader());
    auto const lagging = NodeId { leader == "n3" ? "n2" : "n3" };

    cluster.Partition({ lagging });
    ProposeSeveral(cluster, 10);
    cluster.Run(60);
    REQUIRE(cluster.At(leader).driver->Node().SnapshotIndex() > cluster.At(lagging).driver->Node().LastApplied());

    cluster.Heal();
    cluster.Run(200);

    // Installed rather than replayed: the entries below the leader's snapshot were
    // never applied on the lagging node, one by one, yet its application holds them.
    CHECK(cluster.At(lagging).driver->Node().SnapshotIndex() != LogIndex::BeforeFirst());
    CHECK(SameApplication(cluster.At(lagging).application, cluster.At(leader).application));
    CHECK(cluster.At(lagging).applied.size() < cluster.At(leader).application.size());

    RequireNoViolations(cluster);
}

TEST_CASE("A node whose own recovered snapshot its application cannot read stays down, and holds nothing",
          "[consensus][raft][cluster][snapshot]")
{
    // The harness half of #1542's follow-up. A restart here goes through
    // `RaftDriver::Create`, production's one seam, so a node whose OWN snapshot is one its
    // application cannot read is refused there -- and here that leaves it DOWN, which is
    // what a process that refused to start is: no driver, and an application holding
    // nothing, because the refusal handed it nothing. The rest of the cluster carries on.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey, 1, CompactOften };
    REQUIRE(SettleOnLeader(cluster));
    ProposeSeveral(cluster, 10);
    cluster.Run(60);

    auto const leader = Unwrap(cluster.Leader());
    auto const follower = NodeId { leader == "n2" ? "n3" : "n2" };
    REQUIRE(cluster.At(follower).driver->Node().SnapshotIndex() != LogIndex::BeforeFirst());

    SECTION("control: its own snapshot, as written, restarts it")
    {
        REQUIRE(cluster.Restart(follower).has_value());
        CHECK(cluster.At(follower).driver != nullptr);
        CHECK_FALSE(cluster.At(follower).application.empty());
    }

    SECTION("its own snapshot, rewritten to bytes the encoder never writes, keeps it down")
    {
        // One field where every entry is two: nothing `EncodeApplication` produced.
        auto& storage = *cluster.At(follower).storage;
        auto recovered = storage.Load();
        REQUIRE(recovered.has_value());
        REQUIRE(recovered->snapshot.has_value());
        auto snapshot = Unwrap(recovered->snapshot);
        snapshot.state = WireFields::Encode({ WireFields::AsBytes(std::string_view { "one field" }) });
        REQUIRE(storage.SaveSnapshot(snapshot).has_value());

        auto const restarted = cluster.Restart(follower);
        REQUIRE_FALSE(restarted.has_value());
        CHECK(restarted.error().code == ConsensusErrorCode::StorageFailure);
        CHECK(restarted.error().context.starts_with("the snapshot as of log entry "));
        CHECK(restarted.error().context.contains("the snapshot this node recovered"));

        // Down, and handed nothing.
        CHECK(cluster.At(follower).driver == nullptr);
        CHECK(cluster.At(follower).application.empty());

        // And the two it left behind still make a majority, and commit without it.
        auto const without = cluster.ProposeOnLeader(FastCache::BytesFromString("without the node that is down"));
        REQUIRE(without.has_value());
        cluster.Run(60);
        CHECK(cluster.At(leader).driver->Node().CommitIndex() >= Unwrap(without));
        CHECK(cluster.At(follower).driver == nullptr);
    }

    RequireNoViolations(cluster);
}

TEST_CASE("A follower that cannot read its leader's snapshot stays behind, applies nothing, and catches up once it can",
          "[consensus][raft][cluster][snapshot]")
{
    // #1552 across a cluster. A follower cut off while the leader compacted can only be
    // caught up by the leader's snapshot; one whose build reads another state format
    // refuses it rather than take on a state it cannot hold, and so stays EXACTLY where it
    // was -- same application, same applied index -- while the other two carry on without
    // it. Nothing is applied on its stale base, and no safety property is disturbed. Once
    // it can read the snapshot (an upgrade), it takes it on and catches up by itself.
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, ClusterKey, 1, CompactOften };
    REQUIRE(SettleOnLeader(cluster));
    auto const leader = Unwrap(cluster.Leader());
    auto const behind = NodeId { leader == "n3" ? "n2" : "n3" };

    cluster.Partition({ behind });
    ProposeSeveral(cluster, 10);
    cluster.Run(60);
    REQUIRE(cluster.At(leader).driver->Node().SnapshotIndex() > cluster.At(behind).driver->Node().LastApplied());

    cluster.SetSnapshotsUnreadable(behind, true);
    auto const heldBefore = cluster.At(behind).application;
    auto const appliedBefore = cluster.At(behind).driver->Node().LastApplied();
    auto const historyBefore = cluster.At(behind).applied.size();

    cluster.Heal();
    cluster.Run(200);

    // Refused, and held as a refusal of the leader's snapshot.
    auto const held = cluster.At(behind).driver->CurrentProgress().installRefusal;
    REQUIRE(held.has_value());
    CHECK(Unwrap(held).leader == leader);

    // Stayed exactly where it was: nothing installed, nothing applied after it.
    CHECK(cluster.At(behind).driver->Node().LastApplied() == appliedBefore);
    CHECK(SameApplication(cluster.At(behind).application, heldBefore));
    CHECK(cluster.At(behind).applied.size() == historyBefore);

    // The next entry commits without it, and does not reach it either.
    auto const next = cluster.ProposeOnLeader(FastCache::BytesFromString("after the refusal"));
    REQUIRE(next.has_value());
    cluster.Run(60);
    CHECK(cluster.At(leader).driver->Node().CommitIndex() >= Unwrap(next));
    CHECK(cluster.At(behind).applied.size() == historyBefore);
    RequireNoViolations(cluster);

    // Upgraded: it reads the snapshot now, takes it on and catches up, and the refusal ends.
    cluster.SetSnapshotsUnreadable(behind, false);
    cluster.Run(200);
    CHECK_FALSE(cluster.At(behind).driver->CurrentProgress().installRefusal.has_value());
    CHECK(SameApplication(cluster.At(behind).application, cluster.At(leader).application));
    RequireNoViolations(cluster);
}
