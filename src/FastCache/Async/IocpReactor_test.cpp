// SPDX-License-Identifier: Apache-2.0
#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/SleepUntil.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Clock.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <atomic>
    #include <chrono>
    #include <coroutine>
    #include <thread>

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
    co_await FastCache::SleepUntil { .reactor = &reactor, .deadline = deadline };
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
