// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>

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

TimePoint RaftDriver::NextDeadline() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _node.NextDeadline();
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

    auto proposed = _node.Propose(std::move(payload), now);
    if (!proposed.has_value())
        return std::unexpected { proposed.error() };

    auto const index = proposed->index;
    if (auto done = Deliver(std::move(proposed->output)); !done.has_value())
        return std::unexpected { done.error() };

    return index;
}

Task<void> RaftDriver::Run(IReactor* reactor)
{
    while (reactor != nullptr && !_stopped.load(std::memory_order_relaxed) && !Failure().has_value())
    {
        // The node owns its own deadline, so the loop never has to know whether
        // it is waiting on an election or a heartbeat -- and a spurious early
        // wake-up costs nothing, because `Tick` before the deadline does nothing.
        //
        // Read through the accessor, which takes the lock: another thread may be
        // mid-`Propose` and moving the very deadline this is about to sleep on.
        co_await SleepUntil { .reactor = reactor, .deadline = NextDeadline() };

        if (_stopped.load(std::memory_order_relaxed))
            break;

        reactor->Clock().Refresh();
        (void) Tick(reactor->Clock().Now());
    }
}

} // namespace FastCache::Consensus
