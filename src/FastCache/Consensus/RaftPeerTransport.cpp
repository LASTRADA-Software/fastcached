// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftWire.hpp>

#include <format>
#include <stdexcept>
#include <utility>

namespace FastCache::Consensus
{

namespace
{

    /// Write every byte of `bytes` to `socket`.
    ///
    /// A coroutine because `ISocket::Write` is awaitable, driven to completion by
    /// `SyncRun` on the sender's own thread. That is sound precisely because the
    /// socket is a blocking one: it resolves its awaitable synchronously, so the
    /// task is never left suspended — which `SyncRun` refuses to read from.
    ///
    /// The socket arrives by **pointer rather than reference**, which reads as a
    /// stylistic slip and is not: a coroutine's reference parameter is bound
    /// before the first suspension and then outlives every frame that could have
    /// kept it alive. `RaftDriver::Run` takes its reactor the same way, for the
    /// same reason.
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

RaftPeerTransport::RaftPeerTransport(
    NodeId self, std::vector<PeerEndpoint> peers, IConnector& connector, ILogger& logger, PeerTransportOptions options):
    _self { std::move(self) },
    _connector { connector },
    _logger { logger },
    _options { options }
{
    for (auto& endpoint: peers)
    {
        // A node's own id in the peer table would give it a socket to itself and
        // a sender thread to drive it. Skipped rather than rejected: a
        // configuration that lists every member uniformly is the natural thing to
        // write, and making the transport tolerate it is cheaper than making
        // every caller filter first.
        if (endpoint.id == _self)
            continue;

        auto peer = std::make_unique<Peer>();
        peer->endpoint = std::move(endpoint);
        _peers.emplace(peer->endpoint.id, std::move(peer));
    }
}

RaftPeerTransport::~RaftPeerTransport()
{
    Stop();
}

void RaftPeerTransport::Start()
{
    if (_running.exchange(true, std::memory_order_acq_rel))
        return;

    for (auto& [id, peer]: _peers)
        peer->worker = std::jthread { [this, raw = peer.get()] { RunSender(*raw); } };
}

void RaftPeerTransport::Stop() noexcept
{
    _stopping.store(true, std::memory_order_release);

    // Every sender is woken before any is joined. Waking them one at a time and
    // joining as we go would serialize shutdown behind each peer's current dial,
    // so a cluster with three unreachable peers would take three dial timeouts
    // to stop instead of one.
    for (auto& [id, peer]: _peers)
    {
        {
            std::scoped_lock const guard { peer->mutex };
            peer->outbox.clear();
        }
        peer->wake.notify_all();
    }

    for (auto& [id, peer]: _peers)
        if (peer->worker.joinable())
            peer->worker.join();

    _running.store(false, std::memory_order_release);
}

void RaftPeerTransport::Send(NodeId const& to, RaftMessage message)
{
    if (_stopping.load(std::memory_order_acquire))
        return;

    auto const found = _peers.find(to);
    if (found == _peers.end())
    {
        // A member with no endpoint. Counted rather than thrown, because this is
        // reached from the driver's send loop and a throw there would take down a
        // node over a configuration gap that costs it one peer.
        _dropped.fetch_add(1, std::memory_order_relaxed);
        _logger.Log(LogLevel::Debug, std::format("raft: no endpoint configured for peer {}", to));
        return;
    }

    // Framed here rather than on the sender thread, for two reasons. The queue
    // then holds bytes, so the drop policy bounds *memory* rather than a message
    // count whose cost varies by orders of magnitude between a heartbeat and a
    // full AppendEntries. And an over-large message is refused where there is a
    // caller to tell, instead of on a thread whose only recourse is to log.
    std::vector<std::byte> frame;
    try
    {
        frame = RaftWire::Encode(message);
    }
    catch (std::length_error const& error)
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        _logger.Log(LogLevel::Warn, std::format("raft: message to {} too large to frame: {}", to, error.what()));
        return;
    }

    auto& peer = *found->second;
    {
        std::scoped_lock const guard { peer.mutex };
        // The OLDEST is dropped, not the newest. What is waiting behind a dead
        // connection is stale by definition, and for a follower the newest
        // AppendEntries subsumes every older one -- so keeping the newest is what
        // makes the peer useful the moment it comes back.
        while (peer.outbox.size() >= _options.maxQueuedPerPeer)
        {
            peer.outbox.pop_front();
            _dropped.fetch_add(1, std::memory_order_relaxed);
        }
        peer.outbox.push_back(std::move(frame));
    }
    peer.wake.notify_one();
}

void RaftPeerTransport::RunSender(Peer& peer) noexcept
{
    std::unique_ptr<ISocket> socket;
    auto connected = false;

    auto const dropConnection = [&](std::string_view why) {
        if (connected)
        {
            _connected.fetch_sub(1, std::memory_order_relaxed);
            connected = false;
            _logger.Log(LogLevel::Info, std::format("raft: peer {} disconnected: {}", peer.endpoint.id, why));
        }
        socket.reset();
    };

    while (!_stopping.load(std::memory_order_acquire))
    {
        if (socket == nullptr)
        {
            // No I/O bound, which is what this transport did before the parameter
            // existed. Adding one is a real improvement -- a peer that accepts and
            // then stalls parks this thread -- but it changes when a peer is
            // declared dead, so it is its own decision rather than a side effect.
            auto dialed = _connector.Connect(
                peer.endpoint.host, peer.endpoint.port, _options.dialTimeout, std::chrono::milliseconds { 0 });
            if (!dialed.has_value())
            {
                // Logged at Debug, not Warn. A peer being down is the ordinary
                // condition Raft is built for, and one line per backoff interval
                // per peer at Warn would bury the messages that do need reading.
                _logger.Log(LogLevel::Debug,
                            std::format("raft: could not reach peer {} at {}:{}: {}",
                                        peer.endpoint.id,
                                        peer.endpoint.host,
                                        peer.endpoint.port,
                                        dialed.error().context));

                // Waiting on the condition variable rather than sleeping is what
                // makes the backoff interruptible: a stop is observed at once
                // instead of after the interval elapses.
                std::unique_lock lock { peer.mutex };
                peer.wake.wait_for(
                    lock, _options.reconnectBackoff, [this] { return _stopping.load(std::memory_order_acquire); });
                continue;
            }

            socket = std::move(*dialed);
            connected = true;
            _connected.fetch_add(1, std::memory_order_relaxed);
            _logger.Log(
                LogLevel::Info,
                std::format(
                    "raft: connected to peer {} at {}:{}", peer.endpoint.id, peer.endpoint.host, peer.endpoint.port));
        }

        std::vector<std::byte> frame;
        {
            std::unique_lock lock { peer.mutex };
            peer.wake.wait(lock,
                           [this, &peer] { return !peer.outbox.empty() || _stopping.load(std::memory_order_acquire); });
            if (_stopping.load(std::memory_order_acquire))
                break;
            frame = std::move(peer.outbox.front());
            peer.outbox.pop_front();
        }

        auto delivered = false;
        try
        {
            delivered = SyncRun(WriteFrame(socket.get(), frame));
        }
        catch (std::exception const& error)
        {
            // A blocking socket resolves synchronously, so `SyncRun` completing
            // is the normal case; a throw here means the socket did something
            // this thread cannot drive. Treated as a dead connection rather than
            // allowed to escape, because an exception leaving a `noexcept` thread
            // body terminates the process -- taking down a node over one peer.
            _logger.Log(LogLevel::Warn, std::format("raft: writing to peer {} failed: {}", peer.endpoint.id, error.what()));
        }

        if (!delivered)
        {
            // The message is gone, and that is correct rather than merely
            // accepted: the transport is best-effort by contract and the
            // algorithm re-sends on the next heartbeat. Retrying it on a fresh
            // connection would deliver a message the node has since superseded.
            dropConnection("write failed");
            _dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    dropConnection("stopping");
}

} // namespace FastCache::Consensus
