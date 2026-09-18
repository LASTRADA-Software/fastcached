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

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <variant>
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

/// A configuration of voters and no learners -- every configuration this file used
/// before learners existed, spelled once.
/// @param voters The voters.
/// @return The configuration.
[[nodiscard]] Configuration Voters(std::vector<NodeId> voters)
{
    return Configuration { .voters = std::move(voters), .learners = {} };
}

/// A configuration of voters and learners.
/// @param voters The voters.
/// @param learners The learners.
/// @return The configuration.
[[nodiscard]] Configuration WithLearners(std::vector<NodeId> voters, std::vector<NodeId> learners)
{
    return Configuration { .voters = std::move(voters), .learners = std::move(learners) };
}

/// A three-node configuration for `self`.
/// @param self Which member this node is.
/// @return The configuration.
[[nodiscard]] RaftConfig ThreeNodes(NodeId self = "n1")
{
    return RaftConfig { .self = std::move(self),
                        .voters = { "n1", "n2", "n3" },
                        .learners = {},
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
    config.voters = { config.self };
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
            Acknowledge(peer, at);
    }

    /// Acknowledge everything this leader holds, from one member.
    /// @param who The member answering.
    /// @param at When.
    void Acknowledge(NodeId const& who, std::int64_t at)
    {
        (void) node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                    .result = AppendResult::Accepted,
                                                    .matchIndex = node.Log().LastIndex(),
                                                    .followerId = who },
                            At(at));
    }
};

/// A leader of term 1 over three voters that has admitted `n4` as a LEARNER, with
/// that change committed.
///
/// The starting point every promotion and demotion case needs, built through the
/// same proposal a real leader makes, so none of them rests on a configuration no
/// leader could have reached.
struct LearnerFixture: LeaderFixture
{
    LearnerFixture()
    {
        REQUIRE(node.ProposeMembership(WithLearners({ "n1", "n2", "n3" }, { "n4" }), At(200)).has_value());
        AcknowledgeAll(201);
        REQUIRE(node.CommitIndex() == node.Log().LastIndex());
    }
};

} // namespace

TEST_CASE("A configuration round-trips through an entry payload", "[consensus][raft][membership]")
{
    // A length-prefixed field per member, so an id may hold any byte and there is
    // no separator to escape -- and the two sets as two nested lists, so a member
    // keeps the set it was proposed in (#1449).
    auto const configuration = WithLearners({ "n1", "node-with-dash", "", "n4" }, { "laptop", "" });
    auto const decoded = Membership::Decode(Membership::Encode(configuration));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value_or(Configuration {}) == configuration);
}

TEST_CASE("An empty configuration round-trips as empty", "[consensus][raft][membership]")
{
    auto const decoded = Membership::Decode(Membership::Encode(Configuration {}));
    REQUIRE(decoded.has_value());
    CHECK(Membership::IsEmpty(decoded.value_or(Voters({ "x" }))));
}

TEST_CASE("A malformed configuration payload is refused", "[consensus][raft][membership]")
{
    auto encoded = Membership::Encode(Voters({ "n1", "n2" }));
    encoded.resize(encoded.size() - 1);
    CHECK_FALSE(Membership::Decode(encoded).has_value());
}

TEST_CASE("A configuration payload is exactly the bytes its format documents", "[consensus][raft][membership]")
{
    // The layout is versioned by the two containers that carry it -- a
    // `FileRaftStorage` record and a `RaftWire` frame -- and by nothing inside it
    // (see `RaftMembership.hpp`). So a change to these bytes is a change to BOTH of
    // those versions: `FileRaftStorage`'s `FormatVersion` and
    // `RaftWire::CurrentVersion`. A failure here is where to start.
    //
    // Written out as a literal rather than produced by the encoder, because a pin
    // that re-derived its bytes from the code under test would move with it.
    auto const encoded = Membership::Encode(WithLearners({ "v" }, { "ab" }));
    auto const expected = std::array {
        // Field 1: the voters, a nested list of one id.
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x05 }, // its length
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x01 }, // "v"'s length
        std::byte { 'v' },
        // Field 2: the learners, a nested list of one id.
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x06 }, // its length
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x02 }, // "ab"'s length
        std::byte { 'a' },
        std::byte { 'b' },
    };
    CHECK(encoded == std::vector<std::byte>(expected.begin(), expected.end()));
}

TEST_CASE("A change is classified by how many members move", "[consensus][raft][membership]")
{
    auto const three = Voters({ "n1", "n2", "n3" });

    CHECK(Membership::Classify(three, three) == Membership::ChangeShape::Unchanged);
    CHECK(Membership::Classify(three, Voters({ "n1", "n2", "n3", "n4" })) == Membership::ChangeShape::AddedOne);
    CHECK(Membership::Classify(three, Voters({ "n1", "n2" })) == Membership::ChangeShape::RemovedOne);

    // Two at once is the shape the single-server rule exists to refuse: {n1,n2}
    // is a majority of the old and {n3,n4,n5} a majority of the new, sharing
    // nobody -- so both could elect in the same term.
    CHECK(Membership::Classify(three, Voters({ "n1", "n2", "n3", "n4", "n5" })) == Membership::ChangeShape::Unsafe);

    // A swap is two changes even though the size does not move.
    CHECK(Membership::Classify(three, Voters({ "n1", "n2", "n9" })) == Membership::ChangeShape::Unsafe);

    // Order carries no meaning.
    CHECK(Membership::Classify(three, Voters({ "n3", "n1", "n2" })) == Membership::ChangeShape::Unchanged);
}

TEST_CASE("A learner moving is one change, and so is a promotion or a demotion", "[consensus][raft][membership][learner]")
{
    auto const three = Voters({ "n1", "n2", "n3" });
    auto const withLearner = WithLearners({ "n1", "n2", "n3" }, { "n4" });

    // Adding or removing a learner changes no quorum, and is one change.
    CHECK(Membership::Classify(three, withLearner) == Membership::ChangeShape::AddedOne);
    CHECK(Membership::Classify(withLearner, three) == Membership::ChangeShape::RemovedOne);

    // A promotion is a voter addition; a demotion is a voter removal.
    CHECK(Membership::Classify(withLearner, Voters({ "n1", "n2", "n3", "n4" })) == Membership::ChangeShape::PromotedOne);
    CHECK(Membership::Classify(three, WithLearners({ "n1", "n2" }, { "n3" })) == Membership::ChangeShape::DemotedOne);

    // Nothing moved when only the order did, in either set.
    CHECK(Membership::Classify(WithLearners({ "n1", "n2" }, { "n3", "n4" }), WithLearners({ "n2", "n1" }, { "n4", "n3" }))
          == Membership::ChangeShape::Unchanged);
}

TEST_CASE("Promoting one member and removing another in one proposal is Unsafe", "[consensus][raft][membership][learner]")
{
    // Two VOTER changes at once -- a voter gained and a voter lost -- which is the
    // swap the single-server rule refuses, arriving through the learner set. It is
    // not a technicality: {n2,n3} is a majority of the old voters {n1,n2,n3} and
    // {n1,n4} a majority of the new ones {n1,n2,n4}, and the two share nobody, so
    // both could elect in the same term.
    auto const withLearner = WithLearners({ "n1", "n2", "n3" }, { "n4" });
    CHECK(Membership::Classify(withLearner, Voters({ "n1", "n2", "n4" })) == Membership::ChangeShape::Unsafe);

    // And the other combinations of two, each refused for the same reason.
    CHECK(Membership::Classify(withLearner, WithLearners({ "n1", "n2", "n3", "n4" }, { "n5" }))
          == Membership::ChangeShape::Unsafe); // promote + add a learner
    CHECK(Membership::Classify(withLearner, WithLearners({ "n1", "n2", "n4" }, { "n3" }))
          == Membership::ChangeShape::Unsafe); // promote + demote
    CHECK(Membership::Classify(withLearner, WithLearners({ "n1", "n2" }, { "n3" }))
          == Membership::ChangeShape::Unsafe); // demote + remove a learner
}

TEST_CASE("A configuration that could not operate is refused", "[consensus][raft][membership]")
{
    CHECK_FALSE(Membership::Validate(Voters({})).has_value());
    CHECK_FALSE(Membership::Validate(Voters({ "n1", "" })).has_value());

    // A duplicate would make one node count twice toward a quorum, which is a
    // quorum that does not exist.
    CHECK_FALSE(Membership::Validate(Voters({ "n1", "n2", "n1" })).has_value());

    CHECK(Membership::Validate(Voters({ "n1", "n2", "n3" })).has_value());

    // The two sets are disjoint, nobody may lead a cluster of learners, and a
    // learner's id may not be empty either (#1449).
    CHECK_FALSE(Membership::Validate(WithLearners({ "n1", "n2" }, { "n2" })).has_value());
    CHECK_FALSE(Membership::Validate(WithLearners({}, { "n1" })).has_value());
    CHECK_FALSE(Membership::Validate(WithLearners({ "n1" }, { "" })).has_value());
    CHECK(Membership::Validate(WithLearners({ "n1" }, { "n2" })).has_value());
}

TEST_CASE("A leader adopts a new configuration before it commits", "[consensus][raft][membership]")
{
    // The rule that looks unsafe and is the opposite: a configuration that only
    // took effect once committed could not be used to REACH commitment, because
    // committing it needs a quorum of the very set it describes.
    LeaderFixture fix;
    REQUIRE(fix.node.ActiveConfiguration().voters.size() == 3);

    auto const proposed = fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4" }), At(200));
    REQUIRE(proposed.has_value());

    CHECK(fix.node.ActiveConfiguration().voters.size() == 4);

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

TEST_CASE("A change proposed while one is in flight is a MOMENT, not an invalid configuration",
          "[consensus][raft][membership]")
{
    LeaderFixture fix;

    // One accepted change, deliberately left uncommitted.
    auto const first = fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4" }), At(200));
    REQUIRE(first.has_value());

    auto const second = fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4", "n5" }), At(210));
    REQUIRE_FALSE(second.has_value());

    // The assertion that DISTINGUISHES, and the whole of #196: this used to answer
    // `InvalidConfiguration`, which `SubjectOf` classifies `Command` -- permanent,
    // reported at Warn as a record somebody must go and correct. It clears the
    // instant the first change commits.
    CHECK(second.error().code == ConsensusErrorCode::ConfigurationChangeInFlight);
    CHECK(second.error().code != ConsensusErrorCode::InvalidConfiguration);
    CHECK(SubjectOf(second.error().code) == RefusalSubject::Moment);

    // And it still says why, because a caller that gives up needs to know it may
    // ask again.
    CHECK(second.error().context.contains("already in flight"));
}

TEST_CASE("Proposing the member set already in force is SATISFIED, not a refusal to act on", "[consensus][raft][membership]")
{
    LeaderFixture fix;

    auto const unchanged = fix.node.ProposeMembership(Voters({ "n1", "n2", "n3" }), At(200));
    REQUIRE_FALSE(unchanged.has_value());

    // Neither of the other two answers is true of it. `Command` would report an
    // idempotent request as a record that can never be agreed; `Moment` would
    // abandon a reconcile pass that had nothing left to do. Naming the third state
    // is what #196 settled, and it is this repository's own four-states rule
    // arriving in a consensus taxonomy.
    CHECK(unchanged.error().code == ConsensusErrorCode::MembershipUnchanged);
    CHECK(unchanged.error().code != ConsensusErrorCode::InvalidConfiguration);
    CHECK(SubjectOf(unchanged.error().code) == RefusalSubject::Satisfied);
    CHECK(SubjectOf(unchanged.error().code) != RefusalSubject::Command);
    CHECK(SubjectOf(unchanged.error().code) != RefusalSubject::Moment);

    CHECK(unchanged.error().context.contains("current one"));

    // Nothing was appended, which is why it is not a success carrying an index.
    CHECK(fix.node.ActiveConfiguration().voters.size() == 3);
}

TEST_CASE("A two-member change is refused", "[consensus][raft][membership]")
{
    LeaderFixture fix;

    auto const refused = fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4", "n5" }), At(200));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::InvalidConfiguration);

    // The refusal says why, because "invalid configuration" tells an operator
    // nothing about what would have worked.
    CHECK(refused.error().context.contains("one member"));

    // And nothing moved.
    CHECK(fix.node.ActiveConfiguration().voters.size() == 3);
}

TEST_CASE("A second change is refused until the first commits", "[consensus][raft][membership]")
{
    // A change built on a configuration a truncation can still roll back would
    // have its safety argument made against a set that never existed.
    LeaderFixture fix;

    REQUIRE(fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4" }), At(200)).has_value());

    auto const second = fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4", "n5" }), At(201));
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().context.contains("already in flight"));

    // Once a quorum holds it, the next change is allowed.
    fix.AcknowledgeAll(202);
    REQUIRE(fix.node.CommitIndex() >= LogIndex { .value = 2 });
    CHECK(fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4", "n5" }), At(203)).has_value());
}

TEST_CASE("A non-leader cannot change the configuration", "[consensus][raft][membership]")
{
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes(), random, TimePoint {})).value();

    auto const refused = node.ProposeMembership(Voters({ "n1", "n2" }), At(10));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::NotLeader);
}

TEST_CASE("Proposing the current member set is refused", "[consensus][raft][membership]")
{
    LeaderFixture fix;
    auto const refused = fix.node.ProposeMembership(Voters({ "n3", "n2", "n1" }), At(200));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().context.contains("current one"));
}

TEST_CASE("A follower adopts a configuration it receives", "[consensus][raft][membership]")
{
    // Every node has to reach the same configuration through the log, or a
    // cluster becomes two clusters that each think they are one.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();
    REQUIRE(node.ActiveConfiguration().voters.size() == 3);

    (void) node.Receive(
        AppendEntriesRequest { .term = Term { .value = 1 },
                               .leaderId = "n1",
                               .prevLogIndex = LogIndex::BeforeFirst(),
                               .prevLogTerm = Term::None(),
                               .entries = { LogEntry { .term = Term { .value = 1 },
                                                       .kind = EntryKind::Configuration,
                                                       .payload = Membership::Encode(Voters({ "n1", "n2", "n3", "n4" })) } },
                               .leaderCommit = LogIndex::BeforeFirst() },
        At(10));

    CHECK(node.ActiveConfiguration().voters.size() == 4);
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
        AppendEntriesRequest { .term = Term { .value = 1 },
                               .leaderId = "n1",
                               .prevLogIndex = LogIndex::BeforeFirst(),
                               .prevLogTerm = Term::None(),
                               .entries = { LogEntry { .term = Term { .value = 1 },
                                                       .kind = EntryKind::Configuration,
                                                       .payload = Membership::Encode(Voters({ "n1", "n2", "n3", "n4" })) } },
                               .leaderCommit = LogIndex::BeforeFirst() },
        At(10));
    REQUIRE(node.ActiveConfiguration().voters.size() == 4);

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
    CHECK(node.ActiveConfiguration().voters.size() == 3);
}

TEST_CASE("A restarted node comes back under the configuration in its log", "[consensus][raft][membership]")
{
    // Otherwise a restart silently reverts a membership change the cluster made,
    // and the node counts quorums against a set the others have left behind.
    ScriptedRandomSource random { { 0 } };

    RecoveredState recovered;
    recovered.entries = { LogEntry { .term = Term { .value = 1 },
                                     .kind = EntryKind::Configuration,
                                     .payload = Membership::Encode(Voters({ "n1", "n2" })) } };

    auto node = std::move(RaftNode::Create(ThreeNodes(), random, TimePoint {}, std::move(recovered))).value();
    CHECK(node.ActiveConfiguration() == Voters({ "n1", "n2" }));
}

TEST_CASE("A leader removed from the configuration steps down once it commits", "[consensus][raft][membership]")
{
    // It cannot simply stop: the entry that removes it must be committed first,
    // and only this leader can commit it -- so it keeps leading a cluster it is
    // no longer part of for exactly as long as that takes.
    LeaderFixture fix;

    REQUIRE(fix.node.ProposeMembership(Voters({ "n2", "n3" }), At(200)).has_value());

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
    REQUIRE(fix.node.ProposeMembership(Voters({ "n1", "n2" }), At(200)).has_value());
    CHECK(fix.node.ActiveConfiguration().voters.size() == 2);

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
    //
    // SUBSUMED, and kept deliberately. Since #1095 this is a strictly weaker prefix
    // of `A leader whose joiner never answers keeps leading`, which drives the same
    // arrangement far past this instant -- so it is not independent coverage and
    // should not be counted as such. It is retained because it pins the FIRST
    // heartbeat, which is the window #1061 actually reported, and because it still
    // fails on a wholesale regression of the mechanism. An uncommented subset would
    // read as a separate property to the next person counting cases.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(OneNode(), random, TimePoint {})).value();

    (void) node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);

    REQUIRE(node.ProposeMembership(Voters({ "n1", "n2" }), At(ElectionMin.count())).has_value());
    REQUIRE(node.ActiveConfiguration().voters.size() == 2);

    // The next heartbeat falls due one interval later, and n2 has answered
    // nothing -- it cannot have, since the message admitting it went out with this
    // tick's predecessor and no reply has been delivered.
    (void) node.Tick(At(ElectionMin.count() + 50));

    CHECK(node.CurrentRole() == Role::Leader);
    CHECK(node.KnownLeader() == std::optional<NodeId> { "n1" });
}

TEST_CASE("A leader whose joiner never answers keeps leading, because deposing it elects nobody",
          "[consensus][raft][membership]")
{
    // The DISCRIMINATOR for #1095: red before the fix, green after, and it fails
    // before for exactly the reason the ticket exists.
    //
    // This case used to assert the opposite -- that the leader is deposed here --
    // and that assertion was the defect, not a guard against it. The reasoning it
    // rested on was that a leader which cannot get an answer inside one
    // `electionTimeoutMin` is one its followers are already timing out on. That
    // holds for an established follower and NOT for a joiner: its first exchange
    // carries the leader's log append, the joiner's term adoption and a `nextIndex`
    // walk-back over an empty log -- two fsyncs and a round trip that no
    // steady-state heartbeat pays. The window was sized against the wrong quantity.
    //
    // And deposing here buys NOTHING, which is the half that settles it. The new
    // configuration's quorum is two and the joiner is dead, so no successor can be
    // elected and the ex-leader cannot re-elect alone. Stepping down converts *a
    // leader that cannot commit* into *no leader that also cannot commit* -- the
    // same deadlock #1095 reports, reached by a different route.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(OneNode(), random, TimePoint {})).value();

    (void) node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);
    REQUIRE(node.ProposeMembership(Voters({ "n1", "n2" }), At(ElectionMin.count())).has_value());

    // Past where the old grace closed -- one `electionTimeoutMin` after the
    // proposal, so at `2 * ElectionMin`. Derived rather than written out: at this
    // instant the removed mechanism has certainly expired, so nothing but the
    // committed configuration can be carrying this leader.
    (void) node.Tick(At((2 * ElectionMin.count()) + 50));

    CHECK(node.CurrentRole() == Role::Leader);
    CHECK(node.KnownLeader() == std::optional<NodeId> { "n1" });

    // And it is not a one-heartbeat reprieve: the committed configuration is
    // `{n1}` with a quorum of one, so there is no constant here to outrun. A fix
    // that merely widened the old window would pass the assertion above and fail
    // this one.
    (void) node.Tick(At(10 * ElectionMin.count()));

    CHECK(node.CurrentRole() == Role::Leader);
}

TEST_CASE("Once the change commits, the NEW configuration governs and a silent member deposes the leader",
          "[consensus][raft][membership]")
{
    // The handover, and the guard against the fix becoming a permanent exemption.
    //
    // This replaces a case asserting that an admitted member which ANSWERS keeps
    // the leader that admitted it. Under the committed-configuration rule that
    // case had become vacuous: at 1->2 the committed set is `{n1}` with a quorum of
    // one, so the leader holds whether or not the joiner ever answers, and the
    // assertion could no longer fail. It asserted what both sides produce.
    //
    // What is still worth asserting is what the answer CAUSES. Once the joiner
    // acknowledges, the configuration entry commits, `HasUncommittedConfiguration`
    // goes false, and CheckQuorum hands back to the latest member set -- so from
    // that moment the joiner is an ordinary member whose silence deposes the leader
    // like anybody else's. A fix that kept consulting the committed configuration
    // forever would pass every other case in this file and fail this one.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(OneNode(), random, TimePoint {})).value();

    (void) node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);
    REQUIRE(node.ProposeMembership(Voters({ "n1", "n2" }), At(ElectionMin.count())).has_value());
    REQUIRE(node.ActiveConfiguration().voters.size() == 2);

    // The joiner accepts everything, which is what commits the configuration entry:
    // a quorum of the new set is two, and that is this node plus n2.
    constexpr auto AnsweredAfter = std::int64_t { 20 };
    (void) node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                .result = AppendResult::Accepted,
                                                .matchIndex = node.Log().LastIndex(),
                                                .followerId = "n2" },
                        At(ElectionMin.count() + AnsweredAfter));

    // Asserted rather than assumed: everything below is about what happens AFTER
    // the change commits, so a case in which it never committed would be testing a
    // different rule while still going green.
    REQUIRE(node.CommitIndex() == node.Log().LastIndex());

    // Still leading immediately after, on the new set: n2's contact is real and
    // fresh, so a quorum of two is met.
    (void) node.Tick(At(ElectionMin.count() + 50));
    CHECK(node.CurrentRole() == Role::Leader);

    // Now n2 says nothing further. One `electionTimeoutMin` after its ONLY answer
    // its contact goes stale, and the committed configuration is now `{n1, n2}` --
    // so the quorum is two, this node is one, and it must step down.
    (void) node.Tick(At(ElectionMin.count() + AnsweredAfter + ElectionMin.count() + 10));

    CHECK(node.CurrentRole() == Role::Follower);
    CHECK_FALSE(node.KnownLeader().has_value());

    // The term is untouched, which is what tells a relinquish from a step-down:
    // nothing higher arrived, so nothing is reported as having caused it.
    CHECK(node.CurrentTerm() == Term { .value = 1 });
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
    REQUIRE(fix.node.ActiveConfiguration().voters.size() == 3);

    REQUIRE(fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4" }), At(200)).has_value());
    REQUIRE(fix.node.ActiveConfiguration().voters.size() == 4);

    // The next heartbeat. n2's contact was stamped when it voted and is still
    // inside the window; n3 never answered this leadership at all, and n4 cannot
    // have. Under the defect that is two against a quorum of three.
    (void) fix.node.Tick(At(250));

    CHECK(fix.node.CurrentRole() == Role::Leader);
    CHECK(fix.node.KnownLeader() == std::optional<NodeId> { "n1" });
}

TEST_CASE("A leader that loses the COMMITTED quorum still steps down, mid-change", "[consensus][raft][membership]")
{
    // The over-correction guard, and it is deliberately NOT a discriminator: the
    // old code deposes here too, because its grace for the admitted member has
    // expired by this instant and the new quorum of three was never met. It passes
    // before and after, which is exactly what a guard of this kind does.
    //
    // Saying so matters, because the tempting description -- "a case the fix can
    // actually fail" -- is what this file's own rule refuses. What it guards is the
    // OVER-correction: consulting the committed configuration must not become
    // "a leader mid-change is never deposed". If `HasQuorumContact` were made
    // permissive while a change is in flight, every other case here would still
    // pass and only this one would go red. Shown, not claimed: returning `true`
    // early while `HasUncommittedConfiguration()` reddens this case alone.
    //
    // The arrangement: a three-member leader elected by a BARE quorum holds contact
    // for n2 only -- `BecomeLeader` seeds from the votes actually cast -- and n3 has
    // never answered this leadership. It admits n4, and then n2 goes quiet too. The
    // committed set is still `{n1, n2, n3}`, its quorum is two, and this leader can
    // now count only itself.
    LeaderFixture fix;
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    REQUIRE(fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4" }), At(200)).has_value());
    REQUIRE(fix.node.ActiveConfiguration().voters.size() == 4);

    // Inside n2's contact window, which was stamped when it voted at `ElectionMin`:
    // the committed quorum of two is met by this node and n2, so it leads.
    (void) fix.node.Tick(At(250));
    REQUIRE(fix.node.CurrentRole() == Role::Leader);

    // Past it. n2's vote was stamped at `ElectionMin` and its window closes one
    // `electionTimeoutMin` later, so from `2 * ElectionMin` this leader holds
    // contact with nobody but itself -- one against a committed quorum of two.
    (void) fix.node.Tick(At((2 * ElectionMin.count()) + 50));

    CHECK(fix.node.CurrentRole() == Role::Follower);
    CHECK_FALSE(fix.node.KnownLeader().has_value());
    CHECK(fix.node.CurrentTerm() == Term { .value = 1 });
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
    REQUIRE(fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4" }), At(200)).has_value());

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

TEST_CASE("A promotion and a demotion are each one change at a time", "[consensus][raft][membership][learner]")
{
    // A promotion is a voter addition and a demotion is a voter removal (#1449), so
    // each waits for the change before it to commit exactly as an addition does --
    // the safety argument is made against a configuration a truncation can no longer
    // roll back, whichever set the member moved between.
    LearnerFixture fix;
    REQUIRE(fix.node.ActiveConfiguration() == WithLearners({ "n1", "n2", "n3" }, { "n4" }));

    auto const promoted = fix.node.ProposeMembership(Voters({ "n1", "n2", "n3", "n4" }), At(300));
    REQUIRE(promoted.has_value());
    CHECK(fix.node.ActiveConfiguration() == Voters({ "n1", "n2", "n3", "n4" }));

    // The demotion that would undo it is refused while the promotion is in flight --
    // as a MOMENT, the same answer any second change gets.
    auto const early = fix.node.ProposeMembership(WithLearners({ "n1", "n2", "n3" }, { "n4" }), At(301));
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error().code == ConsensusErrorCode::ConfigurationChangeInFlight);

    // A promotion is a voter ADDITION: four voters now, so it takes three -- this node
    // and two more -- to commit it, and the promoted member is one of the four.
    fix.Acknowledge("n2", 302);
    CHECK(fix.node.CommitIndex() < promoted->index);
    fix.Acknowledge("n4", 303);
    REQUIRE(fix.node.CommitIndex() == promoted->index);

    // Once committed, the demotion is one change and is accepted.
    auto const demoted = fix.node.ProposeMembership(WithLearners({ "n1", "n2", "n3" }, { "n4" }), At(304));
    REQUIRE(demoted.has_value());
    CHECK(fix.node.ActiveConfiguration() == WithLearners({ "n1", "n2", "n3" }, { "n4" }));
}

TEST_CASE("A leader refuses to promote one member and remove another in one proposal",
          "[consensus][raft][membership][learner]")
{
    // The acceptance clause at the node, rather than at `Classify`: the leader is the
    // one place a configuration enters the log, so it is where the refusal has to be
    // seen to hold -- nothing appended, nothing adopted, and the reason named.
    LearnerFixture fix;
    auto const lastBefore = fix.node.Log().LastIndex();

    auto const refused = fix.node.ProposeMembership(Voters({ "n1", "n2", "n4" }), At(300));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::InvalidConfiguration);
    CHECK(SubjectOf(refused.error().code) == RefusalSubject::Command);
    CHECK(refused.error().context.contains("one member"));

    CHECK(fix.node.Log().LastIndex() == lastBefore);
    CHECK(fix.node.ActiveConfiguration() == WithLearners({ "n1", "n2", "n3" }, { "n4" }));
}

TEST_CASE("A learner is replicated to and its acknowledgement commits nothing", "[consensus][raft][membership][learner]")
{
    // Replication goes to voters AND learners; commitment counts voters alone. An
    // entry only a learner holds is not one the cluster can promise to keep, because
    // no future leader's quorum need include it.
    LearnerFixture fix;

    auto const proposed = fix.node.Propose(BytesFromString("x"), At(300));
    REQUIRE(proposed.has_value());

    // The learner is sent it, which is the half of #1449 a learner exists for.
    auto sentToLearner = false;
    for (auto const& message: proposed->output.messages)
        sentToLearner =
            sentToLearner || (message.to == "n4" && std::holds_alternative<AppendEntriesRequest>(message.message));
    CHECK(sentToLearner);

    // Its answer alone commits nothing...
    fix.Acknowledge("n4", 301);
    CHECK(fix.node.CommitIndex() < proposed->index);

    // ...where one voter's does: three voters are a quorum of two with this node.
    fix.Acknowledge("n2", 302);
    CHECK(fix.node.CommitIndex() == proposed->index);
}

TEST_CASE("A leader whose only other member is a learner leads through that learner's silence",
          "[consensus][raft][membership][learner]")
{
    // CheckQuorum at the node, exactly, which is where the rule lives: the cluster
    // case drives the same arrangement through a partition, and this pins it against a
    // clock nothing but the test moves. The committed configuration is one voter and
    // one learner, the learner never answers, and ten election timeouts later the
    // voter still leads -- because a learner's silence is not a lost quorum.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(OneNode(), random, TimePoint {})).value();

    (void) node.Tick(At(ElectionMin.count()));
    REQUIRE(node.CurrentRole() == Role::Leader);
    auto const term = node.CurrentTerm();

    // Adding a learner moves no quorum, so it commits on this node's own log alone,
    // with nobody having answered.
    auto const admitted = node.ProposeMembership(WithLearners({ "n1" }, { "n2" }), At(ElectionMin.count()));
    REQUIRE(admitted.has_value());
    CHECK(node.CommitIndex() == admitted->index);

    for (auto const multiple: std::views::iota(2, 12))
    {
        CAPTURE(multiple);
        (void) node.Tick(At(ElectionMin.count() * multiple));
        REQUIRE(node.CurrentRole() == Role::Leader);
        REQUIRE(node.CurrentTerm() == term);
    }
}

TEST_CASE("A leader demoted to a learner steps down once the demotion commits, and never stands again",
          "[consensus][raft][membership][learner]")
{
    // A learner may not lead, and the demotion is a voter REMOVAL whose commitment
    // only this leader can bring about -- so it leads until then, exactly as a leader
    // removing itself does, and follows afterwards under a standing that never stands.
    LeaderFixture fix;

    REQUIRE(fix.node.ProposeMembership(WithLearners({ "n2", "n3" }, { "n1" }), At(200)).has_value());
    CHECK(fix.node.CurrentRole() == Role::Leader);
    CHECK(fix.node.CurrentStanding() == Standing::Learner);

    // The remaining voters acknowledge, which commits it. This node's own log is not
    // counted: it is not a voter of the configuration deciding its commitment.
    fix.AcknowledgeAll(201);
    CHECK(fix.node.CurrentRole() == Role::Follower);

    // And it waits on nothing from now on, however long nobody speaks to it.
    CHECK(fix.node.NextDeadline() == TimePoint::max());
    auto const quiet = fix.node.Tick(At(100 * ElectionMin.count()));
    CHECK(quiet.messages.empty());
    CHECK(fix.node.CurrentRole() == Role::Follower);
}

TEST_CASE("A node removed from a one-voter configuration does not elect itself", "[consensus][raft][membership]")
{
    // A node counts its OWN vote only while it is a voter. Before that rule a node
    // removed from a two-member cluster reached a quorum of one -- its own pre-vote
    // and its own vote, against a configuration of one voter that is not itself --
    // led a cluster that does not contain it, and deposed the real leader with every
    // term it raised. Found while making every tally count voters only (#1449); it
    // would have been reachable by any 2 -> 1 removal.
    ScriptedRandomSource random { { 0 } };
    auto config = ThreeNodes("n2");
    config.voters = { "n1", "n2" };
    auto node = std::move(RaftNode::Create(std::move(config), random, TimePoint {})).value();

    // The leader, n1, replicates the entry removing n2.
    (void) node.Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                               .leaderId = "n1",
                                               .prevLogIndex = LogIndex::BeforeFirst(),
                                               .prevLogTerm = Term::None(),
                                               .entries = { LogEntry { .term = Term { .value = 1 },
                                                                       .kind = EntryKind::Configuration,
                                                                       .payload = Membership::Encode(Voters({ "n1" })) } },
                                               .leaderCommit = LogIndex { .value = 1 } },
                        At(10));
    REQUIRE(node.CurrentStanding() == Standing::Outsider);

    // Long after its election timer fell due. It may still ask -- nothing changed what
    // an outsider does -- but it cannot count itself, so it cannot win.
    (void) node.Tick(At(10 * ElectionMin.count()));
    (void) node.Tick(At(20 * ElectionMin.count()));

    CHECK(node.CurrentRole() != Role::Leader);
    CHECK(node.CurrentTerm() == Term { .value = 1 });
}

TEST_CASE("Where a node sits in a configuration has one author, and four answers", "[consensus][raft][membership][learner]")
{
    // `StandingOf` is what the node acts on and what every surface reports, so its four
    // answers are asserted here rather than only through a `RaftNode` -- a surface that
    // derived the standing itself would otherwise have nothing to disagree with.
    auto const configuration = Configuration { .voters = { "n1", "n2" }, .learners = { "laptop" } };

    CHECK(Membership::StandingOf(configuration, "n1") == Standing::Voter);
    CHECK(Membership::StandingOf(configuration, "laptop") == Standing::Learner);
    CHECK(Membership::StandingOf(configuration, "stranger") == Standing::Outsider);
    CHECK(Membership::StandingOf(Configuration {}, "n1") == Standing::NoCluster);

    // A configuration of learners alone is not "no cluster" -- it names somebody -- even
    // though `Validate` refuses to let one be proposed.
    CHECK(Membership::StandingOf(Configuration { .voters = {}, .learners = { "laptop" } }, "n1") == Standing::Outsider);
}
