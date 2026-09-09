// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/PlatformConnector.hpp>
#include <FastCache/Net/PlatformListener.hpp>
#include <FastCache/Net/ThreadedAddressResolver.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

#include <tests/FrameSentinel.hpp>
#include <tests/Unwrap.hpp>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// #1041: what a settled dial owes a chain nobody dequeues.
//
// The dial hands its waiter back with `reactor->Submit(waiter)` -- a BORROWED
// handle, erased in `await_suspend` before either connector sees it -- so a
// reactor destroyed before that post is dequeued frees nothing. Correct for a
// dial some `Task` owns, a leak for a detached one.
//
// **Written against `PlatformConnector` rather than one backend, and that is what
// makes it cover two of the seven sites**: it is `EpollConnector` on Linux and
// `KqueueConnector` on macOS, both of which reach `ReactorDial.hpp`'s `SettleDial`,
// and `IocpConnector` on Windows, which has a `Submit` of its own in
// `IocpConnector.cpp`. So one case answers for two sites, ONE PER PLATFORM --
// **neither platform sees both halves**, and a green Linux run says nothing about
// the Windows line.
//
// **The arrangement is the case.** A running reactor drains its own submissions:
// on epoll and kqueue an fd-driven settle is followed by `DrainPendingSubmits()`
// in the same iteration, and on IOCP a completion is processed inside the batch.
// The one window all three share is a timer that fires AFTER the last drain of a
// run, so this drives the deadline rather than the descriptor:
//
//   * a starter coroutine runs ON the reactor thread, begins the dial (which parks
//     with `EINPROGRESS` / `WSA_IO_PENDING`), advances the manual clock past the
//     dial's deadline, and stops the reactor;
//   * every loop here then fires expired timers as the last thing it does --
//     `DrainPendingSubmits(); FireExpiredTimers();` on epoll and kqueue, and
//     `FireExpiredTimers()` after the batch on IOCP, whose `stopRequested` is false
//     because `Stop()`'s packet was posted during the batch it would have to be in;
//   * `SettleDial` posts the waiter, and the loop's next test of `_stopped` ends the
//     run with that post still queued.
//
// No wall clock and no sleep: the only ordering relied on is the loop's own, and it
// is the same shape on all three backends.
namespace
{

using FastCache::Testing::FrameCounters;
using FastCache::Testing::FrameSentinel;

/// A loopback port nothing answers on.
///
/// Bound and released rather than fixed, for the reason in the testing rules: a
/// hard-coded port collides with whatever else the machine is doing. Releasing it
/// is safe here because the reactor has never run, so
/// `TeardownIsSerialisedWithDispatch()` holds by its `!Running()` arm.
///
/// It has to be CLOSED rather than merely unaccepted: a connect to a listening
/// loopback port completes inline, which is the ordinary case `ReactorDial` names,
/// and an inline completion never parks and so never reaches the hand-back.
/// @param reactor Reactor to bind the throwaway listener on.
/// @return The port, or 0 when this host will not bind loopback.
[[nodiscard]] std::uint16_t ClosedLoopbackPort(FastCache::PlatformReactor& reactor)
{
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        return 0;
    return listener->BoundPort();
}

/// A dial owned by NOBODY.
///
/// It stops the reactor when it resumes, which matters only in the case where it
/// DOES resume -- that is the positive control, and this is how its run ends.
FastCache::DetachedTask DialDetached(FastCache::IConnector* connector,
                                     std::uint16_t port,
                                     FastCache::IReactor* reactor,
                                     std::optional<FastCache::SocketResult>* out,
                                     FrameSentinel sentinel,
                                     FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    *out = co_await connector->Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = 1ms });
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    reactor->Stop();
    co_return;
}

/// The same shape, owned by the `Task` the caller holds. The control.
FastCache::Task<void> DialOwned(FastCache::IConnector* connector,
                                std::uint16_t port,
                                FastCache::IReactor* reactor,
                                std::optional<FastCache::SocketResult>* out,
                                FrameSentinel sentinel,
                                FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    *out = co_await connector->Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = 1ms });
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    reactor->Stop();
    co_return;
}

/// Begin a detached dial on the reactor thread, then expire its deadline.
///
/// @param stop Whether to end the run here, which is the whole difference between
///        the leaking case and the control it is measured against.
FastCache::Task<void> StartDialThenExpire(FastCache::IConnector* connector,
                                          std::uint16_t port,
                                          FastCache::PlatformReactor* reactor,
                                          FastCache::ManualClock* clock,
                                          std::optional<FastCache::SocketResult>* out,
                                          bool stop,
                                          FrameCounters* counters)
{
    DialDetached(connector, port, reactor, out, FrameSentinel { counters }, counters);
    clock->Advance(1s);
    if (stop)
        reactor->Stop();
    co_return;
}

/// Expire every deadline and end the run. The owned case's other half.
FastCache::Task<void> ExpireThenStop(FastCache::PlatformReactor* reactor, FastCache::ManualClock* clock)
{
    clock->Advance(1s);
    reactor->Stop();
    co_return;
}

} // namespace

TEST_CASE("A detached dial handed back and never dequeued is freed at teardown", "[net][connect][parkedwork]")
{
    FrameCounters counters;
    std::optional<FastCache::SocketResult> out;
    {
        FastCache::ManualClock clock;
        FastCache::PlatformReactor reactor { clock };
        auto const port = ClosedLoopbackPort(reactor);
        REQUIRE(port != 0);

        FastCache::ThreadedAddressResolver resolver;
        FastCache::PlatformConnector connector { reactor, resolver, clock };

        auto starter = StartDialThenExpire(&connector, port, &reactor, &clock, &out, /*stop*/ true, &counters);
        reactor.Submit(starter.Native());
        reactor.Run();

        // The premise, asserted rather than assumed: the dial parked and the run
        // ended without resuming it, so the hand-back is still in the reactor's
        // queue. Its sibling case below is what shows that hand-back happened at
        // all -- this one cannot, because observing it would mean draining it.
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.completed == 0);
        REQUIRE_FALSE(out.has_value());

        // Destroyed here, and the ORDER is deliberate: the connector and the
        // resolver go first, which is safe because a dial frame's destructor
        // touches neither -- it closes its descriptor, retires its deadline through
        // the reactor, and destroys plain values.
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("A detached dial that times out frees its own frame", "[net][connect][parkedwork]")
{
    // The positive control for the case above, and the reason it is not decoration:
    // that case's finding is that a frame was NOT freed, which is exactly what a run
    // that never reached the hand-back would also produce. This is the same
    // arrangement with the run left alive, so the settle's `Submit` is dequeued, the
    // dial resumes with its timeout, and the chain frees itself. If the port were
    // answering, or the deadline never fired, this case fails and says so.
    FrameCounters counters;
    std::optional<FastCache::SocketResult> out;
    {
        FastCache::ManualClock clock;
        FastCache::PlatformReactor reactor { clock };
        auto const port = ClosedLoopbackPort(reactor);
        REQUIRE(port != 0);

        FastCache::ThreadedAddressResolver resolver;
        FastCache::PlatformConnector connector { reactor, resolver, clock };

        auto starter = StartDialThenExpire(&connector, port, &reactor, &clock, &out, /*stop*/ false, &counters);
        reactor.Submit(starter.Native());
        reactor.Run();

        REQUIRE(counters.parked == 1);
        CHECK(counters.completed == 1);
        REQUIRE(out.has_value());
        // A failure rather than a socket: an inline completion would mean the port
        // is answering and the arrangement above never parks.
        CHECK_FALSE(FastCache::Testing::Unwrap(out).has_value());
        CHECK(counters.destroyed == 1);
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("A dial some Task owns is left alone by the reactor", "[net][connect][parkedwork]")
{
    // The control that must stay GREEN when the ownership rule changes: freeing
    // everything at teardown is as wrong as freeing nothing, and this is the case a
    // reactor that freed what it merely borrows would double free in.
    FrameCounters counters;
    std::optional<FastCache::SocketResult> out;
    {
        FastCache::ManualClock clock;
        FastCache::PlatformReactor reactor { clock };
        auto const port = ClosedLoopbackPort(reactor);
        REQUIRE(port != 0);

        FastCache::ThreadedAddressResolver resolver;
        FastCache::PlatformConnector connector { reactor, resolver, clock };

        auto dial = DialOwned(&connector, port, &reactor, &out, FrameSentinel { &counters }, &counters);
        auto stopper = ExpireThenStop(&reactor, &clock);
        // Submitted in this order and drained in it, so the dial is parked with its
        // deadline armed before the clock moves.
        reactor.Submit(dial.Native());
        reactor.Submit(stopper.Native());
        reactor.Run();

        REQUIRE(counters.parked == 1);
        REQUIRE(counters.completed == 0);
        REQUIRE_FALSE(out.has_value());
        // Reactor destroyed with the post undrained, exactly as in the first case --
        // but this frame has an owner, so it must survive until `dial` goes out of
        // scope.
        CHECK(counters.destroyed == 0);
    }
    // Freed exactly once, by the Task that owns it.
    CHECK(counters.destroyed == 1);
}
