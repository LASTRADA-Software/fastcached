// SPDX-License-Identifier: Apache-2.0
//
// Membership changes, one server at a time. The rule that carries the safety
// argument is the single-member delta: any majority of the old configuration
// and any majority of the new one then share at least one member, so the two
// cannot elect different leaders in the same term.
#include <FastCache/Consensus/IRaftStorage.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

constexpr auto ElectionMin = 150ms;

/// A time point `millis` after the epoch.
/// @param millis Offset in milliseconds.
/// @return The instant.
[[nodiscard]] TimePoint At(std::int64_t millis)
{
    return TimePoint {} + std::chrono::milliseconds { millis };
}

/// A three-node configuration for `self`.
/// @param self Which member this node is.
/// @return The configuration.
[[nodiscard]] RaftConfig ThreeNodes(NodeId self = "n1")
{
    return RaftConfig { .self = std::move(self),
                        .members = { "n1", "n2", "n3" },
                        .electionTimeoutMin = ElectionMin,
                        .electionTimeoutMax = 300ms,
                        .heartbeatInterval = 50ms };
}

/// A node that has been elected leader of term 1.
struct LeaderFixture
{
    ScriptedRandomSource random { { 0 } };
    RaftNode node = std::move(RaftNode::Create(ThreeNodes(), random, TimePoint {})).value();

    LeaderFixture()
    {
        (void) node.Tick(At(ElectionMin.count()));
        for (auto const* const voter: { "n2", "n3" })
            (void) node.Receive(
                PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = voter },
                At(ElectionMin.count()));
        (void) node.Receive(
            RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
            At(ElectionMin.count()));
    }

    /// Acknowledge everything this leader holds, from both peers, so its entries
    /// commit.
    /// @param at When.
    void AcknowledgeAll(std::int64_t at)
    {
        for (auto const* const peer: { "n2", "n3" })
            (void) node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                        .result = AppendResult::Accepted,
                                                        .matchIndex = node.Log().LastIndex(),
                                                        .followerId = peer },
                                At(at));
    }
};

} // namespace

TEST_CASE("A member set round-trips through an entry payload", "[consensus][raft][membership]")
{
    // A length-prefixed field per member, so an id may hold any byte and there is
    // no separator to escape.
    std::vector<NodeId> const members { "n1", "node-with-dash", "", "n4" };
    auto const decoded = Membership::Decode(Membership::Encode(members));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value_or(std::vector<NodeId> {}) == members);
}

TEST_CASE("An empty member set round-trips as empty", "[consensus][raft][membership]")
{
    auto const decoded = Membership::Decode(Membership::Encode(std::vector<NodeId> {}));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value_or(std::vector<NodeId> { "x" }).empty());
}

TEST_CASE("A malformed configuration payload is refused", "[consensus][raft][membership]")
{
    auto encoded = Membership::Encode(std::vector<NodeId> { "n1", "n2" });
    encoded.resize(encoded.size() - 1);
    CHECK_FALSE(Membership::Decode(encoded).has_value());
}

TEST_CASE("A change is classified by how many members move", "[consensus][raft][membership]")
{
    std::vector<NodeId> const three { "n1", "n2", "n3" };

    CHECK(Membership::Classify(three, three) == Membership::ChangeShape::Unchanged);
    CHECK(Membership::Classify(three, std::vector<NodeId> { "n1", "n2", "n3", "n4" }) == Membership::ChangeShape::AddedOne);
    CHECK(Membership::Classify(three, std::vector<NodeId> { "n1", "n2" }) == Membership::ChangeShape::RemovedOne);

    // Two at once is the shape the single-server rule exists to refuse: {n1,n2}
    // is a majority of the old and {n3,n4,n5} a majority of the new, sharing
    // nobody -- so both could elect in the same term.
    CHECK(Membership::Classify(three, std::vector<NodeId> { "n1", "n2", "n3", "n4", "n5" })
          == Membership::ChangeShape::Unsafe);

    // A swap is two changes even though the size does not move.
    CHECK(Membership::Classify(three, std::vector<NodeId> { "n1", "n2", "n9" }) == Membership::ChangeShape::Unsafe);

    // Order carries no meaning.
    CHECK(Membership::Classify(three, std::vector<NodeId> { "n3", "n1", "n2" }) == Membership::ChangeShape::Unchanged);
}

TEST_CASE("A member set that could not operate is refused", "[consensus][raft][membership]")
{
    CHECK_FALSE(Membership::Validate(std::vector<NodeId> {}).has_value());
    CHECK_FALSE(Membership::Validate(std::vector<NodeId> { "n1", "" }).has_value());

    // A duplicate would make one node count twice toward a quorum, which is a
    // quorum that does not exist.
    CHECK_FALSE(Membership::Validate(std::vector<NodeId> { "n1", "n2", "n1" }).has_value());

    CHECK(Membership::Validate(std::vector<NodeId> { "n1", "n2", "n3" }).has_value());
}

TEST_CASE("A leader adopts a new configuration before it commits", "[consensus][raft][membership]")
{
    // The rule that looks unsafe and is the opposite: a configuration that only
    // took effect once committed could not be used to REACH commitment, because
    // committing it needs a quorum of the very set it describes.
    LeaderFixture fix;
    REQUIRE(fix.node.ActiveMembers().size() == 3);

    auto const proposed = fix.node.ProposeMembership({ "n1", "n2", "n3", "n4" }, At(200));
    REQUIRE(proposed.has_value());

    CHECK(fix.node.ActiveMembers().size() == 4);

    // The entry is in the log and is consensus' own: it is never handed to the
    // application, exactly as a NoOp is not.
    auto const* const entry = fix.node.Log().EntryAt(proposed->index);
    REQUIRE(entry != nullptr);
    CHECK(entry->kind == EntryKind::Configuration);
    CHECK(proposed->output.applied.empty());

    // And the new member is replicated to immediately, which is the point of
    // adopting early.
    auto const requests = proposed->output.messages;
    auto sawNewMember = false;
    for (auto const& message: requests)
        if (message.to == "n4")
            sawNewMember = true;
    CHECK(sawNewMember);
}

TEST_CASE("A two-member change is refused", "[consensus][raft][membership]")
{
    LeaderFixture fix;

    auto const refused = fix.node.ProposeMembership({ "n1", "n2", "n3", "n4", "n5" }, At(200));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::InvalidConfiguration);

    // The refusal says why, because "invalid configuration" tells an operator
    // nothing about what would have worked.
    CHECK(refused.error().context.contains("one member"));

    // And nothing moved.
    CHECK(fix.node.ActiveMembers().size() == 3);
}

TEST_CASE("A second change is refused until the first commits", "[consensus][raft][membership]")
{
    // A change built on a configuration a truncation can still roll back would
    // have its safety argument made against a set that never existed.
    LeaderFixture fix;

    REQUIRE(fix.node.ProposeMembership({ "n1", "n2", "n3", "n4" }, At(200)).has_value());

    auto const second = fix.node.ProposeMembership({ "n1", "n2", "n3", "n4", "n5" }, At(201));
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().context.contains("already in flight"));

    // Once a quorum holds it, the next change is allowed.
    fix.AcknowledgeAll(202);
    REQUIRE(fix.node.CommitIndex() >= LogIndex { .value = 2 });
    CHECK(fix.node.ProposeMembership({ "n1", "n2", "n3", "n4", "n5" }, At(203)).has_value());
}

TEST_CASE("A non-leader cannot change the configuration", "[consensus][raft][membership]")
{
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes(), random, TimePoint {})).value();

    auto const refused = node.ProposeMembership({ "n1", "n2" }, At(10));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::NotLeader);
}

TEST_CASE("Proposing the current member set is refused", "[consensus][raft][membership]")
{
    LeaderFixture fix;
    auto const refused = fix.node.ProposeMembership({ "n3", "n2", "n1" }, At(200));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().context.contains("current one"));
}

TEST_CASE("A follower adopts a configuration it receives", "[consensus][raft][membership]")
{
    // Every node has to reach the same configuration through the log, or a
    // cluster becomes two clusters that each think they are one.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();
    REQUIRE(node.ActiveMembers().size() == 3);

    (void) node.Receive(
        AppendEntriesRequest {
            .term = Term { .value = 1 },
            .leaderId = "n1",
            .prevLogIndex = LogIndex::BeforeFirst(),
            .prevLogTerm = Term::None(),
            .entries = { LogEntry { .term = Term { .value = 1 },
                                    .kind = EntryKind::Configuration,
                                    .payload = Membership::Encode(std::vector<NodeId> { "n1", "n2", "n3", "n4" }) } },
            .leaderCommit = LogIndex::BeforeFirst() },
        At(10));

    CHECK(node.ActiveMembers().size() == 4);
}

TEST_CASE("A truncated configuration is rolled back", "[consensus][raft][membership]")
{
    // The consequence of using the latest configuration committed or not: an
    // uncommitted one can be discarded by a conflicting suffix, so the active set
    // is not a value that only ever moves forward. A node that kept a
    // configuration the cluster discarded would count quorums against a set
    // nobody else has.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();

    (void) node.Receive(
        AppendEntriesRequest {
            .term = Term { .value = 1 },
            .leaderId = "n1",
            .prevLogIndex = LogIndex::BeforeFirst(),
            .prevLogTerm = Term::None(),
            .entries = { LogEntry { .term = Term { .value = 1 },
                                    .kind = EntryKind::Configuration,
                                    .payload = Membership::Encode(std::vector<NodeId> { "n1", "n2", "n3", "n4" }) } },
            .leaderCommit = LogIndex::BeforeFirst() },
        At(10));
    REQUIRE(node.ActiveMembers().size() == 4);

    // A leader of a later term overwrites index 1 with an ordinary entry.
    (void) node.Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                               .leaderId = "n3",
                                               .prevLogIndex = LogIndex::BeforeFirst(),
                                               .prevLogTerm = Term::None(),
                                               .entries = { LogEntry { .term = Term { .value = 2 },
                                                                       .kind = EntryKind::Command,
                                                                       .payload = BytesFromString("x") } },
                                               .leaderCommit = LogIndex::BeforeFirst() },
                        At(20));

    // Back to the set it was bootstrapped with, because the log no longer holds
    // any configuration entry at all.
    CHECK(node.ActiveMembers().size() == 3);
}

TEST_CASE("A restarted node comes back under the configuration in its log", "[consensus][raft][membership]")
{
    // Otherwise a restart silently reverts a membership change the cluster made,
    // and the node counts quorums against a set the others have left behind.
    ScriptedRandomSource random { { 0 } };

    RecoveredState recovered;
    recovered.entries = { LogEntry { .term = Term { .value = 1 },
                                     .kind = EntryKind::Configuration,
                                     .payload = Membership::Encode(std::vector<NodeId> { "n1", "n2" }) } };

    auto node = std::move(RaftNode::Create(ThreeNodes(), random, TimePoint {}, std::move(recovered))).value();
    CHECK(node.ActiveMembers() == std::vector<NodeId> { "n1", "n2" });
}

TEST_CASE("A leader removed from the configuration steps down once it commits", "[consensus][raft][membership]")
{
    // It cannot simply stop: the entry that removes it must be committed first,
    // and only this leader can commit it -- so it keeps leading a cluster it is
    // no longer part of for exactly as long as that takes.
    LeaderFixture fix;

    REQUIRE(fix.node.ProposeMembership({ "n2", "n3" }, At(200)).has_value());

    // Still leading: the removal is not committed yet.
    CHECK(fix.node.CurrentRole() == Role::Leader);

    // The remaining members acknowledge, which commits it.
    auto const last = fix.node.Log().LastIndex();
    for (auto const* const peer: { "n2", "n3" })
        (void) fix.node.Receive(
            AppendEntriesResponse {
                .term = Term { .value = 1 }, .result = AppendResult::Accepted, .matchIndex = last, .followerId = peer },
            At(201));

    CHECK(fix.node.CurrentRole() == Role::Follower);
}

TEST_CASE("Removing a member shrinks the quorum it takes to commit", "[consensus][raft][membership]")
{
    // The change has to reach the arithmetic, not only the member list: a quorum
    // computed against a stale size is the one number that makes every other rule
    // unsafe.
    LeaderFixture fix;

    // Three members: this leader plus one acknowledgement is a quorum of two.
    REQUIRE(fix.node.ProposeMembership({ "n1", "n2" }, At(200)).has_value());
    CHECK(fix.node.ActiveMembers().size() == 2);

    auto const last = fix.node.Log().LastIndex();
    (void) fix.node.Receive(
        AppendEntriesResponse {
            .term = Term { .value = 1 }, .result = AppendResult::Accepted, .matchIndex = last, .followerId = "n2" },
        At(201));

    // n2 alone is now a quorum with the leader, so the entry committed on one
    // acknowledgement rather than needing n3 -- which is no longer a member.
    CHECK(fix.node.CommitIndex() == last);
}
