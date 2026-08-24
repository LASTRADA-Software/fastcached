// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/Cancellation.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/IRaftTransport.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IConnector.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
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

/// What learning a peer did.
///
/// Named rather than reported as a `bool`, because a caller reconciling a member
/// set against what it already dials wants to log the two interesting cases and
/// say nothing at all about the third — and "a peer that moved" and "a peer nobody
/// knew" are different sentences to an operator reading why a cluster changed
/// shape.
enum class PeerChange : std::uint8_t
{
    Added = 0,   ///< A peer nobody knew; its sender starts now.
    Readdressed, ///< A known peer that has moved; the live connection is dropped.
    Unchanged,   ///< Already known, at exactly this address.

    /// This node's own record, which a member set always contains.
    ///
    /// Its own row rather than a shared "refused", because it is the one outcome a
    /// caller must stay silent about: a reconciler hands over the whole member set
    /// on every pass, so folding it in with the row below would make a healthy
    /// fleet log its own id once a second forever -- or make an operator suppress
    /// the line that says the transport is shutting down.
    Self,

    Stopping, ///< The transport is stopping; nothing new is dialled.
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

    /// Longest a sender may sit in a backoff it cannot be woken from.
    ///
    /// Teardown lag, in one number. `IReactor::Schedule` cannot be cancelled, so
    /// a backoff is slept in steps of this length with the stop re-read at each
    /// one: a stop is observed within this bound rather than after
    /// `reconnectBackoff`, which is what keeps shutdown independent of how
    /// unreachable a peer happens to be. Costs one wake-up per bound per
    /// unreachable peer, each of which loads one atomic and re-parks.
    std::chrono::milliseconds stopWakeBound { 50 };
};

/// `IRaftTransport` over real sockets, one coroutine per peer on the reactor.
///
/// ## This used to be a thread per peer, and the note said why
///
/// It said the reactor exists for thousands of clients this process did not
/// dial, while peers are a handful known at configuration time -- and that
/// "dialling is the operation the reactor has no seam for, and adding
/// non-blocking connect to three backends to save five threads would be paying
/// the cost in the wrong currency". Three things were wrong with that.
///
/// The seam is not hypothetical and consensus is not paying for it:
/// `IConnector` is coroutine-shaped and `PlatformConnector` implements it on all
/// three backends, built for the launcher and the health probe.
///
/// It was never about saving threads. Thread-per-peer is only *expressible* over
/// a blocking socket, because the write was driven by `SyncRun` -- and this
/// codebase already records, as the first of five defects found the first time
/// consensus was run, that `SyncRun` throws the instant its task really
/// suspends. So the decision pinned the outbound half of this component to
/// `BlockingSocket` for good while the inbound half was already on the reactor:
/// two socket implementations in one class, chosen by direction.
///
/// And it hid a hang. The transport passes no I/O timeout, deliberately -- see
/// `dialTimeout` -- so over a blocking socket there is no `SO_SNDTIMEO`, and a
/// peer that accepts and then stops reading parks the sender in `::send` once the
/// socket buffer fills. `Stop()` cleared the outbox, notified the condition
/// variable, and then joined a thread that was not waiting on it. The socket was
/// a local of the sender, so nothing else could close it. `~RaftPeerTransport`
/// blocked forever and the node died to SIGKILL.
///
/// The rule the old note leaned on is **preserved, not abandoned**: no reactor
/// thread ever blocks. Nothing in the new sender blocks; it suspends. What
/// changes is that one component now has one lifecycle model, one socket
/// implementation, and closes what it opened.
///
/// ## Send never blocks and may drop
///
/// `Send` appends to a bounded queue and returns. It does not wait for a
/// connection, a write, or an acknowledgement, because a driver that waited
/// would let one unreachable follower stall a leader that has a quorum without
/// it -- turning a fault Raft tolerates into one it does not. When the queue is
/// full the **oldest** message is dropped rather than the newest: what is waiting
/// behind a dead connection is stale by definition, and the newest AppendEntries
/// subsumes every older one for that peer.
///
/// It also never resumes the sender inline. `Send` is reached from
/// `RaftDriver::Deliver`, which holds the driver's mutex, and a queue that
/// resumed its consumer there would run the sender's next step inside that lock
/// -- see `AsyncQueue`, where that invariant lives.
class RaftPeerTransport final: public IRaftTransport
{
  public:
    /// Construct over its collaborators; all must outlive the transport.
    /// @param self This node's own id, so a message addressed to it is refused
    ///        rather than looped through a socket.
    /// @param peers Where each other member can be reached.
    /// @param reactor The loop every sender runs on. Named before the connector
    ///        because the connector's sockets are pinned to it.
    /// @param connector How to dial; injected so tests need no network. Must be
    ///        one whose sockets belong to `reactor`.
    /// @param logger Where connection state changes are reported.
    /// @param options Timeouts and queue bound.
    RaftPeerTransport(NodeId self,
                      std::vector<PeerEndpoint> peers,
                      IReactor& reactor,
                      IConnector& connector,
                      ILogger& logger,
                      PeerTransportOptions options = {});

    RaftPeerTransport(RaftPeerTransport const&) = delete;
    RaftPeerTransport(RaftPeerTransport&&) = delete;
    RaftPeerTransport& operator=(RaftPeerTransport const&) = delete;
    RaftPeerTransport& operator=(RaftPeerTransport&&) = delete;

    /// Stops every sender and waits for it. Safe to call after `Stop()`.
    ~RaftPeerTransport() override;

    /// Begin dialling peers. Idempotent.
    ///
    /// Each sender is a **lazy** `Task` submitted to the reactor rather than a
    /// `DetachedTask`, and that is not a style choice: a detached task starts
    /// eagerly on whichever thread constructed it, which here is whatever calls
    /// `Start()` -- typically before the reactor's own thread exists. Submitting
    /// a lazy task means the body's first instruction runs on the reactor thread,
    /// which is what makes "a peer's socket is touched only from the reactor"
    /// true, and everything else here follows from that.
    void Start();

    /// Learn where a peer answers, or that it has moved. Any thread.
    ///
    /// **This is what makes a cluster growable at all.** The member set used to be
    /// fixed at construction, so a node the cluster agreed to admit at runtime was
    /// one nobody dialled — counted towards a quorum it could never contribute to,
    /// which is a cluster that stops forming one. Adding a peer here is the other
    /// half of `RaftNode::ProposeMembership`, and neither is safe without the
    /// other.
    ///
    /// Idempotent, so the natural caller — a reconciler comparing the cluster's
    /// replicated member set against what this node dials, on every pass of its own
    /// loop — hands over the same peers repeatedly and grows nothing.
    ///
    /// A peer that has **moved** keeps its outbox and its queued messages and is
    /// simply dialled somewhere else: its live connection is closed, so the next
    /// write fails, the sender backs off exactly as it does for any dropped
    /// connection, and it redials the new address. Replacing the peer outright
    /// would mean destroying a coroutine frame the reactor may still point into,
    /// which is the one thing that cannot be done from here.
    ///
    /// The redial therefore happens at the next *message* rather than at once: a
    /// sender parked on its outbox is not woken by a socket closing under it. That
    /// costs a member nothing — the next heartbeat is one interval away — and a
    /// peer nobody is sending to does not care which address it is not being sent
    /// to.
    ///
    /// A peer is never *forgotten*, and that is deliberate. A member removed from
    /// the configuration is sent nothing, so its sender parks on an outbox nobody
    /// pushes to and costs one idle connection until this process restarts. Ending
    /// it early needs a per-peer cancellation and a sweep for frames that have
    /// finished, which is machinery bought for a socket.
    /// @param where The peer and where it answers.
    /// @return What this did.
    PeerChange Learn(PeerEndpoint where);

    /// @return How many peers this transport currently dials, self excluded.
    [[nodiscard]] std::size_t PeerCount() const noexcept;

    /// Ask every sender to finish, without waiting. Any thread, including the
    /// reactor's.
    ///
    /// Three things, in this order, because each closes a different suspension
    /// point: cancel the stop token (which wakes every backoff within
    /// `stopWakeBound`), close every outbox (which wakes a sender parked on
    /// `Pop` at once), and close every live socket **on the reactor's thread**
    /// (which is the only thing that completes a parked write, since no I/O
    /// timeout is armed). The last must be on that thread because on epoll and
    /// kqueue `ISocket::Close` completes a parked awaitable by resuming its
    /// coroutine inline -- so closing from elsewhere would run a sender on the
    /// wrong thread. IOCP routes cancellation back through the port and does
    /// not, which is exactly what would make that a bug visible only on POSIX.
    void RequestStop() noexcept;

    /// `RequestStop()`, then wait for every sender to finish. Idempotent, and
    /// called by the destructor.
    ///
    /// **Must not be called from the reactor's thread**: it waits for coroutines
    /// that can only progress there. Bounded rather than indefinite, because a
    /// stuck peer must not turn a stop into a hang -- the shape this transport
    /// used to have.
    void Stop() noexcept;

    /// @return How many peer senders have not yet finished. For teardown
    ///         assertions; a production caller reading this is asking a question
    ///         whose answer is stale before it returns.
    [[nodiscard]] std::size_t SendersRunning() const noexcept;

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
    /// One peer's outbox and the coroutine that drains it.
    ///
    /// Non-movable, because it owns an `AsyncQueue` (which holds a mutex) and a
    /// `Task`. The `unique_ptr` indirection in `_peers` is therefore
    /// load-bearing rather than incidental -- worth saying, because flattening
    /// the map to hold `Peer` by value is the obvious tidy-up and it does not
    /// compile.
    struct Peer
    {
        /// Where to dial.
        ///
        /// The `id` is fixed at construction — it is this peer's key in `_peers` —
        /// while `host` and `port` are guarded by `_peersMutex`, because a peer
        /// that moves is re-addressed in place rather than replaced. A sender reads
        /// them through `AddressOf` rather than directly, so the copy it dials with
        /// is taken under that lock and the `co_await` is not.
        PeerEndpoint endpoint;

        AsyncQueue<std::vector<std::byte>> outbox; ///< Framed messages awaiting the wire.

        /// The live connection, or null.
        ///
        /// **Reactor-thread only.** Written by this peer's sender and read by the
        /// shutdown closer, which hops onto the reactor before touching it -- so
        /// there is nothing here to synchronise. That is the property the lazy
        /// `Task` in `Start()` buys.
        std::unique_ptr<ISocket> socket;

        /// The sender. Lazy, so it cannot start on whichever thread called
        /// `Start()`.
        Task<void> sender;

        /// @param where Where to dial.
        /// @param reactor Loop the outbox posts wake-ups to.
        /// @param options Queue bound and overflow policy.
        Peer(PeerEndpoint where, IReactor& reactor, AsyncQueueOptions options):
            endpoint { std::move(where) },
            outbox { reactor, options }
        {
        }
    };

    /// Where `peer` currently answers, read under `_peersMutex`.
    ///
    /// By value, because the point is to stop holding the lock before the dial
    /// suspends. A sender that read `peer->endpoint` directly would be racing
    /// `Learn`, which re-addresses a peer that has moved.
    /// @param peer Whose address to read.
    /// @return A copy of it.
    [[nodiscard]] PeerEndpoint AddressOf(Peer const& peer) const;

    /// Submit `peer`'s sender to the reactor and count it. `_peersMutex` must be
    /// held.
    /// @param peer The peer to start dialling.
    void StartSender(Peer& peer);

    /// Note that a sender has finished. Reactor thread only.
    void NoteSenderFinished() noexcept;

    /// Report a sender that threw, without throwing. Reactor thread only.
    /// @param peer Which peer's sender it was.
    void NoteSenderThrew(NodeId const& peer) noexcept;

    friend struct PeerSenderAccess;

    NodeId _self;
    IReactor& _reactor;
    IConnector& _connector;
    ILogger& _logger;
    PeerTransportOptions _options;

    /// Cancelled by `RequestStop`; observed by every backoff and loop condition.
    CancellationSource _stop;

    /// Guards `_peers`, each peer's `host`/`port`, and `_lifecycle`.
    ///
    /// `_peers` used to be built at construction and never touched again, so
    /// everything read it unlocked — and a cluster could not grow, because the one
    /// thing this class could not do was learn a member the cluster had just
    /// agreed to admit. It is now written by `Learn` from whatever thread
    /// reconciles the member set, and read by `Send` from the driver's, so the
    /// map needs a lock.
    ///
    /// **Shared**, for the reason `Distributed::MembershipOracle` gives for the
    /// identical shape: the readers are `Send` (once per peer per heartbeat, and
    /// reached while the driver's own mutex is held) and each sender's dial, while
    /// the writer is a reconciler pass that on a healthy fleet changes nothing.
    /// Serializing those readers against each other over a map none of them
    /// modifies would put the reactor's thread in front of the driver's for no
    /// reason at all.
    ///
    /// Held only across the map operation, and never across a push, a dial, a write
    /// or a socket close: `ISocket::Close` resumes a parked sender *inline* on epoll
    /// and kqueue, so a lock held across it would be held across arbitrary sender
    /// code — with `Send`, and therefore the driver's mutex, waiting behind it.
    /// Taking a `Peer*` under the lock and using it outside is safe because a peer
    /// is never *erased*: the pointer a lookup hands back stays valid for this
    /// transport's life.
    mutable std::shared_mutex _peersMutex;

    /// Peers by id, self excluded.
    std::map<NodeId, std::unique_ptr<Peer>> _peers;

    /// Where this transport is in its life, read and written under `_peersMutex`.
    ///
    /// One value rather than a `bool` beside the cancellation token, because `Learn`
    /// has one question with three answers: a peer added before `Start` must be
    /// submitted by `Start`, one added after must submit itself, and one added after
    /// a stop must be refused — otherwise it is either never dialled, dialled twice,
    /// or left as a sender nobody ever finishes. Deciding that against two variables
    /// guarded by different things is an interlock held together by a comment.
    enum class Lifecycle : std::uint8_t
    {
        Idle = 0, ///< Constructed; `Start` has not run.
        Running,  ///< Senders are submitted as peers are learned.
        Stopping, ///< `RequestStop` has run; nothing new is dialled.
    };

    Lifecycle _lifecycle { Lifecycle::Idle };
    std::atomic<std::uint64_t> _dropped { 0 };
    std::atomic<std::size_t> _connected { 0 };

    /// How many senders are still running. The cross-thread view of the same
    /// question `Task::IsReady` answers, which cannot be asked from another
    /// thread while the reactor may be resuming it.
    std::atomic<std::size_t> _sendersRunning { 0 };

    /// The reactor's own thread, recorded the first time a sender runs, so
    /// `Stop()` can assert it is not being called from there -- a wait for
    /// coroutines that only that thread can advance. Turns a hang into a failed
    /// assertion in the debug preset for the price of one comparison.
    std::atomic<std::thread::id> _reactorThread {};
};

} // namespace FastCache::Consensus
