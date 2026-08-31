// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeSurfaces.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Protocol/CompileCacheAuth.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Node
{

class NodeIoLoop;

/// Answers one framed request.
///
/// The seam that lets one accept loop serve every framed surface this node exposes.
/// A node runs a scheduler, a cache tier and a compile worker; each speaks the same
/// `0xFC` framing and differs only in what it answers and how much it will buffer.
/// A second accept loop per surface would be three near-copies of a listener, a
/// shutdown order and a poll timeout -- the copy-paste this codebase treats as a
/// defect rather than a coincidence.
///
/// An interface rather than a `std::function`, deliberately: a responder outlives
/// the endpoint and a closure would keep its whole enclosing scope alive with it.
class IFrameResponder
{
  public:
    virtual ~IFrameResponder() = default;

    IFrameResponder() = default;
    IFrameResponder(IFrameResponder const&) = default;
    IFrameResponder& operator=(IFrameResponder const&) = default;
    IFrameResponder(IFrameResponder&&) = default;
    IFrameResponder& operator=(IFrameResponder&&) = default;

    /// Answer one complete request frame.
    ///
    /// A `Task` because answering may now have to reach the network -- the cache
    /// surface consults an upstream, and that dial suspends rather than blocking
    /// the loop every other connection on this reactor is sharing. A responder
    /// that needs nothing is still free to `co_return` without suspending, which
    /// the scheduler's does; that costs a frame allocation and no round trip.
    ///
    /// @param frame The whole request, header included. A span rather than an
    ///        owning vector because a cache STORE carries an object file and
    ///        copying it here would double the peak footprint on the hot path of
    ///        a parallel build. It must outlive the returned task -- the same
    ///        contract `SendAll` states, and true by construction at the one call
    ///        site, where the backing vector is a local of the calling coroutine.
    /// @param peer The peer's host, for the surfaces whose policy needs one.
    ///        Owned rather than a view: it is short, and every policy-bearing
    ///        responder holds it across a suspension, so a view would make its
    ///        lifetime a rule at each implementation instead of a fact.
    /// @return The encoded reply, or empty to close without answering -- which is
    ///         only ever right when the peer is not speaking this protocol at all.
    [[nodiscard]] virtual Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) = 0;

    /// May this peer send at all, before a byte of its payload is taken?
    ///
    /// **The point is WHEN this is asked, not that it is asked.** Both surfaces
    /// already refused the peers this refuses -- but from inside `Answer`, which runs
    /// after the whole declared payload has been read and charged against the byte
    /// budget. So a caller the surface was always going to refuse could still make
    /// this process allocate for it, which is the cheapest denial there is: the
    /// refusal cost exactly what serving would have
    /// ([#285](https://github.com/LASTRADA-Software/fastcached/issues/285),
    /// [#377](https://github.com/LASTRADA-Software/fastcached/issues/377)).
    ///
    /// **Only decisions that depend on the PEER alone belong here.** That is what
    /// makes the question answerable before the frame exists. A surface's membership
    /// or locality rule qualifies; anything reading the verb, the payload or a
    /// credential does not, and stays in `Answer` where the request is.
    ///
    /// **The responder owns the predicate; the endpoint only asks it earlier.** The
    /// refusal is returned already encoded, so the wording, the wire code and the
    /// counter stay with the surface that decided -- rather than the endpoint growing
    /// its own copy of a rule that would then have two places to drift.
    ///
    /// **The gate inside `Answer` stays, and is still the authority.** This is an
    /// early-out, not a replacement: `Answer` is reachable directly, and a predicate
    /// enforced only at the door is one a later caller can walk around.
    ///
    /// Pure virtual rather than defaulted to "admit everyone", deliberately. A
    /// default would let a surface added later inherit an open door by saying
    /// nothing, which is the shape this codebase records as reopening a hole by
    /// omission. A surface with no peer policy returns `std::nullopt` and says so.
    ///
    /// **Takes the verb as well as the peer, since #290.** Each surface today serves
    /// one verb family, so the surface *is* the policy and every implementation
    /// ignores `opRaw` -- nothing observable changes. It is here because a merged
    /// 0xFC listener cannot express its own acceptance criterion without it: *a cache
    /// FETCH from another machine is refused while a compile from that same peer
    /// succeeds*. Same peer, two verbs, two answers, and a peer-only predicate has
    /// nowhere to put the difference.
    ///
    /// The verb costs nothing to supply. `ServeConnection` decodes the request header
    /// well before it asks this, so the opcode is already in hand at the one call
    /// site -- the earlier note that a peer-only shape is what "lets it be asked
    /// before a frame exists" described an option nobody takes, and it was the
    /// sentence that would have argued against this.
    ///
    /// Raw rather than an `Op`, for the reason `DecidePrePayload` is total over every
    /// byte value: an unknown opcode still has to be refusable, and a policy that
    /// could only be asked about verbs this build knows would admit the ones it does
    /// not.
    ///
    /// @param peer The peer's host, as `Answer` receives it.
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @return The encoded refusal to send back, or nullopt to go on and read the
    ///         payload. A refusal is answered as a **reply and a resynchronization**
    ///         by the caller -- never a close, because the frame declared its length
    ///         and a peer that cannot tell a policy refusal from a dead host retries
    ///         forever.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer,
                                                                           std::uint8_t opRaw) const = 0;

    /// Does this surface require a credential before this verb?
    ///
    /// Asked once per frame rather than cached, because a surface may be
    /// reconfigured and a connection already open must not keep an answer from
    /// before. It is a field read behind a virtual call, not a probe.
    ///
    /// **Takes the verb, since #290.** Every implementation today ignores it, because
    /// each surface serves one verb family -- but a merged 0xFC listener has no
    /// surface-wide answer available. The two production responders answer this
    /// oppositely and both are right: the scheduler requires a credential when one is
    /// configured, and the cache requires none because *a credential readable by every
    /// local build is not a credential*. A merged surface answering `true` refuses
    /// every local `fastcache-cc` FETCH; answering `false` undoes #289. The cache's
    /// reason is a property of its VERBS rather than of the port they arrive on, so it
    /// survives the merge and the answer follows the verb.
    ///
    /// Still not folded into `RefusePeer`, which now also takes the verb: that one
    /// answers before the payload is read and returns an encoded refusal, this one
    /// feeds `DecidePrePayload` alongside the declared length and the connection's
    /// credential state. Two questions at one point in the loop, not one wider one
    /// ([#289](https://github.com/LASTRADA-Software/fastcached/issues/289)).
    ///
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @return True when unauthenticated peers must be refused this verb.
    [[nodiscard]] virtual bool AuthRequired(std::uint8_t opRaw) const noexcept = 0;

    /// Check an `AUTH` payload against this surface's credential.
    ///
    /// The endpoint terminates `AUTH` rather than passing it to `Answer`, because
    /// what the verb changes is **connection state**, and the responder is shared by
    /// every connection on this surface -- exactly as the daemon's handler keeps
    /// `credentialAccepted` in its own loop rather than in the policy.
    ///
    /// @param payload The `AUTH` request payload, already bounded by `MaxAuthPayload`
    ///        through the pre-payload gate.
    /// @return What was established. Only `Accepted` may mark the connection
    ///         authenticated -- `NoPolicy` is answered `Ok` and verifies nothing.
    [[nodiscard]] virtual CredentialOutcome CheckCredential(std::span<std::byte const> payload) const = 0;

    /// Encode -- and count -- a pre-payload refusal `DecidePrePayload` decided.
    ///
    /// The same division of labour as `RefusePeer`: one predicate decides, and the
    /// surface owns the wording, the wire code and the counter, so a refusal cannot
    /// be worded one way here and another way in `Answer`. The endpoint deliberately
    /// does not encode this itself -- it would then own a counter belonging to a
    /// policy it does not implement.
    ///
    /// Pure virtual rather than defaulted for the reason `RefusePeer` is: a surface
    /// that says nothing would silently stop counting its own refusals, and a
    /// security counter reading zero because nobody wired it is indistinguishable
    /// from one reading zero because nothing was refused.
    ///
    /// @param decision Any value other than `Serve`.
    /// @return The encoded refusal to send back. Never empty.
    [[nodiscard]] virtual std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision) const = 0;

    /// Largest request this surface will buffer.
    ///
    /// Per-responder rather than one constant, and the spread is the point: a
    /// scheduler verb carries a fingerprint and a key, while a cache STORE carries a
    /// whole object file. Sizing both for the larger would hand an unauthenticated
    /// peer a way to make the scheduler allocate megabytes.
    [[nodiscard]] virtual std::size_t MaxRequestBytes() const noexcept = 0;

    /// How many connections this surface will hold open at once.
    ///
    /// This counts CONNECTIONS, not requests, and the distinction became load-bearing
    /// when the endpoint learned to serve a connection until the peer stops (#176).
    /// While each connection was one request, the two were the same number and the
    /// cap read as a request cap; once a connection is long-lived, a slot held for
    /// its lifetime is held across the peer's idle time as well. Naming it for
    /// requests and sizing it for requests -- eight, on the cache surface -- would
    /// have let eight attached peers lock out every other client while barely
    /// sending anything.
    ///
    /// So what it bounds is descriptors and coroutine frames, and it is sized for
    /// those. Memory is NOT what this bounds; `MaxInFlightBytes()` below is, which is
    /// why this can afford to be generous.
    [[nodiscard]] virtual std::size_t MaxOpenConnections() const noexcept = 0;

    /// How many declared payload bytes may be in flight across all connections.
    ///
    /// The connection cap alone does not bound memory -- N connections each
    /// declaring `MaxRequestBytes()` is still N times that. This is what actually
    /// bounds it, and it is checkable at exactly the right moment: the header
    /// declares its length before a single payload byte is read.
    ///
    /// It is therefore also the throttle on concurrent WORK, which is the job the
    /// connection cap above stopped being able to do: a surface serving megabyte
    /// objects limits how many move at once by choosing a budget worth about one of
    /// them, and does not need a second count to say the same thing less precisely.
    /// A refusal here is a reply on a kept connection, so a peer that arrives during
    /// a busy moment is told to come back rather than made to reconnect.
    [[nodiscard]] virtual std::size_t MaxInFlightBytes() const noexcept = 0;
};

/// Accepts connections and answers framed requests on each until the peer stops.
///
/// Shaped after `WorkerServer` rather than `Server`, and for the reason that governs
/// the whole node: `Connection` is built around a `CacheEngine`, and a scheduler has
/// no cache. Taking an `IListener&` and running its own loop keeps the node clear of
/// the cache stack while still reusing the reactor and the socket abstraction.
///
/// ## Many requests per connection
///
/// A connection is served until the peer stops sending, which is what the daemon
/// serving this same wire has always done (`Protocol/CompileCacheHandler`). This
/// endpoint answered exactly one request and closed, and that was issue #176 rather
/// than a simpler design: a worker dials once per heartbeat ROUND and then registers
/// every toolchain it found over that one connection, so a machine with two
/// toolchains registered one of them, every round, forever -- reported as a transport
/// failure, which named neither the cause nor the toolchain that lost.
///
/// There is still no per-connection state and no handshake; a loop is not a session.
/// What it buys is that a peer with two things to say may say both, which is a
/// property of the wire (a credential is a frame, so presenting one is two frames)
/// rather than an optimisation.
///
/// ## The payload cap is small on purpose
///
/// A scheduler verb carries a fingerprint, an endpoint and a key, none of which is
/// large, so the ceiling is kilobytes rather than the cache's megabytes. A frame over
/// it is refused with a *reply* naming both numbers, not a close.
///
/// It used to carry a second job: membership was checked inside the service, after
/// the frame had been read, so the small cap was what bounded what a stranger could
/// make this endpoint allocate. `IFrameResponder::RefusePeer` closes that directly
/// now (#285, #377) -- a peer the surface will refuse is refused before its payload
/// is read at all -- and the cap is back to being only what it says it is.
class FrameServer
{
  public:
    /// How long one request may take to arrive before its socket is abandoned.
    ///
    /// This used to be `SO_RCVTIMEO`, applied to every accepted socket by
    /// `BlockingListener::SetTimeouts`. A reactor socket has no such option -- its
    /// reads suspend rather than block -- so without a replacement a client that
    /// connects and sends half a header holds a descriptor and a coroutine frame
    /// until the process dies. A slow-loris on the node's cache port, free.
    ///
    /// Armed once per REQUEST rather than once per connection, so a conversation is
    /// not swept mid-flight. One window covers a request from its first byte to its
    /// answer, and the idle gap before it -- so an attached peer that stops talking is
    /// still swept, which is what keeps the slow-loris property once a connection is
    /// long-lived.
    static constexpr std::chrono::milliseconds RequestTimeout { 5'000 };

    /// How often the sweeper looks for connections past their deadline.
    ///
    /// ONE sweeper per server rather than one timer per connection, and that is the
    /// difference between a bounded cost and a parked frame per client.
    /// `IReactor::Schedule` cannot be cancelled, so a per-connection timer would
    /// stay on the wheel for the full interval after its connection had already
    /// finished -- the leak `Async/DeadlineTimer` documents at length.
    static constexpr std::chrono::milliseconds SweepInterval { RequestTimeout / 4 };

    /// How long a REFUSED connection is given to read its refusal and hang up.
    ///
    /// Shorter than `RequestTimeout` because there is less to wait for: the peer has
    /// its answer already and has only to close, which is a round trip rather than a
    /// request. A refusal takes no connection slot, so nothing counts one -- and a
    /// surface that is refusing is one already at capacity, which is exactly when a
    /// flood must not be able to park a socket per attempt for as long as a real
    /// request gets.
    ///
    /// One sweep interval, because the sweeper's cadence is the floor on how promptly
    /// any deadline can be acted on: asking for less would be a number that reads like
    /// a guarantee and is not one. So a refused socket outlives its answer by at most
    /// two ticks, against the six a served request may take.
    static constexpr std::chrono::milliseconds RefusalTimeout { SweepInterval };

    /// @param io The loop this server accepts and answers on.
    /// @param listener Bound listener; must outlive the run.
    /// @param responder Answers each request; must outlive the run.
    /// @param what Names this surface in log lines, so three endpoints in one
    ///        process are distinguishable when one of them stops accepting.
    /// @param logger Shared logger.
    FrameServer(
        NodeIoLoop& io, IListener& listener, IFrameResponder& responder, std::string_view what, ILogger& logger) noexcept;

    FrameServer(FrameServer const&) = delete;
    FrameServer(FrameServer&&) = delete;
    FrameServer& operator=(FrameServer const&) = delete;
    FrameServer& operator=(FrameServer&&) = delete;
    ~FrameServer();

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    ///
    /// Each accepted connection is served by a `DetachedTask` of its own rather
    /// than inline, which is the entire point: the cache surface consults an
    /// upstream from inside its answer, and serving that inline is what made one
    /// slow dial stall every other client of this node.
    [[nodiscard]] Task<void> Run();

    /// Stop accepting, close what is open, and wait for the connections to end.
    ///
    /// Safe from any thread. The closes are POSTED onto the reactor rather than
    /// done here, and that is not caution: on epoll and kqueue `ISocket::Close`
    /// completes a parked awaitable by resuming its coroutine INLINE, so closing
    /// from another thread would run this server's connection tasks there while the
    /// reactor thread is still driving them. IOCP routes cancellation back through
    /// the port and does not, which is exactly what would make it a race that
    /// passes CI on Windows.
    void Shutdown() noexcept;

    /// Report a loop that threw, without throwing. Called by the owning loop's
    /// exception firewall.
    void NoteLoopThrew() noexcept;

    /// @return How many connections are being served right now. For tests.
    [[nodiscard]] std::size_t OpenConnections() const noexcept;

    /// @return How many declared payload bytes are in flight. For tests.
    [[nodiscard]] std::size_t InFlightBytes() const noexcept;

    /// Implementation detail; public so the .cpp's connection tasks can name it.
    struct State;

  private:
    std::shared_ptr<State> _state;
};

/// The node's scheduler port: listener, server and the thread that serves them,
/// owned as one thing.
///
/// A class rather than three locals in `main()`, for the reason `AdminEndpoint`
/// records: the three have a *destruction order* -- the server must close its
/// listener before the thread serving it can be joined -- and holding them separately
/// expresses that as a `Shutdown()` somebody has to remember at every return path.
/// Here it is the destructor. And `main.cpp` in this binary is in no test target, so
/// wiring that lives there has no unit coverage at all.
class FrameEndpoint
{
  public:
    /// Bind the endpoint and start serving it.
    ///
    /// The error is a diagnostic string rather than one of the project's four error
    /// enums, the same deliberate departure `AdminEndpoint::Start` documents: this
    /// fails in two ways belonging to two taxonomies -- a malformed listen spec is a
    /// `ConfigError`, an address that will not bind is a `NetError` -- the caller's
    /// response is identical either way, and what it needs is the text.
    /// **A surface, never an address.** This used to take the listen spec, the host a
    /// bare port falls back to and the surface's name as three loose arguments, and
    /// every caller supplied its own -- so the port map lived here as well as in the
    /// four other places #288 found it, and a new surface could be opened without
    /// appearing on the map an operator is told to build a firewall from.
    ///
    /// Taking the enumerator makes that impossible rather than merely discouraged:
    /// there is no argument to pass a bare string to, so a port cannot be opened
    /// without a row, and a row cannot exist without `RowsInEnumeratorOrder`
    /// accepting it. A guard that fails the build beats one that fails a suite.
    /// @param io The loop this endpoint accepts and answers on. It must be
    ///        `Start()`ed after every endpoint has been created, so the listener is
    ///        bound and adopted before anything begins accepting.
    /// @param surface Which surface to serve. Its row supplies the address, the host
    ///        a bare port takes and the name used in log lines -- including the
    ///        asymmetry between them, which is the anti-leeching rule rather than a
    ///        caller's preference: a scheduler no peer can dial does nothing, while a
    ///        node's private cache reachable from the network is this machine's whole
    ///        build output served to strangers.
    /// @param cfg What the operator asked for; the row resolves the endpoint from it.
    /// @param responder Answers each request; must outlive the endpoint.
    /// @param logger Where to announce the bound address.
    /// @return The running endpoint, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<FrameEndpoint>, std::string> Start(
        NodeIoLoop& io, NodeSurface surface, NodeConfig const& cfg, IFrameResponder& responder, ILogger& logger);

    /// Stop serving. The loop's own thread is joined by `NodeIoLoop`.
    ~FrameEndpoint();

    FrameEndpoint(FrameEndpoint const&) = delete;
    FrameEndpoint& operator=(FrameEndpoint const&) = delete;

    // Neither movable, for the reason `AdminEndpoint` gives: the reactor holds
    // pointers into `_server`, and `_server` a reference to `_listener`.
    FrameEndpoint(FrameEndpoint&&) = delete;
    FrameEndpoint& operator=(FrameEndpoint&&) = delete;

    /// The address this endpoint actually bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    FrameEndpoint(NodeIoLoop& io,
                  std::unique_ptr<IListener> listener,
                  IFrameResponder& responder,
                  std::string_view what,
                  std::string boundEndpoint,
                  ILogger& logger);

    /// `IListener` and not the platform type, deliberately: naming the concrete one
    /// would drag `<windows.h>` into every header that includes this, and nothing
    /// here needs more than Accept, Close and BoundPort -- the last of which is on
    /// the interface now precisely so this could stop being concrete.
    std::unique_ptr<IListener> _listener;
    std::unique_ptr<FrameServer> _server;
    std::string _boundEndpoint;
};

} // namespace FastCache::Node
