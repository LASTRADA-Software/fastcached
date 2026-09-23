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
#include <ranges>
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
        reactor->submit(handle);
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
        return reactor->clock().now() >= deadline;
    }
    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor->schedule(deadline, handle);
    }
    void await_resume() const noexcept {}
};

FastCache::Task<void> CountYields(FastCache::IReactor* reactor, int* counter, int times)
{
    for ([[maybe_unused]] auto const i: std::views::iota(0, times))
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
    reactor.submit(task.handle());
    reactor.run();

    REQUIRE(counter == 3);
    REQUIRE(task.done());
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
    reactor.submit(t1.handle());
    reactor.submit(t2.handle());
    reactor.run();

    REQUIRE(c1 == 2);
    REQUIRE(c2 == 2);
}

TEST_CASE("TestReactor::Schedule fires a timer when the clock advances", "[reactor]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    bool fired = false;
    auto task = WaitUntil(&reactor, clock.now() + 100ms, &fired);
    reactor.submit(task.handle());

    reactor.run();
    REQUIRE_FALSE(fired);
    REQUIRE(reactor.PendingTimers() == 1);

    clock.advance(99ms);
    reactor.run();
    REQUIRE_FALSE(fired);

    clock.advance(1ms);
    reactor.run();
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

    auto const start = clock.now();
    auto early = WaitUntil(&reactor, start + 10ms, &firedEarly);
    auto late = WaitUntil(&reactor, start + 50ms, &firedLate);
    auto same1 = WaitUntil(&reactor, start + 25ms, &firedSame1);
    auto same2 = WaitUntil(&reactor, start + 25ms, &firedSame2);

    reactor.submit(early.handle());
    reactor.submit(late.handle());
    reactor.submit(same1.handle());
    reactor.submit(same2.handle());
    reactor.run();

    clock.advance(100ms);
    reactor.run();
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
    reactor.submit(task.handle());
    reactor.stop();
    reactor.run();

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
    for ([[maybe_unused]] auto const i: std::views::iota(std::size_t { 0 }, Producers * PerProducer))
        tasks.push_back(Increment(&resumed));

    std::barrier start { static_cast<std::ptrdiff_t>(Producers) };
    std::vector<std::jthread> threads;
    threads.reserve(Producers);
    for (auto const p: std::views::iota(std::size_t { 0 }, Producers))
    {
        threads.emplace_back([&, p] {
            start.arrive_and_wait();
            for (auto const i: std::views::iota(std::size_t { 0 }, PerProducer))
            {
                auto& task = tasks[(p * PerProducer) + i];
                // Half through the ready queue and half through the timer heap,
                // because they are two containers and only one of them being
                // guarded would still pass a test that used either alone.
                if ((i % 2) == 0)
                    reactor.submit(task.handle());
                else
                    reactor.schedule(clock.now(), task.handle());
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
