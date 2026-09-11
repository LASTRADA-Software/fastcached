// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/ParkedWork.hpp>

    #include <winsock2.h>

    #include <coroutine>
    #include <utility>

namespace FastCache::Detail
{

/// Per-dial state for a ConnectEx dial, living in the dialling coroutine's own
/// frame -- the IOCP counterpart of `ReadinessDialOp`.
///
/// `completion` is FIRST because the reactor reinterpret_casts the `lpOverlapped`
/// it dequeues back to an `IocpCompletion*` and then to the enclosing op -- the
/// same layout contract `IocpSocket::Impl::Op` relies on. It lives in the
/// dialling coroutine's frame, so its address is stable for exactly as long as
/// the port can reach it.
///
/// **That last sentence is the invariant, and it is why this type carries no
/// `inFlight` reference the way `IocpSocket::Impl::Op` and
/// `IocpListener::Impl::AcceptOp` do. Do not "make it consistent" with them.**
///
/// The op outlives the operation because the OWNER IS SUSPENDED ON IT: the
/// coroutine cannot leave the frame holding this struct without the completion
/// having arrived, since `co_await ConnectPark { &op }` is resumed only by the
/// completion. Even the timeout path cancels and then keeps waiting, precisely
/// because `CancelIoEx` makes the completion arrive rather than retracting it.
///
/// The socket and the listener cannot use this shape: they are owned by whoever
/// holds their `unique_ptr`, and a destructor is not a coroutine and cannot
/// suspend until a completion lands. That asymmetry is the whole of #465 --
/// removing this comment and adding a reference count here would be harmless, but
/// removing the *waiting* on the belief that a reference count replaces it would
/// reintroduce the defect in the one place that never had it.
///
/// **Templated on the reactor alone, where `ReadinessDialOp` takes a traits
/// triple, and that is a difference rather than an inconsistency**: the readiness
/// dial names a handler type and a socket type as well, so a triple carries three
/// facts. This op names one. A one-alias traits struct would be an indirection
/// standing for nothing, which is what the data-driven rule is against rather
/// than for.
///
/// @tparam Reactor The reactor the settle hands the waiter back to.
template <typename Reactor>
struct ConnectOp
{
    IocpCompletion completion {};
    Reactor* reactor { nullptr };
    ParkedWork waiter {};
    SOCKET socket { INVALID_SOCKET };
    DWORD error { 0 };
    bool settled { false };
    bool timedOut { false };
};

/// Suspends until the completion arrives.
///
/// @tparam Op The `ConnectOp` specialisation being awaited.
template <typename Op>
struct ConnectPark
{
    Op* op { nullptr };

    [[nodiscard]] bool await_ready() const noexcept
    {
        return op->settled;
    }

    /// Templated on the promise for `ResumeOn`'s reason: `SettleConnect` posts
    /// this chain to a reactor that may be destroyed before it dequeues it, and
    /// only the parking coroutine's own promise type knows whether anything else
    /// can free it
    /// ([#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
    /// By the time the completion runs the handle is erased, so this is the last
    /// place the question can be asked.
    /// @tparam Promise The suspending coroutine's promise type.
    /// @param handle The suspended dial.
    /// @return true to stay suspended; false when the dial settled first.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> handle) const noexcept
    {
        if (op->settled)
            return false;
        op->waiter = ParkedWorkFor(handle);
        return true;
    }

    void await_resume() const noexcept {}
};

/// Publish an outcome and hand the waiter back to the reactor.
///
/// The IOCP counterpart of `SettleDial`, and split out of `OnConnectComplete` for
/// the same reason that one is split out of the epoll and kqueue callbacks: the
/// platform callback's job is to say WHAT happened, and this function's job is to
/// hand the chain over. Only the second half has a contract worth a test, and
/// while the two were one function inside an anonymous namespace nothing outside
/// that translation unit could reach it
/// ([#1138](https://github.com/LASTRADA-Software/fastcached/issues/1138)).
///
/// **`Submit(waiter)` and not `Submit(waiter.resume)`, and the difference is the
/// whole point of this function existing.** The borrowing overload frees nothing,
/// so a reactor destroyed before it dequeues the post leaks the chain and
/// everything reachable from it. `ParkedWork::abandon` is what says the chain
/// belongs to nobody, and it is carried only by the owning overload
/// ([#1041](https://github.com/LASTRADA-Software/fastcached/issues/1041)).
///
/// The resume goes through the reactor and never happens inline: the caller is a
/// completion callback running on the reactor's own worker thread, and resuming
/// here would let the dial coroutine return and free the frame this op lives in
/// while the dispatch is still holding a pointer into it.
///
/// @tparam Reactor The reactor type; see `ConnectOp`.
/// @param op The dial being settled.
/// @param error The WSA error code, or 0 for success.
template <typename Reactor>
void SettleConnect(ConnectOp<Reactor>& op, DWORD error) noexcept
{
    // The completion is the SINGLE writer of the outcome, which is why the
    // deadline cancels the operation rather than settling the op itself. That
    // removes the two-writer race the readiness path has to guard against -- and
    // this guard is what keeps that true for a second dispatch of one op.
    if (op.settled)
        return;
    op.settled = true;
    op.error = error;

    if (auto waiter = std::exchange(op.waiter, ParkedWork {}); waiter.resume)
        op.reactor->Submit(waiter);
}

} // namespace FastCache::Detail

#endif // _WIN32
