// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftNode.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <variant>

namespace FastCache::Consensus
{

namespace
{
    /// Overload set for `std::visit`, so each message type gets its own handler
    /// rather than a chain of `if constexpr (std::is_same_v<...>)`.
    template <typename... Handlers>
    struct Overloaded: Handlers...
    {
        using Handlers::operator()...;
    };
} // namespace

RoleTraits const& TraitsOf(Role role) noexcept
{
    // A range-based scan rather than `std::ranges::find`, because the iterator
    // type of a `std::array` is a raw pointer in libstdc++ and libc++ but a class
    // type in MSVC's standard library. Holding it in an `auto` variable therefore
    // cannot be spelled to satisfy both: clang-tidy's readability-qualified-auto
    // requires `auto const*` where it deduces a pointer, and that exact spelling
    // fails to deduce on MSVC. Not naming the iterator at all sidesteps a
    // difference that says nothing about this function.
    for (auto const& row: RoleTable)
        if (row.role == role)
            return row;

    // Unreachable: every enumerator has a row and `Role` has no other values.
    // Returning the first row rather than dereferencing past the end keeps that
    // true by construction if a row is ever removed by accident.
    return RoleTable.front();
}

std::expected<RaftNode, ConsensusError> RaftNode::Create(RaftConfig config, IRandomSource& random, TimePoint now)
{
    if (auto valid = config.Validate(); !valid.has_value())
        return std::unexpected { valid.error() };

    return RaftNode { std::move(config), random, now };
}

RaftNode::RaftNode(RaftConfig config, IRandomSource& random, TimePoint now):
    _config { std::move(config) },
    _peers { _config.Peers() },
    _random { random }
{
    ArmElectionTimer(now);
}

Role RaftNode::CurrentRole() const noexcept
{
    return _role;
}

Term RaftNode::CurrentTerm() const noexcept
{
    return _currentTerm;
}

std::optional<NodeId> const& RaftNode::VotedFor() const noexcept
{
    return _votedFor;
}

std::optional<NodeId> const& RaftNode::KnownLeader() const noexcept
{
    return _knownLeader;
}

RaftLog const& RaftNode::Log() const noexcept
{
    return _log;
}

TimePoint RaftNode::NextDeadline() const noexcept
{
    return TraitsOf(_role).timer == TimerKind::Election ? _electionDeadline : _heartbeatDeadline;
}

void RaftNode::ArmElectionTimer(TimePoint now)
{
    auto const low = static_cast<std::uint64_t>(_config.electionTimeoutMin.count());
    auto const high = static_cast<std::uint64_t>(_config.electionTimeoutMax.count());
    auto const drawn = _random.UniformInRange(low, high);
    _electionDeadline = now + std::chrono::milliseconds { static_cast<std::chrono::milliseconds::rep>(drawn) };
}

void RaftNode::MarkPersist(RaftOutput& output) const
{
    output.persist = PersistentState { .currentTerm = _currentTerm, .votedFor = _votedFor };
}

void RaftNode::BroadcastToPeers(RaftOutput& output, RaftMessage const& message) const
{
    for (auto const& peer: _peers)
        output.messages.push_back(OutboundMessage { .to = peer, .message = message });
}

AppendEntriesRequest RaftNode::MakeHeartbeat() const
{
    // Carries no entries, but still the leader's prevLogIndex/prevLogTerm: a
    // heartbeat is also how a follower discovers its log has diverged, and one
    // that named nothing would leave a divergent follower undetected until real
    // entries happened to be sent.
    return AppendEntriesRequest { .term = _currentTerm,
                                  .leaderId = _config.self,
                                  .prevLogIndex = _log.LastIndex(),
                                  .prevLogTerm = _log.LastTerm(),
                                  .entries = {},
                                  .leaderCommit = LogIndex::BeforeFirst() };
}

Term RaftNode::TermOf(RaftMessage const& message) noexcept
{
    return std::visit([](auto const& concrete) { return concrete.term; }, message);
}

bool RaftNode::IsMember(NodeId const& id) const
{
    return std::ranges::find(_config.members, id) != _config.members.end();
}

void RaftNode::StepDown(Term term, TimePoint now, RaftOutput& output)
{
    // Whether the election timer is re-armed depends on what this node was, and
    // getting it wrong in either direction costs liveness.
    //
    // §5.2 resets the timer on exactly two events: hearing valid AppendEntries
    // from a current leader, and *granting* a vote. Adopting a higher term is
    // neither. Re-arming here unconditionally would silently defeat the
    // protection `OnRequestVote` documents below, and would defeat it in the case
    // that matters most: a partitioned-but-alive node with a stale log times out,
    // bumps its term and campaigns; every healthy node adopts the term, re-arms,
    // and *then* denies the vote on §5.4.1 grounds. The vote is refused and the
    // timers are pushed out anyway, once per election timeout, forever. Nothing
    // is corrupted and no election ever finishes, with every node reporting
    // itself healthy.
    //
    // A leader is the exception, because a leader runs no election timer at all:
    // its deadline is whatever was left over from before it was elected, which is
    // in the past. Left alone, a deposed leader would immediately time out and
    // campaign at a higher term -- turning one disruption into two. This is
    // LogCabin's rule, which arms the timer only when it was not already running.
    auto const wasLeader = _role == Role::Leader;

    _currentTerm = term;
    _votedFor.reset();
    _knownLeader.reset();
    _role = Role::Follower;
    _votesGranted.clear();
    if (wasLeader)
        ArmElectionTimer(now);

    MarkPersist(output);
}

void RaftNode::StartElection(TimePoint now, RaftOutput& output)
{
    _currentTerm = _currentTerm.Next();
    _role = Role::Candidate;
    _votedFor = _config.self;
    _knownLeader.reset();

    // A candidate votes for itself, which is why a single-node cluster elects
    // immediately and why the quorum test below is the same one for every size.
    _votesGranted.clear();
    _votesGranted.insert(_config.self);

    ArmElectionTimer(now);
    MarkPersist(output);

    if (_votesGranted.size() >= _config.Quorum())
    {
        BecomeLeader(now, output);
        return;
    }

    BroadcastToPeers(output,
                     RequestVoteRequest { .term = _currentTerm,
                                          .candidateId = _config.self,
                                          .lastLogIndex = _log.LastIndex(),
                                          .lastLogTerm = _log.LastTerm() });
}

void RaftNode::BecomeLeader(TimePoint now, RaftOutput& output)
{
    _role = Role::Leader;
    _knownLeader = _config.self;
    _heartbeatDeadline = now + _config.heartbeatInterval;

    // Immediately, not at the next heartbeat interval: until peers hear from the
    // new leader they are still counting down to their own elections, so a
    // deferred first heartbeat gives away part of the timeout the algorithm just
    // spent to establish this leader.
    BroadcastToPeers(output, MakeHeartbeat());
}

RaftOutput RaftNode::Tick(TimePoint now)
{
    auto output = RaftOutput {};
    if (now < NextDeadline())
        return output;

    if (TraitsOf(_role).timer == TimerKind::Election)
    {
        StartElection(now, output);
        return output;
    }

    _heartbeatDeadline = now + _config.heartbeatInterval;
    BroadcastToPeers(output, MakeHeartbeat());
    return output;
}

RaftOutput RaftNode::Receive(RaftMessage const& message, TimePoint now)
{
    auto output = RaftOutput {};

    // §5.1, applied once for every message rather than per handler: a higher term
    // always wins and always demotes. Doing this in each handler is how the rule
    // comes to be applied to requests and forgotten on responses.
    auto const incomingTerm = TermOf(message);
    if (incomingTerm > _currentTerm)
        StepDown(incomingTerm, now, output);

    std::visit(Overloaded {
                   [&](RequestVoteRequest const& request) { OnRequestVote(request, now, output); },
                   [&](RequestVoteResponse const& response) { OnRequestVoteResponse(response, now, output); },
                   [&](AppendEntriesRequest const& request) { OnAppendEntries(request, now, output); },
                   [&](AppendEntriesResponse const& response) { OnAppendEntriesResponse(response, now, output); },
               },
               message);

    return output;
}

void RaftNode::OnRequestVote(RequestVoteRequest const& request, TimePoint now, RaftOutput& output)
{
    auto const reply = [&](VoteDecision decision) {
        output.messages.push_back(OutboundMessage {
            .to = request.candidateId,
            .message = RequestVoteResponse { .term = _currentTerm, .decision = decision, .voterId = _config.self } });
    };

    // A candidate from an older term has already lost; the term in the refusal is
    // what tells it so.
    if (request.term < _currentTerm)
    {
        reply(VoteDecision::Denied);
        return;
    }

    // A non-member cannot be voted for, for the reason its AppendEntries is
    // refused: a granted vote is spent for the whole term and re-arms the election
    // timer, so a machine outside the configuration could both consume this
    // node's vote and delay its candidacy.
    if (!IsMember(request.candidateId))
    {
        reply(VoteDecision::Denied);
        return;
    }

    // At most one vote per term. `votedFor == candidateId` is not laxity: it makes
    // the grant idempotent, so a retransmitted request after a lost response gets
    // the same answer rather than a refusal that would stall an election nobody
    // has any reason to lose.
    auto const alreadyPromised = _votedFor.has_value() && *_votedFor != request.candidateId;
    if (alreadyPromised || !_log.CandidateIsAtLeastAsUpToDate(request.lastLogIndex, request.lastLogTerm))
    {
        reply(VoteDecision::Denied);
        return;
    }

    _votedFor = request.candidateId;
    MarkPersist(output);

    // Only *after* granting. Resetting the election timer for a candidate this
    // node refused would let a node with a stale log delay every healthy node's
    // election simply by asking repeatedly.
    ArmElectionTimer(now);
    reply(VoteDecision::Granted);
}

void RaftNode::OnRequestVoteResponse(RequestVoteResponse const& response, TimePoint now, RaftOutput& output)
{
    // A response from an earlier term, or one addressed to an election this node
    // is no longer running, decides nothing.
    if (_role != Role::Candidate || response.term != _currentTerm)
        return;

    if (response.decision != VoteDecision::Granted)
        return;

    // The voter names itself, and the tally is what decides leadership, so an id
    // from outside the configuration must not reach it. In a three-node cluster a
    // single response bearing a second member's id, plus this node's own vote,
    // would be a quorum -- Election Safety lost to a field nobody checked.
    //
    // This bounds the damage to the configured members rather than eliminating
    // it: one member can still claim another's id. Distinguishing *that* needs
    // the sender's authenticated identity from the transport, which does not
    // exist until the wire does, and is where the check belongs.
    if (!IsMember(response.voterId))
        return;

    _votesGranted.insert(response.voterId);
    if (_votesGranted.size() >= _config.Quorum())
        BecomeLeader(now, output);
}

void RaftNode::OnAppendEntries(AppendEntriesRequest const& request, TimePoint now, RaftOutput& output)
{
    auto const reply = [&](AppendResult result, LogIndex matchIndex) {
        output.messages.push_back(OutboundMessage {
            .to = request.leaderId,
            .message = AppendEntriesResponse {
                .term = _currentTerm, .result = result, .matchIndex = matchIndex, .followerId = _config.self } });
    };

    if (request.term < _currentTerm)
    {
        reply(AppendResult::Rejected, LogIndex::BeforeFirst());
        return;
    }

    // Accepting this makes the sender this node's leader: it publishes
    // `_knownLeader`, which is what clients are redirected to, and re-arms the
    // election timer. A decommissioned node still running -- or anything else
    // that can reach the port -- must not be able to hold the cluster in follower
    // state indefinitely and point its clients at a machine the configuration
    // does not contain.
    if (!IsMember(request.leaderId))
    {
        reply(AppendResult::Rejected, LogIndex::BeforeFirst());
        return;
    }

    // A candidate that hears from a leader of its own term has lost the election
    // -- someone else reached a quorum first -- and must stop competing. Without
    // this a split vote is resolved and then immediately re-run.
    _role = Role::Follower;
    _knownLeader = request.leaderId;
    _votesGranted.clear();
    ArmElectionTimer(now);

    auto const outcome = _log.TryAppend(request.prevLogIndex, request.prevLogTerm, request.entries);
    reply(outcome.result, outcome.matchIndex);
}

void RaftNode::OnAppendEntriesResponse(AppendEntriesResponse const& /*response*/, TimePoint /*now*/, RaftOutput& /*output*/)
{
    // The §5.1 term check in `Receive` has already demoted this node if the
    // responder knew a higher term, which is the whole of what a response means
    // to leader election. Tracking how far each follower has caught up is
    // replication's business and arrives with it.
}

} // namespace FastCache::Consensus
