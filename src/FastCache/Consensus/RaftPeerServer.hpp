// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/IRaftMessageSink.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IListener.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

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

/// The connections a `RaftPeerServer` has accepted and not yet finished with.
///
/// Raw pointers into sockets the per-connection task owns, registered as it
/// starts and removed as it ends -- so this never outlives what it points at, and
/// closing one is always closing a socket that is still there.
struct OpenConnections
{
    /// Guards `sockets`. The accept loop and each ending connection touch it from
    /// the reactor's thread while `Shutdown` touches it from whoever is tearing
    /// the node down.
    std::mutex mutex;

    std::vector<ISocket*> sockets; ///< One per connection currently being served.
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
/// ## The listener must be a REACTOR listener
///
/// Not a preference. A blocking listener makes `co_await Accept()` and every
/// `co_await` inside the per-connection task complete synchronously, so that
/// task -- a `DetachedTask` precisely so several peers can be read at once --
/// runs to completion inline and the accept loop never reaches its next
/// iteration. One peer is then served and no other is ever accepted: in a
/// three-node cluster each node reads from exactly one of its two peers, votes
/// and heartbeats from the third never arrive, and nobody is ever elected.
/// Nothing crashes and nothing logs a fault, which is why it survived until a
/// fixture started three real processes.
///
/// `Run` returns when the listener stops yielding connections, which `Shutdown`
/// arranges by closing it -- a reactor listener completes its parked accept with
/// `Cancelled` when it is closed, which is the wake-up a blocking one would need
/// `SetTimeouts` for.
class RaftPeerServer
{
  public:
    /// Construct over its collaborators; all must outlive the server.
    /// @param listener Bound listener for this node's peer port.
    /// @param reactor The executor that listener and its connections belong to.
    ///        `Shutdown()` posts its closes there rather than performing them on
    ///        the calling thread; see that function for why the pairing is
    ///        load-bearing rather than a convenience.
    /// @param sink Where decoded messages go.
    /// @param logger Where refusals are reported.
    /// @param options Frame and connection limits.
    RaftPeerServer(IListener& listener,
                   IExecutor& reactor,
                   IRaftMessageSink& sink,
                   ILogger& logger,
                   PeerServerOptions options = {}) noexcept;

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    /// @return Task that resolves when the loop exits.
    [[nodiscard]] Task<void> Run();

    /// Stop accepting and close what is already accepted, so `Run()` returns.
    ///
    /// The connections matter as much as the listener. A peer's read is parked on
    /// the reactor, and `IReactor::Run` returns with its parked work exactly where
    /// it was -- so a connection nobody closed is a coroutine frame nobody ever
    /// resumes and nobody ever frees. Closing the socket completes that read with
    /// `Cancelled`, which is how the frame reaches its own end.
    ///
    /// **The closes are POSTED onto the reactor, never performed here**, and that
    /// is not caution
    /// ([#885](https://github.com/LASTRADA-Software/fastcached/issues/885)). On
    /// epoll and kqueue `ISocket::Close` completes a parked read by resuming its
    /// coroutine **inline**, so closing from the stopping thread ran each
    /// per-connection task there -- while the reactor thread was still driving the
    /// others -- and destroyed the `std::unique_ptr<ISocket>` in that task's frame
    /// off the reactor, which is #668's rule
    /// (`IReactor::TeardownIsSerialisedWithDispatch()`) violated. IOCP routes
    /// cancellation back through the port and does not resume inline, which is
    /// exactly what would have made this a defect that passes CI on Windows.
    ///
    /// Measured on Linux before the fix: with the teardown assertion live at
    /// `~EpollSocket`, `cluster-e2e` was the only failure in the whole suite, and a
    /// backtrace filed every violation here. `FrameServer::Shutdown` has posted its
    /// closes for this reason since it was written; this one had the same shape and
    /// did not.
    ///
    /// Safe from any thread. Returns once the closes have run AND every connection
    /// has ended, because the detached connection tasks borrow members of this
    /// object -- bounded, so a stuck peer cannot turn a stop into a hang.
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
    /// Close the listener and every connection currently registered.
    ///
    /// Reactor thread only -- see `Shutdown()`, which posts it there. Copies the
    /// set out under the lock rather than closing under it, because a close
    /// resumes the task that removes itself from that very vector.
    void CloseAll() noexcept;

    IListener& _listener;
    IExecutor& _reactor;
    IRaftMessageSink& _sink;
    ILogger& _logger;
    PeerServerOptions _options;

    OpenConnections _open;

    std::atomic<bool> _shuttingDown { false };

    /// Whether the posted closes have run.
    ///
    /// Waited for alongside `_active`, and NOT folded into it: `_active` is also
    /// what the `maxConnections` cap is judged against, so borrowing a slot for
    /// teardown would refuse one honest peer at the moment of a stop. Its own flag
    /// is also what lets the ceiling message say WHICH of the two it abandoned,
    /// and those are opposite diagnoses -- a wedged peer, or a reactor that
    /// stopped before it could run what it was handed.
    std::atomic<bool> _closesRan { false };
    std::atomic<std::size_t> _active { 0 };
    std::atomic<std::uint64_t> _skipped { 0 };
    std::atomic<std::uint64_t> _delivered { 0 };
};

} // namespace FastCache::Consensus
