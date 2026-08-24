// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(__linux__) || defined(__APPLE__)

    #include <FastCache/Async/DeadlineTimer.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Clock.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/ISocket.hpp>
    #include <FastCache/Net/SocketAddress.hpp>

    #include <sys/socket.h>

    #include <cerrno>
    #include <coroutine>
    #include <expected>
    #include <memory>
    #include <utility>

namespace FastCache::Detail
{

/// Per-dial state, living in the dialling coroutine's own frame.
///
/// In the frame rather than in the connector, and that is a property rather than
/// a convenience: its address is stable for exactly as long as the reactor can
/// reach it, and it disappears with the attempt. A connector holding a slot per
/// dial would let a dial that timed out while still in flight tie one up.
///
/// @tparam Traits Platform triple; see `DialReadiness`.
template <typename Traits>
struct ReadinessDialOp
{
    Traits::Reactor* reactor { nullptr };
    Traits::Handler handler {};
    std::coroutine_handle<> waiter {};
    std::expected<void, NetError> outcome {};
    bool settled { false };
};

/// Suspends until a dial settles.
///
/// Deliberately NOT an `AcceptAwaitable`-shaped type carrying a result. The
/// awaiting frame OWNS the op it reads the outcome from, so there is nothing for
/// a result-carrying awaitable to carry, and building one would have made it a
/// near-copy of `IListener.hpp`'s.
///
/// @tparam Op The `ReadinessDialOp` specialisation being awaited.
template <typename Op>
struct DialPark
{
    Op* op { nullptr };

    [[nodiscard]] bool await_ready() const noexcept
    {
        return op->settled;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const noexcept
    {
        if (op->settled)
            return false;
        op->waiter = handle;
        return true;
    }

    void await_resume() const noexcept {}
};

/// Publish an outcome, mute the fd, and hand the waiter back to the reactor.
///
/// The resume goes through `IReactor::Submit` and never happens inline. The
/// reactor is mid-dispatch on this very handler; resuming here would let the dial
/// coroutine return and free the frame the handler lives in, and the loop would
/// then be holding a pointer into freed memory. The same reason
/// `EpollReactor`'s loop now services at most one callback per fd.
///
/// @param op The dial being settled.
/// @param outcome Success, or why the dial failed.
template <typename Traits>
void SettleDial(ReadinessDialOp<Traits>& op, std::expected<void, NetError> outcome)
{
    if (op.settled)
        return;
    op.settled = true;
    op.outcome = std::move(outcome);

    // Detached before anything else, and the reason is the SPIN rather than the
    // handler's lifetime. Once the op is settled its callbacks return
    // immediately, so a level-triggered fd still reporting an error would be
    // dispatched, ignored, and reported again on the very next iteration -- a
    // busy loop for as long as the fd stays registered.
    //
    // It is deliberately NOT claimed to protect the socket that may be built on
    // this fd afterwards. It looks as though it should: the socket's constructor
    // attaches the same fd, epoll refuses that with EEXIST, and the failure is
    // ignored. But `UpdateInterest` uses EPOLL_CTL_MOD with a fresh
    // `ev.data.ptr`, so the socket's first armed read overwrites the stale
    // registration and the fd ends up pointing at the right handler regardless.
    // Verified by removing this call and watching the byte-transfer case still
    // pass -- so the hazard is real for the reactor's own loop and benign for the
    // socket, and saying otherwise would send the next reader looking for a bug
    // that is not there.
    op.reactor->Detach(&op.handler);

    if (auto waiter = std::exchange(op.waiter, {}); waiter)
        op.reactor->Submit(waiter);
}

/// The readiness-based dial, shared verbatim by epoll and kqueue.
///
/// One body for both because `EpollFdHandler` and `KqueueFdHandler` have
/// identical member names and both reactors spell `Attach`/`UpdateInterest`/
/// `Detach` the same way -- so a three-alias traits struct compiles against
/// either with no `#if` in here at all. Two near-identical connectors differing
/// only by a type is exactly what the data-driven rule forbids.
///
/// @tparam Traits `{ Reactor, Handler, Socket }` for the platform.
/// @param reactor Reactor the resulting socket is pinned to.
/// @param endpoint Candidate to dial. By value: this is a coroutine, so a
///        reference parameter could dangle at the first suspend.
/// @param deadline When to give up on THIS candidate.
/// @return The connected socket, or why this candidate did not produce one.
template <typename Traits>
[[nodiscard]] Task<SocketResult> DialReadiness(typename Traits::Reactor* reactor,
                                               ResolvedEndpoint endpoint,
                                               TimePoint deadline)
{
    OwnedNativeSocket holder { static_cast<NativeSocket>(::socket(endpoint.family, SOCK_STREAM, endpoint.protocol)) };
    if (!holder.Valid())
        co_return std::unexpected(MakeNetError(LastNetworkError(), "socket() failed"));

    if (!SetNonBlocking(holder.Get()))
        co_return std::unexpected(MakeNetError(LastNetworkError(), "could not make the socket non-blocking"));

    // A dialled socket is created by a plain ::socket and so is NOT close-on-exec,
    // unlike an accepted one. It matters because a process that dials and also
    // spawns children -- the compile node runs a compiler per job -- would
    // otherwise hand every child an open peer connection.
    ArmCloseOnExec(holder.Get());

    auto const* const address = reinterpret_cast<sockaddr const*>(endpoint.storage.data());
    auto const issued = ::connect(static_cast<int>(holder.Get()), address, static_cast<socklen_t>(endpoint.length));

    if (issued != 0)
    {
        auto const pending = LastNetworkError();
        if (pending != EINPROGRESS)
            co_return std::unexpected(MakeNetError(pending, "connect() failed"));

        ReadinessDialOp<Traits> op;
        op.reactor = reactor;
        op.handler.fd = static_cast<int>(holder.Get());
        op.handler.owner = &op;

        // Both directions AND the error hook point at the same routine. A refused
        // connect on Linux can be reported with neither EPOLLOUT nor EPOLLIN, so
        // arming only the write side would park forever on an fd the kernel is
        // shouting about -- which is what `onError` was added for.
        op.handler.onReadable = &Traits::Settle;
        op.handler.onWritable = &Traits::Settle;
        op.handler.onError = &Traits::Settle;

        if (!reactor->Attach(&op.handler))
            co_return std::unexpected(MakeNetError(LastNetworkError(), "could not attach the dialling socket"));

        if (!reactor->UpdateInterest(&op.handler, /*read*/ false, /*write*/ true))
        {
            reactor->Detach(&op.handler);
            co_return std::unexpected(MakeNetError(LastNetworkError(), "could not arm write interest for the dial"));
        }

        // Armed AFTER interest, so a deadline that has already passed cannot
        // settle the op before there is anything to detach.
        DeadlineTimer const timer { *reactor,
                                    deadline,
                                    [](void* state) {
                                        auto& timedOut = *static_cast<ReadinessDialOp<Traits>*>(state);
                                        SettleDial(timedOut,
                                                   std::unexpected(NetError { .code = NetErrorCode::Timeout,
                                                                              .systemCode = 0,
                                                                              .context = "connect timed out" }));
                                    },
                                    &op };

        co_await DialPark<ReadinessDialOp<Traits>> { .op = &op };

        if (!op.outcome.has_value())
            co_return std::unexpected(op.outcome.error());
    }

    // Readiness is not success: a refused connect also makes the socket ready.
    // SO_ERROR is the only thing that distinguishes them, and skipping it hands
    // the caller a socket whose first write fails.
    int pendingError = 0;
    auto length = static_cast<socklen_t>(sizeof(pendingError));
    auto const probed = ::getsockopt(static_cast<int>(holder.Get()), SOL_SOCKET, SO_ERROR, &pendingError, &length);
    if (probed != 0 || pendingError != 0)
        co_return std::unexpected(MakeNetError(probed != 0 ? LastNetworkError() : pendingError, "connect did not complete"));

    ApplyHotSocketOptions(holder.Get());

    // The peer string is the ADDRESS, not the requested host: it feeds the
    // `--log-source` prefix, which records the IP, and that is how the accept
    // path already formats it.
    auto peer = FormatPeerAddress(endpoint);

    // The handler is already detached (SettleDial) or was never attached at all
    // (a connect that completed inline, which is the ordinary loopback case). The
    // socket's own constructor attaches this fd for itself; see SettleDial for
    // why a leftover registration would not in fact break it.
    co_return std::make_unique<typename Traits::Socket>(*reactor, static_cast<int>(holder.Release()), std::move(peer));
}

} // namespace FastCache::Detail

#endif // __linux__ || __APPLE__
