// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>

#include <algorithm>
#include <mutex>
#include <utility>

namespace FastCache::Consensus
{

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
    _reportedLeader { _node.KnownLeader() }
{
}

void RaftDriver::ObserveRole(RoleObserver observer)
{
    _onRole = std::move(observer);
}

void RaftDriver::PublishRoleIfChanged()
{
    auto const role = _node.CurrentRole();
    auto const& leader = _node.KnownLeader();
    if (role == _reportedRole && leader == _reportedLeader)
        return;

    _reportedRole = role;
    _reportedLeader = leader;
    if (_onRole)
        _onRole(role, leader);
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
    if (output.restoreSnapshot.has_value())
        _application.RestoreSnapshot(output.restoreSnapshot->state);
    else
        for (auto const& entry: output.applied)
            _application.Apply(entry);

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
    PublishRoleIfChanged();

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

    return Deliver(_node.Receive(message, now));
}

std::expected<LogIndex, ConsensusError> RaftDriver::Propose(std::vector<std::byte> payload, TimePoint now)
{
    auto const guard = std::scoped_lock { _mutex };
    if (_failure.has_value())
        return std::unexpected { *_failure };

    return Land(_node.Propose(std::move(payload), now));
}

std::expected<LogIndex, ConsensusError> RaftDriver::ProposeMembership(std::vector<NodeId> members, TimePoint now)
{
    auto const guard = std::scoped_lock { _mutex };
    if (_failure.has_value())
        return std::unexpected { *_failure };

    return Land(_node.ProposeMembership(std::move(members), now));
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
        reactor->Clock().Refresh();
        auto const now = reactor->Clock().Now();
        (void) Tick(now);

        // The loop condition is the stop check: `Stop` during the sleep is seen
        // when it returns, and the bound above is what makes that at most one
        // heartbeat interval rather than most of an election timeout.
        co_await SleepUntil { .reactor = reactor, .deadline = SleepDeadline(now) };
    }
}

} // namespace FastCache::Consensus
