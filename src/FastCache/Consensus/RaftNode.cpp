// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Consensus/RaftNode.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

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

std::expected<RaftNode, ConsensusError> RaftNode::Create(RaftConfig config,
                                                         IRandomSource& random,
                                                         TimePoint now,
                                                         RecoveredState recovered)
{
    if (auto valid = config.Validate(); !valid.has_value())
        return std::unexpected { valid.error() };

    return RaftNode { std::move(config), random, now, std::move(recovered) };
}

RaftNode::RaftNode(RaftConfig config, IRandomSource& random, TimePoint now, RecoveredState recovered):
    _config { std::move(config) },
    _peers { _config.Peers() },
    _random { random },
    _currentTerm { recovered.state.currentTerm },
    _votedFor { std::move(recovered.state.votedFor) },
    _log { std::move(recovered.entries) },
    _members { _config.members }
{
    // A recovered log may already carry a configuration change, and the node must
    // come back under it rather than under the one it was bootstrapped with --
    // otherwise a restart silently reverts a membership change the cluster made.
    RefreshConfiguration();

    // A recovered node comes back as a follower whatever it was before, which is
    // not a simplification: role is not durable state, and a node that resumed as
    // a leader would be a second leader for a term that has since moved on.
    // `_commitIndex` is deliberately not recovered either -- it is re-learned from
    // the first leader that reaches this node, and starting at zero merely
    // re-applies entries the application already has, which the driver's state
    // machine must tolerate anyway after any restart.
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

LogIndex RaftNode::CommitIndex() const noexcept
{
    return _commitIndex;
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

AppendEntriesRequest RaftNode::MakeAppendEntriesFor(NodeId const& peer) const
{
    auto const found = _nextIndex.find(peer);
    auto const next = found != _nextIndex.end() ? found->second : _log.LastIndex().Advanced(1);
    auto const prevIndex = next.Prev();

    return AppendEntriesRequest { .term = _currentTerm,
                                  .leaderId = _config.self,
                                  .prevLogIndex = prevIndex,
                                  .prevLogTerm = _log.TermAt(prevIndex).value_or(Term::None()),
                                  .entries = _log.EntriesFrom(next),
                                  .leaderCommit = _commitIndex };
}

void RaftNode::ReplicateToPeers(RaftOutput& output) const
{
    for (auto const& peer: _peers)
        output.messages.push_back(OutboundMessage { .to = peer, .message = MakeAppendEntriesFor(peer) });
}

void RaftNode::AdvanceCommitIndex()
{
    // The highest index a quorum holds: sort every member's match index
    // descending and take the one at position quorum-1. This node counts itself,
    // and its own match index is its whole log -- a leader trivially has what it
    // wrote.
    auto matches = std::vector<LogIndex> {};
    matches.reserve(_members.size());

    // Itself, but only while it IS a member. A leader that has been removed keeps
    // replicating until the entry removing it commits -- and that commitment is
    // decided by the NEW configuration, which it is not part of. Counting itself
    // there would let it commit its own removal on a quorum that does not
    // include enough of the members who have to live with it.
    if (IsMember(_config.self))
        matches.push_back(_log.LastIndex());
    for (auto const& peer: _peers)
    {
        auto const found = _matchIndex.find(peer);
        matches.push_back(found != _matchIndex.end() ? found->second : LogIndex::BeforeFirst());
    }

    std::ranges::sort(matches, std::greater {});
    auto const replicated = matches[Quorum() - 1];
    if (replicated <= _commitIndex)
        return;

    // Raft §5.4.2, and the rule whose omission is the Figure 8 defect: a leader
    // may only conclude an entry is committed by counting replicas when that
    // entry is from its OWN term. An entry from an earlier term can sit on a
    // majority and still be overwritten, because a future leader elected under
    // §5.4.1's up-to-dateness test may lack it -- being replicated widely is not
    // the same as being safe. Such entries commit indirectly, carried by the
    // first current-term entry that commits above them, which is why this is a
    // guard rather than a special case: the moment a current-term entry reaches a
    // quorum, everything below it commits with it.
    if (_log.TermAt(replicated) != _currentTerm)
        return;

    _commitIndex = replicated;
}

void RaftNode::ApplyCommitted(RaftOutput& output)
{
    // Bounded by what this node actually holds, not by the commit index alone.
    // The two can legitimately disagree -- a node keeps its commit index across a
    // step-down while its log may still be repaired backwards by the new leader --
    // and advancing `_lastApplied` past the end would mark those indices applied
    // forever, so the entries would be silently skipped when they did arrive and
    // this node's state machine would diverge without anything failing.
    auto const applyThrough = std::min(_commitIndex, _log.LastIndex());

    while (_lastApplied < applyThrough)
    {
        _lastApplied = _lastApplied.Advanced(1);

        auto const* const entry = _log.EntryAt(_lastApplied);
        if (entry == nullptr)
            continue;

        // Consensus' own entries are committed like any other and never
        // delivered: the application asked for none of them and cannot interpret
        // them.
        if (entry->kind == EntryKind::NoOp || entry->kind == EntryKind::Configuration)
            continue;

        output.applied.push_back(AppliedEntry { .index = _lastApplied, .payload = entry->payload });
    }

    // A leader that has been removed from the configuration steps down once the
    // change is committed (§4.2.2). It cannot simply stop: the entry that removes
    // it has to be committed first, and only this leader can commit it -- so it
    // keeps leading a cluster it is no longer part of for exactly as long as it
    // takes to make its own removal durable. Staying leader past that point would
    // let a node outside the configuration keep replicating to it.
    if (_role == Role::Leader && !IsMember(_config.self) && LatestConfigurationIndex() <= _commitIndex)
    {
        _role = Role::Follower;
        _knownLeader.reset();
        _votesGranted.clear();
        _preVotesGranted.clear();
        _nextIndex.clear();
        _matchIndex.clear();
    }
}

void RaftNode::RecordLogAppend(RaftOutput& output, LogIndex fromIndex)
{
    if (fromIndex > _log.LastIndex())
        return;

    output.persistLog = LogAppend { .fromIndex = fromIndex, .entries = _log.EntriesFrom(fromIndex) };
}

Term RaftNode::TermOf(RaftMessage const& message) noexcept
{
    return std::visit([](auto const& concrete) { return concrete.term; }, message);
}

bool RaftNode::IsMember(NodeId const& id) const
{
    return std::ranges::find(_members, id) != _members.end();
}

std::size_t RaftNode::Quorum() const noexcept
{
    // From the ACTIVE member set, never from the bootstrap configuration. A
    // quorum computed against a stale size is the one number that makes every
    // other rule unsafe: too small and a minority commits, too large and a
    // healthy cluster cannot.
    return (_members.size() / 2) + 1;
}

void RaftNode::AdoptMembers(std::vector<NodeId> members)
{
    _members = std::move(members);

    _peers.clear();
    for (auto const& member: _members)
        if (member != _config.self)
            _peers.push_back(member);

    // Progress bookkeeping for a member that is no longer one would count toward
    // a quorum it is not part of.
    std::erase_if(_nextIndex, [this](auto const& entry) { return !IsMember(entry.first); });
    std::erase_if(_matchIndex, [this](auto const& entry) { return !IsMember(entry.first); });
    std::erase_if(_votesGranted, [this](NodeId const& id) { return !IsMember(id); });
    std::erase_if(_preVotesGranted, [this](NodeId const& id) { return !IsMember(id); });
}

void RaftNode::RefreshConfiguration()
{
    // The LATEST configuration in the log, committed or not (§4.3). That looks
    // unsafe and is the opposite: a configuration that only took effect once
    // committed could not be used to *reach* commitment, because committing it
    // needs a quorum of the very set it describes.
    //
    // Re-derived by scanning rather than tracked forward, because an uncommitted
    // change can be rolled back by a truncation -- so this is not a value that
    // only ever moves in one direction, and a node that treated it as one would
    // keep a configuration the cluster has discarded.
    for (auto index = _log.LastIndex().value; index >= 1; --index)
    {
        auto const* const entry = _log.EntryAt(LogIndex { .value = index });
        if (entry == nullptr || entry->kind != EntryKind::Configuration)
            continue;

        auto decoded = Membership::Decode(entry->payload);
        if (decoded.has_value() && !decoded->empty())
            AdoptMembers(*std::move(decoded));
        return;
    }

    // No configuration entry: the bootstrap set is the active one.
    AdoptMembers(_config.members);
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

    // Per-follower progress is a leader's bookkeeping about a term it no longer
    // leads. Keeping it would let a re-elected node resume from stale guesses
    // about logs that have moved on since. `_commitIndex` and `_lastApplied` are
    // deliberately NOT reset: they record what is already committed cluster-wide,
    // which no change of leadership can un-decide.
    _nextIndex.clear();
    _matchIndex.clear();

    if (wasLeader)
        ArmElectionTimer(now);

    MarkPersist(output);
}

void RaftNode::StartPreVote(TimePoint now, RaftOutput& output)
{
    // Nothing durable changes here, and that is the entire point: no term
    // increment, no vote recorded, no `MarkPersist`. A round that wrote anything
    // would cost a disk flush per election timeout on every node that cannot
    // reach a leader -- which is every node, during exactly the partition this
    // is meant to make cheap.
    _role = Role::PreCandidate;
    _knownLeader.reset();

    _preVotesGranted.clear();
    _preVotesGranted.insert(_config.self);

    ArmElectionTimer(now);

    // A single-node cluster is its own quorum, so it goes straight to a real
    // election -- the pre-vote round costs it nothing and skips no step.
    if (_preVotesGranted.size() >= Quorum())
    {
        StartElection(now, output);
        return;
    }

    // The term asked about is one ABOVE this node's own: the question is "would
    // you support me if I stood", and the term it would stand in is the one a
    // voter has to compare its log against.
    BroadcastToPeers(output,
                     PreVoteRequest { .term = _currentTerm.Next(),
                                      .candidateId = _config.self,
                                      .lastLogIndex = _log.LastIndex(),
                                      .lastLogTerm = _log.LastTerm() });
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

    // The pre-vote round is over; its answers were about whether to stand, not
    // about who to elect, and counting them here would be counting votes nobody
    // cast.
    _preVotesGranted.clear();

    ArmElectionTimer(now);
    MarkPersist(output);

    if (_votesGranted.size() >= Quorum())
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

    // Optimistic for nextIndex, pessimistic for matchIndex, and both are
    // deliberate. A new leader does not know how far any follower has caught up,
    // so it guesses that each is fully caught up and walks the guess back on
    // rejection -- one round trip per missing entry, against a wrong optimistic
    // guess costing nothing but that. matchIndex starts at zero because it
    // records what is *known*, and nothing is yet.
    _nextIndex.clear();
    _matchIndex.clear();
    for (auto const& peer: _peers)
    {
        _nextIndex[peer] = _log.LastIndex().Advanced(1);
        _matchIndex[peer] = LogIndex::BeforeFirst();
    }

    // A no-op of this leader's own term, and it is the companion to the §5.4.2
    // guard rather than a nicety. That guard refuses to commit an earlier term's
    // entry by replica count, so a leader whose log ends in fully-replicated
    // entries from the previous term can never commit them -- and therefore
    // applies nothing -- until a client happens to propose something. With no
    // client traffic the cluster is live, healthy, and permanently stuck one term
    // behind. Committing one entry of the current term releases everything below
    // it, so a leader creates that entry itself instead of waiting to be handed
    // one. Appended AFTER nextIndex is initialized, so it is inside the range
    // each follower is sent rather than past it.
    auto const noOp = _log.Append(LogEntry { .term = _currentTerm, .kind = EntryKind::NoOp, .payload = {} });
    MarkPersist(output);
    RecordLogAppend(output, noOp);

    // Immediately, not at the next heartbeat interval: until peers hear from the
    // new leader they are still counting down to their own elections, so a
    // deferred first heartbeat gives away part of the timeout the algorithm just
    // spent to establish this leader.
    ReplicateToPeers(output);

    // A single-node cluster is its own quorum, so the no-op is committed the
    // moment it is written -- which is what releases any earlier-term entries.
    AdvanceCommitIndex();
    ApplyCommitted(output);
}

RaftOutput RaftNode::Tick(TimePoint now)
{
    auto output = RaftOutput {};
    if (now < NextDeadline())
        return output;

    if (TraitsOf(_role).timer == TimerKind::Election)
    {
        // A pre-vote round, not an election. The term is untouched until a
        // quorum says an election is winnable, which is what keeps a node that
        // has been partitioned away from returning with an inflated term and
        // deposing a leader that was working perfectly well.
        StartPreVote(now, output);
        return output;
    }

    _heartbeatDeadline = now + _config.heartbeatInterval;
    ReplicateToPeers(output);
    return output;
}

LogIndex RaftNode::LatestConfigurationIndex() const
{
    for (auto index = _log.LastIndex().value; index >= 1; --index)
    {
        auto const* const entry = _log.EntryAt(LogIndex { .value = index });
        if (entry != nullptr && entry->kind == EntryKind::Configuration)
            return LogIndex { .value = index };
    }
    return LogIndex::BeforeFirst();
}

bool RaftNode::HasUncommittedConfiguration() const
{
    // Any configuration entry above the commit index. Scanned downward and
    // stopped at the commit index rather than walked whole, because everything
    // at or below it is settled by definition.
    for (auto index = _log.LastIndex().value; index > _commitIndex.value; --index)
    {
        auto const* const entry = _log.EntryAt(LogIndex { .value = index });
        if (entry != nullptr && entry->kind == EntryKind::Configuration)
            return true;
    }
    return false;
}

std::expected<RaftNode::Proposal, ConsensusError> RaftNode::ProposeMembership(std::vector<NodeId> members, TimePoint now)
{
    if (_role != Role::Leader)
        return std::unexpected { FastCache::NotLeader(_knownLeader) };

    if (auto valid = Membership::Validate(members); !valid.has_value())
        return std::unexpected { valid.error() };

    // One change at a time, and it must have committed. A second built on a
    // configuration that a truncation can still roll back would have its safety
    // argument made against a set that never existed.
    if (HasUncommittedConfiguration())
        return std::unexpected { InvalidConfiguration("a membership change is already in flight; wait for it to commit") };

    switch (Membership::Classify(_members, members))
    {
        case Membership::ChangeShape::Unchanged:
            return std::unexpected { InvalidConfiguration("the proposed member set is the current one") };
        case Membership::ChangeShape::Unsafe:
            return std::unexpected { InvalidConfiguration(
                "only one member may be added or removed at a time; two majorities that share no member "
                "could otherwise elect two leaders in one term") };
        case Membership::ChangeShape::AddedOne:
        case Membership::ChangeShape::RemovedOne:
            break;
    }

    auto output = RaftOutput {};
    auto const index = _log.Append(
        LogEntry { .term = _currentTerm, .kind = EntryKind::Configuration, .payload = Membership::Encode(members) });

    // Adopted here, before commitment and before replication, because a
    // configuration that waited for commitment could not be used to REACH it:
    // committing this entry needs a quorum of the very set it describes.
    AdoptMembers(std::move(members));

    RecordLogAppend(output, index);
    ReplicateToPeers(output);
    _heartbeatDeadline = now + _config.heartbeatInterval;

    AdvanceCommitIndex();
    ApplyCommitted(output);

    return Proposal { .index = index, .output = std::move(output) };
}

std::expected<RaftNode::Proposal, ConsensusError> RaftNode::Propose(std::vector<std::byte> payload, TimePoint now)
{
    if (_role != Role::Leader)
        return std::unexpected { FastCache::NotLeader(_knownLeader) };

    auto output = RaftOutput {};
    auto const index =
        _log.Append(LogEntry { .term = _currentTerm, .kind = EntryKind::Command, .payload = std::move(payload) });

    // The log is durable state, so the entry has to reach stable storage before
    // it is replicated -- a leader that sends an entry it then loses on restart
    // can be asked about it by a follower that kept it. `persistLog` is what
    // carries that; `MarkPersist` alone says nothing about the log, which is what
    // an earlier draft of this got wrong.
    RecordLogAppend(output, index);
    ReplicateToPeers(output);

    // These entries are a heartbeat too, so the next one is due a full interval
    // from now rather than from whenever the last one happened to go out.
    _heartbeatDeadline = now + _config.heartbeatInterval;

    // A single-node cluster has a quorum of one and its own log is that quorum,
    // so the entry is committed the moment it is appended and there is nobody to
    // hear from. Without this it would sit uncommitted until some other event.
    AdvanceCommitIndex();
    ApplyCommitted(output);

    return Proposal { .index = index, .output = std::move(output) };
}

RaftOutput RaftNode::Receive(RaftMessage const& message, TimePoint now)
{
    auto output = RaftOutput {};

    // §5.1, applied once for every message rather than per handler: a higher term
    // always wins and always demotes. Doing this in each handler is how the rule
    // comes to be applied to requests and forgotten on responses.
    //
    // The pre-vote messages are the two deliberate exemptions, and getting either
    // wrong silently defeats the whole mechanism:
    //
    //   - A `PreVoteRequest` carries the term its sender WOULD use, not one it
    //     holds. Adopting it here would let a partitioned node inflate everyone
    //     else's term just by asking -- which is precisely the disruption the
    //     round exists to prevent, reintroduced by the code meant to prevent it.
    //   - A GRANTED `PreVoteResponse` echoes that same not-yet-entered term. Left
    //     to the rule below it would demote the node its grant is encouraging, so
    //     a pre-vote round could never succeed at all. A *refused* one carries the
    //     voter's own term, and stepping down on that is exactly right: it means
    //     this node is behind.
    if (!IsPreVoteExempt(message))
    {
        auto const incomingTerm = TermOf(message);
        if (incomingTerm > _currentTerm)
            StepDown(incomingTerm, now, output);
    }

    std::visit(Overloaded {
                   [&](PreVoteRequest const& request) { OnPreVote(request, now, output); },
                   [&](PreVoteResponse const& response) { OnPreVoteResponse(response, now, output); },
                   [&](RequestVoteRequest const& request) { OnRequestVote(request, now, output); },
                   [&](RequestVoteResponse const& response) { OnRequestVoteResponse(response, now, output); },
                   [&](AppendEntriesRequest const& request) { OnAppendEntries(request, now, output); },
                   [&](AppendEntriesResponse const& response) { OnAppendEntriesResponse(response, now, output); },
               },
               message);

    return output;
}

bool RaftNode::IsPreVoteExempt(RaftMessage const& message) noexcept
{
    if (std::holds_alternative<PreVoteRequest>(message))
        return true;

    auto const* const response = std::get_if<PreVoteResponse>(&message);
    return response != nullptr && response->decision == VoteDecision::Granted;
}

void RaftNode::OnPreVote(PreVoteRequest const& request, TimePoint now, RaftOutput& output)
{
    // A refusal carries THIS node's term so a sender that is behind learns it.
    // A grant echoes the request's term, so the sender is not demoted by the very
    // answer that encourages it -- see the exemption in `Receive`.
    auto const reply = [&](VoteDecision decision) {
        output.messages.push_back(OutboundMessage {
            .to = request.candidateId,
            .message = PreVoteResponse { .term = decision == VoteDecision::Granted ? request.term : _currentTerm,
                                         .decision = decision,
                                         .voterId = _config.self } });
    };

    // Nothing below writes, records a vote, or moves a timer. A pre-vote must
    // leave this node exactly as it found it -- otherwise a node asking
    // repeatedly from behind a partition could delay every healthy node's
    // election simply by asking, which is the disruption in a new costume.
    if (request.term <= _currentTerm || !IsMember(request.candidateId))
    {
        reply(VoteDecision::Denied);
        return;
    }

    // The one condition beyond the log check, and the one that does the work:
    // this node must not have heard from a leader recently. A cluster with a
    // healthy leader refuses every pre-vote, so a partitioned node gets no
    // quorum, never increments its term, and cannot disturb anything when it
    // returns. `_electionDeadline` is where "recently" already lives: it is
    // re-armed by every AppendEntries this node accepts.
    if (now < _electionDeadline)
    {
        reply(VoteDecision::Denied);
        return;
    }

    reply(_log.CandidateIsAtLeastAsUpToDate(request.lastLogIndex, request.lastLogTerm) ? VoteDecision::Granted
                                                                                       : VoteDecision::Denied);
}

void RaftNode::OnPreVoteResponse(PreVoteResponse const& response, TimePoint now, RaftOutput& output)
{
    // An answer to a question this node is no longer asking decides nothing. The
    // term compared against is the one that was ASKED about -- one above this
    // node's own -- because that is what a grant echoes.
    if (_role != Role::PreCandidate || response.term != _currentTerm.Next())
        return;

    if (response.decision != VoteDecision::Granted)
        return;

    if (!IsMember(response.voterId))
        return;

    _preVotesGranted.insert(response.voterId);
    if (_preVotesGranted.size() >= Quorum())
        StartElection(now, output);
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
    if (_votesGranted.size() >= Quorum())
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
    if (outcome.result == AppendResult::Accepted)
    {
        // The log is durable state on a follower exactly as on a leader: these
        // entries are about to be acknowledged, and a leader may commit on that
        // acknowledgement, so losing them to a restart loses a committed entry.
        // `fromIndex` is where the leader's run starts, which is also the
        // truncation point when this repaired a divergent suffix.
        if (!request.entries.empty())
        {
            RecordLogAppend(output, request.prevLogIndex.Advanced(1));

            // The log moved, so the active configuration may have moved with it
            // -- forwards on a new entry, and BACKWARDS when a conflicting
            // suffix was truncated. Re-derived rather than tracked, because an
            // uncommitted configuration is exactly the kind that gets rolled
            // back, and a node that kept one the cluster discarded would count
            // quorums against a set nobody else has.
            //
            // Inside the guard, not beside it: a heartbeat carries no entries
            // and cannot have moved anything, so scanning on one would be a walk
            // of the log per interval per peer for a result that cannot change.
            RefreshConfiguration();
        }

        // Bounded by what THIS request established, not by the leader's own
        // commit index alone. The leader may have committed entries this follower
        // has not received yet -- an AppendEntries can be delayed or truncated by
        // the network -- and adopting its number outright would mark entries
        // committed that are absent from this log, so the next thing to apply
        // would be whatever happened to sit at that index.
        //
        // And never downward. Guarding on `leaderCommit > _commitIndex` alone is
        // not enough, because the value assigned is the *minimum*: a delayed
        // duplicate carrying a high leaderCommit and a low match index would take
        // the commit index backwards, and `CommitIndex()` promises that an entry
        // at or below it is never taken back. A stale duplicate producing exactly
        // that low match index is not hypothetical -- RaftLog::TryAppend has a
        // test for it.
        auto const advanced = std::min(request.leaderCommit, outcome.matchIndex);
        _commitIndex = std::max(_commitIndex, advanced);
    }

    reply(outcome.result, outcome.matchIndex);
    ApplyCommitted(output);
}

void RaftNode::OnAppendEntriesResponse(AppendEntriesResponse const& response, TimePoint /*now*/, RaftOutput& output)
{
    // The §5.1 term check in `Receive` has already demoted this node if the
    // responder knew a higher term. What is left matters only to a leader still
    // leading the term it asked in: a response to a previous term's request says
    // nothing about this one.
    if (_role != Role::Leader || response.term != _currentTerm || !IsMember(response.followerId))
        return;

    if (response.result == AppendResult::Rejected)
    {
        // The consistency check failed, so this leader's guess about where the
        // follower's log agrees was too high. Walk it back and retry immediately
        // rather than at the next heartbeat: each round trip recovers exactly one
        // index, so waiting an interval per index makes catching up take
        // heartbeat-interval times divergence.
        // Floored at the first index: `Prev()` saturates at zero, which names no
        // entry, and sending from there would ask the follower about a position
        // that cannot exist.
        auto& next = _nextIndex[response.followerId];
        next = std::max(next.Prev(), LogIndex { .value = 1 });

        output.messages.push_back(
            OutboundMessage { .to = response.followerId, .message = MakeAppendEntriesFor(response.followerId) });
        return;
    }

    // Never move a match index backwards. Responses arrive out of order, and an
    // older one carrying a smaller index would un-acknowledge entries this
    // follower has already confirmed -- and since match indices are what decide
    // commitment, that can only end with a committed entry treated as
    // uncommitted.
    // Clamped to this leader's own log as well. A follower cannot hold more than
    // was sent to it, so a larger number is nonsense -- but taken at face value it
    // pushes `nextIndex` past the end, and every subsequent AppendEntries then
    // names a `prevLogIndex` that does not exist, is rejected, and walks back one
    // index per round trip: that peer never converges again. It reaches
    // `AdvanceCommitIndex` too, where only `TermAt` returning nullopt for a
    // phantom index prevents committing one.
    auto& match = _matchIndex[response.followerId];
    match = std::max(match, std::min(response.matchIndex, _log.LastIndex()));
    _nextIndex[response.followerId] = match.Advanced(1);

    AdvanceCommitIndex();
    ApplyCommitted(output);
}

} // namespace FastCache::Consensus
