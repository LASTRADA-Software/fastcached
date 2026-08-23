// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftOutput.hpp>

namespace FastCache::Consensus
{

/// Where a message that arrived from a peer is handed on.
///
/// An interface rather than a `std::function`, for the reason the project prefers
/// seams generally and one specific to this shape: the sink is a long-lived
/// collaborator holding a `RaftDriver`, and a closure would capture whatever
/// enclosing scope built it and keep that alive for the server's lifetime.
///
/// A header of its own, like every other seam here — `IRaftStorage`,
/// `IRaftTransport`, `IRaftStateMachine`, `IConnector`. It began inside
/// `RaftPeerServer.hpp`, which made implementing it require including the one
/// concrete class that consumes it: an edge pointing the wrong way, and the
/// daemon that will implement this has no business knowing the server's frame
/// caps or its accept loop.
class IRaftMessageSink
{
  public:
    IRaftMessageSink() = default;
    IRaftMessageSink(IRaftMessageSink const&) = delete;
    IRaftMessageSink(IRaftMessageSink&&) = delete;
    IRaftMessageSink& operator=(IRaftMessageSink const&) = delete;
    IRaftMessageSink& operator=(IRaftMessageSink&&) = delete;
    virtual ~IRaftMessageSink() = default;

    /// Act on one message from a peer.
    ///
    /// Returns nothing: the wire is one-way and best-effort, so there is no reply
    /// to produce and nothing the reader could do with a refusal. A node that
    /// disagrees answers by sending its own message, not by failing this call.
    /// @param message The decoded message.
    virtual void Deliver(RaftMessage message) = 0;
};

} // namespace FastCache::Consensus
