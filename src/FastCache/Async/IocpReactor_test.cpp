// SPDX-License-Identifier: Apache-2.0
#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Clock.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <atomic>
    #include <chrono>
    #include <coroutine>
    #include <thread>
    #include <vector>

namespace
{

/// Awaitable that yields back to the reactor's ready queue (Submit).
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

/// Awaitable that resumes when the reactor's clock reaches `deadline`.
struct SleepUntil
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

FastCache::DetachedTask Worker(FastCache::IReactor& reactor, std::atomic<int>& counter, int yields)
{
    for (auto i = 0; i < yields; ++i)
    {
        counter.fetch_add(1, std::memory_order_relaxed);
        co_await YieldAwaitable { reactor };
    }
    co_return;
}

FastCache::DetachedTask TimerWorker(FastCache::IReactor& reactor,
                                    FastCache::TimePoint deadline,
                                    std::atomic<bool>& fired,
                                    FastCache::IReactor* stopReactor)
{
    co_await SleepUntil { reactor, deadline };
    fired.store(true, std::memory_order_release);
    if (stopReactor)
        stopReactor->Stop();
    co_return;
}

} // namespace

TEST_CASE("IocpReactor::Submit resumes a coroutine on the reactor thread", "[reactor][iocp]")
{
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };

    std::atomic<int> counter { 0 };
    Worker(reactor, counter, 3);

    // The worker is now suspended on its first YieldAwaitable. Tell the
    // reactor to stop once the counter has reached 3 by polling on a
    // separate thread.
    std::jthread stopper { [&reactor, &counter] {
        while (counter.load(std::memory_order_relaxed) < 3)
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        reactor.Stop();
    } };

    reactor.Run();
    REQUIRE(counter.load(std::memory_order_relaxed) == 3);
}

TEST_CASE("IocpReactor drains from several threads and one Stop() joins them all", "[reactor][iocp]")
{
    // Regression for the connection-concurrency ceiling: the persistent
    // server now drains the reactor from a pool of threads. This proves the
    // pool model works — N threads pull from one port concurrently — and that
    // a single Stop() chains a wake-up through every thread (a missing chain
    // would hang join() below and the test would deadlock).
    constexpr unsigned ThreadCount = 4;
    constexpr int WorkerCount = 8;
    constexpr int YieldsPerWorker = 50;

    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock, ThreadCount };

    std::atomic<int> counter { 0 };
    for (auto i = 0; i < WorkerCount; ++i)
        Worker(reactor, counter, YieldsPerWorker);

    std::jthread stopper { [&reactor, &counter] {
        while (counter.load(std::memory_order_relaxed) < WorkerCount * YieldsPerWorker)
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        reactor.Stop();
    } };

    std::vector<std::jthread> threads;
    for (auto i = 1U; i < ThreadCount; ++i)
        threads.emplace_back([&reactor] { reactor.Run(); });
    reactor.Run();   // this thread is the fourth drainer
    threads.clear(); // joins; deadlocks if Stop() failed to wake every thread

    REQUIRE(counter.load(std::memory_order_relaxed) == WorkerCount * YieldsPerWorker);
}

TEST_CASE("IocpReactor::Schedule fires a timer", "[reactor][iocp]")
{
    using namespace std::chrono_literals;

    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };

    std::atomic<bool> fired { false };
    TimerWorker(reactor, clock.Now() + 25ms, fired, &reactor);
    reactor.Run();
    REQUIRE(fired.load(std::memory_order_acquire));
}

#endif // _WIN32
