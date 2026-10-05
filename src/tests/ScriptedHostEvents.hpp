// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/HostEvents.hpp>

#include <cstddef>

namespace FastCache::Testing
{

/// The host, scripted: a case fires the event the OS would have, and reads who is listening.
///
/// ONE fake for every consumer, for `ScriptedSocket.hpp`'s reason. It delivers through the
/// production `HostEventHub` rather than a list of its own, so the contract a consumer is tested
/// against -- `Unsubscribe` waits for a delivery in flight, a sink may leave from inside its own
/// -- is the one it gets in production; a copy that skipped the wait would pass every case a
/// consumer's teardown race could fail. `Fire` delivers on the CALLING thread, as the SCM's
/// control handler does, so a sink that blocks (a suspend waiting for its withdrawal) blocks the
/// case exactly as it would block the handler.
class ScriptedHostEvents final: public IHostEvents
{
  public:
    void Subscribe(IHostEventSink& sink) override
    {
        _hub.Subscribe(sink);
    }

    void Unsubscribe(IHostEventSink& sink) noexcept override
    {
        _hub.Unsubscribe(sink);
    }

    /// Deliver @p event to every subscriber, in subscription order.
    /// @param event What the host said.
    void Fire(HostEvent event)
    {
        _hub.OnHostEvent(event);
    }

    /// @return How many sinks listen now -- what a wiring case asserts.
    [[nodiscard]] std::size_t SubscriberCount() const
    {
        return _hub.SubscriberCount();
    }

  private:
    HostEventHub _hub;
};

} // namespace FastCache::Testing
