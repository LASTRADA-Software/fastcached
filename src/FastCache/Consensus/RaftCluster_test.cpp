// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftClusterHarness.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

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
    for (auto step = std::size_t { 0 }; step < steps; ++step)
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
    RaftClusterHarness cluster { { "n1", "n2", "n3" } };

    REQUIRE(SettleOnLeader(cluster));
    CHECK(cluster.Leaders().size() == 1);
    RequireNoViolations(cluster);
}

TEST_CASE("A five-node cluster elects exactly one leader", "[consensus][raft][cluster]")
{
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, 7 };

    REQUIRE(SettleOnLeader(cluster));
    CHECK(cluster.Leaders().size() == 1);
    RequireNoViolations(cluster);
}

TEST_CASE("A committed entry reaches every node", "[consensus][raft][cluster]")
{
    RaftClusterHarness cluster { { "n1", "n2", "n3" } };
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
    RaftClusterHarness cluster { { "n1", "n2", "n3" } };
    REQUIRE(SettleOnLeader(cluster));

    for (auto index = 0; index < 8; ++index)
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
        for (auto index = std::size_t { 0 }; index < reference.size(); ++index)
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
    // An isolated node that was already leader keeps reporting itself leader:
    // nothing here deposes it. A leader does now track whether a majority still
    // answers it -- that is what lets it refuse a challenger's pre-vote -- but
    // ACTING on that answer by stepping down is a separate mechanism, as is a
    // leader lease. An isolated *follower* keeps standing for election and keeps
    // losing. Either way the guarantee is the same and it is the one asserted
    // here: nothing the minority does can be committed.
    RaftClusterHarness cluster { { "n1", "n2", "n3" } };
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
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, 3 };
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
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, 11 };
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
    RaftClusterHarness cluster { { "n1", "n2", "n3" } };
    REQUIRE(SettleOnLeader(cluster));

    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("before")).has_value());
    cluster.Run(60);

    auto const leader = cluster.Leader();
    REQUIRE(leader.has_value());
    cluster.Restart(leader.value_or(NodeId {}));
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
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, 5 };
    REQUIRE(SettleOnLeader(cluster));
    REQUIRE(cluster.ProposeOnLeader(FastCache::BytesFromString("durable")).has_value());
    cluster.Run(80);

    for (auto const& id: { "n1", "n2", "n3" })
    {
        cluster.Restart(id);
        cluster.Run(200);
    }

    REQUIRE(SettleOnLeader(cluster, 300));

    // Every node still holds the committed entry in its log.
    for (auto const& id: { "n1", "n2", "n3" })
    {
        auto const& log = cluster.At(id).driver->Node().Log();
        auto found = false;
        for (auto index = std::uint64_t { 1 }; index <= log.LastIndex().value; ++index)
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
    RaftClusterHarness cluster { { "n1", "n2", "n3" }, 13 };
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
    RaftClusterHarness cluster { { "n1", "n2", "n3", "n4", "n5" }, 23 };
    cluster.SetLossPercent(15);

    auto proposals = 0;
    for (auto round = 0; round < 12; ++round)
    {
        if (round % 4 == 1)
            cluster.Partition({ "n1", "n2" });
        else if (round % 4 == 3)
            cluster.Heal();

        cluster.Run(60);

        if (cluster.ProposeOnLeader(FastCache::BytesFromString("r" + std::to_string(round))).has_value())
            ++proposals;

        if (round % 5 == 2)
            cluster.Restart("n3");

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
    RaftClusterHarness cluster { { "n1", "n2", "n3" } };
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

    REQUIRE(cluster.ProposeMembershipOnLeader({ "n1", "n2", "n3", "n4" }).has_value());
    cluster.Run(120);

    CHECK(cluster.At("n4").driver->Node().HasCluster());
    CHECK(cluster.At("n4").driver->Node().ActiveMembers().size() == 4);

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

TEST_CASE("An admitted member survives its own restart", "[consensus][raft][cluster][membership]")
{
    // The whole point of putting membership in the log rather than on a command
    // line. The joiner was started with no bootstrap set and has none to fall back
    // on, so a restart that did not re-derive its configuration from its own log
    // would come back with no cluster and wait to be admitted a second time.
    RaftClusterHarness cluster { { "n1", "n2", "n3" } };
    REQUIRE(SettleOnLeader(cluster));

    cluster.Join("n4");
    cluster.Run(30);
    REQUIRE(cluster.ProposeMembershipOnLeader({ "n1", "n2", "n3", "n4" }).has_value());
    cluster.Run(120);
    REQUIRE(cluster.At("n4").driver->Node().HasCluster());

    cluster.Restart("n4");
    CHECK(cluster.At("n4").driver->Node().HasCluster());
    CHECK(cluster.At("n4").driver->Node().ActiveMembers().size() == 4);

    cluster.Run(200);
    RequireNoViolations(cluster);
}
