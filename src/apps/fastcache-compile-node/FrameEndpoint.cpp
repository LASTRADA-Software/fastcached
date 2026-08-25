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

    std::atomic<std::size_t> inFlight { 0 };
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

    /// Holds a connection's slot in the in-flight count for as long as it is served.
    ///
    /// RAII rather than a decrement at each of the task's exits: a connection ends
    /// several ways -- a foreign magic, an oversize declaration, a short read, a
    /// normal answer -- and the exit that gets forgotten leaks a slot, after which
    /// the surface refuses everything forever while looking perfectly healthy.
    class ServedSlot
    {
      public:
        explicit ServedSlot(FrameServer::State* state) noexcept:
            _state { state }
        {
            _state->inFlight.fetch_add(1, std::memory_order_acq_rel);
        }

        ServedSlot(ServedSlot const&) = delete;
        ServedSlot(ServedSlot&&) = delete;
        ServedSlot& operator=(ServedSlot const&) = delete;
        ServedSlot& operator=(ServedSlot&&) = delete;

        ~ServedSlot()
        {
            _state->inFlight.fetch_sub(1, std::memory_order_acq_rel);
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

    /// Answer one connection, then close it.
    ///
    /// A free function taking raw pointers rather than a capturing lambda: a
    /// coroutine's closure outlives the expression that created it.
    /// @param state Shared server state.
    /// @param owned The accepted socket; this task owns it.
    DetachedTask ServeConnection(std::shared_ptr<FrameServer::State> shared, std::unique_ptr<ISocket> owned)
    {
        auto* const state = shared.get();
        auto socket = std::move(owned);
        ServedSlot const slot { state };

        // Registered with its deadline BEFORE the first read, so a client that sends
        // half a header is swept rather than holding a frame until the process dies.
        auto const deadline = state->io.Reactor().Clock().Now() + FrameServer::RequestTimeout;
        state->Track(socket.get(), deadline);

        // The firewall: this is a DetachedTask, whose unhandled_exception terminates
        // the process, so one client's answer must not take the node with it.
        try
        {
            // The peer's HOST. A connection's source port is ephemeral and is not the
            // peer's endpoint, so for a surface whose policy needs an identity -- the
            // scheduler's -- there is nothing else here that could supply one.
            auto peer = socket->PeerAddress();

            auto const cap = state->responder.MaxRequestBytes();
            ByteReader reader { *socket, /*maxLineBytes*/ 1, cap };
            auto const header = co_await reader.ReadExactly(Wire::RequestHeaderSize);
            if (header.has_value())
            {
                auto const decoded = Wire::DecodeRequestHeader(*header);
                if (!decoded.has_value())
                {
                    // A foreign magic: the peer is not speaking this protocol, and with
                    // no declared length there is nowhere to resynchronize to. Closing
                    // is the only thing left, and is what an empty answer means here.
                }
                else if (decoded->payloadLength > cap)
                {
                    // Refused with a reply naming BOTH numbers, because "too large"
                    // without the ceiling tells an operator nothing about a 64 KiB
                    // limit. The bytes are never taken: the check is on the declared
                    // length, before the read.
                    (void) co_await WriteAll(
                        socket.get(),
                        Wire::EncodeErrorReply(
                            Wire::ErrorCode::PayloadTooLarge,
                            std::format("{} exceeds the {} {}-byte request cap", decoded->payloadLength, state->what, cap)));
                }
                else if (auto const budget = state->responder.MaxInFlightBytes();
                         budget != 0
                         && state->inFlightBytes.load(std::memory_order_acquire) + decoded->payloadLength > budget)
                {
                    // Checked on the DECLARED length, before a payload byte is read,
                    // so an over-budget request costs no allocation at all. The
                    // connection cap alone would not bound memory: N connections each
                    // declaring the per-request maximum is still N times it.
                    (void) co_await WriteAll(
                        socket.get(),
                        Wire::EncodeErrorReply(Wire::ErrorCode::EndpointBusy,
                                               std::format("{} has {} of {} bytes in flight",
                                                           state->what,
                                                           state->inFlightBytes.load(std::memory_order_acquire),
                                                           budget)));
                }
                else
                {
                    BudgetedBytes const bytes { state, decoded->payloadLength };
                    if (auto const payload = co_await reader.ReadExactly(decoded->payloadLength); payload.has_value())
                    {
                        std::vector<std::byte> frame { header->begin(), header->end() };
                        frame.insert(frame.end(), payload->begin(), payload->end());

                        // Awaited: answering may reach the network -- the cache surface
                        // consults an upstream -- and that suspends rather than
                        // blocking every other connection on this loop, which is the
                        // whole reason this task exists separately from the accept
                        // loop.
                        if (auto const reply = co_await state->responder.Answer(frame, std::move(peer)); !reply.empty())
                            (void) co_await WriteAll(socket.get(), reply);
                    }
                }
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
    /// the loop on a client that is not reading. It takes no served slot and reads
    /// nothing, so it cannot itself be what keeps the surface at capacity -- but it
    /// IS registered with the sweeper, so a client that never reads its refusal is
    /// still let go.
    DetachedTask RefuseAtCapacity(std::shared_ptr<FrameServer::State> shared, std::unique_ptr<ISocket> owned)
    {
        auto* const state = shared.get();
        auto socket = std::move(owned);
        auto const deadline = state->io.Reactor().Clock().Now() + FrameServer::RequestTimeout;
        state->Track(socket.get(), deadline);
        try
        {
            (void) co_await WriteAll(socket.get(),
                                     Wire::EncodeErrorReply(Wire::ErrorCode::EndpointBusy,
                                                            std::format("{} is already serving {} requests",
                                                                        state->what,
                                                                        state->responder.MaxConcurrentRequests())));
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

std::size_t FrameServer::InFlight() const noexcept
{
    return _state->inFlight.load(std::memory_order_acquire);
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
        auto const concurrent = state->responder.MaxConcurrentRequests();
        if (concurrent != 0 && state->inFlight.load(std::memory_order_acquire) >= concurrent)
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

std::expected<std::unique_ptr<FrameEndpoint>, std::string> FrameEndpoint::Start(NodeIoLoop& io,
                                                                                std::string_view listenSpec,
                                                                                std::string_view defaultHost,
                                                                                IFrameResponder& responder,
                                                                                std::string_view what,
                                                                                ILogger& logger)
{
    auto const endpoint = ParseEndpoint(listenSpec, defaultHost);
    if (!endpoint.has_value())
        return std::unexpected { std::format("'{}' is not [<address>:]<port>", listenSpec) };

    auto listener = PlatformListener::Bind(io.Reactor(), endpoint->first, endpoint->second);
    if (!listener || !listener->IsBound())
        return std::unexpected { std::format("cannot bind {}:{} ({})",
                                             endpoint->first,
                                             endpoint->second,
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
    auto bound = std::format("{}:{}", endpoint->first, listener->BoundPort());
    logger.Logf(LogLevel::Info, "{} listening on {}", what, bound);

    // `new` rather than `make_unique` because the constructor is private: the two ways
    // to reach it are this factory, which has already proved the listener is bound,
    // and nothing else.
    return std::unique_ptr<FrameEndpoint> { new FrameEndpoint {
        io, std::move(listener), responder, what, std::move(bound), logger } };
}

} // namespace FastCache::Node
