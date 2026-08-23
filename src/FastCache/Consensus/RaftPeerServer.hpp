// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/IRaftMessageSink.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IListener.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FastCache::Consensus
{

/// Limits a peer connection is held to.
struct PeerServerOptions
{
    /// Largest frame payload this node will buffer from a peer.
    ///
    /// The wire's length field is a u32, so a peer — or something that is not a
    /// peer at all — can declare four gigabytes. Without a cap the declared
    /// length *is* the allocation, which makes a single frame a
    /// memory-exhaustion vector. The default is sized for an AppendEntries
    /// carrying a healthy batch of configuration entries, which is what this log
    /// holds; it is deliberately far below what the field can express.
    std::size_t maxFrameBytes { 8U * 1024U * 1024U };

    /// How many peer connections may be served at once.
    ///
    /// A cluster needs one per peer. The cap is what keeps a misconfigured or
    /// hostile client from opening thousands, and it is generous enough that a
    /// reconnecting peer never waits behind its own stale connection.
    std::size_t maxConnections { 64 };
};

/// Accepts peer connections and turns their frames into `RaftMessage`s.
///
/// The inbound counterpart to `RaftPeerTransport`. It runs on the reactor like
/// every other server here, because accepting and reading are what the reactor
/// already does — only dialling had no seam, which is why the outbound side owns
/// threads and this side does not.
///
/// ## What closes a connection and what does not
///
/// The distinction is the whole reason the wire declares a frame length, and
/// getting it backwards costs a mixed-version fleet its replication:
///
/// - A frame whose **type or version** this build does not know is *skipped*.
///   The header still decoded, so the reader knows exactly how many bytes to
///   step over, and the next frame — which it very likely does understand —
///   still arrives. Closing here would make a node running a newer build
///   silently partition itself from every older peer.
/// - A frame whose **magic is wrong**, or whose payload does not parse, ends the
///   connection. In both cases this reader and that sender disagree about where
///   frames begin, so there is nothing to resynchronize to and every later byte
///   is a guess.
///
/// ## The listener must be able to wake
///
/// `Run` returns when the listener stops yielding connections, which `Shutdown`
/// arranges by closing it. POSIX does **not** unblock a parked `accept()` when
/// another thread closes the listening socket, so a blocking listener must have
/// had `SetTimeouts` applied by whoever bound it — otherwise a stop hangs until
/// the supervisor escalates to SIGKILL, which is exactly the defect
/// `WorkerServer::Run` records having shipped once.
class RaftPeerServer
{
  public:
    /// Construct over its collaborators; all must outlive the server.
    /// @param listener Bound listener for this node's peer port.
    /// @param sink Where decoded messages go.
    /// @param logger Where refusals are reported.
    /// @param options Frame and connection limits.
    RaftPeerServer(IListener& listener, IRaftMessageSink& sink, ILogger& logger, PeerServerOptions options = {}) noexcept;

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    /// @return Task that resolves when the loop exits.
    [[nodiscard]] Task<void> Run();

    /// Stop accepting, so `Run()` returns.
    void Shutdown() noexcept;

    /// How many frames were stepped over because this build did not know them.
    ///
    /// Counted rather than only logged, because it is the number that says a
    /// fleet is mid-upgrade: steady and non-zero means some peer speaks
    /// something this node does not, which is expected during a rollout and a
    /// misconfiguration afterwards.
    /// @return The cumulative skipped-frame count.
    [[nodiscard]] std::uint64_t SkippedFrames() const noexcept
    {
        return _skipped.load(std::memory_order_relaxed);
    }

    /// How many messages have been delivered to the sink.
    /// @return The cumulative delivered count.
    [[nodiscard]] std::uint64_t DeliveredMessages() const noexcept
    {
        return _delivered.load(std::memory_order_relaxed);
    }

  private:
    /// Serve one peer connection until it closes or desynchronizes.
    /// @param socket The accepted connection.
    /// @return Task that resolves when the connection ends.

    IListener& _listener;
    IRaftMessageSink& _sink;
    ILogger& _logger;
    PeerServerOptions _options;

    std::atomic<bool> _shuttingDown { false };
    std::atomic<std::size_t> _active { 0 };
    std::atomic<std::uint64_t> _skipped { 0 };
    std::atomic<std::uint64_t> _delivered { 0 };
};

} // namespace FastCache::Consensus
