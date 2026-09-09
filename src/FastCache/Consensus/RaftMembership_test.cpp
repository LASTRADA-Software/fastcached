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

/// A configuration naming only `self`: the machine that leads itself, which is
/// what every fleet is before its second member is admitted.
///
/// `ThreeNodes` with its member set narrowed rather than the same literals typed
/// again, so the timings cannot come to differ from every other case in this file
/// while a comment still claims they match.
/// @param self Which member this node is.
/// @return The configuration.
[[nodiscard]] RaftConfig OneNode(NodeId self = "n1")
{
    auto config = ThreeNodes(std::move(self));
    config.members = { config.self };
    return config;
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

TEST_CASE("The one member a change adds is named, and only when there is one", "[consensus][raft][membership]")
{
    // The companion to `Classify`, and its answer decides which peer a leader is
    // still waiting to hear from for the first time. Every shape but `AddedOne`
    // answers nothing, deliberately: crediting a peer that was not added would
    // count a member that had already gone quiet toward the quorum.
    std::vector<NodeId> const three { "n1", "n2", "n3" };

    CHECK(Membership::AddedMember(three, std::vector<NodeId> { "n1", "n2", "n3", "n4" }) == std::optional<NodeId> { "n4" });

    // Position carries no meaning, so the addition is found wherever it sits.
    CHECK(Membership::AddedMember(three, std::vector<NodeId> { "n0", "n3", "n1", "n2" }) == std::optional<NodeId> { "n0" });

    CHECK_FALSE(Membership::AddedMember(three, three).has_value());
    CHECK_FALSE(Membership::AddedMember(three, std::vector<NodeId> { "n1", "n2" }).has_value());
    CHECK_FALSE(Membership::AddedMember(three, std::vector<NodeId> { "n1", "n2", "n3", "n4", "n5" }).has_value());

    // A swap gains one and loses one, so it names no single addition even though
    // the arithmetic of "one new id" would find one.
    CHECK_FALSE(Membership::AddedMember(three, std::vector<NodeId> { "n1", "n2", "n9" }).has_value());

    // Growing from nothing is the shape a cluster of one has when it admits its
    // second member, which is the case the whole rule is about.
    CHECK(Membership::AddedMember(std::vector<NodeId> { "n1" }, std::vector<NodeId> { "n1", "n2" })
          == std::optional<NodeId> { "n2" });
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

TEST_CASE("A leader is not deposed by the member it has just admitted", "[consensus][raft][membership]")
{
    // CheckQuorum measures SILENCE, and silence is only measurable against
    // something that would otherwise have been said. Adopting a configuration
    // grows the quorum at the instant the entry is appended -- which is the rule
    // that makes the change committable at all -- so the member it adds is counted
    // before it has been asked anything. Read as silence, the leader is deposed at
    // its very next heartbeat for a lack of contact that could not have existed.
    //
    // The two-member shape is the one that shows it, because it is the one with no
    // recovery: the member just admitted holds no configuration of its own, so it
    // grants no votes, and the deposed leader needs its vote to reach a quorum of
    // two. Nothing campaigns again, in that term or any other.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(OneNode(), random, TimePoint {})).value();

    (void) node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);

    REQUIRE(node.ProposeMembership({ "n1", "n2" }, At(ElectionMin.count())).has_value());
    REQUIRE(node.ActiveMembers().size() == 2);

    // The next heartbeat falls due one interval later, and n2 has answered
    // nothing -- it cannot have, since the message admitting it went out with this
    // tick's predecessor and no reply has been delivered.
    (void) node.Tick(At(ElectionMin.count() + 50));

    CHECK(node.CurrentRole() == Role::Leader);
    CHECK(node.KnownLeader() == std::optional<NodeId> { "n1" });
}

TEST_CASE("A member that never answers still deposes the leader that admitted it", "[consensus][raft][membership]")
{
    // The guard against over-correcting the case above into "a leader that has
    // changed the configuration is never deposed". What a newly admitted member is
    // owed is the same window every other member's silence is measured over, and
    // not one instant more: past it, a leader whose quorum answers nothing must
    // give up, or `--cluster-admit` naming an address nothing listens on would pin
    // leadership on a node that can commit nothing.
    //
    // Not a control that passes either way, and saying so matters: neutered, this
    // case goes red at every assertion in it, because a leader deposed at its first
    // heartbeat is neither leading at 200 nor a plain follower at 350 -- it has
    // already armed a timer and begun campaigning. What it guards against is the
    // OVER-correction, and that is the last assertion alone: a grace that never
    // expired would leave this node leading a cluster of two that has answered
    // nothing, and only this case would notice.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(OneNode(), random, TimePoint {})).value();

    (void) node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);
    REQUIRE(node.ProposeMembership({ "n1", "n2" }, At(ElectionMin.count())).has_value());

    // Every heartbeat inside the window, so a fix that granted only the first is
    // not mistaken for one that grants the window.
    for (auto const beat: { 50, 100 })
    {
        (void) node.Tick(At(ElectionMin.count() + beat));
        CHECK(node.CurrentRole() == Role::Leader);
    }

    (void) node.Tick(At((2 * ElectionMin.count()) + 50));

    CHECK(node.CurrentRole() == Role::Follower);
    CHECK_FALSE(node.KnownLeader().has_value());

    // The term is untouched, which is what tells this from a step-down: nothing
    // higher arrived, so nothing is reported as having caused it.
    CHECK(node.CurrentTerm() == Term { .value = 1 });
}

TEST_CASE("An admitted member that answers keeps the leader that admitted it", "[consensus][raft][membership]")
{
    // The other direction, and the reason the window above is a grace rather than
    // a suspension: once the member answers -- with a REJECTION here, which is
    // what a joiner with an empty log actually sends -- the ordinary contact
    // record takes over and the leader keeps leading indefinitely.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(OneNode(), random, TimePoint {})).value();

    (void) node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);
    REQUIRE(node.ProposeMembership({ "n1", "n2" }, At(ElectionMin.count())).has_value());

    // How long after the proposal the admitted member's first answer arrives. It is
    // named because the assertion below is arithmetic on it: the two windows run
    // from the proposal and from this answer, so they close exactly this far apart
    // and that gap is the only place they can be told apart.
    constexpr auto AnsweredAfter = std::int64_t { 20 };

    (void) node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                .result = AppendResult::Rejected,
                                                .matchIndex = LogIndex::BeforeFirst(),
                                                .followerId = "n2" },
                        At(ElectionMin.count() + AnsweredAfter));

    (void) node.Tick(At(ElectionMin.count() + 50));
    CHECK(node.CurrentRole() == Role::Leader);

    // Inside that gap: past the admission's own window, which closes one election
    // timeout after the proposal, and inside the contact's, which closes one after
    // the answer. Derived rather than written out, so it goes on naming the gap if
    // `ElectionMin` ever moves instead of silently landing outside it. At this
    // instant only the ordinary contact record can be carrying the leader, so a fix
    // that granted a permanent exemption and one that hands over to contact are
    // told apart here and nowhere else.
    (void) node.Tick(At((2 * ElectionMin.count()) + (AnsweredAfter / 2)));

    CHECK(node.CurrentRole() == Role::Leader);
}

TEST_CASE("A leader elected by a bare quorum survives admitting a fourth member", "[consensus][raft][membership]")
{
    // The shape #1077 hit in the wild, and it is the one that says whether the
    // grace merely makes the defect rarer or closes it.
    //
    // A three-member leader is elected by a BARE quorum -- itself and one peer --
    // so `BecomeLeader` seeds contact from the votes actually cast and the third
    // member has none. That is not a degraded cluster; it is what every election
    // in an odd-sized cluster looks like until the first heartbeat comes back.
    // Admitting a fourth then takes the quorum from two to three while the
    // evidence stays at two, so the leader deposes itself -- and the member it was
    // admitting is stranded in no cluster at all, because the configuration entry
    // dies with the leadership that appended it.
    //
    // What the grace restores is exactly the tolerance the cluster had BEFORE the
    // admission: one live peer was enough for a quorum of two, and one live peer
    // plus the member just admitted is enough for a quorum of three. Admission
    // stops costing fault tolerance it was never meant to cost.
    LeaderFixture fix;
    REQUIRE(fix.node.CurrentRole() == Role::Leader);
    REQUIRE(fix.node.ActiveMembers().size() == 3);

    REQUIRE(fix.node.ProposeMembership({ "n1", "n2", "n3", "n4" }, At(200)).has_value());
    REQUIRE(fix.node.ActiveMembers().size() == 4);

    // The next heartbeat. n2's contact was stamped when it voted and is still
    // inside the window; n3 never answered this leadership at all, and n4 cannot
    // have. Under the defect that is two against a quorum of three.
    (void) fix.node.Tick(At(250));

    CHECK(fix.node.CurrentRole() == Role::Leader);
    CHECK(fix.node.KnownLeader() == std::optional<NodeId> { "n1" });
}

TEST_CASE("The retry after a lost leadership is not itself a second step-down", "[consensus][raft][membership]")
{
    // Why the fleet did not simply recover. `--cluster-admit` commits the member's
    // RECORD first, so every node holds it, and the reconciler on whichever node
    // leads next re-proposes the quorum change. Under the defect that retry is the
    // very thing that deposes the next leader -- so the cluster has a repair loop
    // whose every iteration re-triggers the fault, which is an election storm
    // rather than a recovery, and #1077 spent its whole 60 s budget inside one.
    //
    // Modelled here as the retry alone: a fresh leader of the next term, elected
    // by a bare quorum exactly as the last one was, proposing the same change.
    // Whether it survives is whether the loop converges.
    LeaderFixture fix;
    REQUIRE(fix.node.ProposeMembership({ "n1", "n2", "n3", "n4" }, At(200)).has_value());

    // It survives its own heartbeat, so the entry it appended has time to reach a
    // quorum -- which is the whole of what the admitted member is waiting for.
    (void) fix.node.Tick(At(250));
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    // And the peers acknowledge it, so the change commits and the member is in.
    auto const last = fix.node.Log().LastIndex();
    for (auto const* const peer: { "n2", "n3" })
        (void) fix.node.Receive(
            AppendEntriesResponse {
                .term = Term { .value = 1 }, .result = AppendResult::Accepted, .matchIndex = last, .followerId = peer },
            At(260));

    CHECK(fix.node.CommitIndex() == last);
    CHECK(fix.node.CurrentRole() == Role::Leader);
}
