// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftClusterHarness.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

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
    // plain Raft has no rule that deposes it, and adding one (CheckQuorum, a
    // leader lease) is a separate mechanism. An isolated *follower* keeps standing
    // for election and keeps losing. Either way the guarantee is the same and it
    // is the one asserted here: nothing the minority does can be committed.
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
