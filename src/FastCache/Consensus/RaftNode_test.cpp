// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

constexpr auto ElectionMin = 150ms;

/// The instant every test starts at.
constexpr auto Start = TimePoint {};

/// `Start` plus a number of milliseconds.
/// @param milliseconds How far past the start.
/// @return The instant.
[[nodiscard]] TimePoint At(std::int64_t milliseconds)
{
    return Start + std::chrono::milliseconds { milliseconds };
}

/// A three-node configuration seen from `self`.
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

/// Build a node, failing the test if the configuration is refused.
///
/// `.value()` rather than a `has_value()` check plus a dereference: it throws on
/// a refusal, which Catch2 reports with the error, and it leaves no unchecked
/// access for the static analyzer to object to.
/// @param config The cluster configuration.
/// @param random Source for election-timeout jitter.
/// @param now When the node starts.
/// @return The constructed node.
[[nodiscard]] RaftNode MakeNode(RaftConfig config, IRandomSource& random, TimePoint now = Start)
{
    return std::move(RaftNode::Create(std::move(config), random, now)).value();
}

/// Every message in `output` of one concrete type.
/// @param output The output to scan.
/// @return The messages, in order.
template <typename Message>
[[nodiscard]] std::vector<Message> MessagesOfType(RaftOutput const& output)
{
    auto found = std::vector<Message> {};
    for (auto const& outbound: output.messages)
        if (auto const* const concrete = std::get_if<Message>(&outbound.message))
            found.push_back(*concrete);

    return found;
}

/// A node driven by a scripted random source that always draws the low bound, so
/// every election timeout is exactly `ElectionMin` and the tests are arithmetic
/// rather than guesswork.
/// Drive `node` through a pre-vote round so it becomes a candidate.
///
/// An election timeout no longer starts an election -- it starts a pre-vote
/// round, and the real election follows only once a quorum answers that one is
/// winnable. Cases about what a *candidate* does go through here so the extra
/// round has one home instead of being spelled at each of them.
/// Grants come from every id any fixture here uses, rather than from a count the
/// helper would have to be told: a cluster of five needs three and a cluster of
/// three needs two, and once the round is carried the node is a candidate, so
/// every later grant is ignored by the role check. A grant from an id outside
/// the configuration is ignored too, which is what makes one list serve both.
/// @param node The node to drive.
/// @param at When the timeout falls due.
void DriveToCandidate(RaftNode& node, TimePoint at)
{
    auto const standingFor = node.CurrentTerm().Next();
    (void) node.Tick(at);
    for (auto const* const voter: { "n2", "n3", "n4", "n5" })
        (void) node.Receive(PreVoteResponse { .term = standingFor, .decision = VoteDecision::Granted, .voterId = voter },
                            at);
}

struct Fixture
{
    ScriptedRandomSource random { { 0 } };
    RaftNode node = MakeNode(ThreeNodes(), random);

    /// Drive this node from follower to candidate of term 1.
    ///
    /// Two steps rather than one, because an election timeout no longer starts an
    /// election: it starts a pre-vote round, and the real election follows only
    /// once a quorum says it is winnable. Every case that wants a candidate goes
    /// through here so the extra round has one home.
    /// @return The output produced by the pre-vote that carried the round.
    RaftOutput StandForElection()
    {
        (void) node.Tick(At(ElectionMin.count()));
        return node.Receive(
            PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
            At(ElectionMin.count()));
    }

    /// Drive this node to leadership of term 1 by granting it a quorum.
    /// @return The output produced by the winning vote.
    RaftOutput ElectAsLeader()
    {
        (void) StandForElection();
        return node.Receive(
            RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
            At(ElectionMin.count()));
    }
};

} // namespace

TEST_CASE("A fresh node is a follower that has not voted", "[consensus][raft][election]")
{
    Fixture fix;

    CHECK(fix.node.CurrentRole() == Role::Follower);
    CHECK(fix.node.CurrentTerm() == Term::None());
    CHECK_FALSE(fix.node.VotedFor().has_value());
    CHECK_FALSE(fix.node.KnownLeader().has_value());
    CHECK(fix.node.NextDeadline() == At(ElectionMin.count()));
}

TEST_CASE("Ticking before the deadline does nothing", "[consensus][raft][election]")
{
    // Every real reactor wakes spuriously, so an early tick has to be free rather
    // than merely harmless.
    Fixture fix;

    auto const output = fix.node.Tick(At(ElectionMin.count() - 1));

    CHECK(output.messages.empty());
    CHECK_FALSE(output.persist.has_value());
    CHECK(fix.node.CurrentRole() == Role::Follower);
}

TEST_CASE("An election timeout starts a pre-vote round, not an election", "[consensus][raft][prevote]")
{
    // The disruption this prevents: a node partitioned away times out, increments
    // its term, times out again, and returns carrying a term far above everyone
    // else's -- at which point §5.1 obliges a healthy leader to step down and the
    // cluster runs an election it did not need, exactly when the partition heals.
    Fixture fix;

    auto const output = fix.node.Tick(At(ElectionMin.count()));

    CHECK(fix.node.CurrentRole() == Role::PreCandidate);

    // Nothing moved and nothing was written. A round that changed durable state
    // would cost a disk flush per election timeout on every node that cannot
    // reach a leader -- which is every node, during the partition this exists to
    // make cheap.
    CHECK(fix.node.CurrentTerm() == Term::None());
    CHECK_FALSE(fix.node.VotedFor().has_value());
    CHECK_FALSE(output.persist.has_value());

    // No real vote was solicited.
    CHECK(MessagesOfType<RequestVoteRequest>(output).empty());

    auto const asked = MessagesOfType<PreVoteRequest>(output);
    REQUIRE(asked.size() == 2); // both peers, never itself

    // The term asked about is one ABOVE this node's own: the question is "would
    // you support me if I stood", and that is the term it would stand in.
    CHECK(asked[0].term == Term { .value = 1 });
    CHECK(asked[0].candidateId == "n1");
}

TEST_CASE("A quorum of pre-votes starts the real election", "[consensus][raft][prevote]")
{
    Fixture fix;
    (void) fix.node.Tick(At(ElectionMin.count()));
    REQUIRE(fix.node.CurrentRole() == Role::PreCandidate);

    auto const output =
        fix.node.Receive(PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                         At(ElectionMin.count()));

    CHECK(fix.node.CurrentRole() == Role::Candidate);
    CHECK(fix.node.CurrentTerm() == Term { .value = 1 });
    CHECK(fix.node.VotedFor() == std::optional<NodeId> { "n1" });

    // Only NOW is anything durable: the vote for itself must reach stable storage
    // before the request goes out, or a crash-restart votes again in the same
    // term.
    REQUIRE(output.persist.has_value());
    CHECK(Unwrap(output.persist).currentTerm == Term { .value = 1 });
    CHECK(Unwrap(output.persist).votedFor == std::optional<NodeId> { "n1" });

    auto const requests = MessagesOfType<RequestVoteRequest>(output);
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].term == Term { .value = 1 });
    CHECK(requests[0].candidateId == "n1");
}

TEST_CASE("A single-node cluster elects itself immediately", "[consensus][raft][election]")
{
    ScriptedRandomSource random { { 0 } };
    auto config = ThreeNodes();
    config.members = { "n1" };
    RaftNode node = MakeNode(config, random);

    auto const output = node.Tick(At(ElectionMin.count()));

    CHECK(node.CurrentRole() == Role::Leader);
    CHECK(node.KnownLeader() == std::optional<NodeId> { "n1" });
    CHECK(MessagesOfType<RequestVoteRequest>(output).empty());
}

TEST_CASE("A quorum of votes wins the election", "[consensus][raft][election]")
{
    Fixture fix;
    auto const output = fix.ElectAsLeader();

    CHECK(fix.node.CurrentRole() == Role::Leader);
    CHECK(fix.node.KnownLeader() == std::optional<NodeId> { "n1" });

    // Heartbeats go out immediately rather than at the next interval: until peers
    // hear from the new leader they are still counting down to their own
    // elections.
    auto const heartbeats = MessagesOfType<AppendEntriesRequest>(output);
    REQUIRE(heartbeats.size() == 2);
    CHECK(heartbeats[0].leaderId == "n1");

    // Not empty: a new leader writes one no-op of its own term, and that entry is
    // what later releases any earlier-term entries for commitment.
    REQUIRE(heartbeats[0].entries.size() == 1);
    CHECK(heartbeats[0].entries[0].kind == EntryKind::NoOp);
    CHECK(heartbeats[0].entries[0].term == Term { .value = 1 });
}

TEST_CASE("A retransmitted vote response is not counted twice", "[consensus][raft][election]")
{
    // Two counted votes from one node is a quorum that does not exist, which is
    // why the tally is a set rather than a counter.
    ScriptedRandomSource random { { 0 } };
    auto config = ThreeNodes();
    config.members = { "n1", "n2", "n3", "n4", "n5" }; // quorum 3
    RaftNode node = MakeNode(config, random);

    DriveToCandidate(node, At(ElectionMin.count()));
    auto const granted =
        RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" };

    (void) node.Receive(granted, At(200));
    (void) node.Receive(granted, At(201));

    // Two distinct votes (itself and n2) is not the three this cluster needs.
    CHECK(node.CurrentRole() == Role::Candidate);

    (void) node.Receive(
        RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n3" }, At(202));
    CHECK(node.CurrentRole() == Role::Leader);
}

TEST_CASE("A node grants at most one vote per term", "[consensus][raft][election]")
{
    Fixture fix;

    auto const first = fix.node.Receive(RequestVoteRequest { .term = Term { .value = 5 },
                                                             .candidateId = "n2",
                                                             .lastLogIndex = LogIndex::BeforeFirst(),
                                                             .lastLogTerm = Term::None() },
                                        At(10));
    REQUIRE(MessagesOfType<RequestVoteResponse>(first).at(0).decision == VoteDecision::Granted);

    auto const second = fix.node.Receive(RequestVoteRequest { .term = Term { .value = 5 },
                                                              .candidateId = "n3",
                                                              .lastLogIndex = LogIndex::BeforeFirst(),
                                                              .lastLogTerm = Term::None() },
                                         At(11));
    CHECK(MessagesOfType<RequestVoteResponse>(second).at(0).decision == VoteDecision::Denied);
    CHECK(fix.node.VotedFor() == std::optional<NodeId> { "n2" });
}

TEST_CASE("Re-asking the same candidate gets the same answer", "[consensus][raft][election]")
{
    // Idempotent rather than lax: a retransmission after a lost response must not
    // be refused, or an election nobody had reason to lose stalls until timeout.
    Fixture fix;
    auto const request = RequestVoteRequest { .term = Term { .value = 5 },
                                              .candidateId = "n2",
                                              .lastLogIndex = LogIndex::BeforeFirst(),
                                              .lastLogTerm = Term::None() };

    (void) fix.node.Receive(request, At(10));
    auto const again = fix.node.Receive(request, At(11));

    CHECK(MessagesOfType<RequestVoteResponse>(again).at(0).decision == VoteDecision::Granted);
}

TEST_CASE("A candidate from an older term is refused", "[consensus][raft][election]")
{
    Fixture fix;
    DriveToCandidate(fix.node, At(ElectionMin.count())); // now a candidate at term 1

    auto const output = fix.node.Receive(RequestVoteRequest { .term = Term::None(),
                                                              .candidateId = "n2",
                                                              .lastLogIndex = LogIndex::BeforeFirst(),
                                                              .lastLogTerm = Term::None() },
                                         At(200));

    auto const responses = MessagesOfType<RequestVoteResponse>(output);
    REQUIRE(responses.size() == 1);
    CHECK(responses[0].decision == VoteDecision::Denied);
    // The refusal carries this node's term, which is how the stale candidate
    // learns it has already lost.
    CHECK(responses[0].term == Term { .value = 1 });
}

TEST_CASE("A vote is refused to a candidate whose log is behind", "[consensus][raft][election]")
{
    // Raft §5.4.1, and what Leader Completeness rests on: a candidate missing a
    // committed entry must not be electable.
    Fixture fix;
    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = { LogEntry { .term = Term { .value = 2 }, .payload = {} } },
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(10));
    REQUIRE(fix.node.Log().LastIndex() == LogIndex { .value = 1 });

    auto const output = fix.node.Receive(RequestVoteRequest { .term = Term { .value = 3 },
                                                              .candidateId = "n3",
                                                              .lastLogIndex = LogIndex::BeforeFirst(),
                                                              .lastLogTerm = Term::None() },
                                         At(20));

    CHECK(MessagesOfType<RequestVoteResponse>(output).at(0).decision == VoteDecision::Denied);
    // It still adopted the higher term -- refusing the vote and ignoring the term
    // are different things.
    CHECK(fix.node.CurrentTerm() == Term { .value = 3 });
}

TEST_CASE("A higher term on a request demotes this node", "[consensus][raft][election]")
{
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 9 },
                                                   .leaderId = "n3",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = {},
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(300));

    CHECK(fix.node.CurrentRole() == Role::Follower);
    CHECK(fix.node.CurrentTerm() == Term { .value = 9 });
    CHECK(fix.node.KnownLeader() == std::optional<NodeId> { "n3" });
}

TEST_CASE("A higher term on a RESPONSE also demotes this node", "[consensus][raft][election]")
{
    // The easily-forgotten half of §5.1. A leader that applies the term rule only
    // to requests keeps believing it leads a term the cluster has moved past, and
    // goes on answering clients as though it did.
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    auto const output = fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 7 },
                                                                 .result = AppendResult::Rejected,
                                                                 .matchIndex = LogIndex::BeforeFirst(),
                                                                 .followerId = "n2" },
                                         At(300));

    CHECK(fix.node.CurrentRole() == Role::Follower);
    CHECK(fix.node.CurrentTerm() == Term { .value = 7 });
    CHECK_FALSE(fix.node.KnownLeader().has_value());
    REQUIRE(output.persist.has_value());
    CHECK(Unwrap(output.persist).currentTerm == Term { .value = 7 });
}

TEST_CASE("A candidate steps down when a leader of its own term appears", "[consensus][raft][election]")
{
    // Without this a split vote is resolved and then immediately re-run.
    Fixture fix;
    DriveToCandidate(fix.node, At(ElectionMin.count()));
    REQUIRE(fix.node.CurrentRole() == Role::Candidate);
    REQUIRE(fix.node.CurrentTerm() == Term { .value = 1 });

    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = {},
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(200));

    CHECK(fix.node.CurrentRole() == Role::Follower);
    CHECK(fix.node.KnownLeader() == std::optional<NodeId> { "n2" });
}

TEST_CASE("A stale AppendEntries neither demotes nor delays this node", "[consensus][raft][election]")
{
    Fixture fix;
    fix.ElectAsLeader();

    auto const output = fix.node.Receive(AppendEntriesRequest { .term = Term::None(),
                                                                .leaderId = "n3",
                                                                .prevLogIndex = LogIndex::BeforeFirst(),
                                                                .prevLogTerm = Term::None(),
                                                                .entries = {},
                                                                .leaderCommit = LogIndex::BeforeFirst() },
                                         At(300));

    CHECK(fix.node.CurrentRole() == Role::Leader);
    auto const responses = MessagesOfType<AppendEntriesResponse>(output);
    REQUIRE(responses.size() == 1);
    CHECK(responses[0].result == AppendResult::Rejected);
    CHECK(responses[0].term == Term { .value = 1 });
}

TEST_CASE("A refused vote does not reset the election timer", "[consensus][raft][election]")
{
    // Otherwise a node with a stale log could delay every healthy node's election
    // indefinitely just by asking repeatedly.
    Fixture fix;
    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = { LogEntry { .term = Term { .value = 2 }, .payload = {} } },
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(10));
    auto const deadlineAfterLeader = fix.node.NextDeadline();

    // A candidate with an empty log asks, and is refused on §5.4.1 grounds.
    (void) fix.node.Receive(RequestVoteRequest { .term = Term { .value = 3 },
                                                 .candidateId = "n3",
                                                 .lastLogIndex = LogIndex::BeforeFirst(),
                                                 .lastLogTerm = Term::None() },
                            At(20));

    // Unchanged. §5.2 resets the timer on exactly two events -- valid
    // AppendEntries from a current leader, and *granting* a vote -- and adopting
    // the higher term that came with this request is neither.
    //
    // This assertion is the point of the case. An earlier draft re-armed the
    // timer inside StepDown, which made the deadline move to At(20) here and the
    // test happily documented that as expected. It is not: a partitioned node
    // with a stale log would then push every healthy node's deadline out once per
    // election timeout, forever, while being refused each time -- a cluster that
    // never finishes an election and reports itself healthy from every node.
    CHECK(fix.node.NextDeadline() == deadlineAfterLeader);
    CHECK(deadlineAfterLeader == At(10 + ElectionMin.count()));
    // The term is still adopted -- refusing the vote and ignoring the term are
    // different things.
    CHECK(fix.node.CurrentTerm() == Term { .value = 3 });
}

TEST_CASE("A deposed leader re-arms the election timer it was not running", "[consensus][raft][election]")
{
    // The exception to the rule above, and why "never re-arm on step-down" is not
    // the fix. A leader runs no election timer, so its deadline is whatever was
    // left over from before it was elected -- already in the past. Left alone, a
    // deposed leader times out immediately and campaigns at a higher term, turning
    // one disruption into two.
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 7 },
                                                    .result = AppendResult::Rejected,
                                                    .matchIndex = LogIndex::BeforeFirst(),
                                                    .followerId = "n2" },
                            At(500));

    REQUIRE(fix.node.CurrentRole() == Role::Follower);
    CHECK(fix.node.NextDeadline() == At(500 + ElectionMin.count()));
}

TEST_CASE("A vote response from outside the configuration is ignored", "[consensus][raft][election]")
{
    // The voter names itself and the tally decides leadership, so an unconfigured
    // id must not reach it: here one such response plus this node's own vote would
    // otherwise be a quorum of the three-node cluster.
    Fixture fix;
    DriveToCandidate(fix.node, At(ElectionMin.count()));
    REQUIRE(fix.node.CurrentRole() == Role::Candidate);

    (void) fix.node.Receive(
        RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "stranger" },
        At(200));

    CHECK(fix.node.CurrentRole() == Role::Candidate);
}

TEST_CASE("A non-member cannot become this node's leader", "[consensus][raft][election]")
{
    // Accepting would publish it as the leader clients are redirected to, and
    // re-arm the election timer -- so anything able to reach the port could hold
    // the cluster in follower state and point its clients at a machine the
    // configuration does not contain.
    Fixture fix;

    auto const output = fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 3 },
                                                                .leaderId = "stranger",
                                                                .prevLogIndex = LogIndex::BeforeFirst(),
                                                                .prevLogTerm = Term::None(),
                                                                .entries = {},
                                                                .leaderCommit = LogIndex::BeforeFirst() },
                                         At(20));

    CHECK_FALSE(fix.node.KnownLeader().has_value());
    CHECK(MessagesOfType<AppendEntriesResponse>(output).at(0).result == AppendResult::Rejected);
    CHECK(fix.node.NextDeadline() == At(ElectionMin.count()));
}

TEST_CASE("A non-member is not voted for", "[consensus][raft][election]")
{
    Fixture fix;

    auto const output = fix.node.Receive(RequestVoteRequest { .term = Term { .value = 3 },
                                                              .candidateId = "stranger",
                                                              .lastLogIndex = LogIndex::BeforeFirst(),
                                                              .lastLogTerm = Term::None() },
                                         At(20));

    CHECK(MessagesOfType<RequestVoteResponse>(output).at(0).decision == VoteDecision::Denied);
    CHECK_FALSE(fix.node.VotedFor().has_value());
}

TEST_CASE("An unrunnable configuration is refused rather than constructed", "[consensus][raft][election]")
{
    // The factory is what makes RaftConfig::Validate unbypassable. With `self`
    // outside `members`, Peers() returns every member while Quorum() still assumes
    // this node is one of them, so the node would count a self-vote it is not
    // entitled to -- silent, not loud.
    ScriptedRandomSource random { { 0 } };
    auto config = ThreeNodes();
    config.self = "outsider";

    auto const created = RaftNode::Create(config, random, Start);

    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == ConsensusErrorCode::InvalidConfiguration);
}

TEST_CASE("A leader heartbeats on its interval", "[consensus][raft][election]")
{
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.NextDeadline() == At(ElectionMin.count() + 50));

    auto const early = fix.node.Tick(At(ElectionMin.count() + 49));
    CHECK(early.messages.empty());

    auto const due = fix.node.Tick(At(ElectionMin.count() + 50));
    CHECK(MessagesOfType<AppendEntriesRequest>(due).size() == 2);
    CHECK(fix.node.NextDeadline() == At(ElectionMin.count() + 100));
}

TEST_CASE("Election Safety: one term cannot produce two leaders", "[consensus][raft][election]")
{
    // Asserted through the mechanism that guarantees it rather than by inspection:
    // two candidates in one term cannot both gather a quorum, because any two
    // majorities of five share a member and that member votes once.
    auto members = std::vector<NodeId> { "n1", "n2", "n3", "n4", "n5" };

    ScriptedRandomSource voterRandom { { 0 } };
    auto voterConfig = ThreeNodes("n3");
    voterConfig.members = members;
    RaftNode voter = MakeNode(voterConfig, voterRandom);

    auto const ask = [](NodeId candidate) {
        return RequestVoteRequest { .term = Term { .value = 4 },
                                    .candidateId = std::move(candidate),
                                    .lastLogIndex = LogIndex::BeforeFirst(),
                                    .lastLogTerm = Term::None() };
    };

    auto const toFirst = voter.Receive(ask("n1"), At(10));
    auto const toSecond = voter.Receive(ask("n2"), At(11));

    CHECK(MessagesOfType<RequestVoteResponse>(toFirst).at(0).decision == VoteDecision::Granted);
    CHECK(MessagesOfType<RequestVoteResponse>(toSecond).at(0).decision == VoteDecision::Denied);
}

TEST_CASE("The election timeout is drawn fresh every time", "[consensus][raft][election]")
{
    // A fixed per-node timeout makes the shortest-timeout node win every election
    // forever -- and if that node is partitioned but alive, its repeated
    // candidacies disrupt the cluster on a fixed schedule.
    // The two draws must yield *different* timeouts, or the test cannot tell a
    // fresh draw from a fixed one: 0 clamps to the 150ms low bound, Highest() to
    // the 300ms high bound.
    ScriptedRandomSource random { { 0, ScriptedRandomSource::Highest() } };
    RaftNode node = MakeNode(ThreeNodes(), random);

    REQUIRE(node.NextDeadline() == At(150));
    (void) node.Tick(At(150));

    // Re-armed at t=150 with a second, longer draw.
    CHECK(node.NextDeadline() == At(150 + 300));
    CHECK(random.DrawCount() == 2);
}

TEST_CASE("Role traits cover every role exactly once", "[consensus][raft][election]")
{
    // The table is what a Learner row will be added to, so a missing or duplicated
    // row should fail here rather than as a mis-armed timer somewhere else.
    CHECK(TraitsOf(Role::Follower).timer == TimerKind::Election);
    CHECK(TraitsOf(Role::Candidate).timer == TimerKind::Election);
    CHECK(TraitsOf(Role::Leader).timer == TimerKind::Heartbeat);
    CHECK(TraitsOf(Role::Leader).name == "leader");

    for (auto const& row: RoleTable)
        CHECK(TraitsOf(row.role).role == row.role);
}

// --------------------------------------------------------------------------
// Log replication and commitment (Raft §5.3, §5.4.2).

TEST_CASE("Only a leader accepts a proposal", "[consensus][raft][replication]")
{
    Fixture fix;

    auto const refused = fix.node.Propose(FastCache::BytesFromString("x"), At(10));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::NotLeader);
    // Nobody leads yet, which is a different answer from "ask that node instead"
    // and the one that means give up rather than chase it.
    CHECK_FALSE(refused.error().knownLeader.has_value());

    fix.ElectAsLeader();
    auto const accepted = fix.node.Propose(FastCache::BytesFromString("x"), At(200));
    REQUIRE(accepted.has_value());
    // Index 2, because becoming leader wrote a no-op at index 1.
    CHECK(accepted->index == LogIndex { .value = 2 });
}

TEST_CASE("A follower redirects a proposal to the leader it knows", "[consensus][raft][replication]")
{
    Fixture fix;
    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = {},
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(10));

    auto const refused = fix.node.Propose(FastCache::BytesFromString("x"), At(20));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().knownLeader == std::optional<std::string> { "n2" });
}

TEST_CASE("A proposal is replicated to every peer and persisted first", "[consensus][raft][replication]")
{
    Fixture fix;
    fix.ElectAsLeader();

    auto const proposed = fix.node.Propose(FastCache::BytesFromString("hello"), At(200));
    REQUIRE(proposed.has_value());

    // The log is durable state: a leader that sends an entry and then loses it on
    // restart can be asked about it by a follower that kept it. It is persistLog
    // that carries this -- `persist` holds only term and vote, so asserting on it
    // would pass while proving nothing about the entry.
    REQUIRE(proposed->output.persistLog.has_value());
    auto const logged = Unwrap(proposed->output.persistLog);
    CHECK(logged.fromIndex == LogIndex { .value = 2 });
    REQUIRE(logged.entries.size() == 1);
    CHECK(FastCache::AsStringView(logged.entries[0].payload) == "hello");

    auto const sent = MessagesOfType<AppendEntriesRequest>(proposed->output);
    REQUIRE(sent.size() == 2);
    // Both entries: this leader was elected with an empty log, so each peer's
    // nextIndex is still 1 and the no-op has not been acknowledged yet.
    REQUIRE(sent[0].entries.size() == 2);
    CHECK(sent[0].prevLogIndex == LogIndex::BeforeFirst());
    CHECK(sent[0].entries[0].kind == EntryKind::NoOp);
    CHECK(sent[0].entries[1].kind == EntryKind::Command);
}

TEST_CASE("An entry commits once a quorum has it", "[consensus][raft][replication]")
{
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.Propose(FastCache::BytesFromString("v1"), At(200)).has_value());
    REQUIRE(fix.node.CommitIndex() == LogIndex::BeforeFirst());

    // Three-node cluster: the leader plus one follower is a quorum. Index 1 is
    // the leader's no-op and index 2 is "v1".
    auto const output = fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                                 .result = AppendResult::Accepted,
                                                                 .matchIndex = LogIndex { .value = 2 },
                                                                 .followerId = "n2" },
                                         At(210));

    CHECK(fix.node.CommitIndex() == LogIndex { .value = 2 });
    // One applied entry, not two: the no-op is committed like any other and never
    // delivered, so the indices seen here skip it.
    REQUIRE(output.applied.size() == 1);
    CHECK(output.applied[0].index == LogIndex { .value = 2 });
    CHECK(FastCache::AsStringView(output.applied[0].payload) == "v1");
}

TEST_CASE("A committed entry is applied exactly once", "[consensus][raft][replication]")
{
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.Propose(FastCache::BytesFromString("v1"), At(200)).has_value());

    auto const accepted = AppendEntriesResponse { .term = Term { .value = 1 },
                                                  .result = AppendResult::Accepted,
                                                  .matchIndex = LogIndex { .value = 2 },
                                                  .followerId = "n2" };

    auto const first = fix.node.Receive(accepted, At(210));
    auto const again = fix.node.Receive(accepted, At(211));

    CHECK(first.applied.size() == 1);
    // The driver applies blindly, so a second emission would be a second
    // application of the same command.
    CHECK(again.applied.empty());
}

TEST_CASE("A single-node cluster commits its own proposal immediately", "[consensus][raft][replication]")
{
    ScriptedRandomSource random { { 0 } };
    auto config = ThreeNodes();
    config.members = { "n1" };
    RaftNode node = MakeNode(config, random);
    DriveToCandidate(node, At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);

    auto const proposed = node.Propose(FastCache::BytesFromString("solo"), At(200));

    REQUIRE(proposed.has_value());
    // Index 1 is the no-op this node wrote on election; "solo" is index 2.
    CHECK(node.CommitIndex() == LogIndex { .value = 2 });
    REQUIRE(proposed->output.applied.size() == 1);
    CHECK(FastCache::AsStringView(proposed->output.applied[0].payload) == "solo");
}

TEST_CASE("A rejection walks nextIndex back and retries at once", "[consensus][raft][replication]")
{
    // Each round trip recovers exactly one index, so retrying at the next
    // heartbeat instead would make catching up cost interval-times-divergence.
    //
    // The log has to exist *before* this node is elected. `nextIndex` is a guess
    // made at election time from the leader's own last index and moved only by
    // responses -- appending entries afterwards does not advance it -- so a
    // leader elected with an empty log sits at nextIndex 1 and a rejection has
    // nowhere further back to walk.
    Fixture fix;
    (void) fix.node.Receive(
        AppendEntriesRequest {
            .term = Term { .value = 1 },
            .leaderId = "n2",
            .prevLogIndex = LogIndex::BeforeFirst(),
            .prevLogTerm = Term::None(),
            .entries = { LogEntry { .term = Term { .value = 1 }, .payload = FastCache::BytesFromString("a") },
                         LogEntry { .term = Term { .value = 1 }, .payload = FastCache::BytesFromString("b") } },
            .leaderCommit = LogIndex::BeforeFirst() },
        At(10));
    REQUIRE(fix.node.Log().LastIndex() == LogIndex { .value = 2 });

    DriveToCandidate(fix.node, At(200));
    (void) fix.node.Receive(
        RequestVoteResponse { .term = Term { .value = 2 }, .decision = VoteDecision::Granted, .voterId = "n2" }, At(201));
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    auto const output = fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 2 },
                                                                 .result = AppendResult::Rejected,
                                                                 .matchIndex = LogIndex::BeforeFirst(),
                                                                 .followerId = "n2" },
                                         At(210));

    auto const retries = MessagesOfType<AppendEntriesRequest>(output);
    REQUIRE(retries.size() == 1);
    // nextIndex went from 3 to 2, so the retry starts one entry further back and
    // carries "b" plus the no-op this node wrote at index 3 on election.
    CHECK(retries[0].prevLogIndex == LogIndex { .value = 1 });
    CHECK(retries[0].entries.size() == 2);
    CHECK(retries[0].entries[1].kind == EntryKind::NoOp);
}

TEST_CASE("An out-of-order response never moves a match index backwards", "[consensus][raft][replication]")
{
    // Match indices decide commitment, so un-acknowledging a confirmed entry can
    // only end with a committed entry treated as uncommitted.
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.Propose(FastCache::BytesFromString("a"), At(200)).has_value());
    REQUIRE(fix.node.Propose(FastCache::BytesFromString("b"), At(201)).has_value());

    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                    .result = AppendResult::Accepted,
                                                    .matchIndex = LogIndex { .value = 2 },
                                                    .followerId = "n2" },
                            At(210));
    REQUIRE(fix.node.CommitIndex() == LogIndex { .value = 2 });

    // A straggler from an earlier request arrives late.
    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                    .result = AppendResult::Accepted,
                                                    .matchIndex = LogIndex { .value = 1 },
                                                    .followerId = "n2" },
                            At(211));

    CHECK(fix.node.CommitIndex() == LogIndex { .value = 2 });
}

TEST_CASE("A follower's commit index is bounded by what the request carried", "[consensus][raft][replication]")
{
    // The leader may have committed entries this follower has not received --
    // AppendEntries can be delayed or truncated -- and adopting its number
    // outright would mark absent entries committed, so the next thing applied
    // would be whatever happened to sit at that index.
    Fixture fix;

    auto const output =
        fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                                .leaderId = "n2",
                                                .prevLogIndex = LogIndex::BeforeFirst(),
                                                .prevLogTerm = Term::None(),
                                                .entries = { LogEntry { .term = Term { .value = 2 },
                                                                        .payload = FastCache::BytesFromString("a") } },
                                                .leaderCommit = LogIndex { .value = 9 } },
                         At(10));

    CHECK(fix.node.CommitIndex() == LogIndex { .value = 1 });
    REQUIRE(output.applied.size() == 1);
    CHECK(FastCache::AsStringView(output.applied[0].payload) == "a");
}

TEST_CASE("Figure 8: an earlier term's entry is not committed by replica count", "[consensus][raft][replication]")
{
    // Raft §5.4.2, and the defect the paper devotes a figure to. An entry from an
    // earlier term can sit on a majority and still be overwritten, because a
    // future leader elected under §5.4.1 may lack it -- widely replicated is not
    // the same as safe. Counting replicas for such an entry loses committed data;
    // it must instead commit indirectly, carried by a current-term entry above it.
    Fixture fix;

    // Take an entry from term 2 as a follower, so this node's log holds one.
    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = { LogEntry { .term = Term { .value = 2 },
                                                                           .payload = FastCache::BytesFromString("old") } },
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(10));
    REQUIRE(fix.node.Log().LastIndex() == LogIndex { .value = 1 });
    REQUIRE(fix.node.CommitIndex() == LogIndex::BeforeFirst());

    // Now win term 3 and hear that a follower holds that term-2 entry.
    DriveToCandidate(fix.node, At(200));
    REQUIRE(fix.node.CurrentTerm() == Term { .value = 3 });
    (void) fix.node.Receive(
        RequestVoteResponse { .term = Term { .value = 3 }, .decision = VoteDecision::Granted, .voterId = "n2" }, At(201));
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 3 },
                                                    .result = AppendResult::Accepted,
                                                    .matchIndex = LogIndex { .value = 1 },
                                                    .followerId = "n2" },
                            At(210));

    // A quorum holds index 1, but it is from term 2 and this leader is on term 3.
    // Index 2 is this leader's own no-op, which the follower has not confirmed.
    CHECK(fix.node.CommitIndex() == LogIndex::BeforeFirst());

    // Replicating a term-3 entry commits it AND carries the term-2 entry with it.
    // "new" lands at index 3, behind the no-op at index 2.
    REQUIRE(fix.node.Propose(FastCache::BytesFromString("new"), At(220)).has_value());
    auto const output = fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 3 },
                                                                 .result = AppendResult::Accepted,
                                                                 .matchIndex = LogIndex { .value = 3 },
                                                                 .followerId = "n2" },
                                         At(230));

    CHECK(fix.node.CommitIndex() == LogIndex { .value = 3 });
    // Two delivered, not three: the no-op at index 2 committed with them and was
    // never handed to the application.
    REQUIRE(output.applied.size() == 2);
    CHECK(output.applied[0].index == LogIndex { .value = 1 });
    CHECK(FastCache::AsStringView(output.applied[0].payload) == "old");
    CHECK(output.applied[1].index == LogIndex { .value = 3 });
    CHECK(FastCache::AsStringView(output.applied[1].payload) == "new");
}

TEST_CASE("Losing leadership does not un-commit anything", "[consensus][raft][replication]")
{
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.Propose(FastCache::BytesFromString("v1"), At(200)).has_value());
    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                    .result = AppendResult::Accepted,
                                                    .matchIndex = LogIndex { .value = 1 },
                                                    .followerId = "n2" },
                            At(210));
    REQUIRE(fix.node.CommitIndex() == LogIndex { .value = 1 });

    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 5 },
                                                   .leaderId = "n3",
                                                   .prevLogIndex = LogIndex { .value = 1 },
                                                   .prevLogTerm = Term { .value = 1 },
                                                   .entries = {},
                                                   .leaderCommit = LogIndex { .value = 1 } },
                            At(300));

    REQUIRE(fix.node.CurrentRole() == Role::Follower);
    // Commitment is a cluster-wide fact; no change of leadership can un-decide it.
    CHECK(fix.node.CommitIndex() == LogIndex { .value = 1 });
}

TEST_CASE("A follower's commit index never moves backwards", "[consensus][raft][replication]")
{
    // Guarding only on `leaderCommit > _commitIndex` is not enough, because the
    // value assigned is the *minimum* of that and what the request established. A
    // delayed duplicate carrying a high leaderCommit and a low match index would
    // take the commit index backwards, and CommitIndex() promises an entry at or
    // below it is never taken back.
    Fixture fix;

    auto const twoEntries = std::vector {
        LogEntry { .term = Term { .value = 1 }, .kind = EntryKind::Command, .payload = FastCache::BytesFromString("a") },
        LogEntry { .term = Term { .value = 1 }, .kind = EntryKind::Command, .payload = FastCache::BytesFromString("b") }
    };

    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = twoEntries,
                                                   .leaderCommit = LogIndex { .value = 2 } },
                            At(10));
    REQUIRE(fix.node.CommitIndex() == LogIndex { .value = 2 });

    // A stale duplicate of "entry 1 only", arriving late with a high leaderCommit.
    // TryAppend accepts it (the entry is already present at the same term) and
    // reports a match index of 1, which is below what is already committed.
    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = { twoEntries[0] },
                                                   .leaderCommit = LogIndex { .value = 9 } },
                            At(11));

    CHECK(fix.node.CommitIndex() == LogIndex { .value = 2 });
}

TEST_CASE("A follower records its appended entries as durable work", "[consensus][raft][replication]")
{
    // These entries are about to be acknowledged and a leader may commit on that
    // acknowledgement, so losing them to a restart loses a committed entry.
    Fixture fix;

    auto const output =
        fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                                .leaderId = "n2",
                                                .prevLogIndex = LogIndex::BeforeFirst(),
                                                .prevLogTerm = Term::None(),
                                                .entries = { LogEntry { .term = Term { .value = 1 },
                                                                        .kind = EntryKind::Command,
                                                                        .payload = FastCache::BytesFromString("a") } },
                                                .leaderCommit = LogIndex::BeforeFirst() },
                         At(10));

    REQUIRE(output.persistLog.has_value());
    auto const logged = Unwrap(output.persistLog);
    CHECK(logged.fromIndex == LogIndex { .value = 1 });
    REQUIRE(logged.entries.size() == 1);
    CHECK(FastCache::AsStringView(logged.entries[0].payload) == "a");
}

TEST_CASE("A heartbeat asks for no durable log work", "[consensus][raft][replication]")
{
    // An fsync per heartbeat would put a disk flush on the interval that decides
    // how fast a dead leader is noticed.
    Fixture fix;

    auto const output = fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                                                .leaderId = "n2",
                                                                .prevLogIndex = LogIndex::BeforeFirst(),
                                                                .prevLogTerm = Term::None(),
                                                                .entries = {},
                                                                .leaderCommit = LogIndex::BeforeFirst() },
                                         At(10));

    CHECK_FALSE(output.persistLog.has_value());
}

TEST_CASE("A match index beyond the leader's own log is clamped", "[consensus][raft][replication]")
{
    // Taken at face value it pushes nextIndex past the end, every later
    // AppendEntries then names a prevLogIndex that does not exist, and that peer
    // never converges again.
    Fixture fix;
    fix.ElectAsLeader();
    REQUIRE(fix.node.Log().LastIndex() == LogIndex { .value = 1 });

    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                    .result = AppendResult::Accepted,
                                                    .matchIndex = LogIndex { .value = 99 },
                                                    .followerId = "n2" },
                            At(210));

    // Clamped to the leader's own last index, so the no-op commits and no phantom
    // index is committed above it.
    CHECK(fix.node.CommitIndex() == LogIndex { .value = 1 });

    // And the next request to that peer still names a position that exists.
    auto const next = fix.node.Tick(At(400));
    auto const sent = MessagesOfType<AppendEntriesRequest>(next);
    REQUIRE_FALSE(sent.empty());
    CHECK(sent[0].prevLogIndex <= fix.node.Log().LastIndex());
}

TEST_CASE("A new leader commits a previous term's entries without a client", "[consensus][raft][replication]")
{
    // The companion to the §5.4.2 guard. Without a no-op of the leader's own
    // term, a leader whose log ends in fully replicated entries from the previous
    // term can never commit them -- and applies nothing -- until a client happens
    // to propose. With no client traffic the cluster is live and permanently one
    // term behind.
    Fixture fix;

    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = { LogEntry { .term = Term { .value = 1 },
                                                                           .kind = EntryKind::Command,
                                                                           .payload = FastCache::BytesFromString("old") } },
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(10));
    REQUIRE(fix.node.CommitIndex() == LogIndex::BeforeFirst());

    DriveToCandidate(fix.node, At(200));
    (void) fix.node.Receive(
        RequestVoteResponse { .term = Term { .value = 2 }, .decision = VoteDecision::Granted, .voterId = "n2" }, At(201));
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    // A quorum confirms the no-op at index 2. No client has proposed anything.
    auto const output = fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 2 },
                                                                 .result = AppendResult::Accepted,
                                                                 .matchIndex = LogIndex { .value = 2 },
                                                                 .followerId = "n2" },
                                         At(210));

    CHECK(fix.node.CommitIndex() == LogIndex { .value = 2 });
    REQUIRE(output.applied.size() == 1);
    CHECK(FastCache::AsStringView(output.applied[0].payload) == "old");
}

TEST_CASE("A no-op is committed but never delivered", "[consensus][raft][replication]")
{
    // Tagged rather than inferred from an empty payload, because an empty payload
    // is a legitimate thing for an application to commit.
    Fixture fix;
    fix.ElectAsLeader();

    auto const output = fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                                 .result = AppendResult::Accepted,
                                                                 .matchIndex = LogIndex { .value = 1 },
                                                                 .followerId = "n2" },
                                         At(210));

    CHECK(fix.node.CommitIndex() == LogIndex { .value = 1 });
    CHECK(output.applied.empty());
}

TEST_CASE("A pre-vote does not disturb the node that answers it", "[consensus][raft][prevote]")
{
    // The whole mechanism in one assertion. If answering cost a term change, a
    // vote, or a disk write, the pre-vote round would be the disruption it
    // exists to prevent, wearing a different name.
    Fixture fix;

    auto const output = fix.node.Receive(PreVoteRequest { .term = Term { .value = 99 },
                                                          .candidateId = "n2",
                                                          .lastLogIndex = LogIndex::BeforeFirst(),
                                                          .lastLogTerm = Term::None() },
                                         At(ElectionMin.count()));

    CHECK(fix.node.CurrentTerm() == Term::None());
    CHECK_FALSE(fix.node.VotedFor().has_value());
    CHECK(fix.node.CurrentRole() == Role::Follower);
    CHECK_FALSE(output.persist.has_value());

    // It still answers -- silence would be indistinguishable from being
    // unreachable, and the asker would wait out its timeout for nothing.
    auto const replies = MessagesOfType<PreVoteResponse>(output);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Granted);
}

TEST_CASE("A pre-vote is refused while a leader is being heard from", "[consensus][raft][prevote]")
{
    // The condition that does the work. A cluster with a healthy leader refuses
    // every pre-vote, so a node returning from a partition gets no quorum, never
    // increments its term, and cannot depose anybody.
    Fixture fix;

    // Hearing from a leader re-arms the election timer, which is where
    // "recently" already lives.
    (void) fix.node.Receive(AppendEntriesRequest { .term = Term { .value = 4 },
                                                   .leaderId = "n2",
                                                   .prevLogIndex = LogIndex::BeforeFirst(),
                                                   .prevLogTerm = Term::None(),
                                                   .entries = {},
                                                   .leaderCommit = LogIndex::BeforeFirst() },
                            At(0));

    auto const output = fix.node.Receive(PreVoteRequest { .term = Term { .value = 5 },
                                                          .candidateId = "n3",
                                                          .lastLogIndex = LogIndex::BeforeFirst(),
                                                          .lastLogTerm = Term::None() },
                                         At(1));

    auto const replies = MessagesOfType<PreVoteResponse>(output);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);
}

TEST_CASE("A granted pre-vote does not raise the asker's term", "[consensus][raft][prevote]")
{
    // A grant echoes the term that was ASKED about, which is one above the
    // sender's own. Left to the §5.1 rule it would demote the very node it
    // encourages, and no pre-vote round could ever succeed.
    Fixture fix;
    (void) fix.node.Tick(At(ElectionMin.count()));
    REQUIRE(fix.node.CurrentRole() == Role::PreCandidate);
    REQUIRE(fix.node.CurrentTerm() == Term::None());

    (void) fix.node.Receive(
        PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
        At(ElectionMin.count()));

    // It became a candidate by winning the round, not by being demoted by it.
    CHECK(fix.node.CurrentRole() == Role::Candidate);
    CHECK(fix.node.CurrentTerm() == Term { .value = 1 });
}

TEST_CASE("A refused pre-vote from a later term steps the asker down", "[consensus][raft][prevote]")
{
    // The other half of the exemption. A refusal carries the VOTER's term, and
    // learning that this node is behind is exactly when stepping down is right.
    Fixture fix;
    (void) fix.node.Tick(At(ElectionMin.count()));
    REQUIRE(fix.node.CurrentRole() == Role::PreCandidate);

    (void) fix.node.Receive(
        PreVoteResponse { .term = Term { .value = 9 }, .decision = VoteDecision::Denied, .voterId = "n2" },
        At(ElectionMin.count()));

    CHECK(fix.node.CurrentRole() == Role::Follower);
    CHECK(fix.node.CurrentTerm() == Term { .value = 9 });
}

TEST_CASE("A partitioned node cannot inflate the term it returns with", "[consensus][raft][prevote]")
{
    // The defect the round exists to close, end to end. Before pre-vote, each
    // timeout here incremented the term; a node away for twenty timeouts came
    // back twenty terms ahead and deposed a leader that was working fine.
    Fixture fix;

    for (auto round = 0; round < 20; ++round)
        (void) fix.node.Tick(At(ElectionMin.count() * (round + 1) * 4));

    // Twenty timeouts, no answers, and the term has not moved once.
    CHECK(fix.node.CurrentTerm() == Term::None());
    CHECK(fix.node.CurrentRole() == Role::PreCandidate);
    CHECK_FALSE(fix.node.VotedFor().has_value());
}

TEST_CASE("A pre-vote from outside the configuration is refused", "[consensus][raft][prevote]")
{
    // Same reasoning as a real vote: a machine outside the configuration must
    // not be able to make this node believe an election is warranted.
    Fixture fix;

    auto const output = fix.node.Receive(PreVoteRequest { .term = Term { .value = 1 },
                                                          .candidateId = "stranger",
                                                          .lastLogIndex = LogIndex::BeforeFirst(),
                                                          .lastLogTerm = Term::None() },
                                         At(ElectionMin.count()));

    auto const replies = MessagesOfType<PreVoteResponse>(output);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);
}

TEST_CASE("A pre-vote from a node with a stale log is refused", "[consensus][raft][prevote]")
{
    // §5.4.1 applies to the question as much as to the vote: encouraging a node
    // that could not win wastes an election, and encouraging one that could win
    // but should not is how Leader Completeness is lost.
    Fixture fix;
    (void) fix.ElectAsLeader();
    auto const proposal = fix.node.Propose(FastCache::BytesFromString("x"), At(200));
    REQUIRE(proposal.has_value());

    auto const output = fix.node.Receive(PreVoteRequest { .term = Term { .value = 9 },
                                                          .candidateId = "n2",
                                                          .lastLogIndex = LogIndex::BeforeFirst(),
                                                          .lastLogTerm = Term::None() },
                                         At(10'000));

    auto const replies = MessagesOfType<PreVoteResponse>(output);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);
}

TEST_CASE("Standing for election does not make a node refuse its peers' pre-votes", "[consensus][raft][prevote]")
{
    // Pre-vote asks "is there a live leader?", and a node that has just begun its
    // own campaign must still answer honestly. Keying that on `_electionDeadline`
    // — which starting a campaign re-arms — makes it answer "yes, I heard from a
    // leader" for a full timeout, so peers timing out at the same instant refuse
    // each other. Nothing fails; elections just take some forty rounds instead of
    // one, which reads as a livelock in a cluster test and as nothing at all in a
    // unit test that only ever has one candidate.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();

    // Time out and start a pre-vote round of its own, which re-arms the deadline.
    auto const own = node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::PreCandidate);
    REQUIRE_FALSE(MessagesOfType<PreVoteRequest>(own).empty());

    // A peer that timed out at the same moment now asks. This node has heard from
    // no leader at all, so it must grant.
    auto const answer = node.Receive(PreVoteRequest { .term = node.CurrentTerm().Next(),
                                                      .candidateId = "n3",
                                                      .lastLogIndex = LogIndex::BeforeFirst(),
                                                      .lastLogTerm = Term::None() },
                                     At(ElectionMin.count()));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Granted);
}

TEST_CASE("A node that has just heard from a leader refuses a pre-vote", "[consensus][raft][prevote]")
{
    // The other half, and the reason the check exists at all: a partitioned node
    // rejoining must not be able to depose a healthy leader. Losing this while
    // fixing the case above would trade a slow election for an unstable cluster.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();

    (void) node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                               .leaderId = "n1",
                                               .prevLogIndex = LogIndex::BeforeFirst(),
                                               .prevLogTerm = Term::None(),
                                               .entries = {},
                                               .leaderCommit = LogIndex::BeforeFirst() },
                        At(10));

    auto const answer = node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                      .candidateId = "n3",
                                                      .lastLogIndex = LogIndex::BeforeFirst(),
                                                      .lastLogTerm = Term::None() },
                                     At(20));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);

    // And once the leader has been silent for a full minimum timeout, it grants —
    // otherwise a dead leader could never be replaced.
    auto const later = node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                     .candidateId = "n3",
                                                     .lastLogIndex = LogIndex::BeforeFirst(),
                                                     .lastLogTerm = Term::None() },
                                    At(10 + ElectionMin.count()));

    auto const granted = MessagesOfType<PreVoteResponse>(later);
    REQUIRE(granted.size() == 1);
    CHECK(granted[0].decision == VoteDecision::Granted);
}

TEST_CASE("A leader that still hears from a quorum refuses a pre-vote", "[consensus][raft][prevote]")
{
    // The hole the follower-side check leaves. `_lastLeaderContact` is set only
    // where a leader spoke to this node, and a leader never hears from a leader --
    // so it ages out on the one node best placed to refuse, and the node whose own
    // quorum proves the cluster is healthy was the only one that granted.
    //
    // A leader answers from that quorum instead: CheckQuorum, decided from the
    // responses it already receives rather than from a clock it must trust.
    Fixture fix;
    (void) fix.ElectAsLeader();
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    // Deliberately far enough past the election that the contact `BecomeLeader`
    // seeded from the votes has aged out: what refuses below is this response and
    // nothing left over from winning.
    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                    .result = AppendResult::Accepted,
                                                    .matchIndex = LogIndex { .value = 1 },
                                                    .followerId = "n2" },
                            At(400));

    // n3 lost contact with the leader alone -- an asymmetric partition, a
    // saturated link, a paused process -- and campaigns. Its log is current, so
    // the liveness question is the only thing left that can refuse.
    auto const answer = fix.node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                          .candidateId = "n3",
                                                          .lastLogIndex = LogIndex { .value = 1 },
                                                          .lastLogTerm = Term { .value = 1 } },
                                         At(500));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);
}

TEST_CASE("A leader that has lost its quorum grants a pre-vote", "[consensus][raft][prevote]")
{
    // The other half, and it is not optional: a leader that refused without
    // tracking whether it still HAS a quorum is a partitioned leader blocking its
    // own replacement forever. Losing this while fixing the case above trades a
    // spurious election for a cluster that can never hold another one.
    Fixture fix;
    (void) fix.ElectAsLeader();
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    // Nobody has answered since the election, which was exactly a minimum timeout
    // ago -- so no peer is live and this node alone is not a majority of three.
    auto const answer = fix.node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                          .candidateId = "n3",
                                                          .lastLogIndex = LogIndex { .value = 1 },
                                                          .lastLogTerm = Term { .value = 1 } },
                                         At(ElectionMin.count() * 2));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Granted);
}

TEST_CASE("A leader counts a rejected AppendEntries response as contact", "[consensus][raft][prevote]")
{
    // A rejection says the follower's log disagrees, not that the follower is
    // gone -- it answered. Hooking the record onto `AdvanceFollowerProgress`,
    // which only the accepted branch reaches, is the obvious spelling and would
    // make a leader repairing a divergent follower believe it had lost the very
    // quorum that is talking to it.
    Fixture fix;
    (void) fix.ElectAsLeader();

    (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                    .result = AppendResult::Rejected,
                                                    .matchIndex = LogIndex::BeforeFirst(),
                                                    .followerId = "n2" },
                            At(400));

    auto const answer = fix.node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                          .candidateId = "n3",
                                                          .lastLogIndex = LogIndex { .value = 1 },
                                                          .lastLogTerm = Term { .value = 1 } },
                                         At(500));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);
}

TEST_CASE("A leader counts an InstallSnapshot response as contact", "[consensus][raft][prevote]")
{
    // The second of the two ways a follower answers, and the arm a single-case
    // test would omit -- a follower far enough behind is caught up by snapshot and
    // says so on this message and no other, so a leader that counted only
    // AppendEntries would lose its quorum precisely while repairing it.
    Fixture fix;
    (void) fix.ElectAsLeader();

    (void) fix.node.Receive(InstallSnapshotResponse { .term = Term { .value = 1 },
                                                      .result = AppendResult::Accepted,
                                                      .matchIndex = LogIndex { .value = 1 },
                                                      .followerId = "n2" },
                            At(400));

    auto const answer = fix.node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                          .candidateId = "n3",
                                                          .lastLogIndex = LogIndex { .value = 1 },
                                                          .lastLogTerm = Term { .value = 1 } },
                                         At(500));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);
}

TEST_CASE("A leader hearing from less than a quorum grants a pre-vote", "[consensus][raft][prevote]")
{
    // What refuses is a majority, not "somebody answered". A leader reachable by
    // one node out of four is a leader the cluster has to be able to replace, and
    // counting any contact at all would let it veto that from inside a minority.
    ScriptedRandomSource random { { 0 } };
    auto config = ThreeNodes();
    config.members = { "n1", "n2", "n3", "n4", "n5" }; // quorum 3
    RaftNode node = MakeNode(config, random);

    DriveToCandidate(node, At(ElectionMin.count()));
    for (auto const* const voter: { "n2", "n3" })
        (void) node.Receive(
            RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = voter },
            At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);

    // One follower of four still answering, long enough after the election that
    // the votes that carried it no longer count: two live members against a
    // quorum of three.
    (void) node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                .result = AppendResult::Accepted,
                                                .matchIndex = LogIndex { .value = 1 },
                                                .followerId = "n2" },
                        At(400));

    auto const answer = node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                      .candidateId = "n3",
                                                      .lastLogIndex = LogIndex { .value = 1 },
                                                      .lastLogTerm = Term { .value = 1 } },
                                     At(500));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Granted);
}

TEST_CASE("A leader refuses a pre-vote before its first heartbeat is answered", "[consensus][raft][prevote]")
{
    // Winning an election IS contact from a quorum -- those votes were cast just
    // now -- so `BecomeLeader` seeds the record from them. Without that a new
    // leader answers "I have no quorum" until the first heartbeat comes back,
    // granting pre-votes for a round trip at the moment the cluster is least able
    // to afford another election.
    Fixture fix;
    (void) fix.ElectAsLeader();
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    auto const answer = fix.node.Receive(PreVoteRequest { .term = Term { .value = 2 },
                                                          .candidateId = "n3",
                                                          .lastLogIndex = LogIndex { .value = 1 },
                                                          .lastLogTerm = Term { .value = 1 } },
                                         At(ElectionMin.count() + 10));

    auto const replies = MessagesOfType<PreVoteResponse>(answer);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].decision == VoteDecision::Denied);
}
