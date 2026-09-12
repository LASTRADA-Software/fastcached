// SPDX-License-Identifier: Apache-2.0
#include "EndpointWriters.hpp"
#include "ExchangeLog.hpp"
#include "FrameEndpoint.hpp"
#include "NodeIoLoop.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/PlatformListener.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Write every byte, or report failure.
    ///
    /// The byte count is checked, not merely the call: a short write leaves the client
    /// blocked on a length the reply promised and never delivered.
    ///
    /// **It takes the writer's NAME, and that parameter is the whole of #675's second
    /// half.** This endpoint's exactly-one-writer property was structural -- the loop
    /// was the only writer because it was the only thing that wrote -- so a lane
    /// reducing the loop's complexity by extracting the nearest block could turn it
    /// into an agreement between two functions with nothing saying so. A write now says
    /// which sanctioned writer it is, so a fifth one is a row in `EndpointWriterTable`
    /// with a reason beside it rather than a call that appeared in a helper.
    /// `EndpointWriters.hpp` carries the argument, and is explicit that this DECLARES
    /// the property rather than enforcing it.
    /// @param writer Which sanctioned writer this is.
    /// @param socket Where to write.
    /// @param bytes What to write.
    /// @return Whether all of it went out.
    [[nodiscard]] Task<bool> WriteAll(EndpointWriter writer, ISocket* socket, std::span<std::byte const> bytes)
    {
        (void) writer;
        auto const written = co_await socket->Write(bytes);
        co_return written.has_value() && *written == bytes.size();
    }

    /// Read and discard until the peer closes, so OUR close can be graceful.
    ///
    /// The counterpart of `WriteAll` on a path that answers and then hangs up: a close
    /// with unread bytes still queued is a reset, and a reset takes the answer back
    /// out of the peer's receive buffer. Returning as soon as the peer closes is what
    /// makes this cheap -- a client that has read its refusal closes at once, so the
    /// common case is one read and an EOF.
    ///
    /// Bounded by what the surface would have read had it accepted the request, so a
    /// refusal never costs more than serving would have; a peer past that has stopped
    /// being one worth being polite to and gets the reset. The socket is tracked
    /// besides, so a peer that goes silent without closing is swept rather than
    /// waited on.
    /// @param socket The socket about to be closed.
    /// @param limit How many bytes may be discarded before giving up on politeness.
    Task<void> DrainUntilPeerCloses(ISocket* socket, std::size_t limit)
    {
        std::array<std::byte, 4096> scratch {};
        for (std::size_t discarded = 0; discarded < limit;)
        {
            auto const read = co_await socket->Read(std::span<std::byte> { scratch });
            if (!read.has_value() || *read == 0)
                break;
            discarded += *read;
        }
        co_return;
    }

    /// What this endpoint answers a connection it has no room for.
    ///
    /// A `Cc::SurfaceRefusal` like every other refusal row on this wire -- the wire
    /// code and the counter as ONE fact with two audiences -- and it lives here rather
    /// than in `CompileRefusal` because it is not a compile refusal: it is decided
    /// before a verb exists, and it is answered identically whether this endpoint
    /// serves compiles, a cache tier, a scheduler or all three.
    constexpr Cc::SurfaceRefusal ConnectionsExhausted {
        .code = Wire::ErrorCode::EndpointBusy,
        .counter = IMetricsSink::Counter::NodeFrameConnectionsRefusedAtCapacity,
    };

} // namespace

/// Everything the accept loop and its connection tasks share.
///
/// In one struct behind a pointer rather than as members of `FrameServer`, so the
/// free-function connection tasks can name it without `FrameServer`'s privates
/// being public -- the same reason `EpollSocket::Impl` is spelled this way.
/// Which of a connection's two windows a tracked deadline belongs to.
///
/// A connection is swept for one of two reasons and they are opposite findings, so
/// the tracker records which rather than leaving a sweep to guess
/// ([#243](https://github.com/LASTRADA-Software/fastcached/issues/243)).
enum class SweepPhase : std::uint8_t
{
    /// Waiting for the peer to name a verb: `FrameServer::HeaderTimeout`.
    AwaitingRequest,
    /// Waiting to answer a verb the peer named: `IFrameResponder::RequestTimeout`.
    AwaitingAnswer,
};

/// What one sweep closed, split by why.
///
/// Two numbers rather than a total, because a total is the thing this ticket exists
/// to replace: the sweep already returned one and logged it at Debug, which is a
/// signal that is correct, present, and emitted where nothing scrapes it.
struct SweepTally
{
    std::size_t awaitingRequest { 0 }; ///< Swept before naming a verb.
    std::size_t awaitingAnswer { 0 };  ///< Swept while an answer was owed.

    /// @return How many connections were swept in total, closed and deferred alike.
    [[nodiscard]] std::size_t Total() const noexcept
    {
        return awaitingRequest + awaitingAnswer;
    }
};

/// One connection this surface is serving, and when it must be done by.
struct TrackedConnection
{
    ISocket* socket { nullptr };
    TimePoint deadline {};
    SweepPhase phase { SweepPhase::AwaitingRequest };

    /// Whether a sweep has already closed and counted this one.
    ///
    /// **A close does not deregister the entry, and the gap is not always short.**
    /// `Untrack` runs when the connection's coroutine unwinds, and closing only
    /// resumes it if it is parked on THIS socket -- a coroutine awaiting a responder
    /// that has not answered stays parked, so the entry remains overdue and the next
    /// sweep finds it again. Without this flag it is closed and COUNTED once per
    /// sweep interval for as long as the responder holds, which turns the counter
    /// into a measure of how long something was stuck rather than of how many
    /// connections were swept. Caught by the two-arm case, which read 5 against an
    /// expected 1.
    bool swept { false };

    /// Whether this process, rather than the peer, is what closed this socket.
    ///
    /// **A peer watch cannot tell the two apart, and they are opposite facts.**
    /// `WatchPeer` reports `gone` whenever its wait fails, and a wait fails just as
    /// readily because this node closed the socket underneath it -- a responder swept
    /// past its explanation grace, or a shutdown closing everything. Counted as
    /// `WorkerJobsAbandonedClientGone`, every compile in flight during a node restart
    /// would land in a row whose documentation says a CLIENT went, so an orderly stop
    /// would read as a fleet losing its users. The rule this breaks is
    /// `metrics-and-observability.md`'s: an absence must not be counted as an event.
    ///
    /// Set under the mutex by the three paths that close a socket this endpoint owns,
    /// and read the same way, because the reader is the connection coroutine and the
    /// writer is the sweeper.
    bool closedLocally { false };

    /// Whether `ServeConnection` is parked INSIDE the responder right now.
    ///
    /// **The whole of #523 turns on this one bit**, because it is what separates the
    /// two things a sweep can find. A connection parked on the SOCKET -- dribbling a
    /// header, dribbling a declared payload, pushing out a reply -- is woken only by
    /// the close, and the close is the write side gone; there is no way to explain
    /// anything to it. A connection parked in the responder is not waiting on this
    /// socket at all: the close wakes nothing (`IFrameResponder::RequestTimeout` says
    /// so at length), the write side is idle, and the coroutine will come back on its
    /// own. So exactly there, and nowhere else, the sweep can be a MESSAGE to the
    /// connection instead of an act on its socket.
    ///
    /// Set and cleared only around `co_await responder.Answer(...)`, under the same
    /// mutex `CloseOverdue` reads it beneath. It is therefore true only while the
    /// phase is `AwaitingAnswer`; the phase still decides the counter, because that is
    /// a statement about which WINDOW expired rather than about where the coroutine
    /// was standing.
    bool inResponder { false };

    /// When this connection stops being given the chance to explain itself.
    ///
    /// **Engaged exactly when a sweep deferred to this connection**, so its engagement
    /// IS the obligation and there is no second flag to keep in step with it. It was
    /// briefly a `bool owedExplanation` beside a bare `TimePoint deferredCloseAt`
    /// documented as "meaningful only while the flag is set" -- a pairing invariant
    /// carried in prose, which three separate sites had to remember to touch together.
    ///
    /// Set by the sweep that chose to defer; taken and disengaged by the connection
    /// when `Answer` returns, which is the only place the obligation can be settled.
    /// `CloseExpiredDeferrals` disengages it too, and that is what stops a wedged
    /// responder from holding a descriptor for the life of the process where the old
    /// unconditional close released it. See `FrameServer::ExplanationGraceFor`.
    std::optional<TimePoint> explainBy {};

    /// The window this connection's current deadline was armed from.
    ///
    /// Kept because the grace above is DERIVED from it, and the two are the same
    /// number by construction rather than by two constants agreeing. A flat constant
    /// here was wrong by a factor of 120 on the compile surface -- see
    /// `FrameServer::ExplanationGraceFor`, which carries that story.
    std::chrono::milliseconds window { 0 };
};

struct FrameServer::State
{
    NodeIoLoop& io;
    IListener& listener;
    IFrameResponder& responder;
    std::string what;
    IMetricsSink& metrics;
    ILogger& logger;

    /// Resolves a compile's fingerprint to the compiler this node would spawn.
    ///
    /// Empty on every surface that has no toolchain map -- the cache tier, the
    /// scheduler, and every fixture -- and the renderer then says only what it knows.
    /// Asked per exchange rather than sampled once, because `main` rebuilds the map on
    /// every re-survey (#238) and a captured name would outlive what it describes.
    Node::ToolchainNamer namer;

    /// Which peers have already been told this node cannot speak their wire version.
    ///
    /// On the SERVER state and not the connection, because a build opens a connection
    /// per translation unit -- a per-connection throttle would suppress nothing (#181).
    Node::RefusalThrottle versionRefusals;

    std::atomic<bool> shuttingDown { false };

    /// Sockets being served, each with the instant it must finish by AND which
    /// window that instant belongs to.
    ///
    /// **The phase is not bookkeeping.** Without it a sweep cannot say whether it
    /// closed a peer that never named a verb or a request this surface accepted and
    /// then failed to answer in time, and those are opposite findings: the first is
    /// background noise on any reachable port, the second is a translation unit that
    /// outlived its lease grant. One counter for both would bury the rare one under
    /// the ordinary one -- the same burial the metrics rules warn about for a refusal
    /// a healthy build gets once per exchange.
    ///
    /// Every mutation happens on the reactor thread; the mutex is here because
    /// `Shutdown()` may be called from another one and needs to read the set to
    /// know whether to wait. Raw pointers, because the owning `unique_ptr` lives in
    /// the connection task's frame and the registration is removed before it ends.
    mutable std::mutex mutex;
    std::vector<TrackedConnection> open;

    std::atomic<std::size_t> openConnections { 0 };
    std::atomic<std::size_t> inFlightBytes { 0 };

    /// How many of this server's own loops -- the accept loop and the sweeper --
    /// are still running.
    ///
    /// `Shutdown()` waits for this to reach zero, and that is not tidiness: both
    /// loops hold a reference to this state, and the accept loop additionally holds
    /// the listener. Returning from `Shutdown()` while either is alive means
    /// `~FrameServer` frees state they are still using, and means the port is still
    /// bound after the endpoint claims to have stopped.
    std::atomic<std::size_t> loopsAlive { 0 };

    State(NodeIoLoop& loop,
          IListener& l,
          IFrameResponder& r,
          std::string_view name,
          IMetricsSink& sink,
          ILogger& log,
          Node::ToolchainNamer toolchainNamer = {}) noexcept:
        io { loop },
        listener { l },
        responder { r },
        what { name },
        metrics { sink },
        logger { log },
        namer { std::move(toolchainNamer) }
    {
    }

    /// The entry for @p socket, or nullptr. The caller must already hold `mutex`.
    ///
    /// One place where "which entry belongs to this socket" is answered, rather than
    /// the four hand-written scans this grew to. Two of those had already diverged --
    /// one ran to completion, one returned at the first match -- encoding opposite
    /// assumptions about an at-most-one invariant that neither of them stated.
    ///
    /// It is stated here instead: a socket is registered by exactly one `Track` and
    /// removed by exactly one `Untrack`, so the first match is the only match and
    /// returning early is not an optimisation but the correct reading.
    /// @param socket The socket to look up.
    /// @return Its entry, or nullptr when it is no longer tracked.
    [[nodiscard]] TrackedConnection* FindLocked(ISocket const* socket) noexcept
    {
        auto const found = std::ranges::find(open, socket, &TrackedConnection::socket);
        return found == open.end() ? nullptr : &*found;
    }

    /// Register a socket with the deadline it must finish by.
    /// @param socket The socket to track.
    /// @param deadline When it must have finished.
    /// @param phase Which window `deadline` belongs to.
    /// @param window The window @p deadline was computed from, kept because the
    ///        explanation grace is derived from it.
    void Track(ISocket* socket, TimePoint deadline, SweepPhase phase, std::chrono::milliseconds window)
    {
        std::scoped_lock const guard { mutex };
        open.push_back(TrackedConnection { .socket = socket, .deadline = deadline, .phase = phase, .window = window });
    }

    /// Move a tracked socket's deadline forward.
    ///
    /// A connection serves many requests, so one deadline armed at accept would sweep
    /// a conversation mid-flight. Armed once per REQUEST instead -- before its header
    /// read, and not again until the next one -- it means "how long this request has,
    /// from its first byte to its answer". Deliberately not re-armed after the header:
    /// that would give a payload a fresh budget of its own and let a slow drip hold a
    /// connection indefinitely, which is the property `RequestTimeout` exists for. The
    /// idle gap before a request is inside the same window, so an attached peer that
    /// stops talking is still swept.
    /// @param socket The tracked socket.
    /// @param deadline Its new deadline.
    /// @param phase Which window the new deadline belongs to. Moved with the
    ///        deadline and never separately: a phase that outlived its own window
    ///        would attribute the next sweep to the previous request's state.
    /// @param window The window @p deadline was computed from, moved with it for the
    ///        same reason and read by `CloseOverdue` to size the explanation grace.
    void Rearm(ISocket* socket, TimePoint deadline, SweepPhase phase, std::chrono::milliseconds window)
    {
        std::scoped_lock const guard { mutex };
        if (auto* const entry = FindLocked(socket); entry != nullptr)
        {
            entry->deadline = deadline;
            entry->phase = phase;
            entry->window = window;
            // `swept` is deliberately NOT cleared. A connection this sweeper has
            // already acted on is finished whatever its coroutine does next, and
            // clearing the flag would let a request that started before the sweep be
            // counted a second time when the next deadline passes.
        }
    }

    /// Record that this connection is now parked INSIDE the responder.
    ///
    /// Called immediately before `co_await responder.Answer(...)` and nowhere else;
    /// `LeaveResponder` is the other half. While it holds, `CloseOverdue` defers
    /// rather than closes -- see `TrackedConnection::inResponder` for why that is the
    /// one state where a sweep can be a message instead of an act on the socket.
    ///
    /// No `bool` parameter, because there is nothing to pass: the leave half has to
    /// return the obligation it takes, so it cannot be this function with `false`.
    /// @param socket The tracked socket.
    void EnterResponder(ISocket* socket)
    {
        std::scoped_lock const guard { mutex };
        if (auto* const entry = FindLocked(socket); entry != nullptr)
            entry->inResponder = true;
    }

    /// Leave the responder, and report whether a sweep left an explanation owing.
    ///
    /// One call and one lock rather than a clear followed by a query, so nothing can
    /// run between them. Nothing could anyway -- both this and the sweeper live on the
    /// one reactor thread and there is no suspension point between `Answer` resuming
    /// and this line -- but a two-step version would make that a property of the
    /// caller's statement order rather than of this function.
    ///
    /// **`explainBy` is REPORTED and deliberately not disengaged, and the first
    /// version of this took it.** Taking it looked right -- the obligation is about to
    /// be settled -- and it opened the hole `CloseExpiredDeferrals` exists to close,
    /// one statement past that function's guard. `swept` is already true and is never
    /// cleared, so `CloseOverdue` skips the entry; with `explainBy` disengaged too,
    /// `CloseExpiredDeferrals` skips it as well, and the `co_await WriteAll` that
    /// follows is then outside EVERY bound this server has. A peer that stops draining
    /// fills its receive window, that write parks, and the descriptor, the coroutine
    /// frame, the connection slot and this entry are held for the life of the process
    /// -- which is exactly the regression the grace was added to prevent, and which
    /// the old unconditional close had bounded.
    ///
    /// So the deadline stays armed across the write. It covers the whole deferred
    /// tail, waiting for `Answer` and delivering the refusal alike, which is one
    /// deadline with one meaning rather than two. A sweep that fires mid-write closes
    /// the socket, the parked write resumes with a failure, nothing is counted, and
    /// the connection ends -- the peer that was not reading gets cut off, which is the
    /// bound working. `Untrack` erases the entry a few statements later, so nothing
    /// has to remember to disengage it.
    /// @param socket The tracked socket.
    /// @return True when this connection was swept and still owes its peer a reason.
    [[nodiscard]] bool LeaveResponder(ISocket* socket)
    {
        std::scoped_lock const guard { mutex };
        auto* const entry = FindLocked(socket);
        if (entry == nullptr)
            return false;
        entry->inResponder = false;
        return entry->explainBy.has_value();
    }

    /// Deregister a socket. Must happen before its owner destroys it.
    void Untrack(ISocket* socket)
    {
        std::scoped_lock const guard { mutex };
        std::erase_if(open, [socket](auto const& entry) { return entry.socket == socket; });
    }

    /// Sweep every socket past its deadline: COUNT each by why, then either close it
    /// or hand the decision to the connection that owns its write side.
    ///
    /// **This sweeper never writes to a socket, and that is a rule about ownership
    /// rather than about interleaving.** The obvious repair for the silence -- have
    /// the sweeper send a refusal before closing -- is not merely racy, it is a
    /// use-after-free: the socket is held by `ServeConnection`'s coroutine frame
    /// (`std::unique_ptr<ISocket> owned`) and this table holds a bare pointer, so a
    /// `co_await socket->Write(...)` here would hold that pointer across a suspension
    /// the connection can complete and destroy through. No amount of write-interlocking
    /// fixes that, which is why a lock is a second bug rather than a fix. The
    /// interleaving hazard is real too -- a refusal spliced into somebody's answer is
    /// the crossed-reply failure of `.agent/rules/distributed-compilation.md` reached
    /// from the output side -- but it is the weaker of the two arguments and the one
    /// that looks negotiable.
    ///
    /// **So the sweep asks where the connection is parked, and there are two answers.**
    ///
    /// Parked ON THE SOCKET -- dribbling a header, dribbling a declared payload,
    /// pushing out a reply. Nothing but the close ends a parked read, and the close is
    /// the write side gone, so there is no way to explain anything. It is closed, as it
    /// always was. A peer here has either named no verb at all or is misbehaving with
    /// one, which is the slow-loris shape rather than the long-compile shape.
    ///
    /// Parked INSIDE THE RESPONDER. The close wakes nothing at all here
    /// (`IFrameResponder::RequestTimeout` states this at length: the compile runs to
    /// completion, hops home, and its `WriteAll` fails), so the close is not the
    /// mechanism it looks like -- it is purely destructive, costing the peer its
    /// explanation and buying one descriptor, while the connection SLOT is held by the
    /// coroutine frame either way. So the entry is marked and left open, and the
    /// connection writes its own named refusal when `Answer` returns, from the one
    /// place that owns the write side. Exactly one writer, structurally rather than by
    /// agreement ([#523](https://github.com/LASTRADA-Software/fastcached/issues/523)).
    ///
    /// The counters are unchanged in meaning: `SweepPhase` says which WINDOW expired,
    /// which is a different question from where the coroutine was standing, and
    /// `FrameDeadlineRefusalsSent` -- moved by the connection, not here -- says how
    /// many of the swept were told why.
    /// @param now The reactor's current time.
    /// @return How many were swept in each phase, closed and deferred alike.
    SweepTally CloseOverdue(TimePoint now)
    {
        std::vector<TrackedConnection> overdue;
        {
            std::scoped_lock const guard { mutex };
            for (auto& entry: open)
                if (entry.deadline <= now && !entry.swept)
                {
                    // Marked HERE, under the same lock that found it, so a second
                    // sweeper turn cannot collect the same entry before this one has
                    // been acted on.
                    entry.swept = true;

                    // A close from here is THIS node's, not the peer's -- see
                    // `TrackedConnection::closedLocally`. Marked even when the close is
                    // deferred, because the deferral ends in `CloseExpiredDeferrals`
                    // closing it just the same.
                    entry.closedLocally = true;

                    // Engaged only for the connections that can act on it, so the
                    // field's engagement IS the obligation and there is no second
                    // flag to keep in step. Sized from the entry's OWN window rather
                    // than a flat constant -- see `FrameServer::ExplanationGraceFor`.
                    if (entry.inResponder)
                        entry.explainBy = now + FrameServer::ExplanationGraceFor(entry.window);
                    overdue.push_back(entry);
                }
        }

        SweepTally tally;
        // Acted on outside the lock: `Close` completes a parked read by resuming its
        // coroutine inline, and that coroutine calls `Untrack`, which takes this
        // same mutex.
        for (auto const& entry: overdue)
        {
            // Counted BEFORE the close, for the reason `SocketDeadlineTarget` records
            // for the launcher's deadline: the close resumes a coroutine inline, so a
            // tally written afterwards is written after that coroutine has already
            // observed the socket shut.
            auto& seen = entry.phase == SweepPhase::AwaitingRequest ? tally.awaitingRequest : tally.awaitingAnswer;
            seen += 1;
            metrics.Increment(entry.phase == SweepPhase::AwaitingRequest ? IMetricsSink::Counter::FrameRequestDeadlineSweeps
                                                                         : IMetricsSink::Counter::FrameAnswerDeadlineSweeps);
            if (!entry.explainBy.has_value())
                entry.socket->Close();
        }
        return tally;
    }

    /// Whether this endpoint is what closed @p socket.
    ///
    /// Asked by the connection before it files an abandoned delivery, so this node's
    /// own teardowns do not arrive in a counter documented as a client-side story. An
    /// entry already erased answers false: `Untrack` runs when the coroutine unwinds,
    /// which is after this is read.
    /// @param socket The connection's socket.
    /// @return True when a sweep or a shutdown closed it here.
    [[nodiscard]] bool ClosedLocally(ISocket const* socket) const
    {
        std::scoped_lock const guard { mutex };
        auto const found = std::ranges::find(open, socket, &TrackedConnection::socket);
        return found != open.end() && found->closedLocally;
    }

    /// Close the connections that were left open to explain themselves and did not.
    ///
    /// The bound on the deferral above. `Answer` is an interface and nothing here can
    /// promise it returns, so a responder that wedges would otherwise hold a
    /// descriptor for the life of the process where the old unconditional close
    /// released it -- a small regression, but a real one, and the sort that is
    /// invisible until a machine runs out of descriptors.
    ///
    /// Nothing is counted here. The sweep that deferred already counted it; this is
    /// the same connection reaching its second deadline, not a second sweep of it, and
    /// a row that rose twice for one connection is exactly the defect
    /// `TrackedConnection::swept` exists to prevent one level up.
    ///
    /// Collected under the lock and closed outside it, for the same reason
    /// `CloseOverdue` above is written that way and not because the shape looked
    /// tidy: `Close` completes a parked read by resuming its coroutine INLINE, and
    /// that coroutine calls `Untrack`, which takes this same mutex. Closing inside
    /// the loop deadlocks.
    /// @param now The reactor's current time.
    /// @return How many deferrals ran out of grace.
    std::size_t CloseExpiredDeferrals(TimePoint now)
    {
        std::vector<ISocket*> expired;
        {
            std::scoped_lock const guard { mutex };
            for (auto& entry: open)
                if (entry.explainBy.has_value() && *entry.explainBy <= now)
                {
                    entry.explainBy.reset();
                    entry.closedLocally = true; // See `TrackedConnection::closedLocally`.
                    expired.push_back(entry.socket);
                }
        }
        for (auto* socket: expired)
            socket->Close();
        return expired.size();
    }

    /// Close the listener and every open connection. Reactor thread only.
    void CloseAll()
    {
        listener.Close();

        std::vector<ISocket*> sockets;
        {
            std::scoped_lock const guard { mutex };
            for (auto& entry: open)
            {
                entry.closedLocally = true; // See `TrackedConnection::closedLocally`.
                sockets.push_back(entry.socket);
            }
        }
        for (auto* socket: sockets)
            socket->Close();
    }

    [[nodiscard]] std::size_t OpenCount() const
    {
        std::scoped_lock const guard { mutex };
        return open.size();
    }
};

namespace
{

    /// Holds a connection's slot in the open count for as long as it is served.
    ///
    /// RAII rather than a decrement at each of the task's exits: a connection ends
    /// several ways -- a foreign magic, an oversize declaration, a short read, a peer
    /// that simply stops -- and the exit that gets forgotten leaks a slot, after which
    /// the surface refuses everything forever while looking perfectly healthy. The
    /// loop added for #176 multiplied those exits, which is exactly when a hand-placed
    /// decrement would have started being wrong.
    class OpenConnectionSlot
    {
      public:
        explicit OpenConnectionSlot(FrameServer::State* state) noexcept:
            _state { state }
        {
            _state->openConnections.fetch_add(1, std::memory_order_acq_rel);
        }

        OpenConnectionSlot(OpenConnectionSlot const&) = delete;
        OpenConnectionSlot(OpenConnectionSlot&&) = delete;
        OpenConnectionSlot& operator=(OpenConnectionSlot const&) = delete;
        OpenConnectionSlot& operator=(OpenConnectionSlot&&) = delete;

        ~OpenConnectionSlot()
        {
            _state->openConnections.fetch_sub(1, std::memory_order_acq_rel);
        }

      private:
        FrameServer::State* _state;
    };

    /// Holds a request's declared bytes in the in-flight budget while it is read.
    class BudgetedBytes
    {
      public:
        BudgetedBytes(FrameServer::State* state, std::size_t bytes) noexcept:
            _state { state },
            _bytes { bytes }
        {
            _state->inFlightBytes.fetch_add(_bytes, std::memory_order_acq_rel);
        }

        BudgetedBytes(BudgetedBytes const&) = delete;
        BudgetedBytes(BudgetedBytes&&) = delete;
        BudgetedBytes& operator=(BudgetedBytes const&) = delete;
        BudgetedBytes& operator=(BudgetedBytes&&) = delete;

        ~BudgetedBytes()
        {
            Release();
        }

        /// Give the reservation back now, rather than at the end of the scope.
        ///
        /// For a verb whose owner accounts for the same bytes itself
        /// (`IFrameResponder::HoldsOwnByteBudget`), holding this across `Answer`
        /// counts one buffer in two pools for as long as the answer takes -- which
        /// for a compile is the compile (#448).
        ///
        /// Idempotent, and it must be: the destructor still runs, and a release owed
        /// once that ran twice would underflow the budget to near `SIZE_MAX` and
        /// refuse the surface's every subsequent request with `EndpointBusy`.
        void Release() noexcept
        {
            if (_bytes == 0)
                return;
            _state->inFlightBytes.fetch_sub(_bytes, std::memory_order_acq_rel);
            _bytes = 0;
        }

      private:
        FrameServer::State* _state;
        std::size_t _bytes;
    };

    /// Answer requests on one connection until the peer stops sending.
    ///
    /// A LOOP, and that is issue #176 rather than a refinement. This served exactly
    /// one request and closed, while two callers in this tree send more than one down
    /// a connection they opened once: the heartbeat dials per ROUND and then registers
    /// or heartbeats every toolchain over it (`main.cpp`), and `Cc::Exchange` writes
    /// AUTH and the command back to back and reads two replies (`CacheProtocol.cpp`).
    /// The first silently cost a two-toolchain worker half its fleet presence; the
    /// second made a credential impossible to present at all.
    ///
    /// The daemon serving this identical wire has always looped
    /// (`CompileCacheHandler.cpp`), so what follows is that policy applied here rather
    /// than a second opinion about one protocol: a refusal is a reply and a
    /// resynchronization, and only a frame with no recognisable header -- and so no
    /// length to step over -- ends the connection.
    ///
    /// A free function taking raw pointers rather than a capturing lambda: a
    /// coroutine's closure outlives the expression that created it.
    /// @param state Shared server state.
    /// @param owned The accepted socket; this task owns it.
    /// Answer an `AUTH` frame and record what it established on this connection.
    ///
    /// Separated from `ServeConnection` because it needs none of the loop: one
    /// payload, one flag, one reply. That keeps the loop under the
    /// cognitive-complexity ceiling and puts the credential rules where they can be
    /// read without the framing around them.
    ///
    /// @param responder The surface whose credential this is.
    /// @param payload The AUTH request body, already bounded by `MaxAuthPayload`.
    /// @param opRaw The AUTH opcode as received, so the refusal reaches the surface
    ///        that owns the credential rather than being encoded here.
    /// @param credentialAccepted This connection's flag; set only on `Accepted`.
    /// @return The reply frame to write.
    [[nodiscard]] std::vector<std::byte> AnswerAuth(IFrameResponder const& responder,
                                                    std::span<std::byte const> payload,
                                                    std::uint8_t opRaw,
                                                    bool& credentialAccepted)
    {
        auto const outcome = responder.CheckCredential(payload);

        // `NoPolicy` answers Ok and sets NOTHING. A surface with no credential must
        // not break a token-configured client, and must not mark it authenticated
        // either -- nothing was verified, and a later reconfiguration would otherwise
        // inherit the blessing.
        if (outcome == CredentialOutcome::Accepted)
            credentialAccepted = true;

        // Total over the enumerators, so a fifth outcome cannot be answered by
        // falling through to Ok -- the one wrong answer here, because it would tell a
        // client its credential was accepted.
        //
        // Both refusals are ANSWERED BY THE SURFACE, which owns the credential and so
        // owns the counter. Encoded here they moved nothing at all, and the second one
        // is the expensive silence: a peer presenting a WRONG token is exactly what
        // `SchedulerRequestsRefusedUnauthenticated` exists to make visible, that
        // counter fires only on the pre-payload gate, and so credential guessing was
        // invisible to the one series an operator would go looking at (#447).
        switch (outcome)
        {
            case CredentialOutcome::Malformed:
                return responder.EndpointRefusalReply(EndpointRefusal::CredentialMalformed, opRaw, {});
            case CredentialOutcome::Rejected:
                return responder.EndpointRefusalReply(EndpointRefusal::CredentialRejected, opRaw, "authentication failed");
            case CredentialOutcome::NoPolicy:
            case CredentialOutcome::Accepted:
                break;
        }
        return Wire::EncodeReply(Wire::Status::Ok, {});
    }

    /// Encode the refusal a connection owes its peer after a sweep deferred to it.
    ///
    /// Lifted out of `ServeConnection` for the reason `AnswerAuth` above was, and the
    /// loop's own note says it: it "sits at its cognitive-complexity ceiling", so an
    /// arm that needs one verb and one number is exactly the part that reads better
    /// without the framing around it.
    ///
    /// **The window is named, and that is the whole message.** "Too slow" without the
    /// number tells an operator nothing about which timeout to raise -- and for a
    /// compile the answer is always the lease grant rather than this worker's speed,
    /// which is a thing a person can only act on if the reply says what the grant was.
    /// The same reason the in-flight refusal names both its figures.
    ///
    /// **Encoded and counted by the SURFACE**, exactly as every other refusal in that
    /// loop is: the endpoint owns WHEN the question is asked, the surface owns the
    /// answer. `EndpointRefusal::AnswerDeadline` is the one row every surface answers
    /// without a counter, because the deadline is the endpoint's own fact and the
    /// endpoint already counts both halves of it -- see
    /// `Node::AnswerDeadlineIsTheEndpointsRationale`.
    /// **The window is PASSED, not re-asked**, and the first version re-asked it four
    /// lines below the comment saying why not to. `RequestTimeout` is a virtual on a
    /// surface that may be reconfigured, so a second call can answer a different
    /// number from the one that armed the deadline and sized the grace -- and the
    /// refusal would then name a window that did not expire, sending an operator to
    /// tune the wrong one. The serve loop reads it once and carries it here.
    /// @param state The server state, for the surface and its name.
    /// @param opRaw The verb whose window expired.
    /// @param window That verb's window, as read when the deadline was armed.
    /// @return The encoded refusal. Never empty.
    [[nodiscard]] std::vector<std::byte> DeadlineRefusalReply(FrameServer::State const& state,
                                                              std::uint8_t opRaw,
                                                              std::chrono::milliseconds window)
    {
        return state.responder.EndpointRefusalReply(
            EndpointRefusal::AnswerDeadline,
            opRaw,
            std::format("{} allows {} ms to answer this verb", state.what, window.count()));
    }

    /// What one peer watch learned while a responder was answering.
    ///
    /// Written only by `WatchPeer` and read only by `ServeConnection`, both on the one
    /// reactor thread the node's framed surfaces share -- so plain members, with no
    /// atomics and no lock. The `shared_ptr` is not for thread safety either: it is for
    /// LIFETIME, because the watcher can still be parked when the connection's frame
    /// unwinds.
    struct PeerWatch
    {
        /// True once the watcher has reached a terminal state and freed its frame.
        ///
        /// **The distinction the whole handoff turns on.** Finished means the socket's
        /// single read-op slot is free and the loop may read again; not finished means
        /// the watcher is parked in `WaitReadable` and only `Close()` can retrieve it.
        bool finished { false };

        /// True when the peer is gone: EOF, a reset, or a socket error.
        bool gone { false };

        /// True once the connection has asked this watch what it learned.
        ///
        /// **What separates a departure the connection could act on from one it could
        /// not**, and `FramePeerWatchDepartures` is the only thing that reads it.
        /// Every honest client hangs up as well -- `Cc::RunOneExchange` reads its
        /// reply and closes -- so a counter without this discriminator would rise once
        /// per watched exchange and bury the mid-answer case it exists to show.
        ///
        /// It is set AFTER the flags are read and never before: set first, a watcher
        /// concluding inside `AbandonIfPeerGone`'s one yield would be acted on and not
        /// counted, and the two rows would disagree in the one direction the counter's
        /// documentation promises they cannot.
        ///
        /// **One of two questions, not the whole of it.** This bit can only say what
        /// the CONNECTION did; a sweep closing the socket while that connection is
        /// still parked inside the responder leaves it false, and `RecordDeparture`
        /// asks `ClosedLocally` for exactly that case. Deleting either question fails
        /// a different case on purpose.
        bool consulted { false };

        /// Where an abandoned delivery on this request is recorded.
        ///
        /// **A plain value, and that is the point.** The surface answers
        /// `PeerWatchCounter` with an *optional*, and the watch exists exactly when that
        /// optional was engaged -- so carrying it here consults the optional once, at
        /// the only place that can answer it, and leaves the loop holding a value it
        /// cannot mis-handle. Held as a second optional beside the watch, the guard
        /// (`watch != nullptr`) and the dereference sat three statements apart and were
        /// coupled by an invariant a reader had to reconstruct, which is what
        /// `bugprone-unchecked-optional-access` objects to and it is right to.
        IMetricsSink::Counter counter {};
    };

    /// How a peer left, as the transport reported it.
    ///
    /// **An enum and not a bool**, because the question already has more than two
    /// answers: a watch also reaches `RecordDeparture` for a socket THIS NODE closed,
    /// which is neither of these and is suppressed there rather than named here. A
    /// bool would have to be read as "abortive, or else something", which is the shape
    /// this tree keeps having to undo.
    enum class PeerDeparture : std::uint8_t
    {
        /// The peer reset the connection: `WaitReadable` answered an ERROR.
        Abortive,

        /// The peer closed gracefully: `WaitReadable` answered `0`, which is EOF.
        Graceful,

        Last,
    };

    /// What each cause adds beyond the shared departure rows.
    struct PeerDepartureRow
    {
        PeerDeparture cause {};

        /// The sibling counter this cause also increments, if it has one.
        ///
        /// Disengaged is a DECISION and not an omission: the graceful half is exactly
        /// `departures - abortive`, so a row for it would carry no fact the pair does
        /// not already carry, and it is the half nobody alerts on.
        std::optional<IMetricsSink::Counter> counter {};
    };

    /// The causes, in enumerator order.
    ///
    /// A table rather than an `if` at the one call site, so a third cause -- a peer
    /// swept by this node, say, were it ever named here rather than suppressed -- is a
    /// row and not a new arm threaded through `RecordDeparture`.
    constexpr std::array<PeerDepartureRow, static_cast<std::size_t>(PeerDeparture::Last)> PeerDepartureTable { {
        { .cause = PeerDeparture::Abortive, .counter = IMetricsSink::Counter::FramePeerWatchDeparturesAbortive },
        { .cause = PeerDeparture::Graceful, .counter = std::nullopt },
    } };
    static_assert(RowsInEnumeratorOrder(PeerDepartureTable, &PeerDepartureRow::cause));

    /// Mark a watch as having reached "gone", and count it when it still means
    /// something.
    ///
    /// One function rather than the same two statements at each of `WatchPeer`'s two
    /// arms, because those arms are exactly what nobody may forget: they set one flag,
    /// so a green suite proves nothing about which of them is live, and an arm that
    /// set the flag without the count would be invisible to the very row that exists
    /// to show it ([#673](https://github.com/LASTRADA-Software/fastcached/issues/673)).
    ///
    /// **TWO questions guard the count, and neither covers the other.** They exclude
    /// two different things that both reach a parked watcher looking exactly like a
    /// client hanging up:
    ///
    /// - `PeerWatch::consulted` excludes the ORDINARY hang-up. Every honest client
    ///   closes once it has its reply, and by then the connection has read the watch,
    ///   so an unconditional count would rise once per watched exchange.
    /// - `ClosedLocally` excludes THIS NODE's teardowns -- a sweep past the
    ///   explanation grace, a shutdown -- which arrive while the connection is still
    ///   inside the responder and has consulted nothing, so the flag above is false
    ///   and says nothing about them. It is the same question `AbandonIfPeerGone` asks
    ///   before it files an abandoned delivery, for the same reason and one row over:
    ///   an orderly stop must not read as a fleet losing its users.
    ///
    /// Asked in that order because the first is a plain bool and the second takes the
    /// state's mutex, so the ordinary exchange -- which is nearly all of them -- pays
    /// nothing.
    ///
    /// **The unguarded row is counted first, and it is not bookkeeping.** A filed
    /// departure is a count of BAD things, and a count of bad things staying at zero
    /// is the healthy reading AND what a watch that never armed or never concluded
    /// looks like; `FramePeerWatchDeparturesObserved` is the separate assertion that
    /// the good thing happened, which is what makes the flat row mean something to an
    /// operator and to a test. Incremented in the SAME statement sequence as the
    /// decision, with nothing suspending between, so a reader that has seen this row
    /// move knows the row below has already been decided for that watch -- which is
    /// the only observation a case can make that does not depend on when a given
    /// reactor retrieves a parked watcher.
    /// @param state The server state: the counters, and whether this node closed it.
    /// @param socket The connection, to ask that second question about.
    /// @param watch The watch reaching its verdict.
    /// @param cause How the peer left, which decides only the sibling row: both
    ///        suppressions above apply to it unchanged, so the abortive row is a
    ///        SUBSET of the departures row rather than a parallel tally that could
    ///        disagree with it.
    void RecordDeparture(FrameServer::State& state, ISocket const* socket, PeerWatch& watch, PeerDeparture cause)
    {
        watch.gone = true;
        state.metrics.Increment(IMetricsSink::Counter::FramePeerWatchDeparturesObserved);
        if (watch.consulted || state.ClosedLocally(socket))
            return;
        state.metrics.Increment(IMetricsSink::Counter::FramePeerWatchDepartures);
        if (auto const& row = PeerDepartureTable.at(static_cast<std::size_t>(cause)); row.counter.has_value())
            state.metrics.Increment(*row.counter);
    }

    /// Watch for the peer going away while a responder answers.
    ///
    /// **It reads and it never writes**, which is the whole reason this shape was
    /// chosen: `ServeConnection` stays the only writer on the socket, so the
    /// exactly-one-writer property below remains structural rather than becoming a
    /// rule to remember.
    ///
    /// **One wake, then it is done.** It never re-arms, so the socket's single read-op
    /// slot is held for at most one park -- see `PeerWatch::finished`, and
    /// `IFrameResponder::PeerWatchCounter` for what double-arming that slot costs
    /// (a silently leaked coroutine frame, on both epoll and IOCP).
    ///
    /// **A readable edge is not a disconnect.** Calling one "gone" would make a worker
    /// discard a perfectly good object every time a client pipelined a request, which
    /// is a worse bug than the one this closes.
    ///
    /// **The `Read` below is no longer what draws that distinction, and this comment
    /// used to say it was** (#711). It claimed `EpollSocket::WaitReadable` reported
    /// readiness for `got >= 0` -- EOF and pending data indistinguishable -- and that
    /// IOCP's zero-byte `WSARecv` could not separate them either. #677 made both false:
    /// `WaitReadable`'s contract is now `0` for EOF and `>0` for data, consuming
    /// nothing, and every transport in this tree honours it. `EpollSocket` and
    /// `KqueueSocket` return the count their `MSG_PEEK` measured rather than a flat 1;
    /// `IocpSocket`'s completion still carries `bytes == 0` for every case, so its
    /// `Dispatch` does the peek itself under `readPeekOnly`; `InMemorySocket` answers
    /// `0` only when its inbound pipe is drained AND write-closed; `TlsSocket`
    /// delegates or reports buffered plaintext.
    ///
    /// So the probe `Read`, `PeerWatch::pulled` and the `PrimeWith` that undid the
    /// consumption were a mechanism whose justification had gone, and
    /// [#1090](https://github.com/LASTRADA-Software/fastcached/issues/1090) removed
    /// them. `WaitReadable` consumes nothing, so a pipelined request now simply stays
    /// in the kernel's receive queue until the loop's own reader collects it -- which
    /// deletes the prepend-ordering hazard rather than documenting it.
    ///
    /// **The two arms did NOT collapse into `!readable.has_value() || *readable == 0`,
    /// which is what #1090 proposed.** That form reaches the right verdict and cannot
    /// say WHICH departure it was, and the two are different diagnoses:
    /// [#1092](https://github.com/LASTRADA-Software/fastcached/issues/1092) needs an
    /// abortive close told apart from a graceful one, because only the first is worth
    /// an alert. So the arms stay, each naming its `PeerDeparture`, and the cleanup
    /// keeps the distinction the transport already draws.
    ///
    /// @param state The server state, shared for the same LIFETIME reason the socket
    ///        is: this watcher can still be parked when the connection's frame has
    ///        gone, and it reaches a counter and the local-close question through
    ///        this.
    ///
    ///        **What that pointer does and does not buy, stated because the
    ///        comfortable version is wrong.** It keeps the STRUCT alive; it does not
    ///        keep `metrics`, `logger`, `io` or `listener` alive, which are
    ///        references to objects `State` does not own. Those outlive it by their
    ///        owners' construction order -- the sink and the loop are `main`'s and
    ///        the endpoint's, both declared before the surface -- and not by anything
    ///        this parameter enforces. So this is protection against the connection
    ///        frame unwinding, which is the case that happens, and not a general
    ///        lifetime guarantee.
    /// @param socket The connection, shared so a parked wait cancelled by `Close()`
    ///        cannot resume onto a destroyed socket -- the second use-after-free
    ///        `RedisResp`'s readable watcher records, in a tree where `Close()`
    ///        resumes inline on epoll and marshals on IOCP.
    /// @param watch Where the answer is left; shared with the connection.
    DetachedTask WatchPeer(std::shared_ptr<FrameServer::State> state,
                           std::shared_ptr<ISocket> socket,
                           std::shared_ptr<PeerWatch> watch)
    {
        // **ONE call decides the cause, and that is what makes the split clean.** This
        // reads the socket exactly once: `WaitReadable` reports the count (#677) and
        // nothing looks at the socket again before the verdict, so each arm carries a
        // single cause.
        //
        // Reintroducing a read between the wait and the verdict brings back a
        // TWO-CAUSE arm and files a reset as a goodbye. That is what the removed probe
        // did: its `if (!got.has_value() || *got == 0)` folded EOF together with the
        // probe failing on a socket `WaitReadable` had just called ready -- a reset
        // landing in the window between the two calls. Both are departures and the
        // total is unchanged, so the misfiling is silent. The clean two-way split is
        // therefore a CONSEQUENCE of #1090 rather than a property of this function,
        // which is the opposite of what #1092 warned about and the same fact.
        auto const readable = co_await socket->WaitReadable();
        if (!readable.has_value())
        {
            // An ERROR is an abortive close -- the peer reset, or the socket was closed
            // under us, including by the `Close()` the connection issues to retrieve
            // this very frame. That last one is this node's own teardown and must not
            // read as a client crashing, which is why `RecordDeparture` still asks
            // `ClosedLocally`: the cause names what the TRANSPORT saw, and the
            // suppressions decide whether it is anybody's business.
            RecordDeparture(*state, socket.get(), *watch, PeerDeparture::Abortive);
        }
        else if (*readable == 0)
        {
            // Zero is EOF: the peer finished sending and said goodbye. Ordinary, and
            // counted apart from the arm above for exactly that reason.
            RecordDeparture(*state, socket.get(), *watch, PeerDeparture::Graceful);
        }
        // Anything else is bytes pending -- a pipelined request, which proves the peer
        // is still there. `WaitReadable` consumed none of them, so there is nothing to
        // hand back and the loop's reader will collect them itself.
        watch->finished = true;
        co_return;
    }

    /// Wait, bounded, for a peer watch to reach a terminal state.
    ///
    /// **Closing straight away loses the reply that was just written**, and that is
    /// measured rather than feared: the pipelined case received its first answer and
    /// then ZERO further bytes, because a close over a socket with I/O still pending is
    /// abortive and takes the buffered reply with it. `RefuseAtCapacity` documents the
    /// same hazard from the other direction -- a peer that saw a reset instead of the
    /// refusal it had been sent.
    ///
    /// So the peer is given the chance to take the answer and hang up, and the watcher
    /// is what observes it doing so: the client's FIN is a readable edge, which is
    /// exactly what that coroutine is parked on. The ordinary client --
    /// `Cc::RunOneExchange`, which reads its reply and closes -- is gone within one
    /// step.
    ///
    /// Polled rather than woken, for the reason `NextWakeStep` states: `IReactor::Schedule`
    /// cannot be cancelled, so a wait that must also be woken by something else sleeps
    /// in steps and re-reads, leaving nothing parked behind it. The loop is inlined
    /// rather than delegated to `InterruptibleSleepUntil` for the reason that header
    /// gives for its own three siblings; the arithmetic is the part they share.
    ///
    /// Bounded by `RefusalTimeout`, and for its reason: the peer has its answer and has
    /// only to read it, which is a round trip rather than a request. One that dawdles
    /// past that is closed on anyway, which is exactly what happened before this
    /// existed. It costs the CLIENT nothing either way -- the reply has already gone
    /// out, and this only makes the hang-up orderly.
    ///
    /// One function rather than a loop at each of the two sites that need it, because
    /// the second is the deferred deadline refusal (#523) -- and a rule remembered at
    /// one write and forgotten at the other is how that explanation goes missing again.
    /// What a connection does next, once a watched reply has gone out.
    ///
    /// Named rather than a bool because the caller acts on it at a `break`, and
    /// `if (settle(...))` at that site reads as neither of the two things it could
    /// mean. The three states a watch can be IN stay on `PeerWatch`; this is only what
    /// the connection does about them, and two of those three collapse here.
    enum class AfterWatch : std::uint8_t
    {
        KeepServing,   ///< The peer is there; anything it pipelined has been primed.
        EndConnection, ///< Stop serving this connection and close.
    };

    /// **Parameters by VALUE, not by reference**, which clang-tidy enforces on
    /// coroutines and is right to: a coroutine's frame outlives the expression that
    /// created it, so a reference parameter is bound to storage the caller may already
    /// have destroyed. The same rule `Cc::Exchange` carries for the same reason. Both
    /// referents happen to outlive every call here -- the reactor is the loop itself
    /// and the watch is held by `ServeConnection` -- and that is exactly the argument
    /// that is not checkable at the call site, which is why the rule is not "unless you
    /// are sure".
    /// @param reactor The loop this connection runs on; never null.
    /// @param watch The watch to wait on, shared so it cannot die under this frame.
    Task<void> AwaitWatchQuiet(IReactor* reactor, std::shared_ptr<PeerWatch const> watch)
    {
        if (watch == nullptr)
            co_return; // Not watching: nothing is parked, so nothing has to be waited out.

        auto const until = reactor->Clock().Now() + FrameServer::RefusalTimeout;
        while (!watch->finished && reactor->Clock().Now() < until)
            co_await SleepUntil { .reactor = reactor,
                                  .deadline = NextWakeStep(reactor->Clock().Now(), until, FrameServer::GracefulCloseStep) };
        co_return;
    }

    /// Start watching for this peer going away, when the surface asks for it.
    ///
    /// **Armed only when the reader holds nothing**, and the reason is now the only one
    /// left: a peer that has just pipelined a request has proved it is there, which is
    /// the entire question this watch exists to ask, so watching would cost a park to
    /// learn what is already known.
    ///
    /// It used to carry a second reason -- the watcher consumed bytes and `PrimeWith`
    /// prepended them back, which corrupts the stream if anything was already buffered.
    /// That hazard is gone rather than guarded: since
    /// [#1090](https://github.com/LASTRADA-Software/fastcached/issues/1090) the watcher
    /// only peeks, so there is nothing to prepend and nothing to order wrongly.
    ///
    /// The optional is consulted HERE and nowhere else; what travels on is a plain
    /// counter on the watch, so the loop holds no optional it could mis-handle.
    /// @param state The server state: the surface that decides, and the sink the
    ///        watcher records through. By reference, and copied only on the arming
    ///        path, because this runs for EVERY verb while the two surfaces that watch
    ///        nothing -- the scheduler and the cache tier -- are the high-frequency
    ///        ones; an owning parameter charged both of them a control-block
    ///        increment for a pointer this function returns without having used.
    /// @param opRaw The verb, as received.
    /// @param reader The connection's reader; a non-empty buffer declines the watch.
    /// @param socket The connection, shared with the watcher for its lifetime.
    /// @return The watch, or null when this request is not watched.
    [[nodiscard]] std::shared_ptr<PeerWatch> ArmPeerWatch(std::shared_ptr<FrameServer::State> const& state,
                                                          std::uint8_t opRaw,
                                                          ByteReader const& reader,
                                                          std::shared_ptr<ISocket> socket)
    {
        auto const counter = state->responder.PeerWatchCounter(opRaw);
        if (!counter.has_value() || !reader.Buffered().empty())
            return nullptr;

        auto watch = std::make_shared<PeerWatch>(PeerWatch { .counter = *counter });
        WatchPeer(state, std::move(socket), watch);
        return watch;
    }

    /// Marks a peer watch consulted when the request that armed it ends, however it
    /// ends.
    ///
    /// RAII for the reason `OpenConnectionSlot` above is RAII, and it is the same
    /// failure: a request ends several ways -- a pulse that will not let go, a
    /// deferred refusal, an abandoned delivery, a write that fails, a throw -- and the
    /// exit that gets forgotten leaves the watch looking actionable after the
    /// connection has stopped being able to act, so the `socket->Close()` that follows
    /// arrives at the watcher as a departure.
    ///
    /// **It covers only the closes the CONNECTION performs**, which is what its scope
    /// can express, and that is why `RecordDeparture` asks `ClosedLocally` as well
    /// rather than instead. A sweep past the explanation grace closes the socket while
    /// this scope is still open -- the connection is parked inside the responder and
    /// has ended nothing -- so this guard is structurally unable to see it. Measured,
    /// not reasoned: the swept case counted a departure until that second question was
    /// added.
    ///
    /// It is a backstop and not the mechanism: `AbandonIfPeerGone` marks the watch
    /// itself, immediately after reading the flags, because only there is the ORDER
    /// right. See `PeerWatch::consulted`.
    class ConsultedAtExit
    {
      public:
        /// @param watch The watch to mark; null is the ordinary unwatched request.
        ///        NOT owning, exactly as `OpenConnectionSlot` above is not: this is
        ///        declared after the watch in the same block, so reverse destruction
        ///        order already keeps the pointee alive across `~ConsultedAtExit` on
        ///        every exit -- fallthrough, `break` and a throw alike -- and an owning
        ///        copy would buy that same guarantee a second time, at an atomic apiece.
        explicit ConsultedAtExit(PeerWatch* watch) noexcept:
            _watch { watch }
        {
        }

        ~ConsultedAtExit()
        {
            if (_watch != nullptr)
                _watch->consulted = true;
        }

        ConsultedAtExit(ConsultedAtExit const&) = delete;
        ConsultedAtExit(ConsultedAtExit&&) = delete;
        ConsultedAtExit& operator=(ConsultedAtExit const&) = delete;
        ConsultedAtExit& operator=(ConsultedAtExit&&) = delete;

      private:
        PeerWatch* _watch;
    };

    /// Whether the peer has already gone, asked before the reply is written -- and if
    /// so, record it.
    ///
    /// **The one question worth asking BEFORE the reply**, and the only one: the other
    /// two answers a watch can carry -- still parked, and woke with a pipelined request
    /// -- survive the write, and one of them can only BECOME true during it. So they
    /// are `SettleWatch`'s, after.
    ///
    /// Lifted out of `ServeConnection` beside its sibling and for the same reason: that
    /// loop sits at its cognitive-complexity ceiling, and this is an arm that reads
    /// better without the framing around it. **It touches no write side** -- it yields,
    /// reads two flags and answers -- so the loop remains the only writer, structurally
    /// rather than by agreement.
    /// **It records the abandonment as well as reporting it**, and that is deliberate:
    /// the decision and the counter are one fact, and split across a helper and its call
    /// site they were a nested condition in the loop that owns neither.
    /// **It writes exactly one flag on the watch**, and that is not a widening of its
    /// job: `PeerWatch::consulted` records that this question was ASKED, which only
    /// this function can say, and it has to be said in the same statement sequence as
    /// the read or the ordering `FramePeerWatchDepartures` rests on becomes a rule
    /// somebody has to remember. It still touches no write side of the socket.
    /// @param state The server state, for the counter and the local-close question.
    /// @param socket The connection, to ask whether THIS node closed it.
    /// @param watch The watch, shared so it cannot die under the yield; may be null.
    /// @param hadReply Whether there was an answer to deliver at all.
    /// @return True when the peer went before the answer was ready.
    Task<bool> AbandonIfPeerGone(FrameServer::State* state,
                                 ISocket const* socket,
                                 std::shared_ptr<PeerWatch> watch,
                                 bool hadReply)
    {
        // **Both arms end the connection without writing, which is why they are one
        // question.** An empty reply is `IFrameResponder::Answer`'s way of saying "close
        // without answering" -- only ever right when the peer is not speaking this
        // protocol at all -- and a peer that has gone is not owed the answer either.
        // They are counted differently and acted on identically, so the counting stays
        // below and the decision is asked once.
        if (!hadReply)
            co_return true;

        auto* const reactor = &state->io.Reactor();
        if (watch == nullptr)
            co_return false; // Not watching: nothing was learned, so nothing is claimed.

        // **One turn of the loop before reading the flags**, and it is what makes a
        // request pipelined DURING the answer survive rather than merely usually
        // survive. The watcher's wake is a reactor event and so is a compile hopping
        // home, so at this instant the wake can be queued and not yet run -- and reading
        // the flags then would end a connection whose peer had just proved it was there.
        //
        // Yielding lets everything already queued run first, on the one thread both of
        // them use. It cannot wait for a watcher that is genuinely parked, and must not:
        // this yields ONCE and re-reads, rather than waiting on something that may never
        // arrive.
        if (!watch->finished)
            co_await ResumeOn { *reactor };

        // **Read, then marked, and the order is the whole of it.** A watcher that
        // concluded inside the yield above is acted on here AND counted by
        // `RecordDeparture`, because the mark is still unset while it runs; marked
        // first, that departure would be acted on and not counted, which is the one
        // direction `FramePeerWatchDepartures` promises cannot happen.
        auto const gone = watch->gone;
        watch->consulted = true;
        if (!gone)
            co_return false;

        // Counted only when there was something to deliver AND when a CLIENT is what
        // went: an empty reply means the surface had already decided to close, and a
        // socket this process closed -- a sweep past the grace, a shutdown -- reaches
        // the watcher looking exactly like a peer that hung up. Filing either would put
        // this node's own teardowns in a row documented as a client-side story.
        if (hadReply && !state->ClosedLocally(socket))
            state->metrics.Increment(watch->counter);
        co_return true;
    }

    /// Settle a watch once the reply has been written, and say what to do next.
    ///
    /// Lifted out of `ServeConnection` for the reason `AnswerAuth` and
    /// `DeadlineRefusalReply` were: that loop sits at its cognitive-complexity ceiling,
    /// and an arm that needs one wait and three branches reads better without the
    /// framing around it. The ordering argument it implements lives at the call site,
    /// where the `WriteAll` it must follow is visible.
    /// @param reactor The loop this connection runs on; never null.
    /// @param watch The watch, shared so it cannot die under the wait.
    /// @return Whether this connection may serve another request.
    ///
    /// It took a `ByteReader*` until #1090, to hand back the bytes the probe `Read` had
    /// consumed. The watcher only peeks now, so there is nothing to hand back and the
    /// parameter went with the priming rather than being left unused.
    Task<AfterWatch> SettleWatch(IReactor* reactor, std::shared_ptr<PeerWatch> watch)
    {
        if (watch == nullptr)
            co_return AfterWatch::KeepServing; // Not watching: the loop is unchanged.

        co_await AwaitWatchQuiet(reactor, watch);

        // **A watch still parked ends the connection, and the caller's `break` IS the
        // cancellation**: it falls through to `socket->Close()`, the only thing on
        // `ISocket` that can retrieve a coroutine parked in `WaitReadable`. Looping
        // round instead would arm a `Read` over that parked wait and silently leak its
        // frame; `IFrameResponder::PeerWatchCounter` carries the derivation.
        if (!watch->finished)
            co_return AfterWatch::EndConnection;

        // The peer took its answer and hung up, which is the ordinary end of a watched
        // exchange and not an abandoned delivery: the reply already went out.
        if (watch->gone)
            co_return AfterWatch::EndConnection;

        // Nothing to hand back: the watcher only ever peeked. A pipelined request is
        // still in the kernel's receive queue and the loop's own reader takes it from
        // there, which is why this no longer primes anything (#1090).
        co_return AfterWatch::KeepServing;
    }

    /// The state a progress pulse and its connection share.
    ///
    /// Shaped after `PeerWatch` above, and it is the mirror image of it: that one READS
    /// while the responder answers, this one WRITES. Both exist because
    /// `ServeConnection` is suspended inside `Answer` and can do neither itself.
    struct ProgressPulse
    {
        /// Set by the connection when the answer is ready; read by the pulse before
        /// every sleep and before every write.
        ///
        /// A plain `bool` and not an atomic: the connection task and this pulse are two
        /// coroutines on the ONE reactor thread these surfaces share, so there is no
        /// concurrency here to synchronise -- only interleaving. Saying so here is what
        /// makes that a stated precondition rather than an assumption somebody has to
        /// reconstruct from the absence of a lock.
        bool stopped { false };

        /// True once the pulse has reached its end and is no longer touching the socket.
        ///
        /// **The distinction the hand-back turns on**, exactly as `PeerWatch::finished`
        /// is: finished means the socket's write side is free and the connection may
        /// write the reply; not finished means the pulse may be suspended inside
        /// `Write`, and arming a second write over that is the shared-slot defect this
        /// endpoint already carries a watch for on the read side.
        bool finished { false };
    };

    /// Write `Status::Progress` to the peer every `interval` until told to stop.
    ///
    /// **The liveness signal a dispatched compile owes its client**
    /// ([#245](https://github.com/LASTRADA-Software/fastcached/issues/245)). A client
    /// bounds a compile with one flat deadline, and one number cannot answer both *how
    /// slow may this legitimately be* and *how fast is a worker that has stopped making
    /// progress noticed* -- the first is minutes by construction, so the second was
    /// minutes too. This is what makes silence measurable, and therefore what lets the
    /// client bound the silence instead of the duration.
    ///
    /// **It writes and it never reads**, which is the mirror of `WatchPeer`'s rule and
    /// keeps the two out of each other's way: the pulse never touches the read side,
    /// the watch never touches the write side, and `ServeConnection` remains the only
    /// thing that does both.
    ///
    /// **It sleeps in steps rather than for the whole interval**, for the reason
    /// `AwaitWatchQuiet` gives: `IReactor::Schedule` cannot be cancelled, so a wait that
    /// must also be endable by something else sleeps in steps and re-reads. Waiting out
    /// a whole five-second interval after `Answer` returned would hold every reply that
    /// long, which is the pulse making the thing it measures slower.
    ///
    /// **The frame carries nothing**, which is `Status::Progress`'s contract rather than
    /// this function's choice: partial diagnostics here would be a second, racy channel
    /// for output the result frame already carries whole. `EncodeProgressReply` takes no
    /// argument, so there is nothing to pass.
    ///
    /// @param reactor The loop this connection runs on; never null.
    /// @param socket The connection, shared so a write cancelled by `Close()` cannot
    ///        resume onto a destroyed socket -- the same lifetime rule `WatchPeer` has.
    /// @param pulse Where its state is left; shared with the connection.
    /// @param interval How long between frames.
    DetachedTask PulseProgress(IReactor* reactor,
                               std::shared_ptr<ISocket> socket,
                               std::shared_ptr<ProgressPulse> pulse,
                               std::chrono::milliseconds interval)
    {
        while (!pulse->stopped)
        {
            auto const until = reactor->Clock().Now() + interval;
            while (!pulse->stopped && reactor->Clock().Now() < until)
                co_await SleepUntil { .reactor = reactor,
                                      .deadline =
                                          NextWakeStep(reactor->Clock().Now(), until, FrameServer::GracefulCloseStep) };

            // Re-read AFTER the sleep and before the write, so a pulse that was told to
            // stop during its own interval never puts a frame in front of the reply it
            // is standing aside for.
            if (pulse->stopped)
                break;

            // **A failed write ends the pulse and reports nothing**, and that boundary is
            // the point: the pulse WRITES, `WatchPeer` READS, and detecting a peer that
            // left belongs to the reader. It is parked on this same socket for the whole
            // answer, a departure makes that socket readable, and `AbandonIfPeerGone` is
            // what counts it -- so a second verdict raised from the write side would be a
            // second observation of one departure, reached by a path with no counter of
            // its own. Stopping is all this has to do; the connection learns it from the
            // mechanism that already knows.
            if (!co_await WriteAll(EndpointWriter::Pulse, socket.get(), Wire::EncodeProgressReply()))
                break;
        }
        pulse->finished = true;
        co_return;
    }

    /// Start pulsing when the surface says this verb is long enough to need it.
    ///
    /// The optional is consulted HERE and nowhere else, exactly as `ArmPeerWatch` does
    /// with its counter: what travels on is a pulse or a null, so the loop holds no
    /// optional it could mis-handle.
    ///
    /// A non-positive interval is treated as *do not pulse*, which is how every ceiling
    /// in this tree spells the absence of a bound -- and here the arithmetic would say
    /// the opposite of the value: a zero interval is a write per reactor turn for the
    /// whole of a compile.
    /// @param responder The surface, which decides whether this verb is pulsed.
    /// @param opRaw The verb, as received.
    /// @param reactor The loop this connection runs on; never null.
    /// @param socket The connection, shared with the pulse for its lifetime.
    /// @return The pulse, or null when this request is not pulsed.
    [[nodiscard]] std::shared_ptr<ProgressPulse> ArmProgressPulse(IFrameResponder const& responder,
                                                                  std::uint8_t opRaw,
                                                                  IReactor* reactor,
                                                                  std::shared_ptr<ISocket> socket)
    {
        auto const interval = responder.ProgressInterval(opRaw);
        if (!interval.has_value() || *interval <= std::chrono::milliseconds::zero())
            return nullptr;

        auto pulse = std::make_shared<ProgressPulse>();
        PulseProgress(reactor, std::move(socket), pulse, *interval);
        return pulse;
    }

    /// Stop a pulse and wait, bounded, for it to let go of the socket.
    ///
    /// **Called before the connection writes ANYTHING, and that is the whole contract.**
    /// While the responder answers, the pulse is the only writer on this socket; the
    /// moment `Answer` returns, the connection has to become the only writer again, and
    /// a reply written over a pulse still suspended inside `Write` is two writes sharing
    /// one write-op slot -- the same defect family this endpoint already arms a watch
    /// against on the read side.
    ///
    /// **Ordinarily it costs one step.** The pulse is driven by a clock this side owns
    /// rather than by the peer, so it observes `stopped` within one
    /// `GracefulCloseStep` and ends; it can only be slower than that if it is suspended
    /// inside a five-byte `Write`, which means the peer has stopped reading.
    ///
    /// **A pulse still parked past the bound ends the connection**, and the caller's
    /// `break` IS the cancellation -- it falls through to `socket->Close()`, which is
    /// the only thing on `ISocket` that can retrieve a coroutine suspended in `Write`.
    /// That is `SettleWatch`'s rule on the read side, applied to the write side for the
    /// same reason: continuing would arm a second write over a parked one.
    ///
    /// Bounded by `RefusalTimeout` for its reason -- what is being waited for is a
    /// round trip's worth of nothing, not a request.
    /// @param reactor The loop this connection runs on; never null.
    /// @param pulse The pulse, shared so it cannot die under the wait; may be null.
    /// @return True when the connection may go on to write; false when the pulse is
    ///         still holding the socket and this connection must end.
    Task<bool> SettlePulse(IReactor* reactor, std::shared_ptr<ProgressPulse> pulse)
    {
        if (pulse == nullptr)
            co_return true; // Not pulsing: nothing was ever armed, so nothing is held.

        pulse->stopped = true;
        auto const until = reactor->Clock().Now() + FrameServer::RefusalTimeout;
        while (!pulse->finished && reactor->Clock().Now() < until)
            co_await SleepUntil { .reactor = reactor,
                                  .deadline = NextWakeStep(reactor->Clock().Now(), until, FrameServer::GracefulCloseStep) };
        co_return pulse->finished;
    }

    /// Take the socket back from the pulse, and clear the responder mark if it will not
    /// let go.
    ///
    /// `SettlePulse` plus the one thing the connection owes on its way out, in one place
    /// rather than as two statements and a `break` inside a loop that is already at its
    /// cognitive-complexity ceiling -- which is why `AnswerAuth`, `DeadlineRefusalReply`,
    /// `AbandonIfPeerGone` and `SettleWatch` were each lifted out of it in turn. Adding
    /// two arms there put it at **65 against a threshold of 60**, which is a build
    /// failure and was the right one: that ceiling is what keeps the loop readable, and
    /// the fix is to lift an arm out rather than to raise the number.
    ///
    /// The mark is cleared rather than left for the way out because this path `break`s,
    /// and the sweeper shares this reactor thread: `Untrack` erases the entry the bit
    /// lives on, but a sweep landing between the two must go on deferring rather than
    /// closing a socket somebody may still be writing.
    /// @param state The server state, for the reactor and the responder mark.
    /// @param socket The connection whose mark is cleared; not owned.
    /// @param pulse The pulse, shared so it cannot die under the wait; may be null.
    /// @return True when the connection may write its reply; false when it must end.
    Task<bool> ReclaimFromPulse(FrameServer::State* state, ISocket* socket, std::shared_ptr<ProgressPulse> pulse)
    {
        if (co_await SettlePulse(&state->io.Reactor(), pulse))
            co_return true;
        (void) state->LeaveResponder(socket);
        co_return false;
    }

    /// Leave the responder, and when a sweep was deferred while inside it, tell the peer
    /// so and say the connection is over.
    ///
    /// **The one place a deferred sweep is observed, and deliberately one.** The loop
    /// holds seven `WriteAll` calls; a "check whether you were swept first" rule spread
    /// over them would be a rule to remember six times and to forget at the eighth
    /// somebody adds. It is instead a consequence of where the mark can be SET at all,
    /// and the refusal leaves by the same statement sequence every other reply does --
    /// so "exactly one writer" stays structural rather than agreed.
    ///
    /// Lifted out of `ServeConnection` for the reason `AnswerAuth`, `DeadlineRefusalReply`,
    /// `AbandonIfPeerGone` and `SettleWatch` were: that loop sits at its
    /// cognitive-complexity ceiling, and an arm needing a write, a counter and a bounded
    /// wait reads better without the framing around it. #245's pulse is what finally
    /// pushed it over -- 65 against a threshold of 60 -- and lifting an arm is the answer
    /// to that rather than raising the number.
    ///
    /// **It leaves the responder unconditionally**, which is the half a reader must not
    /// lose to the rename: the mark has to be cleared on the ordinary path too, and it is
    /// this call that does it.
    /// @param state The server state, for the mark, the writer and the counter.
    /// @param socket The connection; not owned.
    /// @param opRaw The verb, as received, for the refusal's own wording.
    /// @param window The verb's answer window, as the responder named it.
    /// @param watch The peer watch, shared so it cannot die under the wait; may be null.
    /// @return True when this connection was swept and must end.
    Task<bool> ExplainIfSwept(FrameServer::State* state,
                              ISocket* socket,
                              std::uint8_t opRaw,
                              std::chrono::milliseconds window,
                              std::shared_ptr<PeerWatch const> watch)
    {
        if (!state->LeaveResponder(socket))
            co_return false;

        if (co_await WriteAll(EndpointWriter::DeferredSweep, socket, DeadlineRefusalReply(*state, opRaw, window)))
            // Counted only when it went OUT. A peer that had already hung up was told
            // nothing, and a row saying otherwise would make the gap between this and the
            // sweep row mean something else.
            state->metrics.Increment(IMetricsSink::Counter::FrameDeadlineRefusalsSent);

        // The same abortive-close hazard the ordinary reply path has, on the one surface
        // that arms a watch: this explanation is written and then the caller's `break`
        // closes, and a close over a parked wait discards what was just buffered. #523 is
        // specifically about this explanation REACHING the peer, so it gets the same wait
        // -- which is why that wait is a function rather than a loop written twice.
        co_await AwaitWatchQuiet(&state->io.Reactor(), watch);

        // And then it ends, whether or not the refusal landed. This connection has been
        // swept; the answer it was holding is not owed to anybody any more, and continuing
        // the loop would serve a second request on a connection the sweeper has written
        // off.
        co_return true;
    }

    /// How a refused request gets back to a frame boundary, so the connection stays
    /// usable.
    ///
    /// **A PRIVATE enum -- nothing transmits it and nothing stores it -- so it states no
    /// ordinals.** An explicit `= N` here would assert a contract that does not exist.
    enum class Resynchronize : std::uint8_t
    {
        StepOver, ///< Read and discard exactly what the header declared.
        Oversize, ///< The declaration is past the cap, so the step-over is bounded too.
    };

    /// A refusal decided from a request HEADER, before a payload byte is read.
    ///
    /// Returned by value and owning: the reply is bytes this endpoint will hand to a
    /// write, not a view into anything the decision borrowed. `.agent/rules/wire-and-protocol.md`
    /// is explicit that a struct a decoder returns by value must not borrow from what it
    /// decoded, and the same reasoning governs a decision returned to a caller that then
    /// suspends.
    struct HeaderRefusal
    {
        std::vector<std::byte> reply; ///< What to send. Encoded and counted by the surface.
        Resynchronize resynchronize;  ///< How to reach the next frame boundary afterwards.
    };

    /// Whether this header is refused, and with what.
    ///
    /// **Four refusals, one question, and it writes nothing** -- which is what makes
    /// lifting it out of `ServeConnection` legal at all (#675). That loop holds the
    /// endpoint's exactly-one-writer property, so an extraction that takes a write with
    /// it turns the property into an agreement between two functions; this takes the
    /// DECISION and leaves every byte to the loop, which is why the four `WriteAll`
    /// calls that used to sit in these branches are now one.
    ///
    /// **The ORDER is the load-bearing part and it is now stated in one place.** Each
    /// step's reasoning is on the step:
    ///
    ///  1. The surface-wide cap, on the DECLARED length, so nothing is allocated.
    ///  2. Admission, ahead of any resource decision -- a peer this surface will not
    ///     serve must not be able to reach one, or a flood of refusable frames exhausts
    ///     the budget and makes the surface answer `EndpointBusy` to the peers it does
    ///     serve, which is the denial reconstructed one step out (#285, #377).
    ///  3. The credential and the per-verb ceiling, through the same `DecidePrePayload`
    ///     the daemon's loop calls, so the two surfaces cannot disagree about which
    ///     verbs are open before authentication.
    ///  4. The in-flight byte budget, last, for the reason step 2 gives.
    ///
    /// @param state The server state: the responder, the budget and the surface's name.
    /// @param peer The peer's HOST, as the kernel reports it; a source port is ephemeral
    ///        and is not an identity, so this is the only thing an admission policy has.
    /// @param decoded The request header, as it decoded.
    /// @param cap The surface-wide request ceiling, read once by the caller.
    /// @param credentialAccepted Whether an AUTH frame on THIS connection was verified.
    /// @return The refusal, or nullopt when the request is to be served.
    [[nodiscard]] std::optional<HeaderRefusal> DecideHeaderRefusal(FrameServer::State* state,
                                                                   std::string_view peer,
                                                                   Wire::RequestHeader const& decoded,
                                                                   std::size_t cap,
                                                                   bool credentialAccepted)
    {
        // Refused with a reply naming BOTH numbers, because "too large" without the
        // ceiling tells an operator nothing about a 64 KiB limit. The bytes are never
        // taken: the check is on the declared length, before the read.
        //
        // Encoded and counted by the SURFACE, exactly as the pre-payload refusal below
        // is, and this branch was the one that was not: it answered `payload-too-large`
        // correctly and moved nothing, so the cheapest probe there is -- a header and no
        // body -- left the series an operator alerts on perfectly flat (#326, undone by
        // the merge and reinstated by #447). Two ceilings, one fact: this is the
        // surface-wide cap and `DecidePrePayload` holds the per-verb one, an operator
        // does the same thing about both, so they share one counter and one decision
        // rather than splitting.
        if (decoded.payloadLength > cap)
            return HeaderRefusal {
                .reply = state->responder.RefusalReply(
                    Wire::PrePayloadDecision::PayloadTooLarge,
                    decoded.opRaw,
                    std::format("{} exceeds the {} {}-byte request cap", decoded.payloadLength, state->what, cap)),
                .resynchronize = Resynchronize::Oversize
            };

        // Admission. The predicate belongs to the responder; this only asks it early.
        if (auto refusal = state->responder.RefusePeer(peer, decoded.opRaw); refusal.has_value())
            return HeaderRefusal { .reply = *std::move(refusal), .resynchronize = Resynchronize::StepOver };

        // The credential, decided from the header and this connection's state (#289). A
        // SECOND question at the same point rather than a wider first one: `RefusePeer`
        // answers on the peer and the verb and returns an encoded refusal; this one
        // answers on the verb alone and feeds the decision alongside the declared length
        // and per-connection state, so folding them together would make neither
        // predicate's name describe it.
        auto const decision = Wire::DecidePrePayload({ .opRaw = decoded.opRaw,
                                                       .declaredLength = decoded.payloadLength,
                                                       .sessionCap = cap,
                                                       .authRequired = state->responder.AuthRequired(decoded.opRaw),
                                                       .credentialAccepted = credentialAccepted });
        if (decision != Wire::PrePayloadDecision::Serve)
            // Encoded and counted by the surface, not here: the endpoint owns WHEN the
            // question is asked, the responder owns the answer.
            return HeaderRefusal { .reply = state->responder.RefusalReply(decision, decoded.opRaw, {}),
                                   .resynchronize = Resynchronize::StepOver };

        // Checked on the DECLARED length, before a payload byte is read, so an
        // over-budget request costs no allocation at all. The connection cap alone would
        // not bound memory: N connections each declaring the per-request maximum is
        // still N times it.
        //
        // Refused rather than closed -- this is only reached for a declaration the
        // surface WOULD have accepted, so the peer is told to come back rather than made
        // to reconnect over a transient budget.
        //
        // The figure in the message is the one the DECISION was taken on, read once.
        // Loaded a second time to format it, the refusal could name a number that does
        // not explain it -- another connection releasing in between yields "has 0 of N
        // bytes in flight" beside a refusal for having too many.
        //
        // Counted by the surface too, and it is the same regression the oversize branch
        // above was: the dedicated compile port answered this with
        // `CompileRefusal::EndpointBusy` and moved
        // `worker_jobs_refused_endpoint_busy_total`, which the operator documentation
        // still promises, and the merged listener answered it with a bare code (#447).
        if (auto const budget = state->responder.MaxInFlightBytes(),
            held = state->inFlightBytes.load(std::memory_order_acquire);
            budget != 0 && held + decoded.payloadLength > budget)
            return HeaderRefusal { .reply = state->responder.EndpointRefusalReply(
                                       EndpointRefusal::InFlightBudget,
                                       decoded.opRaw,
                                       std::format("{} has {} of {} bytes in flight", state->what, held, budget)),
                                   .resynchronize = Resynchronize::StepOver };

        return std::nullopt;
    }

    /// Step over a refused request's body, so the next read starts on a frame boundary.
    ///
    /// **It reads and never writes**, which is what lets it out of `ServeConnection` at
    /// all: the reply has already gone out by the time this runs, and the byte sequence
    /// -- refusal first, resynchronization second -- stays a fact about the loop's own
    /// statements.
    ///
    /// `Skip` discards in chunks and never materialises the frame, so the memory the cap
    /// protects is still never taken.
    ///
    /// The `Oversize` arm is bounded by the wire's own shared factor rather than a
    /// second opinion about it, and past that bound the connection ends -- but not with
    /// the peer's bytes still queued, which would reset it and take the refusal just
    /// written back out of the peer's buffer. Best effort, and bounded like every other
    /// drain here: a peer that keeps sending past that has chosen the reset.
    ///
    /// @param reader The connection's one reader; a POINTER because a coroutine
    ///        parameter must not be a reference, which is the shape `SettleWatch` takes
    ///        its reader in for the same reason.
    /// @param socket The connection; not owned. Read from only.
    /// @param declaredLength What the header said the body was.
    /// @param cap The surface-wide request ceiling.
    /// @param how Which resynchronization the refusal called for.
    /// @return True when the connection may serve another request; false when it must end.
    Task<bool> StepOverRefused(
        ByteReader* reader, ISocket* socket, std::uint32_t declaredLength, std::size_t cap, Resynchronize how)
    {
        if (how == Resynchronize::Oversize)
        {
            auto const drainable = static_cast<std::uint64_t>(cap) * Wire::OversizeDrainFactor;
            if (declaredLength > drainable || !(co_await reader->Skip(declaredLength)).has_value())
            {
                co_await DrainUntilPeerCloses(socket, cap);
                co_return false;
            }
            co_return true;
        }

        co_return (co_await reader->Skip(declaredLength)).has_value();
    }

    /// Serve one connection: read a frame, answer it, repeat until the peer or the
    /// surface is done.
    ///
    /// ## Read this before extracting anything from it
    ///
    /// **This loop holds the endpoint's exactly-one-writer property**, and it sits near
    /// clang-tidy's cognitive-complexity ceiling -- which is a combination that punishes
    /// the obvious way of getting under the line. #675 is the ticket that says so, and
    /// it says so because the next lane to add a branch here meets a build failure about
    /// a function it did not write, whose cheapest fix is to extract the nearest block,
    /// which is as likely as not to be a write.
    ///
    /// So: **extract QUESTIONS, never writes.** `DecideHeaderRefusal` decides what to
    /// refuse and returns bytes; `StepOverRefused` reads and discards; `AbandonIfPeerGone`,
    /// `SettleWatch`, `ArmPeerWatch` and `ReclaimFromPulse` each answer something without
    /// putting a byte on the socket. The four writes that remain -- the folded header
    /// refusal, `AnswerAuth`, the reply, and `ExplainIfSwept`'s deadline refusal -- are
    /// where the ORDER of what a peer receives is decided, and moving one somewhere else
    /// makes that order an agreement between two functions rather than a fact about one.
    /// What that costs is an interleaved partial write, which on this wire is a desync
    /// rather than an error: a client reads a length out of the middle of another frame.
    ///
    /// Every write names itself against `EndpointWriterTable`, so a fifth writer is a
    /// row with a reason rather than a call that appeared in a helper. That table is
    /// explicit that this is DECLARED and not enforced; `EndpointWriters.hpp` says what
    /// enforcing it would take.
    ///
    /// @param shared The server state, shared for the reason the socket below is.
    /// @param owned The accepted connection.
    DetachedTask ServeConnection(std::shared_ptr<FrameServer::State> shared, std::unique_ptr<ISocket> owned)
    {
        auto* const state = shared.get();
        // **Shared rather than unique, for LIFETIME and for nothing else.** The
        // connection is still the only writer and still decides everything; the second
        // reference exists because a peer watch can be parked in `WaitReadable` when
        // this frame unwinds, and the only cancellation on the interface is `Close()`,
        // whose completion resumes that coroutine -- inline on epoll and kqueue,
        // marshalled to a later turn on IOCP. Under a `unique_ptr` the IOCP path
        // resumes the watcher onto a destroyed socket, which is verbatim the second
        // use-after-free `RedisResp`'s readable watcher records and fixes the same way.
        // Costs one control block per connection.
        std::shared_ptr<ISocket> const socket { std::move(owned) };
        OpenConnectionSlot const slot { state };

        // Registered with its deadline BEFORE the first read, so a client that sends
        // half a header is swept rather than holding a frame until the process dies.
        // `Rearm` below moves it forward once per request -- not once per read; see
        // its note for what the window then covers.
        //
        // TWO windows, and which one is armed depends on whether a verb has been
        // named. Until it has, this peer has told the surface nothing, so it gets the
        // short one whatever the surface goes on to serve -- that is where the
        // slow-loris property lives. Once the header decodes, the owner of the verb
        // says how long ITS answer may take, because a cache exchange is a round trip
        // and a dispatched compile is however long a compiler runs (#223, #290).
        auto const deadlineFor = [state](std::chrono::milliseconds window) {
            return state->io.Reactor().Clock().Now() + window;
        };
        state->Track(
            socket.get(), deadlineFor(FrameServer::HeaderTimeout), SweepPhase::AwaitingRequest, FrameServer::HeaderTimeout);

        // The firewall: this is a DetachedTask, whose unhandled_exception terminates
        // the process, so one client's answer must not take the node with it.
        try
        {
            // The peer's HOST. A connection's source port is ephemeral and is not the
            // peer's endpoint, so for a surface whose policy needs an identity -- the
            // scheduler's -- there is nothing else here that could supply one.
            auto const peer = socket->PeerAddress();

            auto const cap = state->responder.MaxRequestBytes();

            // ONE reader for the connection, as the daemon's loop has: it buffers
            // internally, so a per-request reader would discard bytes already pulled
            // off the socket -- which is exactly the pipelined second frame.
            ByteReader reader { *socket, /*maxLineBytes*/ 1, cap };

            // Per CONNECTION, exactly as the daemon keeps it: the responder is shared
            // by every connection on this surface, so a credential accepted here must
            // not bless anyone else. It starts false and is only ever set by an AUTH
            // frame this loop verified -- never seeded from the policy, which would
            // authenticate a connection on the strength of a check that never ran.
            bool credentialAccepted = false;

            while (!state->shuttingDown.load(std::memory_order_acquire))
            {
                // Between requests, not after one: the buffer a 256 MiB object grew
                // into is kept by the vector, and a reader that now lives as long as
                // the connection would hold it for as long as the peer stays attached
                // -- uncounted by the in-flight budget, because nothing is in flight.
                // Whatever a pipelined peer has already sent is kept.
                reader.ReleaseSpareCapacity();

                state->Rearm(socket.get(),
                             deadlineFor(FrameServer::HeaderTimeout),
                             SweepPhase::AwaitingRequest,
                             FrameServer::HeaderTimeout);

                auto const header = co_await reader.ReadExactly(Wire::RequestHeaderSize);
                if (!header.has_value())
                    break; // Routine at a request boundary: the peer is done.

                auto const decoded = Wire::DecodeRequestHeader(*header);
                if (!decoded.has_value())
                    break; // A foreign magic: no declared length, so nowhere to
                           // resynchronize to. Closing is the only thing left.

                // **Four header refusals, one question, one write.** The decision is
                // `DecideHeaderRefusal`, which writes nothing -- that is what makes
                // lifting it out of this loop legal (#675). What stays here is the byte
                // sequence: the reply, then the resynchronization, in that order.
                //
                // Answered BEFORE the body is stepped over, which is the one place this
                // deliberately does not copy the daemon's order. Draining first makes
                // the reply contingent on the peer finishing a write it may already have
                // abandoned -- a client that computed the frame was too large and sent
                // only the header then waits for an answer that arrives when the sweeper
                // closes it, i.e. never usefully. Writing first costs nothing and the
                // resynchronization is just as good: a peer that sends what it declared
                // is still stepped over exactly.
                if (auto const refusal = DecideHeaderRefusal(state, peer, *decoded, cap, credentialAccepted);
                    refusal.has_value())
                {
                    if (!co_await WriteAll(EndpointWriter::Loop, socket.get(), refusal->reply))
                        break;
                    if (!co_await StepOverRefused(
                            &reader, socket.get(), decoded->payloadLength, cap, refusal->resynchronize))
                        break;
                    continue;
                }

                // **The verb's own window, armed at the point this surface decides to
                // SERVE and not one line earlier.** It covers the payload read and the
                // answer, which are the two things a compile makes long.
                //
                // Every branch above is a refusal, and every one of them ends in
                // `reader.Skip(decoded->payloadLength)` -- a read of whatever the peer
                // declared, from a peer that may dribble it. Armed after the header
                // decoded, as it first was, a stranger who merely NAMES `Op::Compile`
                // in a seven-byte header would be handed the compile window before
                // `RefusePeer` had been asked whether they are a member at all: a
                // hundred-and-twentyfold longer hold, pre-admission, on a surface whose
                // whole purpose here was to be harder to exhaust. The comment that used
                // to sit up there reasoned the refusals were "all fast" -- true of the
                // `WriteAll`, false of the skip that follows it.
                //
                // So `HeaderTimeout` governs everything up to this line, which is the
                // rule stated positively: a peer gets the generous window by being
                // SERVED, never by asking.
                // Read ONCE and carried, rather than asked again when the grace is
                // sized: the entry's window and its deadline have to be the same
                // number, and two calls to a virtual a surface may reconfigure between
                // is how they would come to differ.
                auto const answerWindow = state->responder.RequestTimeout(decoded->opRaw);
                state->Rearm(socket.get(), deadlineFor(answerWindow), SweepPhase::AwaitingAnswer, answerWindow);

                BudgetedBytes bytes { state, decoded->payloadLength };
                auto const payload = co_await reader.ReadExactly(decoded->payloadLength);
                if (!payload.has_value())
                    break;

                // AUTH is answered HERE and never reaches `Answer`, because what it
                // changes is this connection's state and the responder is shared by
                // all of them. The same split the daemon makes for the same reason.
                //
                // Lifted out of this loop rather than written inline: the loop sits at
                // its cognitive-complexity ceiling, and an arm needing one payload and
                // one flag is exactly the part that reads fine without the framing.
                if (decoded->opRaw == static_cast<std::uint8_t>(Wire::Op::Auth))
                {
                    if (!co_await WriteAll(EndpointWriter::Loop,
                                           socket.get(),
                                           AnswerAuth(state->responder, *payload, decoded->opRaw, credentialAccepted)))
                        break;
                    continue;
                }

                std::vector<std::byte> frame { header->begin(), header->end() };
                frame.insert(frame.end(), payload->begin(), payload->end());

                // Handed over, for a verb whose owner charges these same bytes itself.
                //
                // The endpoint's budget covers the READ -- which nothing else can, the
                // owner having no frame until this line -- and the owner's covers the
                // answer. Held across `Answer` as well, one buffer sits in two pools
                // for as long as the answer takes, and for a compile that is minutes;
                // this pool is per LISTENER, so compiles filling it refuse the
                // scheduler verbs sharing it (#448).
                //
                // Placed as the last statement before the await, and the gap is empty
                // by construction rather than by timing: `Task` is lazy with symmetric
                // transfer, so the body below runs on THIS thread without returning to
                // the loop, and the node's framed surfaces share one reactor thread --
                // so no suspension point and no other thread can observe the lowered
                // figure. `IFrameResponder::HoldsOwnByteBudget` carries the reasoning
                // and what a broken invariant would cost.
                if (state->responder.HoldsOwnByteBudget(decoded->opRaw))
                    bytes.Release();

                // Armed only when the reader holds nothing: a peer that has just
                // pipelined a request has proved it is there, which is the entire
                // question this watch exists to ask. The prepend-ordering reason that
                // used to stand here went with the probe read (#1090) -- the watcher
                // peeks now, so there is nothing to hand back.
                // Null when this surface does not watch this verb, or when the reader
                // already holds a pipelined request -- see `ArmPeerWatch`, which carries
                // the ordering argument. Every helper below takes that null.
                auto watch = ArmPeerWatch(shared, decoded->opRaw, reader, socket);

                // Closes the window in which this watch's verdict can still change what
                // this connection does -- see `ConsultedAtExit`, which carries why the
                // exits are what nobody may forget. Scoped to the request, not the
                // connection: the socket is closed after the loop, so the departure that
                // close provokes lands after this mark and is this node's own.
                ConsultedAtExit const consulted { watch.get() };

                // The write-side mirror of the watch above: while the responder answers,
                // this connection writes nothing, so the pulse is the only writer and
                // `ReclaimFromPulse` below is what hands the socket back before the reply.
                // Null when this surface does not pulse this verb -- see
                // `ArmProgressPulse` and `IFrameResponder::ProgressInterval` (#245).
                auto pulse = ArmProgressPulse(state->responder, decoded->opRaw, &state->io.Reactor(), socket);

                // Awaited: answering may reach the network -- the cache surface
                // consults an upstream -- and that suspends rather than blocking
                // every other connection on this loop, which is the whole reason
                // this task exists separately from the accept loop.
                //
                // Bracketed by the two marks, and this is the ONLY place either is
                // touched. While the mark is set, a sweep that finds this connection
                // overdue defers instead of closing, because here -- and only here --
                // the coroutine is not parked on the socket, so the close would wake
                // nothing while destroying the write side a refusal has to leave by.
                state->EnterResponder(socket.get());

                // Timed across the responder ONLY, which is the number an operator can
                // act on: it is how long this node took to answer, with no part of the
                // peer's own dribbling in it. A steady clock, because this measures a
                // duration rather than naming an instant.
                auto const startedAt = std::chrono::steady_clock::now();
                auto const reply = co_await state->responder.Answer(frame, peer);

                // **The one place this node says anything about a client**, and it is
                // here because here is where every verb on every surface has already
                // been routed to its owner and answered -- one line covers the cache,
                // the scheduler and the compile surface without any of them knowing
                // about logging. See `ExchangeLog.hpp` for why the LEVEL is per verb,
                // and why the level test lives in `LogExchange` rather than here.
                //
                // Placed before `ReclaimFromPulse` deliberately: it emits no frame and
                // cannot suspend, so it does not sit between the pulse reclamation and
                // the writes that must follow it, and an exchange is recorded even when
                // one of the paths below goes on to `break`. What it claims is that the
                // RESPONDER answered -- not that the reply reached anybody, which is a
                // different fact and belongs to the write.
                Node::LogExchange(
                    state->logger,
                    decoded->opRaw,
                    peer,
                    frame,
                    reply,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt),
                    state->namer);

                // A peer this node cannot speak to AT ALL is louder than its verb's own
                // level -- a refused `fetch` is `Debug`, so at the default level an
                // operator sees a node refusing every request and no line saying so.
                // Said once per peer per interval; `NoteVersionRefusal` owns the whole
                // decision because `ServeConnection` is at the complexity ceiling.
                Node::NoteVersionRefusal(
                    state->logger, state->versionRefusals, peer, reply, std::chrono::steady_clock::now());

                // **Before every write below, and there are three of them.** The pulse is
                // the only writer while the responder answers; this is where that stops
                // being true, and a reply written over a pulse still suspended inside
                // `Write` would be two writes sharing one write-op slot. A pulse that
                // will not let go inside the bound ends the connection, because the
                // `break` -- and the `socket->Close()` it falls through to -- is the only
                // cancellation available for a parked write.
                //
                // Placed while `inResponder` is still SET, deliberately: this suspends,
                // and a sweep landing in that gap must go on deferring rather than
                // closing the socket the refusal has to leave by. `LeaveResponder` below
                // is what ends that window, and it must not run before this does -- which
                // is why the failing path clears the mark itself, inside
                // `ReclaimFromPulse`, rather than leaving a second statement here.
                if (!co_await ReclaimFromPulse(state, socket.get(), pulse))
                    break;

                // Leaves the responder, ALWAYS, and answers whether a sweep was deferred
                // while this connection was inside it -- see `ExplainIfSwept`, which
                // carries why that question is asked in exactly one place and why the
                // refusal has to leave by the same statement sequence every other reply
                // does.
                if (co_await ExplainIfSwept(state, socket.get(), decoded->opRaw, answerWindow, watch))
                    break;

                // Is anybody still there to receive it -- see `PeerAlreadyGone`, which
                // carries why that is the only question asked before the write.
                //
                // The object is then not WRITTEN. It has still been compiled and
                // encoded, since `Answer` returned it, so what this saves is the
                // transfer -- #223 measured 84 MB for one translation unit going to a
                // client that had already given up. Saving the CPU as well needs a
                // cancellable process seam (#661).
                //
                // Counted only when there was something to deliver AND when a CLIENT is
                // what went: an empty reply means the surface had already decided to
                // close, and a socket this process closed -- a sweep past the grace, a
                // shutdown -- reaches the watcher looking exactly like a peer that hung
                // up. Filing either would put this node's own teardowns in a row
                // documented as a client-side story.
                // Nothing to deliver, or nobody left to deliver it to -- see
                // `AbandonIfPeerGone`, which carries both arms and counts them apart.

                if (co_await AbandonIfPeerGone(state, socket.get(), watch, !reply.empty()))
                    break;

                if (!co_await WriteAll(EndpointWriter::Loop, socket.get(), reply))
                    break;

                // **The watch is settled AFTER the reply, never before it**, and that
                // ordering is the whole of it: `WriteAll` suspends -- an object is
                // megabytes and a receive window is not -- so a peer can pipeline its
                // next request DURING the write, and the watcher takes those bytes off
                // the socket. Read before the write, that verdict is "still parked" and
                // the bytes it goes on to pull are dropped: the next `ReadExactly`
                // starts mid-frame, decodes a foreign magic, and closes a connection
                // whose peer did nothing wrong.
                if (co_await SettleWatch(&state->io.Reactor(), watch) == AfterWatch::EndConnection)
                    break;
            }
        }
        catch (...)
        {
            state->logger.Logf(LogLevel::Error, "{}: dropping a connection that threw", state->what);
        }

        // **A throw out of `Answer` leaves `inResponder` set, and that is safe for a
        // reason worth stating rather than leaving to be rediscovered.** Nothing
        // between the throw and the `Untrack` below suspends -- `Logf` is an ordinary
        // call, not a coroutine -- and the sweeper shares this reactor thread, so it
        // cannot run in that gap and cannot observe the stale bit. `Untrack` then
        // erases the entry the bit lives on.
        //
        // So it is NOT an RAII guard, unlike `OpenConnectionSlot` above, and the
        // difference is which failure each prevents: a missed slot release leaks a
        // permanent unit of capacity, while a missed clear here would at worst make one
        // sweep defer rather than close, on an entry about to be erased anyway. What
        // makes the argument hold is the absence of a suspension point, so anything
        // added between the `catch` and this line has to preserve that or take the
        // guard instead.
        //
        // Deregistered before the socket is destroyed, or the sweeper would hold a
        // pointer into a freed object.
        state->Untrack(socket.get());
        socket->Close();
        co_return;
    }

    /// Refuse a connection the surface has no room for, then close it.
    ///
    /// Its own task rather than a write in the accept loop, so refusing never parks
    /// the loop on a client that is not reading. It takes no open-connection slot, so
    /// it cannot itself be what keeps the surface at capacity -- but it IS registered
    /// with the sweeper, so a client that never reads its refusal is still let go.
    ///
    /// It reads, and that is not optional. Closing a socket while the peer's bytes sit
    /// unread in the receive queue is a RESET rather than a graceful close, and a reset
    /// discards data the peer had already buffered -- including the refusal just
    /// written to it. Every real client writes its request and then reads, so the
    /// refusal was never the thing they saw: they saw a connection reset, which names
    /// neither the surface nor the reason. A client that reads WITHOUT writing did
    /// receive it, which is how the two cases below tell the halves apart.
    DetachedTask RefuseAtCapacity(std::shared_ptr<FrameServer::State> shared, std::unique_ptr<ISocket> owned)
    {
        auto* const state = shared.get();
        auto socket = std::move(owned);

        // A SHORTER window than a served request gets. A refused connection has
        // nothing left to do but be read and hang up, so the drain below should end in
        // a round trip; giving it the full `RequestTimeout` would let a flood arriving
        // at capacity park a socket per attempt for five seconds each -- taking no
        // slot, and so counted by nothing. This is the one place where being polite
        // has to stay cheap.
        auto const deadline = state->io.Reactor().Clock().Now() + FrameServer::RefusalTimeout;
        // `AwaitingRequest`, and it is the honest phase rather than the convenient
        // one: this peer was refused at accept, so it has named no verb and never
        // will. A sweep here means the refusal write did not drain, which belongs
        // beside the other pre-verb sweeps and must not be read as a request this
        // surface accepted and failed to answer.
        state->Track(socket.get(), deadline, SweepPhase::AwaitingRequest, FrameServer::RefusalTimeout);
        try
        {
            // **The one refusal on this endpoint that belongs to the endpoint.**
            // Decided at accept, before a header exists, so it names no verb and
            // `MergedResponder` has nothing to route it by -- and contorting the
            // router into answering a question it cannot have the input for is how a
            // default arm ends up wrong later. So `FrameServer` counts it against its
            // own row, which is also what keeps it apart from
            // `EndpointRefusal::InFlightBudget`: the two share `EndpointBusy` on the
            // wire and say opposite things to an operator -- one request is too big
            // right now, against this surface is full -- so they must never sum. Two
            // categories rather than a comment asking somebody to remember (#447).
            if (co_await WriteAll(EndpointWriter::AtCapacity,
                                  socket.get(),
                                  Cc::Refuse(state->metrics,
                                             ConnectionsExhausted,
                                             std::format("{} is already holding {} connections",
                                                         state->what,
                                                         state->responder.MaxOpenConnections()))))
                co_await DrainUntilPeerCloses(socket.get(), state->responder.MaxRequestBytes());
        }
        catch (...)
        {
            state->logger.Logf(LogLevel::Error, "{}: dropping a refusal that threw", state->what);
        }
        state->Untrack(socket.get());
        socket->Close();
        co_return;
    }

    /// Close connections that have outstayed their deadline.
    ///
    /// One per server, on a bounded tick. It counts as an adopted loop for the
    /// reactor's stop rule, because it is a frame parked on the timer wheel and the
    /// reactor must not return while it is.
    DetachedTask SweepOverdue(std::shared_ptr<FrameServer::State> shared)
    {
        auto* const state = shared.get();
        while (!state->shuttingDown.load(std::memory_order_acquire))
        {
            co_await SleepFor(state->io.Reactor(), FrameServer::SweepInterval);
            if (state->shuttingDown.load(std::memory_order_acquire))
                break;
            // Logged with the split, not the total. The two numbers answer different
            // questions -- one is scanners, the other is a request this node accepted
            // and could not answer in time -- and a line carrying only their sum
            // cannot be read for either. The counters carry the same split for anyone
            // scraping rather than reading logs.
            auto const now = state->io.Reactor().Clock().Now();
            if (auto const swept = state->CloseOverdue(now); swept.Total() != 0)
                state->logger.Logf(LogLevel::Debug,
                                   "{}: swept {} connection(s): {} before a verb was named, {} with an answer owed",
                                   state->what,
                                   swept.Total(),
                                   swept.awaitingRequest,
                                   swept.awaitingAnswer);

            // Run AFTER the sweep, on the same instant. A deferral created a moment
            // ago cannot expire in the same turn, and running this first would only
            // ever act on entries the previous turn left -- which is the same set,
            // one interval later. Ordered this way the two read as one pass over one
            // clock reading rather than as two schedules to reason about.
            //
            // Logged at Warning and not Debug, because unlike a sweep this is not
            // routine: it says a responder did not come back within the grace, which
            // is a wedged answer rather than a slow one.
            //
            // **The grace is NOT named in the line, and that is deliberate.** It is
            // derived per connection from the verb that connection was serving, so a
            // surface answering several verbs has several graces and one number here
            // would describe whichever the author guessed. A line naming a figure
            // that does not explain the event it reports is worse than one naming
            // none -- an operator who acts on it is tuning the wrong window.
            if (auto const abandoned = state->CloseExpiredDeferrals(now); abandoned != 0)
                state->logger.Logf(LogLevel::Warn,
                                   "{}: closed {} swept connection(s) whose responder never returned within the "
                                   "grace its own verb's window allows; the answers are wedged, not merely slow",
                                   state->what,
                                   abandoned);
        }
        state->loopsAlive.fetch_sub(1, std::memory_order_acq_rel);
        state->io.NoteLoopFinished();
        co_return;
    }

} // namespace

FrameServer::FrameServer(NodeIoLoop& io,
                         IListener& listener,
                         IFrameResponder& responder,
                         std::string_view what,
                         IMetricsSink& metrics,
                         ILogger& logger,
                         Node::ToolchainNamer namer) noexcept:
    _state { std::make_shared<State>(io, listener, responder, what, metrics, logger, std::move(namer)) }
{
}

FrameServer::~FrameServer() = default;

void FrameServer::NoteLoopThrew() noexcept
{
    _state->logger.Logf(LogLevel::Error, "{}: accept loop threw; this surface has stopped accepting", _state->what);
}

std::size_t FrameServer::OpenConnections() const noexcept
{
    return _state->openConnections.load(std::memory_order_acquire);
}

std::size_t FrameServer::InFlightBytes() const noexcept
{
    return _state->inFlightBytes.load(std::memory_order_acquire);
}

Task<void> FrameServer::Run()
{
    // The shared state is copied out FIRST, before anything can suspend, so nothing
    // after this line touches `this`. That is what lets the owning `FrameServer` be
    // destroyed while this loop is still unwinding -- and the loops are what
    // `Shutdown()` waits for, so it normally is not, but a lifetime that depends on
    // "normally" is the one that bites.
    auto state = _state;

    state->loopsAlive.fetch_add(1, std::memory_order_acq_rel);

    // The sweeper is spawned here rather than by the owner, because it belongs to
    // this loop's lifetime -- and it is counted twice over: once here, so `Shutdown()`
    // waits for it, and once with the owner, because a frame parked on the timer
    // wheel is exactly what the reactor must not return over.
    state->loopsAlive.fetch_add(1, std::memory_order_acq_rel);
    state->io.NoteLoopStarted();
    SweepOverdue(state);

    while (!state->shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await state->listener.Accept();
        if (!accepted.has_value())
        {
            // `Close()` resolves a parked accept with Cancelled, which is how this
            // loop learns it is done -- there is no poll timeout any more, so a stop
            // is observed at once rather than after a quarter second.
            //
            // **`WouldBlock` alone, and NOT `Net::IsDeadlineExpiry`, deliberately.**
            // This listener arms no poll timeout, so `Timeout` cannot arrive here at
            // all and the second operand would be dead. That is a REACHABILITY
            // reason: it is a fact about this listener, so an edit that gives it a
            // poll timeout must switch this to `IsDeadlineExpiry` in the same change.
            //
            // Not a semantic one. `WouldBlock` at an accept whose listener DOES arm a
            // timeout is exactly a deadline expiring -- that is what the admin surface
            // and the Raft peer server call `IsDeadlineExpiry` for -- so "on an accept
            // WouldBlock is not an expiry" is false in general and must not be carried
            // back to those callers, which would stop accepting entirely.
            //
            // Recorded HERE because the note used to live only beside the predicate,
            // where three reviewers in a row did not find it and filed the narrow test
            // as a defect (#824).
            auto const code = accepted.error().code;
            if (code == NetErrorCode::WouldBlock)
                continue;
            state->logger.Logf(LogLevel::Debug, "{}: accept loop ended ({})", state->what, accepted.error().ToString());
            break;
        }

        auto socket = *std::move(accepted);

        // Checked before the connection is served rather than inside it: the cap
        // exists to bound how much this surface buffers at once, and a task that had
        // already started reading would be past the point of refusing cheaply.
        auto const concurrent = state->responder.MaxOpenConnections();
        if (concurrent != 0 && state->openConnections.load(std::memory_order_acquire) >= concurrent)
            RefuseAtCapacity(state, std::move(socket));
        else
            ServeConnection(state, std::move(socket));
    }

    state->loopsAlive.fetch_sub(1, std::memory_order_acq_rel);
    co_return;
}

void FrameServer::Shutdown() noexcept
{
    if (_state->shuttingDown.exchange(true, std::memory_order_acq_rel))
        return;

    // Nothing ever started, so there is nothing to post to and nothing to wait for.
    // Without this an endpoint built and dropped without `NodeIoLoop::Start()` --
    // which is exactly what the bind-failure tests do -- would leave a task queued on
    // a reactor that never turns.
    if (_state->loopsAlive.load(std::memory_order_acquire) == 0)
    {
        _state->listener.Close();
        return;
    }

    // Posted onto the reactor, never done here. See the declaration: on epoll and
    // kqueue `Close` resumes a parked coroutine INLINE, so closing from the stopping
    // thread would run this server's connection tasks on it while the reactor thread
    // is still driving them.
    [](std::shared_ptr<State> state) -> DetachedTask {
        co_await ResumeOn { state->io.Reactor() };
        state->CloseAll();
        co_return;
    }(_state);

    // Waits for the LOOPS as well as the connections, and both halves matter. The
    // loops hold this state and the accept loop holds the listener, so returning
    // early would let `~FrameServer` free what they are still using -- and would
    // leave the port bound after the endpoint claims to have stopped.
    //
    // Bounded, because a stuck connection must not turn a stop into a hang.
    // `DrainWithin` is the one bounded drain in this tree, and this is one of the
    // two sites it came from: the loop that stood here cited the ceiling and
    // cadence `RaftPeerServer::Shutdown` uses and then reimplemented both --
    // accumulating the requested poll instead of measuring it, so the 5 s it named
    // was 15 s on a host whose timer granularity is 15 ms, and the cadence it
    // claimed to share was 5 ms rather than 10 (#452).
    auto const outcome = DrainWithin(
        [&state = *_state] { return state.loopsAlive.load(std::memory_order_acquire) != 0 || state.OpenCount() != 0; });

    if (auto const stuck = _state->loopsAlive.load(std::memory_order_acquire) + _state->OpenCount();
        outcome == DrainResult::Ceiling && stuck != 0)
        _state->logger.Logf(
            LogLevel::Error, "{}: {} loop(s)/connection(s) did not finish within the stop ceiling", _state->what, stuck);
}

FrameEndpoint::FrameEndpoint(NodeIoLoop& io,
                             std::unique_ptr<IListener> listener,
                             IFrameResponder& responder,
                             std::string_view what,
                             std::string boundEndpoint,
                             IMetricsSink& metrics,
                             ILogger& logger,
                             Node::ToolchainNamer namer):
    _io { io },
    _listener { std::move(listener) },
    _server { std::make_unique<FrameServer>(io, *_listener, responder, what, metrics, logger, std::move(namer)) },
    _boundEndpoint { std::move(boundEndpoint) }
{
    // Adopted rather than started here. The loop is run once, by its owner, after
    // every endpoint has been constructed -- which is what lets one reactor thread
    // carry all of them and what makes "the reactor stops when the last loop ends"
    // expressible at all.
    io.Adopt(*_server);
}

FrameEndpoint::~FrameEndpoint()
{
    // Order, not tidiness: this closes the listener and every open connection, which
    // is what lets the accept loop and its connection tasks reach their own ends.
    // The reactor thread itself is joined by `NodeIoLoop`, which outlives this.
    _server->Shutdown();

    // The server before the listener, because `FrameServer::State` holds a reference
    // to it. Explicit rather than left to member order, since the ordering is now
    // load-bearing twice over: the handover below moves the listener out from under
    // that reference.
    _server.reset();

    // And the listener is NOT destroyed here. `Shutdown()` above returned as soon as
    // this server's loops and connections had ended -- which is the very condition
    // that makes the last loop call `NodeIoLoop::NoteLoopFinished()`, and `Stop()`
    // only sets a flag and posts a wakeup, so the reactor is typically still
    // `Running()` at this line and this thread is not its worker. Destroying a
    // reactor-owned listener there clears a pending awaitable while a completion may
    // be dispatched (#668's rule, #840's site); on IOCP the window is wide enough
    // that `Windows-cl-debug` hit it intermittently, in a different case each run
    // (#737). `NodeIoLoop` frees it once its thread has been joined.
    _io.Retire(std::move(_listener));
}

std::size_t FrameEndpoint::InFlightBytes() const noexcept
{
    return _server->InFlightBytes();
}

std::unique_ptr<FrameEndpoint> FrameEndpoint::StartWithListener(NodeIoLoop& io,
                                                                NodeSurface surface,
                                                                std::unique_ptr<IListener> listener,
                                                                std::string boundEndpoint,
                                                                IFrameResponder& responder,
                                                                IMetricsSink& metrics,
                                                                ILogger& logger,
                                                                Node::ToolchainNamer namer)
{
    // `new` rather than `make_unique` because the constructor is private: the ways to
    // reach it are this function and nothing else, and the two factories below have
    // each already proved their listener is bound.
    return std::unique_ptr<FrameEndpoint> { new FrameEndpoint { io,
                                                                std::move(listener),
                                                                responder,
                                                                RowFor(surface).name,
                                                                std::move(boundEndpoint),
                                                                metrics,
                                                                logger,
                                                                std::move(namer) } };
}

std::expected<std::unique_ptr<FrameEndpoint>, std::string> FrameEndpoint::Start(NodeIoLoop& io,
                                                                                NodeSurface surface,
                                                                                NodeConfig const& cfg,
                                                                                IFrameResponder& responder,
                                                                                IMetricsSink& metrics,
                                                                                ILogger& logger,
                                                                                Node::ToolchainNamer namer)
{
    // The row resolves it, so the address this binds and the address
    // `--print-surfaces` prints are the same computation rather than two that agree
    // today. The default host arrives with it, which is what stopped each caller
    // choosing its own -- the cache's loopback and the scheduler's wildcard are the
    // anti-leeching rule, and a rule spelled at two call sites is a rule that drifts.
    // Not served means the spec is empty, or a companion flag that switches this
    // surface on is unset. Never a malformed address -- `StartupPolicyRejection` walks
    // the same rows and refuses that by name, echoing what the operator typed, long
    // before any tier is built.
    auto const resolved = SoleEndpointOf(surface, cfg);
    if (!resolved.has_value())
        return std::unexpected { resolved.error() };
    auto const& endpoint = *resolved;
    auto const& row = RowFor(surface);

    auto listener = PlatformListener::Bind(io.Reactor(), endpoint.host, endpoint.port);
    if (!listener || !listener->IsBound())
        return std::unexpected { std::format("cannot bind {}:{} ({})",
                                             endpoint.host,
                                             endpoint.port,
                                             listener ? listener->BindError() : std::string_view { "null listener" }) };

    // No accept-poll timeout to apply any more, and its absence is the change rather
    // than an omission: it existed only because POSIX does not unblock a parked
    // `accept()` when another thread closes the listening socket. On the reactor
    // `Close()` resolves the parked accept directly, so a stop is observed at once
    // instead of within a quarter second, and there is no way to obtain a listener
    // this endpoint cannot stop.

    // The port the listener ACTUALLY bound, not the one asked for. `0` means "pick a
    // free one", and an endpoint that echoed `:0` back could not tell an operator --
    // or a test -- where it ended up.
    auto bound = std::format("{}:{}", endpoint.host, listener->BoundPort());
    logger.Logf(LogLevel::Info, "{} listening on {}", row.name, bound);

    return StartWithListener(
        io, surface, std::move(listener), std::move(bound), responder, metrics, logger, std::move(namer));
}

std::expected<std::unique_ptr<FrameEndpoint>, std::string> FrameEndpoint::StartAdopted(NodeIoLoop& io,
                                                                                       NodeSurface surface,
                                                                                       int descriptor,
                                                                                       std::string_view advertisedHost,
                                                                                       IFrameResponder& responder,
                                                                                       IMetricsSink& metrics,
                                                                                       ILogger& logger,
                                                                                       Node::ToolchainNamer namer)
{
    auto const& row = RowFor(surface);

    // No `SoleEndpointOf`, and its absence is the point rather than an omission: the
    // unit bound and listened before this process existed, so there is no address here
    // to resolve and re-binding an already-listening socket fails. `Adopt` performs no
    // bind, no listen and no `SO_REUSEADDR` for that reason.
    //
    // Ownership of `descriptor` passes into this call and is not ours again on any
    // path -- the listener closes it in its destructor, including when the adoption
    // itself failed -- so there is deliberately no `::close` anywhere below.
#if defined(_WIN32)
    // Unreachable rather than unsupported, and answered rather than not compiled.
    // Socket activation here is systemd's protocol: `AdoptInheritedDescriptors` is
    // `#if !defined(_WIN32)` and hands back nothing on Windows, so no caller can
    // arrive with a descriptor. What this branch buys is that the signature is the
    // same on every platform -- no `#ifdef` in the header, no call site that has to
    // know -- and that if a Windows activation mechanism is ever wired up it meets a
    // sentence instead of a link error. `IocpListener` has no `Adopt` yet (#465).
    (void) io;
    (void) descriptor;
    (void) advertisedHost;
    (void) responder;
    (void) metrics;
    (void) logger;
    (void) row;
    // **`namer` is deliberately not in that list, and both ways of "fixing" its
    // absence are worse than the asymmetry.** It is a `ToolchainNamer`, which is a
    // `std::function` -- a by-value parameter whose type has a non-trivial
    // destructor, so MSVC's C4100 is not issued for it: the destructor call is a
    // use. The seven above are references, an `int` and a `string_view`, every one
    // trivially destructible, which is why they need the cast and this one does not.
    //
    // `clang-cl` cannot reach `-Wunused-parameter` either: it takes the MSVC `/W4`
    // arm of `cmake/portable/PedanticCompiler.cmake`, whose frontend-variant check is
    // where that routing is stated, and the warning arrives only with the `-Wextra`
    // the Clang arm adds.
    //
    // Written down because the gap READS as an omission and was filed as one
    // ([#1347](https://github.com/LASTRADA-Software/fastcached/issues/1347)): a
    // `(void) namer;` would assert a warning this build cannot emit, and splitting
    // the signature per platform would contradict the paragraph above -- the
    // identical signature is the whole reason this arm exists rather than an
    // `#ifdef` in the header. Measured rather than argued: all three Windows legs
    // built this file green under `PEDANTIC_COMPILER_WERROR=ON`, which is the
    // default for the Windows presets.
    return std::unexpected { std::string { "socket activation is not available on this platform" } };
#else
    auto listener = PlatformListener::Adopt(io.Reactor(), descriptor);
    if (!listener || !listener->IsBound())
        return std::unexpected { std::format("cannot serve the socket-activated descriptor ({})",
                                             listener ? listener->BindError() : std::string_view { "null listener" }) };

    // Asked of the SOCKET. The port is the unit's choice and this process is never
    // told it, so `BoundPort()` is the only thing that knows -- and it is what makes
    // an activated node's log line and its `BoundEndpoint()` say something true
    // rather than echo a `--listen-node` that configured nothing.
    auto bound = FormatHostPort(advertisedHost, listener->BoundPort());
    logger.Logf(LogLevel::Info, "{} serving a socket-activated listener, advertised as {}", row.name, bound);

    return StartWithListener(
        io, surface, std::move(listener), std::move(bound), responder, metrics, logger, std::move(namer));
#endif
}

} // namespace FastCache::Node
