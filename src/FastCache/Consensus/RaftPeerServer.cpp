// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace FastCache::Consensus
{

namespace
{

    /// Serve one peer connection as a detached coroutine.
    ///
    /// A free function taking raw pointers so the accept loop can spawn it
    /// without capturing `this`, the same shape `ServeAdminConnection` uses —
    /// and for the same reason: the coroutine outlives the loop iteration that
    /// started it, so a captured `this` would be a lifetime question at every
    /// suspension point rather than a documented one here. `Shutdown` drains
    /// before returning, which is what makes the borrowed pointers safe.
    ///
    /// @param socket The accepted connection; owned for its lifetime.
    /// @param sink Where decoded messages go.
    /// @param logger Where refusals are reported.
    /// @param options Frame limits.
    /// @param skipped Counter for frames stepped over.
    /// @param delivered Counter for messages handed on.
    /// @param active In-flight connection counter, decremented on exit.
    /// Keeps one connection in `OpenConnections` for as long as it is being served.
    ///
    /// RAII rather than a call at each of the loop's five exits, because the one
    /// that gets forgotten is a dangling pointer `Shutdown` would then close --
    /// and a use-after-free at teardown is the shape of bug that reproduces once
    /// in a hundred runs and never on the machine looking at it.
    class RegisteredConnection
    {
      public:
        /// @param open Where to register; must outlive this.
        /// @param socket The connection.
        RegisteredConnection(OpenConnections* open, ISocket* socket) noexcept:
            _open { open },
            _socket { socket }
        {
            auto const guard = std::scoped_lock { _open->mutex };
            _open->sockets.push_back(_socket);
        }

        RegisteredConnection(RegisteredConnection const&) = delete;
        RegisteredConnection(RegisteredConnection&&) = delete;
        RegisteredConnection& operator=(RegisteredConnection const&) = delete;
        RegisteredConnection& operator=(RegisteredConnection&&) = delete;

        ~RegisteredConnection()
        {
            auto const guard = std::scoped_lock { _open->mutex };
            std::erase(_open->sockets, _socket);
        }

      private:
        OpenConnections* _open;
        ISocket* _socket;
    };

    DetachedTask ServePeer(std::unique_ptr<ISocket> socket,
                           IRaftMessageSink* sink,
                           ILogger* logger,
                           PeerServerOptions options,
                           std::atomic<std::uint64_t>* skipped,
                           std::atomic<std::uint64_t>* delivered,
                           std::atomic<std::size_t>* active,
                           OpenConnections* open)
    {
        RegisteredConnection const registration { open, socket.get() };
        // No line is ever read on this wire, so the line cap is nominal; the
        // payload cap is the frame cap, which makes the reader refuse an
        // over-large frame even if the explicit check below were ever removed.
        ByteReader reader { *socket, /*maxLineBytes=*/1, options.maxFrameBytes };
        auto const peer = socket->PeerAddress();

        while (true)
        {
            auto const headerBytes = co_await reader.ReadExactly(RaftWire::HeaderSize);
            if (!headerBytes.has_value())
                break; // EOF, or the peer went away. Ordinary.

            auto const header = RaftWire::DecodeHeader(*headerBytes);
            if (!header.has_value())
            {
                // A wrong magic is the one condition under which the reader
                // cannot find where this frame ends. There is nothing to
                // resynchronize to, so every later byte would be a guess.
                logger->Log(LogLevel::Warn, std::format("raft: peer {} sent a frame with no valid magic; closing", peer));
                break;
            }

            if (header->payloadLength > options.maxFrameBytes)
            {
                // Refused BEFORE the payload is buffered, exactly as the
                // compile-cache handler's cap is: checking afterwards would let
                // a peer force the very allocation the cap exists to deny, once
                // per frame.
                logger->Log(LogLevel::Warn,
                            std::format("raft: peer {} declared a {}-byte frame over the {}-byte cap; closing",
                                        peer,
                                        header->payloadLength,
                                        options.maxFrameBytes));
                break;
            }

            auto const payload = co_await reader.ReadExactly(header->payloadLength);
            if (!payload.has_value())
                break;

            auto decoded = RaftWire::DecodeMessage(*header, *payload);
            if (decoded.has_value())
            {
                sink->Deliver(*std::move(decoded));
                delivered->fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // The payload has already been consumed, so the reader is still in
            // sync whatever the verdict -- which is the whole point of the
            // declared length, and why the two outcomes below can differ.
            auto const code = decoded.error().code;
            if (code == ConsensusErrorCode::UnknownMessageType || code == ConsensusErrorCode::UnsupportedVersion)
            {
                // Stepped over, not fatal. A peer running a newer build is the
                // ordinary condition during a rolling upgrade, and closing here
                // would partition this node from every peer ahead of it.
                skipped->fetch_add(1, std::memory_order_relaxed);
                logger->Log(LogLevel::Debug,
                            std::format("raft: skipped a frame from peer {}: {}", peer, decoded.error().context));
                continue;
            }

            // A malformed payload means this reader and that sender disagree
            // about the bytes, so the connection is no longer trustworthy even
            // though this particular frame was consumed cleanly.
            logger->Log(LogLevel::Warn,
                        std::format("raft: peer {} sent a malformed frame ({}); closing", peer, decoded.error().context));
            break;
        }

        socket->Close();
        active->fetch_sub(1, std::memory_order_acq_rel);
    }

} // namespace

RaftPeerServer::RaftPeerServer(IListener& listener,
                               IRaftMessageSink& sink,
                               ILogger& logger,
                               PeerServerOptions options) noexcept:
    _listener { listener },
    _sink { sink },
    _logger { logger },
    _options { options }
{
}

Task<void> RaftPeerServer::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            // A poll timeout is how this loop wakes to observe Shutdown() on
            // POSIX, where closing the listening socket does not unblock a
            // parked accept(). Not a failure.
            auto const code = accepted.error().code;
            if (code == NetErrorCode::WouldBlock || code == NetErrorCode::Timeout)
                continue;
            _logger.Log(LogLevel::Debug, std::format("raft: peer accept loop ended ({})", accepted.error().ToString()));
            co_return;
        }

        auto const before = _active.fetch_add(1, std::memory_order_acq_rel);
        if (before >= _options.maxConnections)
        {
            // Closed rather than answered: the peer wire has no reply shape, so
            // there is nothing to say. A peer whose connection is refused
            // redials on its own backoff.
            _active.fetch_sub(1, std::memory_order_acq_rel);
            (*accepted)->Close();
            continue;
        }

        // Detached, because peer connections are long-lived and concurrent.
        // Serving them inline -- which is right for the compile worker, whose
        // connections are one CPU-bound job each -- would mean reading only one
        // peer ever, and a cluster that never hears from the rest.
        ServePeer(std::move(*accepted), &_sink, &_logger, _options, &_skipped, &_delivered, &_active, &_open);
    }
    co_return;
}

void RaftPeerServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();

    // And every connection already accepted. Closing one completes whatever read
    // was parked on it, which is how each per-connection task reaches its own end
    // -- a task still parked when the reactor's `Run()` returns is a frame nobody
    // resumes and nobody frees. Copied out under the lock rather than closed under
    // it, because a close resumes the task that removes itself from this very
    // vector.
    auto sockets = std::vector<ISocket*> {};
    {
        auto const guard = std::scoped_lock { _open.mutex };
        sockets = _open.sockets;
    }
    for (auto* socket: sockets)
        socket->Close();

    // Detached connection coroutines borrow the sink, the logger and the
    // counters held on this object, so they must drain before Shutdown returns
    // -- otherwise a reader suspended on a slow socket could touch freed members
    // after the server is destroyed. Bounded rather than unconditional: a stuck
    // peer must not turn a stop into a hang, which is how a service ends up
    // killed by its supervisor instead of stopping.
    //
    // This wait was correct, and was then copied twice by loops that cited it and
    // counted their polls instead of measuring them. So it is now the shared
    // `DrainWithin`, and the ceiling and the cadence live there with it (#452).
    if (DrainWithin([this] { return _active.load(std::memory_order_acquire) > 0; }) == DrainResult::Ceiling)
        _logger.Logf(LogLevel::Error,
                     "raft: {} peer connection(s) did not finish within the stop ceiling",
                     _active.load(std::memory_order_acquire));
}

} // namespace FastCache::Consensus
