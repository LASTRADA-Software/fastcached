// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <variant>

namespace FastCache::Consensus
{

namespace
{
    /// @p refusal, saying WHERE in the recovered state it was found.
    /// @param where The snapshot, or a log entry by index.
    /// @param refusal What the application said; its code is kept.
    /// @return The located refusal.
    [[nodiscard]] ConsensusError Located(std::string_view where, ConsensusError refusal)
    {
        refusal.context = std::format("{}: {}", where, refusal.context);
        return refusal;
    }
} // namespace

std::expected<std::unique_ptr<RaftDriver>, ConsensusError> RaftDriver::Create(RaftNode node,
                                                                              IRaftStorage& storage,
                                                                              IRaftTransport& transport,
                                                                              IRaftStateMachine& application,
                                                                              CompactionPolicy compaction)
{
    if (auto recovered = Recover(node, application); !recovered.has_value())
        return std::unexpected { std::move(recovered).error() };

    // `new` rather than `make_unique`, which cannot reach a private constructor -- and
    // the constructor is private so this function's recovery cannot be skipped.
    return std::unique_ptr<RaftDriver> { new RaftDriver { std::move(node), storage, transport, application, compaction } };
}

std::expected<void, ConsensusError> RaftDriver::Recover(RaftNode const& node, IRaftStateMachine& application)
{
    // Recovery's half of `IRaftStateMachine::RestoreSnapshot` (#1542): the other half is
    // an installed snapshot, which `Deliver` hands over. A node holding a snapshot before
    // any driver exists recovered it -- nothing has run yet to compact or install one --
    // and its applied index sits at the snapshot's boundary, so the state the snapshot
    // describes reaches the application HERE or never.
    //
    // The commands above the snapshot FIRST, and read-only: every one of them will be
    // applied once a leader commits it, so one this application cannot read is state this
    // node cannot run on. Asked before the snapshot is restored so that a refusal leaves
    // the application untouched rather than holding a snapshot with no future.
    auto const& log = node.Log();
    auto const first = node.SnapshotIndex().value + 1;
    auto const last = std::max(log.LastIndex().value, node.SnapshotIndex().value);
    for (auto const index: std::views::iota(first, last + 1))
    {
        auto const* const entry = log.EntryAt(LogIndex { .value = index });
        if (entry == nullptr || entry->kind != EntryKind::Command)
            continue;
        if (auto readable = application.CanRead(entry->payload); !readable.has_value())
            return std::unexpected { Located(std::format("log entry {}", index), std::move(readable).error()) };
    }

    // Asked of the boundary rather than of the bytes: an application's empty state is a
    // legitimate snapshot, and "no snapshot" is `BeforeFirst`, never an empty buffer.
    if (node.SnapshotIndex() == LogIndex::BeforeFirst())
        return {};

    // Last, and wholesale or not at all -- the contract `RestoreSnapshot` states -- so a
    // refusal here too means the application was handed nothing.
    if (auto restored = application.RestoreSnapshot(node.CurrentSnapshot().state, SnapshotOrigin::Recovered);
        !restored.has_value())
        return std::unexpected { Located(std::format("the snapshot as of log entry {}", node.SnapshotIndex().value),
                                         std::move(restored).error()) };
    return {};
}

RaftDriver::RaftDriver(RaftNode node,
                       IRaftStorage& storage,
                       IRaftTransport& transport,
                       IRaftStateMachine& application,
                       CompactionPolicy compaction) noexcept:
    _node { std::move(node) },
    _storage { storage },
    _transport { transport },
    _application { application },
    _compaction { compaction },
    _reportedRole { _node.CurrentRole() },
    _reportedTerm { _node.CurrentTerm() },
    _reportedLeader { _node.KnownLeader() }
{
}

void RaftDriver::ObserveRole(RoleObserver observer)
{
    _onRole = std::move(observer);
}

void RaftDriver::ObserveInstallRefusal(InstallRefusalObserver observer)
{
    auto const guard = std::scoped_lock { _mutex };
    _onInstallRefusal = std::move(observer);
}

void RaftDriver::PublishInstallRefusal(std::optional<InstallRefusal> refusal)
{
    if (refusal == _installRefusal)
        return;

    _installRefusal = std::move(refusal);
    if (_onInstallRefusal)
        _onInstallRefusal(_installRefusal);
}

void RaftDriver::PublishRoleIfChanged(std::optional<TermAdoption> const& cause)
{
    auto const role = _node.CurrentRole();
    auto const term = _node.CurrentTerm();
    auto const& leader = _node.KnownLeader();
    if (role == _reportedRole && term == _reportedTerm && leader == _reportedLeader)
        return;

    _reportedRole = role;
    _reportedTerm = term;
    _reportedLeader = leader;
    if (_onRole)
        _onRole(RoleChange { .role = role, .term = term, .knownLeader = leader, .cause = cause });
}

RaftNode const& RaftDriver::Node() const noexcept
{
    return _node;
}

std::optional<ConsensusError> RaftDriver::Failure() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _failure;
}

void RaftDriver::Stop() noexcept
{
    _stopped.store(true, std::memory_order_relaxed);
}

std::expected<void, ConsensusError> RaftDriver::Deliver(RaftOutput output)
{
    if (_failure.has_value())
        return std::unexpected { *_failure };

    // Durable state before anything leaves this node, and term-and-vote before
    // the log. Both orderings are load-bearing and are argued in the header.
    if (output.persist.has_value())
    {
        if (auto written = _storage.SaveState(*output.persist); !written.has_value())
        {
            _failure = written.error();
            return std::unexpected { written.error() };
        }
    }

    if (output.persistLog.has_value())
    {
        if (auto written = _storage.SaveLog(*output.persistLog); !written.has_value())
        {
            _failure = written.error();
            return std::unexpected { written.error() };
        }
    }

    // Third, and still before anything is sent. A node that acknowledged a
    // snapshot it had not written would retract that acknowledgement on restart --
    // after a leader may already have counted it towards commitment -- and a node
    // that had discarded the entries it replaces would come back missing them
    // outright.
    if (output.saveSnapshot.has_value())
    {
        if (auto written = _storage.SaveSnapshot(*output.saveSnapshot); !written.has_value())
        {
            _failure = written.error();
            return std::unexpected { written.error() };
        }
    }

    for (auto& outbound: output.messages)
        _transport.Send(outbound.to, std::move(outbound.message));

    // Last: peers cannot make progress until the messages are out, and applying
    // is local.
    //
    // A restore REPLACES rather than advances, so it is delivered instead of the
    // applied entries and not alongside them -- the snapshot already includes
    // everything up to its index, and replaying entries over it would re-apply
    // what it contains.
    //
    // A leader's snapshot reaches here only once `CanRestore` accepted it (`Receive`), so
    // a refusal now is the application contradicting its own answer -- after the node has
    // persisted the snapshot and acknowledged it. Nothing sound follows from that: every
    // later entry would be applied on the state the snapshot was meant to replace, which
    // is #1552's defect by another road. So it stops the driver, as a storage failure does,
    // and says why.
    if (output.restoreSnapshot.has_value())
    {
        if (auto restored = _application.RestoreSnapshot(output.restoreSnapshot->state, SnapshotOrigin::Installed);
            !restored.has_value())
        {
            _failure = restored.error();
            return std::unexpected { std::move(restored).error() };
        }
    }
    else
        for (auto const& entry: output.applied)
            _application.Apply(entry);

    // A refusal ends when this node has applied past the snapshot it refused -- by one it
    // could read, or by entries a later leader still held. Asked here, where every path
    // that applies anything comes out.
    if (_installRefusal.has_value() && _node.LastApplied() >= _installRefusal->index)
        PublishInstallRefusal(std::nullopt);

    // Reported here and only here, which is the point of putting it at the end of
    // `Deliver` rather than in `Tick`, `Receive` and `Propose`: those are three ways
    // in and this is the one place all of them come out, so a fourth entry point
    // cannot forget to announce a role it changed.
    //
    // AFTER the outputs, deliberately. An observer told "you lead now" before the
    // vote that made it true had been persisted and the heartbeats sent would be
    // acting on a leadership this node had not yet established -- and if a storage
    // write above fails, the early return means it is never told at all, which is
    // correct: a node whose durable state would not write has not become anything.
    PublishRoleIfChanged(output.adoptedTerm);

    // And only now is there anything to compact: the entries this step applied are
    // what moved `LastApplied` past the snapshot boundary.
    return CompactIfDue();
}

std::expected<void, ConsensusError> RaftDriver::CompactIfDue()
{
    if (_compaction.appliedEntriesBeforeCompaction == 0)
        return {};

    // The unsnapshotted, already-applied span -- which is what a restart replays
    // and what the log holds in memory. `LastApplied` can sit AT the boundary (a
    // follower that has just installed a snapshot) but never below it, so the
    // subtraction is guarded rather than assumed.
    auto const applied = _node.LastApplied();
    auto const covered = _node.Log().SnapshotIndex();
    if (applied <= covered || applied.value - covered.value < _compaction.appliedEntriesBeforeCompaction)
        return {};

    auto output = RaftOutput {};
    if (!_node.CompactThroughApplied(_application.TakeSnapshot(), output))
        return {};

    // `CompactThroughApplied` sets this whenever it returns true; the check is here
    // because dereferencing on a contract rather than on a value is how a later
    // change to that contract becomes a crash instead of a compile error.
    if (!output.saveSnapshot.has_value())
        return {};

    // The one durability write this produces, and the store discards the covered
    // prefix as part of it -- snapshot first, entries afterwards, so a crash in
    // between leaves a durable snapshot beside a log that still holds what it
    // covers, which recovery reconciles.
    if (auto written = _storage.SaveSnapshot(*output.saveSnapshot); !written.has_value())
    {
        _failure = written.error();
        return std::unexpected { written.error() };
    }

    return {};
}

std::expected<void, ConsensusError> RaftDriver::Tick(TimePoint now)
{
    auto const guard = std::scoped_lock { _mutex };
    if (_failure.has_value())
        return std::unexpected { *_failure };

    return Deliver(_node.Tick(now));
}

std::expected<void, ConsensusError> RaftDriver::Receive(RaftMessage const& message, TimePoint now)
{
    auto const guard = std::scoped_lock { _mutex };
    if (_failure.has_value())
        return std::unexpected { *_failure };

    // A leader's snapshot is asked about BEFORE the node sees it (#1552), because the node
    // is where the answer has to take effect: taken on, a snapshot resets the log, moves
    // both indices and is persisted and acknowledged within this one step, and the
    // application is handed it only at the end. An application refusing it THEN would
    // hold its old state under an applied index that said otherwise.
    auto const* const install = std::get_if<InstallSnapshotRequest>(&message);
    auto unreadable = std::optional<ConsensusError> {};
    if (install != nullptr)
        if (auto readable = _application.CanRestore(install->state); !readable.has_value())
            unreadable = std::move(readable).error();

    auto output = _node.Receive(
        message, now, unreadable.has_value() ? SnapshotReadability::Unreadable : SnapshotReadability::Readable);
    auto const refused = output.refusedSnapshot;
    auto delivered = Deliver(std::move(output));

    // Only a refusal the node actually made is reported: a snapshot it already covered
    // needed no reading, and is answered as it always was.
    if (refused.has_value() && install != nullptr && unreadable.has_value())
        PublishInstallRefusal(
            InstallRefusal { .index = *refused, .leader = install->leaderId, .reason = *std::move(unreadable) });
    return delivered;
}

std::expected<LogIndex, ConsensusError> RaftDriver::Propose(std::vector<std::byte> payload, TimePoint now)
{
    auto const guard = std::scoped_lock { _mutex };
    if (_failure.has_value())
        return std::unexpected { *_failure };

    return Land(_node.Propose(std::move(payload), now));
}

std::expected<LogIndex, ConsensusError> RaftDriver::ProposeMembership(Configuration configuration, TimePoint now)
{
    auto const guard = std::scoped_lock { _mutex };
    if (_failure.has_value())
        return std::unexpected { *_failure };

    return Land(_node.ProposeMembership(std::move(configuration), now));
}

RaftDriver::Progress RaftDriver::CurrentProgress() const
{
    auto const guard = std::scoped_lock { _mutex };
    return Progress { .configuration = _node.ActiveConfiguration(),
                      .commitIndex = _node.CommitIndex(),
                      .term = _node.CurrentTerm(),
                      .role = _node.CurrentRole(),
                      // Copied out rather than referenced, like `configuration`: the
                      // timer loop and every peer reader move this, and the lock ends
                      // with this statement.
                      .knownLeader = _node.KnownLeader(),
                      .matchIndex = _node.MatchIndices(),
                      .installRefusal = _installRefusal };
}

std::expected<LogIndex, ConsensusError> RaftDriver::Land(std::expected<RaftNode::Proposal, ConsensusError> proposed)
{
    if (!proposed.has_value())
        return std::unexpected { proposed.error() };

    // The index is read BEFORE the output is moved from, which is the whole reason
    // this is one function rather than two: a proposal is a durability write and a
    // broadcast, and the value the caller wants lives in the object being handed
    // to the delivery step.
    auto const index = proposed->index;
    if (auto done = Deliver(std::move(proposed->output)); !done.has_value())
        return std::unexpected { done.error() };

    return index;
}

TimePoint RaftDriver::SleepDeadline(TimePoint now) const
{
    // The node owns its own deadline, so the loop never has to know whether it is
    // waiting on an election or a heartbeat -- and a spurious early wake-up costs
    // nothing, because `Tick` before the deadline does nothing.
    //
    // Bounded by the heartbeat interval, and that bound is the whole point. This
    // loop parks on a deadline read BEFORE it suspends, and `SleepUntil` cannot be
    // cancelled: it hands the coroutine to the reactor's timer wheel and nothing
    // takes it back. Something else can meanwhile move the node's deadline
    // *earlier* -- `Propose` from another thread, and decisively `Receive` from a
    // peer-reader coroutine sharing this very reactor, which is where the vote
    // that wins an election arrives. Winning turns an election deadline up to
    // `electionTimeoutMax` away into a heartbeat deadline one interval away. Sleeping to the stale
    // value then delays the new leader's SECOND heartbeat by most of an election
    // timeout, a follower whose randomized timeout is at the short end elects
    // itself, and the cluster does the same thing again one term later: measured
    // at nine role changes in twelve seconds on a healthy three-node cluster with
    // nothing else wrong. A leader is unaffected in cost, since it already wakes
    // at exactly this cadence; a follower wakes a few times per election timeout
    // and does nothing. This is the same answer `BlockingListener::SetTimeouts`
    // gives to the same shape of problem -- a wait nothing can interrupt is
    // bounded rather than left to be woken.
    auto const guard = std::scoped_lock { _mutex };
    return std::min(_node.NextDeadline(), now + _node.HeartbeatInterval());
}

Task<void> RaftDriver::Run(IReactor* reactor)
{
    while (reactor != nullptr && !_stopped.load(std::memory_order_relaxed) && !Failure().has_value())
    {
        reactor->clock().refresh();
        auto const now = reactor->clock().now();
        (void) Tick(now);

        // The loop condition is the stop check: `Stop` during the sleep is seen
        // when it returns, and the bound above is what makes that at most one
        // heartbeat interval rather than most of an election timeout.
        co_await SleepUntil { .reactor = reactor, .deadline = SleepDeadline(now) };
    }
}

} // namespace FastCache::Consensus
