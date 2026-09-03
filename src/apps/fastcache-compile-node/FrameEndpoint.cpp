// SPDX-License-Identifier: Apache-2.0
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
    /// @param socket Where to write.
    /// @param bytes What to write.
    /// @return Whether all of it went out.
    [[nodiscard]] Task<bool> WriteAll(ISocket* socket, std::span<std::byte const> bytes)
    {
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

    State(
        NodeIoLoop& loop, IListener& l, IFrameResponder& r, std::string_view name, IMetricsSink& sink, ILogger& log) noexcept
        :
        io { loop },
        listener { l },
        responder { r },
        what { name },
        metrics { sink },
        logger { log }
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
    /// use-after-free: the socket is owned by `ServeConnection`'s coroutine frame
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
            for (auto const& entry: open)
                sockets.push_back(entry.socket);
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

        /// Bytes the watcher took off the socket proving the peer is still there.
        ///
        /// A pipelined request, in whole or in part. They are handed back rather than
        /// consumed because the loop's `ByteReader` is the only thing that may parse
        /// this stream -- see `ServeConnection`, which primes them.
        std::vector<std::byte> pulled;
    };

    /// How much a watcher takes when the socket turns out to have data on it.
    ///
    /// Small on purpose: whatever it does not take stays in the kernel's receive queue
    /// and the loop's own reader collects it a moment later. This buffer exists to
    /// answer one question -- EOF or data -- not to read a request.
    constexpr std::size_t PeerWatchProbeBytes = 512;

    /// How often a connection ending under a parked watch re-reads that watch.
    ///
    /// Short, because it is pure teardown latency on this side and the peer already
    /// has its answer; the ceiling that matters is `FrameServer::RefusalTimeout`, which
    /// bounds the whole wait. Nothing is measured against this, so it needs no
    /// justification beyond being small enough not to matter and large enough not to
    /// spin.
    constexpr std::chrono::milliseconds GracefulCloseStep { 5 };

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
    /// **A readable edge is not a disconnect**, and this is the entire reason the
    /// `Read` below exists. `EpollSocket::WaitReadable` probes with `recv(MSG_PEEK)`
    /// and reports readiness for `got >= 0` -- which is EOF (0) and pending data (>0)
    /// reported identically; IOCP's zero-byte `WSARecv` cannot separate them either.
    /// Calling a readable edge "gone" would make a worker discard a perfectly good
    /// object every time a client pipelined a request, which is a worse bug than the
    /// one this closes.
    ///
    /// @param socket The connection, shared so a parked wait cancelled by `Close()`
    ///        cannot resume onto a destroyed socket -- the second use-after-free
    ///        `RedisResp`'s readable watcher records, in a tree where `Close()`
    ///        resumes inline on epoll and marshals on IOCP.
    /// @param watch Where the answer is left; shared with the connection.
    DetachedTask WatchPeer(std::shared_ptr<ISocket> socket, std::shared_ptr<PeerWatch> watch)
    {
        auto const readable = co_await socket->WaitReadable();
        if (!readable.has_value())
        {
            // The socket errored or was closed under us -- including by the `Close()`
            // the connection issues to retrieve this very frame. Either way there is
            // nobody to answer, and a connection tearing itself down does not care
            // which of the two it was.
            watch->gone = true;
            watch->finished = true;
            co_return;
        }

        std::array<std::byte, PeerWatchProbeBytes> probe {};
        auto const got = co_await socket->Read(std::span<std::byte> { probe });
        if (!got.has_value() || *got == 0)
            watch->gone = true;
        else
            watch->pulled.assign(probe.begin(), probe.begin() + static_cast<std::ptrdiff_t>(*got));
        watch->finished = true;
        co_return;
    }

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

                if (decoded->payloadLength > cap)
                {
                    // Refused with a reply naming BOTH numbers, because "too large"
                    // without the ceiling tells an operator nothing about a 64 KiB
                    // limit. The bytes are never taken: the check is on the declared
                    // length, before the read.
                    //
                    // Answered BEFORE the body is stepped over, which is the one place
                    // this deliberately does not copy the daemon's order. Draining
                    // first makes the reply contingent on the peer finishing a write
                    // it may already have abandoned -- a client that computed the
                    // frame was too large and sent only the header then waits for an
                    // answer that arrives when the sweeper closes it, i.e. never
                    // usefully. Writing first costs nothing and the resynchronization
                    // is just as good: a peer that sends what it declared is still
                    // stepped over exactly.
                    //
                    // Encoded and counted by the SURFACE, exactly as the pre-payload
                    // refusal below is, and this branch was the one that was not:
                    // it answered `payload-too-large` correctly and moved nothing, so
                    // the cheapest probe there is -- a header and no body -- left the
                    // series an operator alerts on perfectly flat (#326, undone by the
                    // merge and reinstated by #447). Two ceilings, one fact: this is
                    // the surface-wide cap and `DecidePrePayload` holds the per-verb
                    // one, an operator does the same thing about both, so they share
                    // one counter and one decision rather than splitting.
                    if (!co_await WriteAll(socket.get(),
                                           state->responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge,
                                                                         decoded->opRaw,
                                                                         std::format("{} exceeds the {} {}-byte request cap",
                                                                                     decoded->payloadLength,
                                                                                     state->what,
                                                                                     cap))))
                        break;

                    // Then the body is STEPPED OVER so the connection stays usable,
                    // bounded by the wire's own shared factor rather than a second
                    // opinion about it. `Skip` discards in chunks and never
                    // materialises the frame, so the memory the cap protects is still
                    // never taken.
                    auto const drainable = static_cast<std::uint64_t>(cap) * Wire::OversizeDrainFactor;
                    if (decoded->payloadLength > drainable || !(co_await reader.Skip(decoded->payloadLength)).has_value())
                    {
                        // Past the bound the connection ends -- but not with the peer's
                        // bytes still queued, which would reset it and take the refusal
                        // just written back out of the peer's buffer. Best effort, and
                        // bounded like every other drain here: a peer that keeps sending
                        // past that has chosen the reset.
                        co_await DrainUntilPeerCloses(socket.get(), cap);
                        break;
                    }
                    continue;
                }

                // Admission, BEFORE the budget is charged and before a payload byte
                // is read. Third of three pre-payload refusals in this loop, and the
                // same shape as the two around it: decided on what the header
                // declared, answered as a reply, resynchronized by stepping over the
                // body the peer said it was sending.
                //
                // Ordered ahead of the byte budget on purpose. A peer this surface
                // will not serve should not be able to reach a resource decision at
                // all -- otherwise a flood of refusable frames could still exhaust
                // the budget and make the surface answer `EndpointBusy` to the peers
                // it does serve, which is the denial reconstructed one step further
                // out (#285, #377).
                //
                // The predicate belongs to the responder; this only asks it earlier.
                if (auto refusal = state->responder.RefusePeer(peer, decoded->opRaw); refusal.has_value())
                {
                    // Answered first, then drained, exactly as the two refusals below
                    // and above are: draining first would make the reply contingent on
                    // a peer finishing a write it may already have abandoned.
                    if (!co_await WriteAll(socket.get(), *refusal))
                        break;
                    if (!(co_await reader.Skip(decoded->payloadLength)).has_value())
                        break;
                    continue;
                }

                // The credential, decided from the header and this connection's state
                // (#289). A SECOND question at the same point rather than a wider
                // first one: `RefusePeer` answers on the peer and the verb before the
                // payload is read, and returns an encoded refusal; this one answers on
                // the verb alone and feeds the decision alongside the declared length
                // and per-connection state, so folding them together would make
                // neither predicate's name describe it.
                //
                // `DecidePrePayload` is the same function the daemon's loop calls, so
                // the two surfaces cannot disagree about which verbs are open, what
                // they may carry, or in which order those are decided.
                auto const decision = Wire::DecidePrePayload({ .opRaw = decoded->opRaw,
                                                               .declaredLength = decoded->payloadLength,
                                                               .sessionCap = cap,
                                                               .authRequired = state->responder.AuthRequired(decoded->opRaw),
                                                               .credentialAccepted = credentialAccepted });
                if (decision != Wire::PrePayloadDecision::Serve)
                {
                    // Encoded and counted by the surface, not here: the endpoint owns
                    // WHEN the question is asked, the responder owns the answer.
                    if (!co_await WriteAll(socket.get(), state->responder.RefusalReply(decision, decoded->opRaw, {})))
                        break;
                    if (!(co_await reader.Skip(decoded->payloadLength)).has_value())
                        break;
                    continue;
                }

                if (auto const budget = state->responder.MaxInFlightBytes(),
                    held = state->inFlightBytes.load(std::memory_order_acquire);
                    budget != 0 && held + decoded->payloadLength > budget)
                {
                    // Checked on the DECLARED length, before a payload byte is read,
                    // so an over-budget request costs no allocation at all. The
                    // connection cap alone would not bound memory: N connections each
                    // declaring the per-request maximum is still N times it.
                    //
                    // Drained rather than closed, and it is bounded by `cap` above --
                    // this branch is only reached for a declaration the surface WOULD
                    // have accepted, so the peer is told to come back rather than made
                    // to reconnect over a transient budget. Answered first, for the
                    // reason the oversize branch is.
                    //
                    // The figure in the message is the one the DECISION was taken on,
                    // read once. Loaded a second time to format it, the refusal could
                    // name a number that does not explain it -- another connection
                    // releasing in between yields "has 0 of N bytes in flight" beside
                    // a refusal for having too many.
                    //
                    // Counted by the surface too, and it is the same regression the
                    // oversize branch above was: the dedicated compile port answered
                    // this with `CompileRefusal::EndpointBusy` and moved
                    // `worker_jobs_refused_endpoint_busy_total`, which the operator
                    // documentation still promises, and the merged listener answered
                    // it with a bare code (#447).
                    if (!co_await WriteAll(socket.get(),
                                           state->responder.EndpointRefusalReply(
                                               EndpointRefusal::InFlightBudget,
                                               decoded->opRaw,
                                               std::format("{} has {} of {} bytes in flight", state->what, held, budget))))
                        break;
                    if (!(co_await reader.Skip(decoded->payloadLength)).has_value())
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
                    if (!co_await WriteAll(socket.get(),
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
                // **Armed only when the reader holds nothing, and that is what makes
                // priming the pulled bytes ordering-safe.** `PrimeWith` prepends, so
                // handing it bytes read AFTER something already buffered would put the
                // later bytes first and corrupt the stream. With the buffer empty here
                // it cannot happen: the loop is the only other thing that touches this
                // reader and it is suspended for the whole of `Answer`, on the one
                // reactor thread these surfaces share.
                //
                // Declining to watch when bytes are already queued costs nothing worth
                // having, either -- a peer that has just pipelined a request has proved
                // it is there, which is the entire question this watch exists to ask.
                auto const watchCounter = state->responder.PeerWatchCounter(decoded->opRaw);
                std::shared_ptr<PeerWatch> watch;
                if (watchCounter.has_value() && reader.Buffered().empty())
                {
                    watch = std::make_shared<PeerWatch>();
                    WatchPeer(socket, watch);
                }

                state->EnterResponder(socket.get());
                auto const reply = co_await state->responder.Answer(frame, peer);

                // **The one place a deferred sweep is observed, and deliberately one.**
                // This loop held six `WriteAll` calls before this line and holds seven
                // with it; a "check whether you were swept first" rule spread over them
                // would be a rule to remember six times and to forget at the eighth
                // somebody adds. Here it is instead a consequence of where the mark can
                // be set at all, and the refusal leaves by the same statement sequence
                // every other reply does -- so "exactly one writer" is structural
                // rather than agreed.
                if (state->LeaveResponder(socket.get()))
                {
                    if (co_await WriteAll(socket.get(), DeadlineRefusalReply(*state, decoded->opRaw, answerWindow)))
                        // Counted only when it went OUT. A peer that had already hung
                        // up was told nothing, and a row saying otherwise would make
                        // the gap between this and the sweep row mean something else.
                        state->metrics.Increment(IMetricsSink::Counter::FrameDeadlineRefusalsSent);

                    // And then it ends, whether or not the refusal landed. This
                    // connection has been swept; the answer it was holding is not
                    // owed to anybody any more, and continuing the loop would serve
                    // a second request on a connection the sweeper has written off.
                    break;
                }

                // **What the watch learned. Three states, and each is a different
                // thing to do** -- which is why `PeerWatch` carries two flags and not
                // one: "the peer went" and "the watcher is still parked" are not
                // opposites, and a single bool could not tell either from "it woke,
                // found a request, and finished".
                if (watch)
                {
                    // **One turn of the loop before reading the flags**, and it is what
                    // makes a request pipelined DURING the answer survive rather than
                    // merely usually survive. The watcher's wake is a reactor event and
                    // so is a compile hopping home, so at this instant the wake can be
                    // queued and not yet run -- and reading the flags then would end a
                    // connection whose peer had just proved it was there.
                    //
                    // Yielding lets everything already queued run first, on the one
                    // thread both of them use. It cannot wait for a watcher that is
                    // genuinely parked, and must not: this yields ONCE and re-reads,
                    // rather than waiting on something that may never arrive.
                    if (!watch->finished)
                        co_await ResumeOn { state->io.Reactor() };

                    if (watch->gone)
                    {
                        // The object is NOT written. This is the waste #223 measured
                        // at 84 MB for one translation unit, going to a client that
                        // had already given up -- and until now nothing here observed
                        // it, because the loop is not reading while a compile runs.
                        //
                        // Counted only when there was something to deliver. An empty
                        // reply means the surface had already decided to close, and
                        // filing that as an abandoned delivery would inflate the row
                        // with events that cost nothing.
                        if (!reply.empty())
                            state->metrics.Increment(*watchCounter);
                        break;
                    }

                    // Bytes the watcher took to tell EOF from data. They belong to a
                    // pipelined request and go back into the one reader allowed to
                    // parse this stream; ordering is safe because the watch is armed
                    // only when that reader holds nothing.
                    reader.PrimeWith(watch->pulled);
                }

                if (reply.empty())
                    break; // `IFrameResponder::Answer` documents an empty reply as
                           // "close without answering", which is only ever right
                           // when the peer is not speaking this protocol at all.
                if (!co_await WriteAll(socket.get(), reply))
                    break;

                // **A watch still parked ends the connection, and the `break` IS the
                // cancellation**: it falls through to the `socket->Close()` below,
                // which is the only thing on `ISocket` that can retrieve a coroutine
                // parked in `WaitReadable`.
                //
                // The alternative is to loop round and issue the next `Read` while the
                // watcher is still parked, and that is not merely undefined -- `Read`
                // and `WaitReadable` share one read-op slot and each begins by nulling
                // the parked awaitable, so the second arm drops the first coroutine's
                // resume handle and leaks its frame, silently, per request, on both
                // epoll and IOCP. `IFrameResponder::PeerWatchCounter` carries the whole
                // argument, including why this costs nothing against any client in
                // this tree and what check would falsify that.
                if (watch && !watch->finished)
                {
                    // **Closing straight away loses the reply that was just written**,
                    // and it is measured rather than feared: the pipelined-request case
                    // received the first answer and then ZERO further bytes, because a
                    // close over a socket with I/O still pending is abortive and takes
                    // the buffered reply with it. That is the same hazard
                    // `RefuseAtCapacity` documents from the other direction -- a peer
                    // that never saw the refusal it was sent, only a reset.
                    //
                    // So the peer is given the chance to take the answer and hang up,
                    // and the watcher is what observes it doing so: the client's FIN is
                    // a readable edge, which is precisely what this coroutine is parked
                    // on. When it fires, the close below is graceful and the frame is
                    // already unwound.
                    //
                    // Polled in short steps rather than waited on, for the reason
                    // `InterruptibleSleepUntil` gives for the same shape: a wait that
                    // parks a second frame on the timer wheel leaves it there after the
                    // wait has ended. Bounded by `RefusalTimeout`, and for its reason --
                    // the peer has its answer already and has only to read it, which is
                    // a round trip rather than a request. A peer that dawdles past that
                    // is closed on anyway, which is exactly what happened before this
                    // block existed.
                    //
                    // It costs the CLIENT nothing: the reply went out at the `WriteAll`
                    // above, and this waits only to make the hang-up orderly.
                    auto const until = state->io.Reactor().Clock().Now() + FrameServer::RefusalTimeout;
                    while (!watch->finished && state->io.Reactor().Clock().Now() < until)
                        co_await SleepFor(state->io.Reactor(), GracefulCloseStep);
                    break;
                }
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
            if (co_await WriteAll(socket.get(),
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
                         ILogger& logger) noexcept:
    _state { std::make_shared<State>(io, listener, responder, what, metrics, logger) }
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
                             ILogger& logger):
    _listener { std::move(listener) },
    _server { std::make_unique<FrameServer>(io, *_listener, responder, what, metrics, logger) },
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
}

std::size_t FrameEndpoint::InFlightBytes() const noexcept
{
    return _server->InFlightBytes();
}

std::expected<std::unique_ptr<FrameEndpoint>, std::string> FrameEndpoint::Start(NodeIoLoop& io,
                                                                                NodeSurface surface,
                                                                                NodeConfig const& cfg,
                                                                                IFrameResponder& responder,
                                                                                IMetricsSink& metrics,
                                                                                ILogger& logger)
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

    // `new` rather than `make_unique` because the constructor is private: the ways to
    // reach it are this factory and `StartAdopted`, each of which has already proved
    // its listener is bound, and nothing else.
    return std::unique_ptr<FrameEndpoint> { new FrameEndpoint {
        io, std::move(listener), responder, row.name, std::move(bound), metrics, logger } };
}

std::expected<std::unique_ptr<FrameEndpoint>, std::string> FrameEndpoint::StartAdopted(NodeIoLoop& io,
                                                                                       NodeSurface surface,
                                                                                       int descriptor,
                                                                                       std::string_view advertisedHost,
                                                                                       IFrameResponder& responder,
                                                                                       IMetricsSink& metrics,
                                                                                       ILogger& logger)
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

    return std::unique_ptr<FrameEndpoint> { new FrameEndpoint {
        io, std::move(listener), responder, row.name, std::move(bound), metrics, logger } };
#endif
}

} // namespace FastCache::Node
