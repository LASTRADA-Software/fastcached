// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/PlatformConnector.hpp>

// ---------------------------------------------------------------------------
// #1041: what a settled dial owes a chain nobody dequeues.
//
// The dial hands its waiter back with `reactor->Submit(waiter)`. Until #1041 that
// call could only BORROW, because `IReactor` re-declares the borrowing overload
// and hides `IExecutor::Submit(ParkedWork)` from everything holding an
// `IReactor&` -- so a reactor destroyed before the post is dequeued freed nothing.
//
// **This file covers the POSIX site only, and says so rather than implying more.**
// `ReactorDial.hpp`'s `SettleDial` is the epoll and kqueue half;
// `IocpDial.hpp`'s `Detail::SettleConnect` is the IOCP one, and it has cases of
// its own in `IocpConnector_test.cpp` since
// [#1138](https://github.com/LASTRADA-Software/fastcached/issues/1138). See the
// `#else` at the foot of this file.
//
// Named by SYMBOL and not by line. This paragraph cited `IocpConnector.cpp:116`
// for as long as #1138 was open, and by the time it was closed the `Submit` had
// moved to line 123 -- three citations in two files, all stale, none wrong when
// written.
#if defined(__linux__) || defined(__APPLE__)

    #include <FastCache/Async/IReactor.hpp>
    #include <FastCache/Async/PlatformReactor.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Clock.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/ISocket.hpp>
    #include <FastCache/Net/PlatformListener.hpp>
    #include <FastCache/Net/ReactorDial.hpp>
    #include <FastCache/Net/SocketAddress.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <algorithm>
    #include <chrono>
    #include <cstdint>
    #include <memory>
    #include <ranges>
    #include <string>
    #include <tuple>
    #include <utility>
    #include <vector>

    #include <tests/FrameSentinel.hpp>

using namespace std::chrono_literals;

namespace
{

using FastCache::Testing::FrameCounters;
using FastCache::Testing::FrameSentinel;

/// The handler `DialReadiness` drives, carrying the five members it uses.
///
/// Its own type rather than `EpollFdHandler`, so this file compiles unchanged on
/// both readiness backends -- which is the same reason `ReactorDial` is a template
/// over a traits triple in the first place.
struct RecordingDialHandler
{
    int fd { -1 };
    void* owner { nullptr };
    void (*onReadable)(RecordingDialHandler* self) { nullptr };
    void (*onWritable)(RecordingDialHandler* self) { nullptr };
    void (*onError)(RecordingDialHandler* self) { nullptr };
};

/// A reactor that runs no loop, records what it is handed, and models teardown.
///
/// **Why a double rather than the real reactor, measured rather than preferred.**
/// On epoll and kqueue `DrainPendingSubmits()` follows handler dispatch
/// unconditionally, so a descriptor-driven settle is always drained in the
/// iteration that produced it and leaves no window at all. The only entries that
/// survive an iteration are those submitted during that drain or during
/// `FireExpiredTimers` -- both on the deadline path, which is reached only if the
/// descriptor does not become ready first. On loopback it always does: a refused
/// connect answers promptly and a listening port completes inline without parking.
/// So the real loop cannot be made to hold this submission without depending on
/// how a particular host schedules a particular connect, which is a property of
/// the machine rather than of the code.
///
/// With no loop, nothing dispatches, and the case drives `Traits::Settle` itself.
/// The ordering is then not a race at all.
///
/// **It is narrower than a real reactor, never wider.** Every difference goes the
/// safe way: it runs no loop, so nothing it is handed is ever resumed; it takes no
/// locks, because a case drives it from one thread where a real `Submit` is
/// callable from any; and it answers `TeardownIsSerialisedWithDispatch()` through
/// the `!Running()` arm, which is the truth here rather than a shortcut. It accepts
/// no `Submit` shape a real reactor refuses and answers nothing inline that a real
/// one parks -- **a fake more permissive than the thing it stands for** is a family
/// this project has paid for repeatedly, and cheapness is a reason it was easy to
/// write rather than a reason it is faithful.
///
/// **What is modelled and what is real.** The `SettleDial` body under test is the
/// real one, instantiated over this triple exactly as the platform connectors
/// instantiate it over theirs. What this class stands in for is the reactor's
/// teardown: handed work is held as `Detail::Parked`, which frees an unowned chain
/// when it is destroyed and leaves a borrowed one alone -- the same contract
/// `EpollReactor::AbandonParkedWork` implements. That is not circular: what the
/// case measures is whether `SettleDial` SAYS the chain is unowned, and before
/// #1041 it could not say so at all.
class RecordingDialReactor final: public FastCache::IReactor
{
  public:
    /// @param clock Clock the dial's deadline is read from.
    explicit RecordingDialReactor(FastCache::IClock& clock) noexcept:
        _clock { clock }
    {
    }

    void Stop() noexcept override {}

    void Submit(std::coroutine_handle<> handle) override
    {
        _borrowed.push_back(handle);
    }

    void Submit(FastCache::ParkedWork work) override
    {
        // Recorded separately, because `Detail::Parked` deliberately exposes only
        // the handle it would resume -- the root it may free is not something a
        // consumer is meant to read, and this double is the one place that has to.
        _handedOwned.push_back(static_cast<bool>(work.abandon));
        _handed.emplace_back(work);
    }

    void Schedule(FastCache::TimePoint /*deadline*/, std::coroutine_handle<> handle) override
    {
        // RECORDED rather than dropped, and that is the difference between a double
        // and a more permissive stand-in: a real reactor queues this, so a double
        // that silently discarded it would make a caller that used the borrowing
        // form look correct here and leak in production.
        _borrowedTimers.push_back(handle);
    }

    void Schedule(FastCache::TimePoint /*deadline*/, FastCache::ParkedWork work) override
    {
        _scheduled.emplace_back(work);
    }

    /// Retract a pending resumption, the way every real reactor here does.
    ///
    /// Answering a blanket `false` would have been legal -- `DeadlineTimer`
    /// documents *cannot retract* as a supported reply -- and it would have made
    /// this double answer differently from every reactor it stands for, which is
    /// the shape that has bitten this project three times in a week. `Take()`
    /// before erasing, because after this the caller is the only one who may
    /// resume or destroy the chain, so the entry must do neither on its way out.
    /// @param handle A handle previously submitted or scheduled here.
    /// @return Whether it was found and retracted.
    [[nodiscard]] bool CancelPending(std::coroutine_handle<> handle) noexcept override
    {
        if (!handle)
            return false;
        for (auto* container: { &_handed, &_scheduled })
        {
            auto const found = std::ranges::find(*container, handle, &FastCache::Detail::Parked::Handle);
            if (found != container->end())
            {
                std::ignore = found->Take();
                container->erase(found);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] FastCache::IClock& Clock() noexcept override
    {
        return _clock;
    }

    /// @return Always true; there is no descriptor set to fail against.
    [[nodiscard]] bool Attach(RecordingDialHandler* handler) noexcept
    {
        _attached = handler;
        return true;
    }

    [[nodiscard]] bool UpdateInterest(RecordingDialHandler* /*handler*/, bool /*read*/, bool /*write*/) noexcept
    {
        return true;
    }

    void Detach(RecordingDialHandler* /*handler*/) noexcept {}

    /// @return The handler the dial registered, so a case can settle it.
    [[nodiscard]] RecordingDialHandler* Attached() const noexcept
    {
        return _attached;
    }

    /// @return How many chains were handed over WITH ownership.
    [[nodiscard]] std::size_t Handed() const noexcept
    {
        return _handed.size();
    }

    /// @return How many were handed over as borrowed handles.
    [[nodiscard]] std::size_t Borrowed() const noexcept
    {
        return _borrowed.size();
    }

    /// @return Whether the most recent hand-back named a chain root to free.
    [[nodiscard]] bool LastHandedCarriedOwnership() const noexcept
    {
        return !_handedOwned.empty() && _handedOwned.back();
    }

  protected:
    void RunLoop() override {}

  private:
    FastCache::IClock& _clock;
    RecordingDialHandler* _attached { nullptr };
    std::vector<std::coroutine_handle<>> _borrowed;
    std::vector<std::coroutine_handle<>> _borrowedTimers;
    std::vector<FastCache::Detail::Parked> _handed;
    std::vector<bool> _handedOwned;
    std::vector<FastCache::Detail::Parked> _scheduled;
};

/// Stands in for the connected socket the success path would build.
///
/// Never reached by these cases -- every dial here is settled as a failure -- but
/// `DialReadiness` names the type, so it has to exist and be constructible the way
/// the real ones are.
class RecordingDialSocket final: public FastCache::ISocket
{
  public:
    RecordingDialSocket(RecordingDialReactor& /*reactor*/, int /*fd*/, std::string const& /*peer*/) noexcept {}

    [[nodiscard]] FastCache::IoAwaitable Read(std::span<std::byte> /*buffer*/) override
    {
        return FastCache::IoAwaitable { FastCache::IoResult { 0 } };
    }

    [[nodiscard]] FastCache::IoAwaitable Write(std::span<std::byte const> /*buffer*/) override
    {
        return FastCache::IoAwaitable { FastCache::IoResult { 0 } };
    }

    [[nodiscard]] FastCache::IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> /*segments*/,
                                                       std::shared_ptr<void const> /*keepAlive*/) override
    {
        return FastCache::IoAwaitable { FastCache::IoResult { 0 } };
    }

    void Close() noexcept override
    {
        _closed = true;
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }

  private:
    bool _closed { false };
};

/// The triple `Detail::DialReadiness` is instantiated over here.
///
/// The same three aliases and one function the platform connectors declare; only
/// the types differ, which is the whole point of the template.
struct RecordingDialTraits
{
    using Reactor = RecordingDialReactor;
    using Handler = RecordingDialHandler;
    using Socket = RecordingDialSocket;

    static void Settle(RecordingDialHandler* self)
    {
        FastCache::Detail::SettleDial(*static_cast<FastCache::Detail::ReadinessDialOp<RecordingDialTraits>*>(self->owner),
                                      std::unexpected(FastCache::NetError { .code = FastCache::NetErrorCode::ConnRefused,
                                                                            .systemCode = 0,
                                                                            .context = "scripted refusal" }));
    }
};

/// A loopback port nothing answers on.
///
/// Bound and released rather than fixed, for the reason in the testing rules. It
/// has to be CLOSED rather than merely unaccepted: a connect to a listening
/// loopback port completes inline, which `ReactorDial` names as the ordinary case,
/// and an inline completion never parks and so never reaches the hand-back.
/// @return The port, or 0 when this host will not bind loopback.
[[nodiscard]] std::uint16_t ClosedLoopbackPort()
{
    FastCache::ManualClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        return 0;
    return listener->BoundPort();
}

/// Resolve a loopback endpoint, or an empty vector when this host will not.
[[nodiscard]] std::vector<FastCache::ResolvedEndpoint> LoopbackEndpoints(std::uint16_t port)
{
    auto resolved = FastCache::DefaultAddressResolver().Resolve("127.0.0.1", port);
    if (!resolved.has_value())
        return {};
    return resolved.value();
}

/// A dial owned by NOBODY.
FastCache::DetachedTask DialDetached(RecordingDialReactor* reactor,
                                     FastCache::ResolvedEndpoint endpoint,
                                     FastCache::TimePoint deadline,
                                     FrameSentinel sentinel,
                                     FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    auto outcome = co_await FastCache::Detail::DialReadiness<RecordingDialTraits>(
        reactor, endpoint, deadline, FastCache::KeepAlive::No);
    (void) outcome;
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// The same shape, owned by the `Task` the caller holds. The control.
FastCache::Task<void> DialOwned(RecordingDialReactor* reactor,
                                FastCache::ResolvedEndpoint endpoint,
                                FastCache::TimePoint deadline,
                                FrameSentinel sentinel,
                                FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    auto outcome = co_await FastCache::Detail::DialReadiness<RecordingDialTraits>(
        reactor, endpoint, deadline, FastCache::KeepAlive::No);
    (void) outcome;
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

} // namespace

TEST_CASE("A detached dial handed back and never dequeued is freed at teardown", "[net][connect][parkedwork]")
{
    auto const port = ClosedLoopbackPort();
    REQUIRE(port != 0);
    auto const endpoints = LoopbackEndpoints(port);
    REQUIRE_FALSE(endpoints.empty());

    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        RecordingDialReactor reactor { clock };

        DialDetached(&reactor, endpoints.front(), clock.Now() + 30s, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);

        // The premise, asserted rather than assumed: the connect went to
        // EINPROGRESS and the dial parked, so there is a waiter to hand back.
        REQUIRE(reactor.Attached() != nullptr);
        REQUIRE(counters.completed == 0);

        // Measured as a DELTA across the settle rather than as a total: arming the
        // dial puts `DeadlineTimer`'s own coroutine on this reactor too, so a count
        // would be answering about two things and reporting one.
        auto const handedBefore = reactor.Handed();
        auto const borrowedBefore = reactor.Borrowed();

        // The site under test. Nothing dispatches here, so this IS the settle
        // rather than a race with one.
        RecordingDialTraits::Settle(reactor.Attached());

        // The positive control: exactly one hand-back happened. A case whose
        // finding is that a frame was NOT freed has to show first that the site it
        // is about was reached at all.
        REQUIRE((reactor.Handed() - handedBefore) + (reactor.Borrowed() - borrowedBefore) == 1);

        // What DISTINGUISHES. Before #1041 the dial reached `Submit` just the same
        // and the line above still passed -- the chain went over as a BORROWED
        // handle, because `IReactor` hid the overload that could say otherwise.
        CHECK(reactor.Borrowed() - borrowedBefore == 0);
        CHECK(reactor.Handed() - handedBefore == 1);
        CHECK(reactor.LastHandedCarriedOwnership());
        REQUIRE(counters.completed == 0);

        // The reactor is destroyed WITHOUT dequeuing, exactly as a stopped one is.
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("A dial some Task owns is left alone by the reactor", "[net][connect][parkedwork]")
{
    // The control, and it is mandatory rather than decoration: freeing everything
    // at teardown is as wrong as freeing nothing, and this is the case a reactor
    // that freed what it merely borrows would double free in. It must stay GREEN
    // under the change that makes the case above pass.
    auto const port = ClosedLoopbackPort();
    REQUIRE(port != 0);
    auto const endpoints = LoopbackEndpoints(port);
    REQUIRE_FALSE(endpoints.empty());

    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        RecordingDialReactor reactor { clock };

        auto dial = DialOwned(&reactor, endpoints.front(), clock.Now() + 30s, FrameSentinel { &counters }, &counters);
        dial.Native().resume();
        REQUIRE(counters.parked == 1);
        REQUIRE(reactor.Attached() != nullptr);

        auto const handedBefore = reactor.Handed();
        auto const borrowedBefore = reactor.Borrowed();

        RecordingDialTraits::Settle(reactor.Attached());

        // The same one hand-back. What this case then asserts is BEHAVIOUR -- the
        // frame outlives the reactor -- rather than the shape of what was handed
        // over, and that is a correction rather than a preference.
        //
        // **A control that fails under the break is not a control.** This asserted
        // `LastHandedCarriedOwnership()` was false, which went red BEFORE the fix:
        // with the dial still using the borrowing overload, the most recent handed
        // entry was `DeadlineTimer`'s own coroutine rather than this dial's, so the
        // assertion was reading a different submission and saying nothing about the
        // one under test. Harder to catch than a control that cannot fire, because
        // a red control reads as the instrument working.
        //
        // What replaced it is also the better assertion on its own merits: an
        // over-eager fix that freed what it merely borrows would free this frame at
        // the reactor's teardown, and that is what the next line sees.
        REQUIRE((reactor.Handed() - handedBefore) + (reactor.Borrowed() - borrowedBefore) == 1);
        CHECK(counters.destroyed == 0);
    }
    // Freed exactly once, by the Task that owns it.
    CHECK(counters.destroyed == 1);
}

#else // no readiness dial on this platform

// `ReactorDial.hpp` is epoll and kqueue only, so this translation unit is empty on
// Windows -- and that is stated rather than left as an accident of the `#if`.
//
// **The IOCP `Submit` is covered, and it is covered somewhere else.**
// `Detail::SettleConnect` in `Net/IocpDial.hpp` has its own red-under-mutation
// pair in `IocpConnector_test.cpp`
// ([#1138](https://github.com/LASTRADA-Software/fastcached/issues/1138)). Until
// then it was covered by an ARGUMENT and this comment said so: the compile-time
// evidence that the site bound to the borrowing overload before #1041, plus the
// propagation measured on the POSIX side. Both still hold and neither is a
// measurement of the site, which is why the ticket stayed open.
//
// What made it demonstrable was moving the op, the awaitable and the hand-back out
// of an anonymous namespace and splitting the settle away from the completion
// callback -- so the thing left in the `.cpp` decides only WHAT happened, exactly
// as the readiness callbacks above do. **That is a change to production structure
// for a test's benefit and it is stated rather than slid in.** The alternative
// considered and rejected was driving `IocpReactor` directly with a
// `KeyResumeCoroutine` and a `KeyStop` in a controlled order: that measures the
// reactor's side-table reconciliation, which is a different property.

#endif // __linux__ || __APPLE__
