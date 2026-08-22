// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftNode.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

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

/// Unwrap an optional for assertion, yielding a default-constructed value when
/// empty.
///
/// The same device LeaseTable_test and CompileCacheHandler_test use, for the same
/// reason: clang-tidy's optional analysis cannot see a `has_value()` guard through
/// Catch2's REQUIRE macro, so a direct `*x` after one reads as an unchecked
/// access. Going through `value_or` is provably safe, and the preceding REQUIRE
/// still fails the test first when the optional is empty — so the default is
/// never actually observed.
template <typename T>
[[nodiscard]] T Unwrap(std::optional<T> const& value)
{
    return value.value_or(T {});
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
struct Fixture
{
    ScriptedRandomSource random { { 0 } };
    RaftNode node = MakeNode(ThreeNodes(), random);

    /// Drive this node to leadership of term 1 by granting it a quorum.
    /// @return The output produced by the winning vote.
    RaftOutput ElectAsLeader()
    {
        (void) node.Tick(At(ElectionMin.count()));
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

TEST_CASE("An election timeout makes a follower stand for election", "[consensus][raft][election]")
{
    Fixture fix;

    auto const output = fix.node.Tick(At(ElectionMin.count()));

    CHECK(fix.node.CurrentRole() == Role::Candidate);
    CHECK(fix.node.CurrentTerm() == Term { .value = 1 });
    CHECK(fix.node.VotedFor() == std::optional<NodeId> { "n1" });

    // The vote for itself must be durable before the request goes out, or a
    // crash-restart votes again in the same term.
    REQUIRE(output.persist.has_value());
    CHECK(Unwrap(output.persist).currentTerm == Term { .value = 1 });
    CHECK(Unwrap(output.persist).votedFor == std::optional<NodeId> { "n1" });

    auto const requests = MessagesOfType<RequestVoteRequest>(output);
    REQUIRE(requests.size() == 2); // both peers, never itself
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
    CHECK(heartbeats[0].entries.empty());
    CHECK(heartbeats[0].leaderId == "n1");
}

TEST_CASE("A retransmitted vote response is not counted twice", "[consensus][raft][election]")
{
    // Two counted votes from one node is a quorum that does not exist, which is
    // why the tally is a set rather than a counter.
    ScriptedRandomSource random { { 0 } };
    auto config = ThreeNodes();
    config.members = { "n1", "n2", "n3", "n4", "n5" }; // quorum 3
    RaftNode node = MakeNode(config, random);

    (void) node.Tick(At(ElectionMin.count()));
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
    (void) fix.node.Tick(At(ElectionMin.count())); // now a candidate at term 1

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
    (void) fix.node.Tick(At(ElectionMin.count()));
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
    (void) fix.node.Tick(At(ElectionMin.count()));
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
