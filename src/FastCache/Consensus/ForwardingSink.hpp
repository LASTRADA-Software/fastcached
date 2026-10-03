// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftMessageSink.hpp>

#include <atomic>
#include <cstdint>
#include <utility>

namespace FastCache::Consensus
{

/// A sink bound to its target AFTER construction: the transport exists before the driver it
/// delivers into.
///
/// `RaftDriver::Create` takes the transport, so the transport is built first -- and a learner's
/// transport takes a sink, because it reads what an acceptor writes back. This breaks that cycle
/// without a null reference: the transport is handed this, and this is bound to the driver's sink
/// once the driver exists.
///
/// ## A message before the bind is DROPPED, never dereferenced
///
/// The caller binds before it starts the transport and asserts that it did (`Bound()`), so
/// nothing arrives unbound in a correctly wired process. Should something -- a reordering nobody
/// meant -- the message is dropped rather than delivered through a null pointer: a crash on the
/// reactor's thread would take every peer with it.
///
/// A drop there is recoverable only while it is TRANSIENT. Raft retransmits whatever it lost, so
/// a message or two before a late bind costs nothing; a sink left unbound drops EVERY message, and
/// a learner behind it never catches up -- which nothing here reports, so the precondition is the
/// guard, not the drop.
///
/// ## `DroppedUnbound()` is an in-process observation, not a series
///
/// No metrics surface renders it, and no `MetricsCatalog` row should: the only thing that can
/// move it is a wiring defect in the same process, so in every correct binary it would read zero
/// forever -- the plausible zero a row no writer in the process could move must not be rendered
/// as. It exists for a test to ask, and for a debugger.
class ForwardingSink final: public IRaftMessageSink
{
  public:
    ForwardingSink() = default;
    ForwardingSink(ForwardingSink const&) = delete;
    ForwardingSink(ForwardingSink&&) = delete;
    ForwardingSink& operator=(ForwardingSink const&) = delete;
    ForwardingSink& operator=(ForwardingSink&&) = delete;
    ~ForwardingSink() override = default;

    /// Name where every later message goes. Once, before the transport is started.
    /// @param target Must outlive every `Deliver`.
    void Bind(IRaftMessageSink& target) noexcept
    {
        _target.store(&target, std::memory_order_release);
    }

    /// @return Whether `Bind` has run: what a caller asserts before it starts the transport.
    [[nodiscard]] bool Bound() const noexcept
    {
        return _target.load(std::memory_order_acquire) != nullptr;
    }

    /// Hand @p message to the bound target, or drop and count it when there is none yet.
    /// @param message The decoded message.
    void Deliver(RaftMessage message) override
    {
        auto* const target = _target.load(std::memory_order_acquire);
        if (target == nullptr)
        {
            _droppedUnbound.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        target->Deliver(std::move(message));
    }

    /// @return How many messages arrived before `Bind`, each dropped. Read by tests; see above.
    [[nodiscard]] std::uint64_t DroppedUnbound() const noexcept
    {
        return _droppedUnbound.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<IRaftMessageSink*> _target { nullptr };
    std::atomic<std::uint64_t> _droppedUnbound { 0 };
};

} // namespace FastCache::Consensus
