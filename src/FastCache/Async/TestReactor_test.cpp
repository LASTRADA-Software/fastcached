// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

/// Awaitable that yields control back to the reactor's ready queue.
struct YieldAwaitable
{
    FastCache::IReactor* reactor { nullptr };

    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor->Submit(handle);
    }
    void await_resume() const noexcept {}
};

/// Awaitable that resumes when the reactor's clock reaches the given deadline.
struct SleepAwaitable
{
    FastCache::IReactor* reactor { nullptr };
    FastCache::TimePoint deadline {};

    [[nodiscard]] bool await_ready() const noexcept
    {
        return reactor->Clock().Now() >= deadline;
    }
    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor->Schedule(deadline, handle);
    }
    void await_resume() const noexcept {}
};

FastCache::Task<void> CountYields(FastCache::IReactor* reactor, int* counter, int times)
{
    for (auto i = 0; i < times; ++i)
    {
        ++(*counter);
        co_await YieldAwaitable { reactor };
    }
    co_return;
}

FastCache::Task<void> WaitUntil(FastCache::IReactor* reactor, FastCache::TimePoint deadline, bool* fired)
{
    co_await SleepAwaitable { .reactor = reactor, .deadline = deadline };
    *fired = true;
    co_return;
}

/// A task that does nothing but record that it ran. Lazy, so handing its handle
/// to the reactor from a producer thread is the only thing that advances it --
/// which is exactly the crossing under test.
FastCache::Task<void> Increment(std::atomic<int>* resumed)
{
    resumed->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

} // namespace

TEST_CASE("TestReactor::Submit resumes a single coroutine and drains", "[reactor]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    int counter = 0;
    auto task = CountYields(&reactor, &counter, 3);
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
    auto t1 = CountYields(&reactor, &c1, 2);
    auto t2 = CountYields(&reactor, &c2, 2);
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
    auto task = WaitUntil(&reactor, clock.Now() + 100ms, &fired);
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
    auto early = WaitUntil(&reactor, start + 10ms, &firedEarly);
    auto late = WaitUntil(&reactor, start + 50ms, &firedLate);
    auto same1 = WaitUntil(&reactor, start + 25ms, &firedSame1);
    auto same2 = WaitUntil(&reactor, start + 25ms, &firedSame2);

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
    auto task = CountYields(&reactor, &counter, 10);
    reactor.Submit(task.Native());
    reactor.Stop();
    reactor.Run();

    // Stop() requested before Run(): no ticks happen.
    REQUIRE(counter == 0);
}

TEST_CASE("TestReactor accepts Submit and Schedule from many threads", "[reactor]")
{
    // `IReactor` documents both as safe to call from any thread, and this double
    // did not honour that -- it touched a bare deque and a bare vector. Nothing
    // noticed while every producer was the test's own thread. The primitives
    // built on top of this reactor (a resolver handing a result back from a
    // worker pool, a queue pushed by a producer thread) cross threads by
    // definition, so a double that cannot be used that way forces every one of
    // those cases onto a real platform reactor, where nothing is deterministic.
    //
    // Run under TSan this fails outright before the mutex; without a sanitizer it
    // corrupts the containers and shows up as a crash inside Tick() naming
    // nothing. The barrier maximises the overlap so the unguarded version does
    // not get away with it.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    constexpr std::size_t Producers = 8;
    constexpr std::size_t PerProducer = 64;

    std::atomic<int> resumed { 0 };
    std::vector<FastCache::Task<void>> tasks;
    tasks.reserve(Producers * PerProducer);
    for (std::size_t i = 0; i < Producers * PerProducer; ++i)
        tasks.push_back(Increment(&resumed));

    std::barrier start { static_cast<std::ptrdiff_t>(Producers) };
    std::vector<std::jthread> threads;
    threads.reserve(Producers);
    for (std::size_t p = 0; p < Producers; ++p)
    {
        threads.emplace_back([&, p] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < PerProducer; ++i)
            {
                auto& task = tasks[(p * PerProducer) + i];
                // Half through the ready queue and half through the timer heap,
                // because they are two containers and only one of them being
                // guarded would still pass a test that used either alone.
                if ((i % 2) == 0)
                    reactor.Submit(task.Native());
                else
                    reactor.Schedule(clock.Now(), task.Native());
            }
        });
    }
    threads.clear(); // join every producer

    REQUIRE(reactor.PendingSubmissions() + reactor.PendingTimers() == Producers * PerProducer);
    reactor.Drain();
    REQUIRE(resumed.load() == static_cast<int>(Producers * PerProducer));
    REQUIRE(reactor.PendingSubmissions() == 0);
    REQUIRE(reactor.PendingTimers() == 0);
}
