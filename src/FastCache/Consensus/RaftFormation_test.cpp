// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftClusterHarness.hpp>
#include <FastCache/Consensus/Standing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;

// Formation, over the whole cluster harness: machines that start alone, a yielded machine that
// dissolves into a learner, and a learner the leader reaches only over the session the learner
// dials. Every message still proves its sender through the real session objects, so the
// intruder cases in `RaftCluster_test.cpp` and the formation cases here are one harness.

namespace
{

/// Every machine any case in this file starts, each under its own key.
/// @return A fresh roster, so a case that revokes changes nobody else's.
[[nodiscard]] std::shared_ptr<Testing::SharedRoster> Roster()
{
    return Testing::SharedRoster::Of({ "n1", "n2", "n3", "n4", "n5" });
}

/// Who each member of a cluster in this file is: itself, under its own key, over @p roster.
/// @param roster What every member believes about everybody's keys.
/// @return The factory the harness requires.
[[nodiscard]] RaftClusterHarness::IdentityFactory MembersOf(std::shared_ptr<Testing::SharedRoster const> roster)
{
    return [roster = std::move(roster)](NodeId const& who) -> std::unique_ptr<IRaftPeerIdentity const> {
        return Testing::TestPeerIdentity::Honest(who, roster);
    };
}

/// Every safety property, reported by name rather than as a bare count.
/// @param cluster The cluster to read.
void RequireNoViolations(RaftClusterHarness const& cluster)
{
    for (auto const& violation: cluster.Violations())
        FAIL_CHECK(violation);

    REQUIRE(cluster.Violations().empty());
}

/// Require that @p member took @p leader's cluster tag: the invariants over it run at all.
///
/// Adoption is what puts a joiner or a dissolved node under the safety properties, and a node
/// that never took a tag is checked by none of them -- so every case with a learner asserts it,
/// or a regression in adoption switches the checks off for exactly the nodes formation is about.
/// @param cluster The cluster.
/// @param member The node that was admitted.
/// @param leader The leader it was admitted by.
void RequireAdopted(RaftClusterHarness const& cluster, NodeId const& member, NodeId const& leader)
{
    CHECK_FALSE(cluster.At(member).cluster.empty());
    CHECK(cluster.At(member).cluster == cluster.At(leader).cluster);
}

/// @param text A command's text.
/// @return Its bytes, as a command's payload.
[[nodiscard]] std::vector<std::byte> Bytes(std::string_view text)
{
    auto bytes = std::vector<std::byte> {};
    bytes.reserve(text.size());
    for (auto const c: text)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

} // namespace

TEST_CASE("Two machines bootstrap as solitary clusters and each leads its own", "[consensus][raft][formation][harness]")
{
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    cluster.Solitary("n1");
    cluster.Solitary("n2");
    cluster.Run(200);

    CHECK(cluster.LeaderOf("n1") == std::optional<NodeId> { "n1" });
    CHECK(cluster.LeaderOf("n2") == std::optional<NodeId> { "n2" });

    // Both led term 1, which Election Safety across ALL nodes would call a violation: in two
    // clusters it is two elections, each with one winner.
    CHECK(cluster.At("n1").driver->Node().CurrentTerm() == cluster.At("n2").driver->Node().CurrentTerm());
    RequireNoViolations(cluster);
}

TEST_CASE("A yielded machine dissolves into a learner that the leader never dials", "[consensus][raft][formation][harness]")
{
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    cluster.Solitary("n1");
    cluster.Solitary("n2");
    cluster.Run(200);
    REQUIRE(cluster.ProposeOnLeaderOf("n2", Bytes("solitary work")).has_value()); // pending keeps serving
    cluster.Run(50);

    cluster.Dissolve("n2");
    REQUIRE(cluster.At("n2").archived.size() == 1);
    CHECK_FALSE(cluster.At("n2").driver->Node().HasCluster());
    CHECK(cluster.At("n2").application.empty());

    // The archived store is the one it ran its own cluster in, work and all -- not a fresh one.
    auto const archived = cluster.At("n2").archived.front()->Load();
    REQUIRE(archived.has_value());
    CHECK(std::ranges::any_of(archived->entries,
                              [](LogEntry const& entry) { return entry.payload == Bytes("solitary work"); }));

    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1" }, .learners = { "n2" } }).has_value());
    cluster.Run(200);

    CHECK(cluster.At("n2").driver->Node().KnownLeader() == std::optional<NodeId> { "n1" });
    CHECK(cluster.At("n2").driver->Node().CurrentStanding() == Standing::Learner);
    CHECK(cluster.LeaderOf("n2") == std::optional<NodeId> { "n1" });
    CHECK(cluster.DialsFrom("n1", "n2") == 0);
    CHECK(cluster.DialsFrom("n2", "n1") > 0);

    // And the learner's OWN messages ride the session it dials two-way. A DELTA, never an absolute
    // zero: its first reply went out before it held any configuration -- standing `NoCluster`,
    // whose link is `NotReached` -- and that one is one-way.
    auto const oneWayBefore = cluster.DialsFrom("n2", "n1", RaftWire::SessionDirection::OneWay);
    auto const twoWayBefore = cluster.DialsFrom("n2", "n1", RaftWire::SessionDirection::TwoWay);
    cluster.Run(100);
    CHECK(cluster.DialsFrom("n2", "n1", RaftWire::SessionDirection::OneWay) == oneWayBefore);
    CHECK(cluster.DialsFrom("n2", "n1", RaftWire::SessionDirection::TwoWay) > twoWayBefore);
    RequireNoViolations(cluster);
}

TEST_CASE("A learner offline for days catches up by snapshot when it dials in again",
          "[consensus][raft][formation][harness]")
{
    RaftClusterHarness cluster {
        std::vector<NodeId> {}, MembersOf(Roster()), 1, CompactionPolicy { .appliedEntriesBeforeCompaction = 8 }
    };
    cluster.Solitary("n1");
    cluster.Join("n2");
    cluster.Run(100);
    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1" }, .learners = { "n2" } }).has_value());
    cluster.Run(200);
    RequireAdopted(cluster, "n2", "n1");

    cluster.SetDialsIn("n2", false);
    for ([[maybe_unused]] auto const entry: std::views::iota(0, 40))
        REQUIRE(cluster.ProposeOnLeaderOf("n1", Bytes("while away")).has_value());
    cluster.Run(2000); // days, in steps; the leader keeps leading and keeps committing alone
    CHECK(cluster.LeaderOf("n1") == std::optional<NodeId> { "n1" });
    CHECK(cluster.DialsFrom("n1", "n2") == 0);

    // The leader compacted past everything the learner holds, so only a snapshot can catch it up.
    REQUIRE(cluster.At("n1").driver->Node().SnapshotIndex() > cluster.At("n2").driver->Node().CommitIndex());

    cluster.SetDialsIn("n2", true);
    cluster.Run(300);
    CHECK(cluster.At("n2").driver->Node().CommitIndex() == cluster.At("n1").driver->Node().CommitIndex());
    CHECK(cluster.At("n2").application.size() == cluster.At("n1").application.size());
    CHECK(cluster.DialsFrom("n1", "n2") == 0);
    RequireNoViolations(cluster);
}

TEST_CASE("A learner returns after leadership moved and the new leader serves it without dialling",
          "[consensus][raft][formation][harness]")
{
    // An operator promoted a second voter, the founder stepped down, the learner comes back.
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    cluster.Solitary("n1");
    cluster.Join("n2");
    cluster.Join("n3");
    cluster.Run(100);
    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1" }, .learners = { "n2" } }).has_value());
    cluster.Run(200);
    REQUIRE(cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1" }, .learners = { "n2", "n3" } })
                .has_value());
    cluster.Run(200);

    // The operator promotes n3 (one change), then demotes the founder (one change): n3 then leads alone.
    REQUIRE(cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1", "n3" }, .learners = { "n2" } })
                .has_value());
    cluster.Run(200);
    cluster.SetDialsIn("n2", false);
    REQUIRE(cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n3" }, .learners = { "n2", "n1" } })
                .has_value());
    cluster.Run(400);
    REQUIRE(cluster.LeaderOf("n3") == std::optional<NodeId> { "n3" });
    // Leading implies it, and the promotion is what this case is about, so it is said.
    CHECK(cluster.At("n3").driver->Node().CurrentStanding() == Standing::Voter);

    cluster.SetDialsIn("n2", true);
    cluster.Run(300);
    CHECK(cluster.At("n2").driver->Node().KnownLeader() == std::optional<NodeId> { "n3" });
    CHECK(cluster.At("n2").driver->Node().CommitIndex() == cluster.At("n3").driver->Node().CommitIndex());
    CHECK(cluster.DialsFrom("n3", "n2") == 0);
    CHECK(cluster.DialsFrom("n2", "n3") > 0);

    // The learner answers the new leader on the session it dials two-way, never one-way.
    auto const oneWayBefore = cluster.DialsFrom("n2", "n3", RaftWire::SessionDirection::OneWay);
    auto const twoWayBefore = cluster.DialsFrom("n2", "n3", RaftWire::SessionDirection::TwoWay);
    cluster.Run(100);
    CHECK(cluster.DialsFrom("n2", "n3", RaftWire::SessionDirection::OneWay) == oneWayBefore);
    CHECK(cluster.DialsFrom("n2", "n3", RaftWire::SessionDirection::TwoWay) > twoWayBefore);
    RequireNoViolations(cluster);
}

TEST_CASE("A learner restarted in every mode resumes the configuration its own log recorded",
          "[consensus][raft][formation][harness]")
{
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    cluster.Solitary("n1");
    cluster.Solitary("n2");
    cluster.Run(200);

    REQUIRE(cluster.Restart("n2").has_value()); // solitary (and pending, which is the same store)
    cluster.Run(100);
    CHECK(cluster.LeaderOf("n2") == std::optional<NodeId> { "n2" });

    cluster.Dissolve("n2");
    REQUIRE(cluster.Restart("n2").has_value()); // dissolved, not yet admitted: still no cluster
    CHECK_FALSE(cluster.At("n2").driver->Node().HasCluster());

    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1" }, .learners = { "n2" } }).has_value());
    cluster.Run(200);
    REQUIRE(cluster.Restart("n2").has_value()); // learner
    cluster.Run(200);
    CHECK(cluster.At("n2").driver->Node().CurrentStanding() == Standing::Learner);
    RequireAdopted(cluster, "n2", "n1");
    CHECK(cluster.DialsFrom("n1", "n2") == 0);

    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1", "n2" }, .learners = {} }).has_value());
    cluster.Run(200);
    REQUIRE(cluster.Restart("n2").has_value()); // voter
    cluster.Run(200);
    CHECK(cluster.At("n2").driver->Node().CurrentStanding() == Standing::Voter);
    CHECK(cluster.DialsFrom("n1", "n2") > 0); // a voter IS dialled
    RequireNoViolations(cluster);
}

TEST_CASE("A forgotten learner is refused at its next dial while the leader goes on committing",
          "[consensus][raft][formation][harness][revocation]")
{
    // One session per message: a revoked learner is refused at a FRESH handshake, so this is the
    // dial, never a live session closing at its next frame. That per-frame property (#1555) is
    // pinned at the transport, by "A revoked learner key closes the two-way session in both
    // directions at the next frame" in `RaftPeerLink_test.cpp`.
    auto const roster = Roster();
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(roster) };
    cluster.Solitary("n1");
    cluster.Join("n2");
    cluster.Run(100);
    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1" }, .learners = { "n2" } }).has_value());
    cluster.Run(200);
    RequireAdopted(cluster, "n2", "n1");
    auto const refusedBefore = cluster.RefusedAt("n2");
    auto const caughtUpTo = cluster.At("n2").driver->Node().CommitIndex();
    REQUIRE(caughtUpTo == cluster.At("n1").driver->Node().CommitIndex());

    roster->Revoke("n2");
    REQUIRE(cluster.ProposeOnLeaderOf("n1", Bytes("after the forget")).has_value());
    cluster.Run(50);
    CHECK(cluster.RefusedAt("n2") > refusedBefore);                     // the next frame to it is refused
    CHECK(cluster.At("n2").driver->Node().CommitIndex() == caughtUpTo); // and nothing reaches it after
    CHECK(cluster.At("n1").driver->Node().CommitIndex() > caughtUpTo);  // while the leader goes on committing
    RequireNoViolations(cluster);
}

TEST_CASE("A member that follows another cluster's leader without dissolving is a violation",
          "[consensus][raft][formation][harness]")
{
    // The harness's own guard, planted. Log Matching compares only members of one cluster, so a
    // machine that joined another cluster WITHOUT dissolving -- the split brain formation must
    // never produce -- would mix two logs with nothing comparing them. The cluster-isolation
    // invariant is what reports it; the yielded case above is its control, where the same
    // admission after a `Dissolve` reports nothing.
    //
    // `RaftNode` refuses an `AppendEntries` from a leader its configuration does not contain, so
    // the planted fault is the one that guard cannot see: each solitary cluster names the other's
    // machine as a learner, as a formation that admitted without dissolving would have left it.
    RaftClusterHarness cluster { std::vector<NodeId> {}, MembersOf(Roster()) };
    cluster.Solitary("n1");
    cluster.Solitary("n2");
    cluster.Run(200);

    // n1 restarts into a later term, so its leadership outranks n2's term-1 one.
    REQUIRE(cluster.Restart("n1").has_value());
    cluster.Run(200);
    REQUIRE(cluster.LeaderOf("n1") == std::optional<NodeId> { "n1" });
    REQUIRE(cluster.At("n1").driver->Node().CurrentTerm() > cluster.At("n2").driver->Node().CurrentTerm());
    REQUIRE(cluster.Violations().empty());

    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n2", Configuration { .voters = { "n2" }, .learners = { "n1" } }).has_value());
    REQUIRE(
        cluster.ProposeMembershipOnLeaderOf("n1", Configuration { .voters = { "n1" }, .learners = { "n2" } }).has_value());
    cluster.Run(200);

    // Neither machine dissolved, so each keeps its own tag -- and one of them took the other's
    // leader, which is exactly what the invariant watches for.
    CHECK(cluster.At("n1").cluster == "n1");
    CHECK(cluster.At("n2").cluster == "n2");
    // Pinned to WHICH member followed WHOM: n1 leads the later term, so n2 is the one that takes
    // the other cluster's leader. A count of any isolation line would pass on a harness that
    // named the wrong member, or the wrong cluster, in its report.
    INFO("violations: " << cluster.Violations().size() << ", the first: "
                        << (cluster.Violations().empty() ? std::string {} : cluster.Violations().front()));
    CHECK(std::ranges::contains(
        cluster.Violations(),
        std::string { "Cluster isolation: n2 of cluster n2 accepted leader n1 of cluster n1 without dissolving" }));
}
