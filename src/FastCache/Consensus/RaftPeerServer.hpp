// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftMessageSink.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftPeerRefusals.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Consensus
{

/// Limits a peer connection is held to.
struct PeerServerOptions
{
    /// Largest frame payload this node will buffer from a peer that has proved its id.
    ///
    /// The wire's length field is a u32, so a peer — or something that is not a
    /// peer at all — can declare four gigabytes. Without a cap the declared
    /// length *is* the allocation, which makes a single frame a
    /// memory-exhaustion vector. The default is sized for an AppendEntries
    /// carrying a healthy batch of configuration entries, which is what this log
    /// holds; it is deliberately far below what the field can express. Before the
    /// proof a much smaller bound applies, `RaftWire::MaxHandshakePayload`.
    std::size_t maxFrameBytes { 8U * 1024U * 1024U };

    /// How many peer connections may be served at once.
    ///
    /// A cluster needs one per peer. The cap is what keeps a misconfigured or
    /// hostile client from opening thousands, and it is generous enough that a
    /// reconnecting peer never waits behind its own stale connection.
    std::size_t maxConnections { 64 };

    /// How long a connection may take to prove an id before it is closed and counted.
    ///
    /// `RaftWire::HandshakeBound`, which the dialling end uses too. Non-positive arms
    /// no deadline at all -- `core::net::armSocketDeadline`'s rule -- which exists for a test that
    /// drives the accept loop over a reactor nothing turns, where a deadline could never
    /// fire and would only be a coroutine frame left parked.
    std::chrono::milliseconds handshakeBound { RaftWire::HandshakeBound };
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

    std::vector<core::net::ISocket*> sockets; ///< One per connection currently being served.
};

/// Accepts peer connections, has each one prove which member it is, and turns their
/// frames into `RaftMessage`s.
///
/// The inbound counterpart to `RaftPeerTransport`. It runs on the reactor like
/// every other server here, because accepting and reading are what the reactor
/// already does.
///
/// ## Nothing is read from a peer that has not proved its id (#1308, #178)
///
/// Every connection opens with `RaftPeerSession`'s handshake: this end sends a
/// challenge before reading a byte, reads exactly one proof no larger than
/// `RaftWire::MaxHandshakePayload` within `PeerServerOptions::handshakeBound`, and
/// answers with a signed verdict. Only an accepted connection has its frames read,
/// and each of those is checked against the session before it is decoded. Every way a
/// connection is refused moves its own counter (`AcceptorRefusals`), because each names
/// a different thing to go and fix. A connection that proves an id is attributed to
/// the member it proved, and a message naming any other sender ends it.
///
/// ## A revoked key ends the connections it proved
///
/// Every frame is re-checked against the roster (`IRaftPeerIdentity::StillProves`) before
/// it is delivered, so an applied forget -- or a re-admission under another key --
/// closes a connection that proved the old key at its next frame, and the redial is judged
/// against the roster as it is then. Pulled per frame rather than pushed by the state
/// machine, because the connection belongs to the reactor's thread and the roster moves on
/// another: a push would be a close from the wrong thread, and a Raft peer is never quiet
/// for long.
///
/// ## What closes a connection and what does not
///
/// - A frame whose **type** this build does not know is *skipped*, once its tag has
///   verified. The header still decoded, so the reader knows exactly how many bytes to
///   step over, and the next frame still arrives.
/// - A frame whose **tag** does not verify, whose **magic** is wrong, whose **version**
///   is not the handshake's, or whose payload does not parse, ends the connection. The
///   version used to be stepped over as well; it is a property of the connection now,
///   settled by the handshake, and stepping over a frame of another version would mean
///   guessing whether a tag follows it.
///
/// ## The listener must be a REACTOR listener
///
/// Not a preference. A blocking listener makes `co_await Accept()` and every
/// `co_await` inside the per-connection task complete synchronously, so that
/// task -- a `core::async::DetachedTask` precisely so several peers can be read at once --
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
    /// How often a refusal a stranger can provoke is logged, at most.
    ///
    /// Counted every time, logged at most this often: anything that can reach the port
    /// can provoke one without holding the key, and a line per connection is a log an
    /// outsider can fill. Discovery's unnameable-beacon line has the same interval for
    /// the same reason.
    static constexpr std::chrono::seconds PreAuthReportInterval { 60 };

    /// Construct over its collaborators; all must outlive the server.
    /// @param listener Bound listener for this node's peer port.
    /// @param reactor The reactor that listener and its connections belong to.
    ///        `Shutdown()` posts its closes there rather than performing them on
    ///        the calling thread, and the handshake bound is armed on it.
    /// @param sink Where decoded messages go.
    /// @param logger Where refusals are reported.
    /// @param metrics Where refusals are counted.
    /// @param identity Who this node is -- the member a dialler must have meant -- what its
    ///        verdicts are signed with, and what a peer's proof is checked against.
    /// @param random Where each connection's challenge nonce and ephemeral key come from. A
    ///        connection this node cannot draw them for is closed before it is challenged (#1527).
    /// @param options Frame, connection and handshake limits.
    RaftPeerServer(core::net::IListener& listener,
                   core::net::EventLoop& reactor,
                   IRaftMessageSink& sink,
                   ILogger& logger,
                   IMetricsSink& metrics,
                   IRaftPeerIdentity const& identity,
                   ISecureRandom& random,
                   PeerServerOptions options = {});

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    /// @return Task that resolves when the loop exits.
    [[nodiscard]] core::async::Task<void> Run();

    /// Stop accepting and close what is already accepted, so `Run()` returns.
    ///
    /// The connections matter as much as the listener. A peer's read is parked on
    /// the reactor, and `core::net::EventLoop::Run` returns with its parked work exactly where
    /// it was -- so a connection nobody closed is a coroutine frame nobody ever
    /// resumes and nobody ever frees. Closing the socket completes that read with
    /// `Cancelled`, which is how the frame reaches its own end.
    ///
    /// **The closes are POSTED onto the reactor, never performed here**, and that
    /// is not caution
    /// ([#885](https://github.com/LASTRADA-Software/fastcached/issues/885)). With
    /// fastcached's own reactor, `Close` on epoll and kqueue completed a parked read by
    /// resuming its coroutine **inline**, so closing from the stopping thread ran each
    /// per-connection task there -- while the reactor thread was still driving the
    /// others -- and destroyed the `std::unique_ptr<ISocket>` in that task's frame
    /// off the reactor, which is #668's rule
    /// (`core::net::EventLoop::TeardownIsSerialisedWithDispatch()`) violated. IOCP routed
    /// cancellation back through the port and did not, which is what made it a defect
    /// that passed CI on Windows. core-cpp's sockets resume a woken waiter in the loop's
    /// next drain on every backend (0.2.1, guarantee G2), but `close()` itself still
    /// touches the loop's registrations, so the closes are still posted.
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

    /// How many frames were stepped over because this build did not know their type.
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
    friend struct PeerServerAccess;

    /// Close the listener and every connection currently registered.
    ///
    /// Reactor thread only -- see `Shutdown()`, which posts it there. Copies the
    /// set out under the lock rather than closing under it, because a close
    /// resumes the task that removes itself from that very vector.
    void CloseAll() noexcept;

    /// Count a refusal decided before the peer proved anything, and log it at most once
    /// per `PreAuthReportInterval`.
    /// @param refusal Which refusal.
    /// @param peer The address the connection came from; the only thing about it that
    ///        is not a claim.
    /// @param detail What was seen, for the log line; never a claimed id.
    void NotePreAuthRefusal(AcceptorRefusal refusal, std::string_view peer, std::string_view detail);

    /// Count a refusal of a peer that proved its id, and log it naming who it proved.
    /// @param refusal Which refusal.
    /// @param peer The address the connection came from.
    /// @param dialler The member the connection proved.
    /// @param detail What was seen, for the log line.
    void NoteProvenRefusal(AcceptorRefusal refusal,
                           std::string_view peer,
                           std::string_view dialler,
                           std::string_view detail);

    /// Say that a connection was closed unchallenged because no nonce could be drawn, at most
    /// once per `PreAuthReportInterval`.
    ///
    /// **Not an `AcceptorRefusal`, and it moves no counter**: every row there names something
    /// about the PEER to go and fix, and this names nothing about it -- the fault is this
    /// host's generator, and it is every connection from every peer. So it is the transport's
    /// "cannot prove this node's id" line one end over: an Error naming the cause, throttled because
    /// anything that can reach the port provokes it.
    /// @param peer The address the connection came from.
    /// @param error Why the draw failed.
    void NoteNoNonce(std::string_view peer, SecureRandomError const& error);

    core::net::IListener& _listener;
    core::net::EventLoop& _reactor;
    IRaftMessageSink& _sink;
    ILogger& _logger;
    IMetricsSink& _metrics;
    IRaftPeerIdentity const& _identity;
    ISecureRandom& _random;
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

    /// Guards `_nextPreAuthReport` and `_nextNoNonceReport`. The connection tasks share the
    /// reactor's thread, but a test drives the accept loop from its own, and a lock costs
    /// nothing at a rate bounded by the throttle it guards.
    std::mutex _reportMutex;

    /// When the next pre-authentication refusal may be logged.
    core::platform::SteadyTimePoint _nextPreAuthReport {};

    /// When the next connection closed for want of a nonce may be logged. Its own throttle,
    /// so a stranger provoking refusals cannot keep the line that names THIS host's fault quiet.
    core::platform::SteadyTimePoint _nextNoNonceReport {};
};

} // namespace FastCache::Consensus
