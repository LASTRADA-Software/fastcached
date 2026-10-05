// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftSessionLink.hpp>

#include <memory>

namespace FastCache::Consensus
{

/// Where an acceptor hands the sessions it may write on. Implemented by `RaftPeerTransport`.
///
/// A learner sits behind NAT, a VPN or a laptop lid, so nobody can dial it: it dials the voters
/// two-way instead, and the session it opened is the only way back to it. `RaftPeerServer`
/// proves that session and hands it here; the transport's `Send` then writes on it.
///
/// Called on the reactor both objects run on, from the connection's own coroutine, so an
/// implementation never sees an attach and the detach of the same link race each other.
class IRaftInboundLinks
{
  public:
    IRaftInboundLinks() = default;
    IRaftInboundLinks(IRaftInboundLinks const&) = delete;
    IRaftInboundLinks(IRaftInboundLinks&&) = delete;
    IRaftInboundLinks& operator=(IRaftInboundLinks const&) = delete;
    IRaftInboundLinks& operator=(IRaftInboundLinks&&) = delete;
    virtual ~IRaftInboundLinks() = default;

    /// A two-way session proved `link->Peer()`; replaces any earlier link for that peer.
    /// @param link The session; never null.
    virtual void Attach(std::shared_ptr<RaftSessionLink> link) = 0;

    /// That session ended; a later link for the same peer is left alone.
    /// @param link The session that ended, as it was attached.
    virtual void Detach(RaftSessionLink const& link) noexcept = 0;
};

} // namespace FastCache::Consensus
