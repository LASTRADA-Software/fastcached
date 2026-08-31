// SPDX-License-Identifier: Apache-2.0
#include "FrameEndpoint.hpp"
#include "NodeIoLoop.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/PlatformListener.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

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

} // namespace

/// Everything the accept loop and its connection tasks share.
///
/// In one struct behind a pointer rather than as members of `FrameServer`, so the
/// free-function connection tasks can name it without `FrameServer`'s privates
/// being public -- the same reason `EpollSocket::Impl` is spelled this way.
struct FrameServer::State
{
    NodeIoLoop& io;
    IListener& listener;
    IFrameResponder& responder;
    std::string what;
    ILogger& logger;

    std::atomic<bool> shuttingDown { false };

    /// Sockets being served, each with the instant it must finish by.
    ///
    /// Every mutation happens on the reactor thread; the mutex is here because
    /// `Shutdown()` may be called from another one and needs to read the set to
    /// know whether to wait. Raw pointers, because the owning `unique_ptr` lives in
    /// the connection task's frame and the registration is removed before it ends.
    mutable std::mutex mutex;
    std::vector<std::pair<ISocket*, TimePoint>> open;

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

    State(NodeIoLoop& loop, IListener& l, IFrameResponder& r, std::string_view name, ILogger& log) noexcept:
        io { loop },
        listener { l },
        responder { r },
        what { name },
        logger { log }
    {
    }

    /// Register a socket with the deadline it must finish by.
    void Track(ISocket* socket, TimePoint deadline)
    {
        std::scoped_lock const guard { mutex };
        open.emplace_back(socket, deadline);
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
    void Rearm(ISocket* socket, TimePoint deadline)
    {
        std::scoped_lock const guard { mutex };
        for (auto& entry: open)
            if (entry.first == socket)
                entry.second = deadline;
    }

    /// Deregister a socket. Must happen before its owner destroys it.
    void Untrack(ISocket* socket)
    {
        std::scoped_lock const guard { mutex };
        std::erase_if(open, [socket](auto const& entry) { return entry.first == socket; });
    }

    /// Close every socket past its deadline. Reactor thread only.
    /// @param now The reactor's current time.
    /// @return How many were closed.
    std::size_t CloseOverdue(TimePoint now)
    {
        std::vector<ISocket*> overdue;
        {
            std::scoped_lock const guard { mutex };
            for (auto const& [socket, deadline]: open)
                if (deadline <= now)
                    overdue.push_back(socket);
        }
        // Closed outside the lock: `Close` completes a parked read by resuming its
        // coroutine inline, and that coroutine calls `Untrack`, which takes this
        // same mutex.
        for (auto* socket: overdue)
            socket->Close();
        return overdue.size();
    }

    /// Close the listener and every open connection. Reactor thread only.
    void CloseAll()
    {
        listener.Close();

        std::vector<ISocket*> sockets;
        {
            std::scoped_lock const guard { mutex };
            for (auto const& [socket, deadline]: open)
                sockets.push_back(socket);
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
            _state->inFlightBytes.fetch_sub(_bytes, std::memory_order_acq_rel);
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
    /// @param credentialAccepted This connection's flag; set only on `Accepted`.
    /// @return The reply frame to write.
    [[nodiscard]] std::vector<std::byte> AnswerAuth(IFrameResponder const& responder,
                                                    std::span<std::byte const> payload,
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
        switch (outcome)
        {
            case CredentialOutcome::Malformed:
                return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame, {});
            case CredentialOutcome::Rejected:
                return Wire::EncodeErrorReply(Wire::ErrorCode::Unauthenticated, "authentication failed");
            case CredentialOutcome::NoPolicy:
            case CredentialOutcome::Accepted:
                break;
        }
        return Wire::EncodeReply(Wire::Status::Ok, {});
    }

    DetachedTask ServeConnection(std::shared_ptr<FrameServer::State> shared, std::unique_ptr<ISocket> owned)
    {
        auto* const state = shared.get();
        auto socket = std::move(owned);
        OpenConnectionSlot const slot { state };

        // Registered with its deadline BEFORE the first read, so a client that sends
        // half a header is swept rather than holding a frame until the process dies.
        // `Rearm` below moves it forward once per request -- not once per read; see
        // its note for what the window then covers.
        auto const deadlineFor = [state] {
            return state->io.Reactor().Clock().Now() + FrameServer::RequestTimeout;
        };
        state->Track(socket.get(), deadlineFor());

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

                state->Rearm(socket.get(), deadlineFor());

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
                    if (!co_await WriteAll(socket.get(),
                                           Wire::EncodeErrorReply(Wire::ErrorCode::PayloadTooLarge,
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
                    if (!co_await WriteAll(socket.get(), state->responder.RefusalReply(decision, decoded->opRaw)))
                        break;
                    if (!(co_await reader.Skip(decoded->payloadLength)).has_value())
                        break;
                    continue;
                }

                if (auto const budget = state->responder.MaxInFlightBytes();
                    budget != 0 && state->inFlightBytes.load(std::memory_order_acquire) + decoded->payloadLength > budget)
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
                    if (!co_await WriteAll(
                            socket.get(),
                            Wire::EncodeErrorReply(Wire::ErrorCode::EndpointBusy,
                                                   std::format("{} has {} of {} bytes in flight",
                                                               state->what,
                                                               state->inFlightBytes.load(std::memory_order_acquire),
                                                               budget))))
                        break;
                    if (!(co_await reader.Skip(decoded->payloadLength)).has_value())
                        break;
                    continue;
                }

                BudgetedBytes const bytes { state, decoded->payloadLength };
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
                    if (!co_await WriteAll(socket.get(), AnswerAuth(state->responder, *payload, credentialAccepted)))
                        break;
                    continue;
                }

                std::vector<std::byte> frame { header->begin(), header->end() };
                frame.insert(frame.end(), payload->begin(), payload->end());

                // Awaited: answering may reach the network -- the cache surface
                // consults an upstream -- and that suspends rather than blocking
                // every other connection on this loop, which is the whole reason
                // this task exists separately from the accept loop.
                auto const reply = co_await state->responder.Answer(frame, peer);
                if (reply.empty())
                    break; // `IFrameResponder::Answer` documents an empty reply as
                           // "close without answering", which is only ever right
                           // when the peer is not speaking this protocol at all.
                if (!co_await WriteAll(socket.get(), reply))
                    break;
            }
        }
        catch (...)
        {
            state->logger.Logf(LogLevel::Error, "{}: dropping a connection that threw", state->what);
        }

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
        state->Track(socket.get(), deadline);
        try
        {
            if (co_await WriteAll(socket.get(),
                                  Wire::EncodeErrorReply(Wire::ErrorCode::EndpointBusy,
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
            if (auto const closed = state->CloseOverdue(state->io.Reactor().Clock().Now()); closed != 0)
                state->logger.Logf(
                    LogLevel::Debug, "{}: closed {} connection(s) past the request deadline", state->what, closed);
        }
        state->loopsAlive.fetch_sub(1, std::memory_order_acq_rel);
        state->io.NoteLoopFinished();
        co_return;
    }

} // namespace

FrameServer::FrameServer(
    NodeIoLoop& io, IListener& listener, IFrameResponder& responder, std::string_view what, ILogger& logger) noexcept:
    _state { std::make_shared<State>(io, listener, responder, what, logger) }
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
    // Bounded, because a stuck connection must not turn a stop into a hang: the
    // ceiling and cadence `RaftPeerServer::Shutdown` already uses, for the reason it
    // records.
    constexpr auto Ceiling = std::chrono::seconds { 5 };
    constexpr auto Poll = std::chrono::milliseconds { 5 };
    auto waited = std::chrono::milliseconds { 0 };
    while ((_state->loopsAlive.load(std::memory_order_acquire) != 0 || _state->OpenCount() != 0) && waited < Ceiling)
    {
        std::this_thread::sleep_for(Poll);
        waited += Poll;
    }

    if (auto const stuck = _state->loopsAlive.load(std::memory_order_acquire) + _state->OpenCount(); stuck != 0)
        _state->logger.Logf(
            LogLevel::Error, "{}: {} loop(s)/connection(s) did not finish within the stop ceiling", _state->what, stuck);
}

FrameEndpoint::FrameEndpoint(NodeIoLoop& io,
                             std::unique_ptr<IListener> listener,
                             IFrameResponder& responder,
                             std::string_view what,
                             std::string boundEndpoint,
                             ILogger& logger):
    _listener { std::move(listener) },
    _server { std::make_unique<FrameServer>(io, *_listener, responder, what, logger) },
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

std::expected<std::unique_ptr<FrameEndpoint>, std::string> FrameEndpoint::Start(
    NodeIoLoop& io, NodeSurface surface, NodeConfig const& cfg, IFrameResponder& responder, ILogger& logger)
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

    // `new` rather than `make_unique` because the constructor is private: the two ways
    // to reach it are this factory, which has already proved the listener is bound,
    // and nothing else.
    return std::unique_ptr<FrameEndpoint> { new FrameEndpoint {
        io, std::move(listener), responder, row.name, std::move(bound), logger } };
}

} // namespace FastCache::Node
