// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>

#include <utility>

namespace FastCache::Consensus
{

RaftDriver::RaftDriver(RaftNode node,
                       IRaftStorage& storage,
                       IRaftTransport& transport,
                       IRaftStateMachine& application) noexcept:
    _node { std::move(node) },
    _storage { storage },
    _transport { transport },
    _application { application }
{
}

RaftNode const& RaftDriver::Node() const noexcept
{
    return _node;
}

TimePoint RaftDriver::NextDeadline() const noexcept
{
    return _node.NextDeadline();
}

std::optional<ConsensusError> const& RaftDriver::Failure() const noexcept
{
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

    for (auto& outbound: output.messages)
        _transport.Send(outbound.to, std::move(outbound.message));

    // Last: peers cannot make progress until the messages are out, and applying
    // is local.
    for (auto const& entry: output.applied)
        _application.Apply(entry);

    return {};
}

std::expected<void, ConsensusError> RaftDriver::Tick(TimePoint now)
{
    if (_failure.has_value())
        return std::unexpected { *_failure };

    return Deliver(_node.Tick(now));
}

std::expected<void, ConsensusError> RaftDriver::Receive(RaftMessage const& message, TimePoint now)
{
    if (_failure.has_value())
        return std::unexpected { *_failure };

    return Deliver(_node.Receive(message, now));
}

std::expected<LogIndex, ConsensusError> RaftDriver::Propose(std::vector<std::byte> payload, TimePoint now)
{
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
    while (reactor != nullptr && !_stopped.load(std::memory_order_relaxed) && !_failure.has_value())
    {
        // The node owns its own deadline, so the loop never has to know whether
        // it is waiting on an election or a heartbeat -- and a spurious early
        // wake-up costs nothing, because `Tick` before the deadline does nothing.
        co_await SleepUntil { .reactor = reactor, .deadline = _node.NextDeadline() };

        if (_stopped.load(std::memory_order_relaxed))
            break;

        reactor->Clock().Refresh();
        (void) Tick(reactor->Clock().Now());
    }
}

} // namespace FastCache::Consensus
