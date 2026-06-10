// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

namespace
{

/// Coroutine that sleeps to an absolute deadline, then records that it fired.
/// @param reactor  Reactor whose clock gates the deadline (or nullptr to resolve inline).
/// @param deadline Absolute instant to wait for.
/// @param fired    Set to true once the wait resolves.
FastCache::Task<void> SleepThenFire(FastCache::IReactor* reactor, FastCache::TimePoint deadline, bool* fired)
{
    co_await FastCache::SleepUntil { .reactor = reactor, .deadline = deadline };
    *fired = true;
    co_return;
}

/// Coroutine that sleeps for a relative delay via SleepFor, then records firing.
/// @param reactor Reactor whose clock anchors and gates the delay.
/// @param delay   Relative duration to wait from the reactor's current clock.
/// @param fired   Set to true once the wait resolves.
FastCache::Task<void> SleepForThenFire(FastCache::IReactor* reactor, FastCache::Duration delay, bool* fired)
{
    co_await FastCache::SleepFor(*reactor, delay);
    *fired = true;
    co_return;
}

} // namespace

TEST_CASE("SleepUntil: a null reactor resolves immediately without suspending", "[reactor][sleep]")
{
    FastCache::SleepUntil const awaitable { .reactor = nullptr, .deadline = FastCache::TimePoint {} + 100ms };
    REQUIRE(awaitable.await_ready());

    bool fired = false;
    auto task = SleepThenFire(nullptr, FastCache::TimePoint {} + 100ms, &fired);
    FastCache::SyncRun(std::move(task));
    REQUIRE(fired);
}

TEST_CASE("SleepUntil: an already-elapsed deadline resolves immediately", "[reactor][sleep]")
{
    FastCache::ManualClock clock { FastCache::TimePoint {} + 1s };
    FastCache::TestReactor reactor { clock };

    FastCache::SleepUntil const past { .reactor = &reactor, .deadline = clock.Now() - 1ms };
    REQUIRE(past.await_ready());

    FastCache::SleepUntil const exact { .reactor = &reactor, .deadline = clock.Now() };
    REQUIRE(exact.await_ready());
}

TEST_CASE("SleepUntil: suspends until the clock reaches the deadline", "[reactor][sleep]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    bool fired = false;
    auto task = SleepThenFire(&reactor, clock.Now() + 100ms, &fired);
    reactor.Submit(task.Native());

    reactor.Run();
    REQUIRE_FALSE(fired);
    REQUIRE(reactor.PendingTimers() == 1);

    clock.Advance(99ms);
    reactor.Run();
    REQUIRE_FALSE(fired);

    clock.Advance(1ms);
    reactor.Run();
    REQUIRE(fired);
    REQUIRE(reactor.PendingTimers() == 0);
}

TEST_CASE("SleepFor: computes the deadline relative to the reactor's clock", "[reactor][sleep]")
{
    FastCache::ManualClock clock { FastCache::TimePoint {} + 1s };
    FastCache::TestReactor reactor { clock };

    auto const awaitable = FastCache::SleepFor(reactor, 50ms);
    REQUIRE(awaitable.reactor == &reactor);
    REQUIRE(awaitable.deadline == clock.Now() + 50ms);
    REQUIRE_FALSE(awaitable.await_ready());

    bool fired = false;
    auto task = SleepForThenFire(&reactor, 50ms, &fired);
    reactor.Submit(task.Native());

    reactor.Run();
    REQUIRE_FALSE(fired);

    clock.Advance(50ms);
    reactor.Run();
    REQUIRE(fired);
}
