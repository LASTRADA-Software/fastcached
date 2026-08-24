// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/InterruptibleSleep.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftWire.hpp>

#include <cassert>
#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace FastCache::Consensus
{

namespace
{

    /// Write every byte of `bytes` to `socket`.
    ///
    /// A coroutine because `ISocket::Write` is awaitable, awaited by the peer's
    /// sender on the reactor. It used to be driven with `SyncRun`, which was sound
    /// only because the socket was a blocking one; now it really suspends, which
    /// is what makes a write completable by closing the socket rather than only by
    /// the peer reading it.
    ///
    /// The socket arrives by **pointer rather than reference**, which reads as a
    /// stylistic slip and is not: a coroutine's reference parameter is bound
    /// before the first suspension and then outlives every frame that could have
    /// kept it alive. `RaftDriver::Run` takes its reactor the same way, for the
    /// same reason.
    ///
    /// `ISocket::Write` is write-all by contract -- "Resolves with the byte count
    /// actually written (== buffer.size() on success)" -- so the count is checked
    /// rather than looped over. A resume loop here would be dead code that reads as
    /// a statement about the interface, and a reader who believed it would write
    /// partial-write handling at every other call site too; the three that already
    /// exist just check the count.
    /// @param socket The connected peer socket; never null.
    /// @param bytes The framed message.
    /// @return Whether every byte reached the socket.
    [[nodiscard]] Task<bool> WriteFrame(ISocket* socket, std::span<std::byte const> bytes)
    {
        auto const written = co_await socket->Write(bytes);
        co_return written.has_value() && *written == bytes.size();
    }

} // namespace

/// Grants the free-function senders access to the transport's privates.
///
/// The senders are free functions taking raw pointers rather than members or
/// lambdas: a coroutine's frame outlives the expression that created it, so
/// capturing `this` is a use-after-free waiting to happen -- the shape
/// `RaftPeerServer::ServePeer` already uses for the same reason.
struct PeerSenderAccess
{
    /// How one attempt at serving a peer ended.
    enum class Outcome : std::uint8_t
    {
        Retry, ///< Dial or write failed; back off and try again.
        Stop,  ///< The transport is shutting down.
    };

    /// Holds one peer in the connected count for as long as its session lasts.
    ///
    /// RAII rather than a decrement at each of the session's exits, because the
    /// one that gets forgotten makes `ConnectedPeers()` report a fleet talking to
    /// peers it is not talking to -- and a readiness probe reading that is a node
    /// that looks healthy and is not. A coroutine frame's locals are destroyed
    /// when the frame completes or is destroyed, so the count is right on every
    /// path including the ones nobody wrote.
    class Session
    {
      public:
        /// @param transport Whose counter to hold.
        /// @param peer Which peer, for the log lines.
        Session(RaftPeerTransport* transport, RaftPeerTransport::Peer* peer) noexcept:
            _transport { transport },
            _peer { peer }
        {
            _transport->_connected.fetch_add(1, std::memory_order_relaxed);
            _transport->_logger.Log(
                LogLevel::Info,
                std::format(
                    "raft: connected to peer {} at {}:{}", _peer->endpoint.id, _peer->endpoint.host, _peer->endpoint.port));
        }

        Session(Session const&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session const&) = delete;
        Session& operator=(Session&&) = delete;

        ~Session()
        {
            _transport->_connected.fetch_sub(1, std::memory_order_relaxed);
            _transport->_logger.Log(LogLevel::Info, std::format("raft: peer {} disconnected", _peer->endpoint.id));
        }

      private:
        RaftPeerTransport* _transport;
        RaftPeerTransport::Peer* _peer;
    };

    /// One connection's life: dial, serve, end.
    static Task<Outcome> ServeOnce(RaftPeerTransport* self, RaftPeerTransport::Peer* peer);

    /// One peer's whole life. Ends only on a stop.
    static Task<void> RunSender(RaftPeerTransport* self, RaftPeerTransport::Peer* peer);

    /// Close every live peer socket, on the reactor's thread.
    static DetachedTask CloseSockets(RaftPeerTransport* self);
};

Task<PeerSenderAccess::Outcome> PeerSenderAccess::ServeOnce(RaftPeerTransport* self, RaftPeerTransport::Peer* peer)
{
    // No I/O bound on the socket, which is unchanged and deliberate: it decides
    // when a peer is declared dead, and that is its own decision rather than a
    // side effect of dialling. What is new is that it is no longer dangerous --
    // a write with no timeout used to be uninterruptible, and now closing the
    // socket completes it.
    auto dialed = co_await self->_connector.Connect(peer->endpoint.host, peer->endpoint.port, self->_options.dialTimeout);
    if (!dialed.has_value())
    {
        // Debug, not Warn. A peer being down is the ordinary condition Raft is
        // built for, and one line per backoff interval per peer at Warn would
        // bury the messages that do need reading.
        self->_logger.Log(LogLevel::Debug,
                          std::format("raft: could not reach peer {} at {}:{}: {}",
                                      peer->endpoint.id,
                                      peer->endpoint.host,
                                      peer->endpoint.port,
                                      dialed.error().context));
        co_return Outcome::Retry;
    }

    // A stop that arrived while the dial was in flight. The dial cannot be
    // abandoned -- destroying a suspended task frees a frame the reactor still
    // points into -- so it is always awaited to completion and the socket it
    // produced is closed here.
    if (self->_stop.Token().IsCancelled())
    {
        dialed.value()->Close();
        co_return Outcome::Stop;
    }

    peer->socket = std::move(dialed.value());
    Session const session { self, peer };

    while (true)
    {
        auto frame = co_await peer->outbox.Pop();
        if (!frame.has_value())
            co_return Outcome::Stop; // the outbox was closed

        // Written from a local of this frame, never from a queue element:
        // `ISocket::Write` requires the buffer to stay at a stable address until
        // the awaitable resumes, and a deque's element addresses do not.
        if (!co_await WriteFrame(peer->socket.get(), *frame))
        {
            self->_dropped.fetch_add(1, std::memory_order_relaxed);
            peer->socket->Close();
            peer->socket.reset();
            co_return Outcome::Retry;
        }
    }
}

Task<void> PeerSenderAccess::RunSender(RaftPeerTransport* self, RaftPeerTransport::Peer* peer)
{
    self->_reactorThread.store(std::this_thread::get_id(), std::memory_order_relaxed);

    auto const token = self->_stop.Token();
    while (!token.IsCancelled())
    {
        auto outcome = Outcome::Retry;
        try
        {
            outcome = co_await ServeOnce(self, peer);
        }
        catch (...)
        {
            // The hazard inverts rather than disappearing. With a `DetachedTask`
            // an escaped exception is std::terminate -- a node killed over one
            // peer. With an unawaited `Task<void>` it is stored in the promise
            // and never read, so the sender would simply end, forever, with
            // nothing logged anywhere: "three nodes sat at undecided with no
            // error" spelled for one peer. So it is caught and reported.
            self->NoteSenderThrew(peer->endpoint.id);
        }

        if (outcome == Outcome::Stop || token.IsCancelled())
            break;

        // Backed off after EVERY session, not only a failed dial. The thread
        // version redialled immediately after a dropped connection, so a peer
        // that accepts and instantly resets produced a tight
        // connect/write-fail/connect loop with no sleep in it. On a private
        // thread that burned one core; on the shared reactor it starves the
        // election timers, which is the "nine role changes in twelve seconds"
        // failure this repository already has a name for.
        if (co_await InterruptibleSleepUntil(&self->_reactor,
                                             token,
                                             self->_reactor.Clock().Now() + self->_options.reconnectBackoff,
                                             self->_options.stopWakeBound)
            == WakeReason::Cancelled)
            break;
    }

    peer->socket.reset();
    self->NoteSenderFinished();
    co_return;
}

DetachedTask PeerSenderAccess::CloseSockets(RaftPeerTransport* self)
{
    // The hop is the point. On epoll and kqueue `ISocket::Close` completes a
    // parked awaitable by resuming its coroutine INLINE, so closing from the
    // thread calling Stop() would run a peer's sender there while the reactor
    // thread is live. IOCP routes cancellation back through the port and does
    // not, which is precisely what would make this a bug that passes CI on
    // Windows and corrupts state on Linux and macOS.
    co_await ResumeOn { self->_reactor };
    for (auto& [id, peer]: self->_peers)
        if (peer->socket != nullptr)
            peer->socket->Close();
    co_return;
}

RaftPeerTransport::RaftPeerTransport(NodeId self,
                                     std::vector<PeerEndpoint> peers,
                                     IReactor& reactor,
                                     IConnector& connector,
                                     ILogger& logger,
                                     PeerTransportOptions options):
    _self { std::move(self) },
    _reactor { reactor },
    _connector { connector },
    _logger { logger },
    _options { options }
{
    for (auto& endpoint: peers)
    {
        if (endpoint.id == _self)
            continue;
        auto id = endpoint.id;
        _peers.emplace(std::move(id),
                       std::make_unique<Peer>(std::move(endpoint),
                                              _reactor,
                                              AsyncQueueOptions { .capacity = _options.maxQueuedPerPeer,
                                                                  .overflow = AsyncQueueOverflow::DropOldest }));
    }
}

RaftPeerTransport::~RaftPeerTransport()
{
    Stop();
}

void RaftPeerTransport::Start()
{
    auto expected = false;
    if (!_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    for (auto& [id, peer]: _peers)
    {
        peer->sender = PeerSenderAccess::RunSender(this, peer.get());
        _sendersRunning.fetch_add(1, std::memory_order_acq_rel);
        // Submitted rather than started: see `Start()`'s doc comment for why the
        // task is lazy and what depends on it.
        _reactor.Submit(peer->sender.Native());
    }
}

void RaftPeerTransport::RequestStop() noexcept
{
    _stop.Cancel();
    for (auto& [id, peer]: _peers)
        peer->outbox.Close();
    if (_sendersRunning.load(std::memory_order_acquire) != 0)
        PeerSenderAccess::CloseSockets(this);
}

void RaftPeerTransport::Stop() noexcept
{
    RequestStop();

    // Nothing to wait for means nothing to deadlock on, so this is safe from any
    // thread -- which is what lets a single-threaded test drive the reactor
    // itself, drain the senders with `RequestStop()`, and then let the destructor
    // call this without tripping the check below.
    if (_sendersRunning.load(std::memory_order_acquire) == 0)
    {
        _running.store(false, std::memory_order_release);
        return;
    }

    // Only now: waiting for coroutines that can only progress on the reactor's
    // thread, FROM that thread, is a deadlock. Asserting turns it into a failed
    // assertion in the debug preset for the price of one comparison, rather than
    // a hang naming nothing.
    assert(_reactorThread.load(std::memory_order_relaxed) != std::this_thread::get_id()
           && "RaftPeerTransport::Stop must not be called from the reactor's own thread");

    // Bounded, because a stuck peer must not turn a stop into a hang -- which is
    // exactly what this transport used to do. Same ceiling and cadence as
    // `RaftPeerServer::Shutdown`, for the same reason.
    constexpr auto Ceiling = std::chrono::seconds { 5 };
    constexpr auto Poll = std::chrono::milliseconds { 10 };
    auto waited = std::chrono::milliseconds { 0 };
    while (_sendersRunning.load(std::memory_order_acquire) != 0 && waited < Ceiling)
    {
        std::this_thread::sleep_for(Poll);
        waited += Poll;
    }

    if (auto const stuck = _sendersRunning.load(std::memory_order_acquire); stuck != 0)
    {
        // Released rather than destroyed. The reactor may still hold these
        // handles, so destroying the frames is undefined behaviour while leaking
        // them is a deliberate, logged, diagnosable loss -- which is the right way
        // round.
        _logger.Log(LogLevel::Error,
                    std::format("raft: {} peer sender(s) did not finish within {} ms; leaking their frames",
                                stuck,
                                Ceiling.count() * 1000));
        for (auto& [id, peer]: _peers)
            std::ignore = peer->sender.Release();
    }

    _running.store(false, std::memory_order_release);
}

std::size_t RaftPeerTransport::SendersRunning() const noexcept
{
    return _sendersRunning.load(std::memory_order_acquire);
}

void RaftPeerTransport::NoteSenderFinished() noexcept
{
    _sendersRunning.fetch_sub(1, std::memory_order_acq_rel);
}

void RaftPeerTransport::NoteSenderThrew(NodeId const& peer) noexcept
{
    // `noexcept` and calling the logger straight, matching
    // `LogConnectionFirewallException`: a logger that throws is a programmer
    // error rather than a condition to recover from, and wrapping it here would
    // only move where that is discovered.
    _logger.Log(LogLevel::Error, std::format("raft: peer {} sender threw; the connection was dropped", peer));
}

void RaftPeerTransport::Send(NodeId const& to, RaftMessage message)
{
    if (_stop.Token().IsCancelled())
        return;

    if (to == _self)
    {
        // A node does not talk to itself over a socket. Refused rather than
        // looped back, so a driver bug shows up here instead of as a connection
        // to this node's own listener.
        _logger.Log(LogLevel::Debug, "raft: refusing to send to self");
        return;
    }

    auto const found = _peers.find(to);
    if (found == _peers.end())
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        _logger.Log(LogLevel::Debug, std::format("raft: no endpoint configured for peer {}", to));
        return;
    }

    // Framed HERE, on the caller's thread, so the queue bound bounds BYTES rather
    // than a count of messages whose size nobody knows yet.
    auto frame = RaftWire::Encode(message);
    if (frame.empty())
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        _logger.Log(LogLevel::Debug, std::format("raft: could not frame a message for peer {}", to));
        return;
    }

    // Never resumes the sender inline -- see `AsyncQueue`. This matters here
    // specifically because `Send` is reached from `RaftDriver::Deliver`, which
    // holds the driver's mutex.
    auto const pushed = found->second->outbox.Push(std::move(frame));
    _dropped.fetch_add(pushed.displaced, std::memory_order_relaxed);
}

} // namespace FastCache::Consensus
