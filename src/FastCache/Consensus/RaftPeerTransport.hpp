// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftTransport.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IConnector.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace FastCache::Consensus
{

/// Where a peer can be reached.
struct PeerEndpoint
{
    NodeId id;                ///< The member this address belongs to.
    std::string host;         ///< Hostname or literal address.
    std::uint16_t port { 0 }; ///< TCP port of that node's peer listener.
};

/// How long the transport waits and how much it will hold, in one place.
///
/// A struct rather than five constructor parameters because they are one
/// decision — how patient this node is with an unreachable peer — and because a
/// caller reading `.dialTimeout` at the call site cannot transpose it with
/// `.reconnectBackoff`, which two adjacent `milliseconds` arguments invite.
struct PeerTransportOptions
{
    /// How long a single dial may take before it is abandoned.
    ///
    /// Bounded rather than left to the OS, whose default runs to minutes: a
    /// sender that cannot abandon a dial cannot notice the peer coming back, and
    /// cannot be stopped.
    std::chrono::milliseconds dialTimeout { 1000 };

    /// How long to wait after a failed attempt before dialling again.
    ///
    /// Flat rather than exponential, deliberately. Raft's whole recovery story
    /// is that a partition heals and replication resumes on the next heartbeat;
    /// a backoff that grows to minutes turns a five-second network blip into a
    /// cluster that stays degraded long after the network is fine. The cost of
    /// being wrong in this direction is one connect attempt per interval per
    /// unreachable peer, which is nothing.
    std::chrono::milliseconds reconnectBackoff { 250 };

    /// How many messages may wait for one peer before the oldest are dropped.
    ///
    /// A bound, not a target. `IRaftTransport` is best-effort by contract, so
    /// dropping is *correct* rather than merely tolerable — the algorithm
    /// assumes any message may be lost and recovers on the next heartbeat. What
    /// is not tolerable is unbounded growth: a peer that is down for an hour
    /// would otherwise accumulate an hour of heartbeats, and the memory that
    /// costs is memory the compile cache wanted.
    std::size_t maxQueuedPerPeer { 256 };
};

/// `IRaftTransport` over real sockets, one sender thread per peer.
///
/// ## Why a thread per peer rather than the reactor
///
/// The reactor exists so the number of concurrent *clients* is bounded by memory
/// rather than by a worker count — thousands of them, none of which this process
/// dialled. Peers are the opposite: a handful, known by name at configuration
/// time, and reached by dialling. Dialling is the operation the reactor has no
/// seam for, and adding non-blocking connect to three backends to save five
/// threads would be paying the cost in the wrong currency.
///
/// The rule that matters is the one this preserves: **no reactor thread ever
/// blocks**. Each peer's sender owns its dial, its socket and its backoff, and
/// the reactor never sees any of it.
///
/// ## Send never blocks and may drop
///
/// `Send` appends to a bounded queue and returns. It does not wait for a
/// connection, a write, or an acknowledgement, because a driver that waited
/// would let one unreachable follower stall a leader that has a quorum without
/// it — turning a fault Raft tolerates into one it does not. When the queue is
/// full the **oldest** message is dropped rather than the newest: what is
/// waiting behind a dead connection is stale by definition, and the newest
/// AppendEntries subsumes every older one for that peer.
class RaftPeerTransport final: public IRaftTransport
{
  public:
    /// Construct over its collaborators; all must outlive the transport.
    /// @param self This node's own id, so a message addressed to it is refused
    ///        rather than looped through a socket.
    /// @param peers Where each other member can be reached.
    /// @param connector How to dial; injected so tests need no network.
    /// @param logger Where connection state changes are reported.
    /// @param options Timeouts and queue bound.
    RaftPeerTransport(NodeId self,
                      std::vector<PeerEndpoint> peers,
                      IConnector& connector,
                      ILogger& logger,
                      PeerTransportOptions options = {});

    RaftPeerTransport(RaftPeerTransport const&) = delete;
    RaftPeerTransport(RaftPeerTransport&&) = delete;
    RaftPeerTransport& operator=(RaftPeerTransport const&) = delete;
    RaftPeerTransport& operator=(RaftPeerTransport&&) = delete;

    /// Stops every sender and joins it. Safe to call after `Stop()`.
    ~RaftPeerTransport() override;

    /// Begin dialling peers. Idempotent.
    void Start();

    /// Stop every sender and join. Idempotent, and called by the destructor.
    ///
    /// Bounded by construction: a sender waits on a condition variable rather
    /// than sleeping, so a stop is observed at once rather than after the
    /// backoff elapses. An unbounded wait here would make shutdown depend on how
    /// unreachable a peer happens to be.
    void Stop() noexcept;

    /// @copydoc IRaftTransport::Send
    void Send(NodeId const& to, RaftMessage message) override;

    /// How many messages have been dropped for want of queue space.
    ///
    /// Exposed because a drop is invisible otherwise: Raft recovers from loss,
    /// so a fleet dropping steadily looks healthy while running slower than it
    /// should. A non-zero and growing count is an operator's signal that a peer
    /// is not keeping up.
    /// @return The cumulative drop count across all peers.
    [[nodiscard]] std::uint64_t DroppedMessages() const noexcept
    {
        return _dropped.load(std::memory_order_relaxed);
    }

    /// How many peers currently hold an open connection.
    /// @return The connected peer count.
    [[nodiscard]] std::size_t ConnectedPeers() const noexcept
    {
        return _connected.load(std::memory_order_relaxed);
    }

  private:
    /// One peer's outbox and the thread that drains it.
    ///
    /// Each peer owns its mutex rather than sharing one: a leader sends to every
    /// follower in the same output, and a single lock would serialize those
    /// appends behind whichever peer's sender happened to hold it.
    struct Peer
    {
        PeerEndpoint endpoint;                     ///< Where to dial.
        std::mutex mutex;                          ///< Guards `outbox`.
        std::condition_variable wake;              ///< Signalled on a new message or on stop.
        std::deque<std::vector<std::byte>> outbox; ///< Framed messages awaiting the wire.
        std::jthread worker;                       ///< Drains `outbox`; joined by `Stop`.
    };

    /// Drain one peer's outbox until stopped.
    /// @param peer The peer to serve.
    void RunSender(Peer& peer) noexcept;

    NodeId _self;
    IConnector& _connector;
    ILogger& _logger;
    PeerTransportOptions _options;

    /// Peers by id. Built once at construction and never mutated afterwards, so
    /// `Send` reads it without a lock; membership changes replace the transport
    /// rather than mutating it under a running sender.
    std::map<NodeId, std::unique_ptr<Peer>> _peers;

    std::atomic<bool> _running { false };
    std::atomic<bool> _stopping { false };
    std::atomic<std::uint64_t> _dropped { 0 };
    std::atomic<std::size_t> _connected { 0 };
};

} // namespace FastCache::Consensus
