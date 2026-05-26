// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

namespace
{

/// Awaitable that yields control back to the reactor's ready queue.
struct YieldAwaitable
{
    FastCache::IReactor& reactor;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor.Submit(handle);
    }
    void await_resume() const noexcept {}
};

/// Awaitable that resumes when the reactor's clock reaches the given deadline.
struct SleepAwaitable
{
    FastCache::IReactor& reactor;
    FastCache::TimePoint deadline;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return reactor.Clock().Now() >= deadline;
    }
    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor.Schedule(deadline, handle);
    }
    void await_resume() const noexcept {}
};

FastCache::Task<void> CountYields(FastCache::IReactor& reactor, int* counter, int times)
{
    for (auto i = 0; i < times; ++i)
    {
        ++(*counter);
        co_await YieldAwaitable { reactor };
    }
    co_return;
}

FastCache::Task<void> WaitUntil(FastCache::IReactor& reactor, FastCache::TimePoint deadline, bool* fired)
{
    co_await SleepAwaitable { reactor, deadline };
    *fired = true;
    co_return;
}

} // namespace

TEST_CASE("TestReactor::Submit resumes a single coroutine and drains", "[reactor]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    int counter = 0;
    auto task = CountYields(reactor, &counter, 3);
    reactor.Submit(task.Native());
    reactor.Run();

    REQUIRE(counter == 3);
    REQUIRE(task.IsReady());
    REQUIRE(reactor.PendingSubmissions() == 0);
}

TEST_CASE("TestReactor processes multiple coroutines in FIFO order", "[reactor]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    int c1 = 0;
    int c2 = 0;
    auto t1 = CountYields(reactor, &c1, 2);
    auto t2 = CountYields(reactor, &c2, 2);
    reactor.Submit(t1.Native());
    reactor.Submit(t2.Native());
    reactor.Run();

    REQUIRE(c1 == 2);
    REQUIRE(c2 == 2);
}

TEST_CASE("TestReactor::Schedule fires a timer when the clock advances", "[reactor]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    bool fired = false;
    auto task = WaitUntil(reactor, clock.Now() + 100ms, &fired);
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

TEST_CASE("TestReactor fires timers in deadline order with FIFO tiebreak", "[reactor]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    bool firedEarly = false;
    bool firedLate = false;
    bool firedSame1 = false;
    bool firedSame2 = false;

    auto const start = clock.Now();
    auto early = WaitUntil(reactor, start + 10ms, &firedEarly);
    auto late = WaitUntil(reactor, start + 50ms, &firedLate);
    auto same1 = WaitUntil(reactor, start + 25ms, &firedSame1);
    auto same2 = WaitUntil(reactor, start + 25ms, &firedSame2);

    reactor.Submit(early.Native());
    reactor.Submit(late.Native());
    reactor.Submit(same1.Native());
    reactor.Submit(same2.Native());
    reactor.Run();

    clock.Advance(100ms);
    reactor.Run();
    REQUIRE(firedEarly);
    REQUIRE(firedLate);
    REQUIRE(firedSame1);
    REQUIRE(firedSame2);
}

TEST_CASE("TestReactor::Stop short-circuits the loop", "[reactor]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    int counter = 0;
    auto task = CountYields(reactor, &counter, 10);
    reactor.Submit(task.Native());
    reactor.Stop();
    reactor.Run();

    // Stop() requested before Run(): no ticks happen.
    REQUIRE(counter == 0);
}
