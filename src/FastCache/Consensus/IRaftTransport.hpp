// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>

namespace FastCache::Consensus
{

/// How a node reaches its peers.
///
/// **Best-effort and non-blocking, returning nothing.** That is the shape Raft
/// wants and it is worth being explicit about, because a `std::expected` here
/// would invite a caller to treat a send failure as something to handle. It is
/// not: the algorithm already assumes every message may be lost, duplicated,
/// delayed or reordered, and it recovers by retrying on the next heartbeat.
/// A driver that waited for a send to complete would let one unreachable
/// follower stall a leader that has a quorum without it — turning a fault Raft
/// tolerates into one it does not.
///
/// Messages are **typed rather than encoded** here. Serialization belongs with
/// the transport that needs it; keeping it out of this seam is what lets a whole
/// cluster run in one process over direct calls, which is how the algorithm gets
/// tested against partitions and reordering at all.
class IRaftTransport
{
  public:
    IRaftTransport() = default;
    IRaftTransport(IRaftTransport const&) = delete;
    IRaftTransport(IRaftTransport&&) = delete;
    IRaftTransport& operator=(IRaftTransport const&) = delete;
    IRaftTransport& operator=(IRaftTransport&&) = delete;
    virtual ~IRaftTransport() = default;

    /// Send `message` toward `to`, or drop it.
    /// @param to The member it is addressed to.
    /// @param message What to send.
    virtual void Send(NodeId const& to, RaftMessage message) = 0;
};

} // namespace FastCache::Consensus
