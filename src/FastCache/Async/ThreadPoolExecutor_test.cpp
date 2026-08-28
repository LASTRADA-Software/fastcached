// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// A latch a case can hold work on, so "these ran at the same time" is a decision
/// rather than a race with the scheduler.
class Latch
{
  public:
    void Wait()
    {
        auto guard = std::unique_lock { _mutex };
        // Bounded, and it says what it waited for: an unbounded wait in a test that
        // regresses is a suite that hangs rather than one that fails.
        (void) _open.wait_for(guard, 5s, [this] { return _released; });
    }

    void Release()
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _released = true;
        }
        _open.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _open;
    bool _released { false };
};

/// Counts arrivals and lets a case wait for a given number of them.
class Arrivals
{
  public:
    void Arrive()
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            ++_count;
        }
        _changed.notify_all();
    }

    [[nodiscard]] bool WaitFor(std::size_t many)
    {
        auto guard = std::unique_lock { _mutex };
        return _changed.wait_for(guard, 5s, [this, many] { return _count >= many; });
    }

    [[nodiscard]] std::size_t Count() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _count;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _count { 0 };
};

/// What one job does after it lands on the pool.
///
/// POINTERS and values, no references and no captures. A coroutine's reference
/// parameters are not kept alive by its frame, and a capturing lambda coroutine is
/// worse: the closure object dies at the end of the full expression that made it,
/// while the frame goes on pointing into it. Both compile, both pass while the
/// stack happens to still hold the values, and both are use-after-free.
struct Job
{
    ThreadPoolExecutor* pool;             ///< Where to run.
    Arrivals* arrived { nullptr };        ///< Counted on arrival, when given.
    Latch* hold { nullptr };              ///< Blocked on, when given.
    std::atomic<bool>* moved { nullptr }; ///< Set to "not the caller's thread".
    std::thread::id caller {};            ///< Which thread asked.
};

/// Hop onto the pool and do what the job says.
/// @param job What to do; every target must outlive the pool it runs on.
DetachedTask RunOn(Job job)
{
    co_await ResumeOn { *job.pool };
    if (job.moved != nullptr)
        job.moved->store(std::this_thread::get_id() != job.caller, std::memory_order_release);
    if (job.arrived != nullptr)
        job.arrived->Arrive();
    if (job.hold != nullptr)
        job.hold->Wait();
    co_return;
}

} // namespace

TEST_CASE("Work awaited onto a pool runs on its threads, not the caller's", "[async][threadpool]")
{
    std::atomic<bool> elsewhere { false };
    Arrivals done;
    // Declared LAST so it is joined FIRST: everything a job touches has to outlive
    // the pool, and locals are destroyed in reverse.
    ThreadPoolExecutor pool { 2 };

    RunOn(Job { .pool = &pool, .arrived = &done, .moved = &elsewhere, .caller = std::this_thread::get_id() });

    REQUIRE(done.WaitFor(1));
    CHECK(elsewhere.load(std::memory_order_acquire));
}

TEST_CASE("A pool of N runs N pieces of blocking work at once", "[async][threadpool]")
{
    // The whole reason a pool exists here rather than a reactor: this work BLOCKS,
    // and on one thread the second piece could not start until the first finished.
    constexpr std::size_t Threads = 3;

    Latch hold;
    Arrivals started;
    ThreadPoolExecutor pool { Threads };

    for ([[maybe_unused]] auto const index: { 0, 1, 2 })
        RunOn(Job { .pool = &pool, .arrived = &started, .hold = &hold });

    // All three are inside the blocking section together, which is the claim.
    CHECK(started.WaitFor(Threads));
    hold.Release();
}

TEST_CASE("A pool never abandons a coroutine it was handed", "[async][threadpool]")
{
    // An unresumed coroutine never runs its destructors and never frees its frame,
    // so a queue dropped at shutdown leaks every job in it along with whatever it
    // holds. Both routes out are checked: drained at stop, and resumed inline when
    // the pool is already stopped.
    Arrivals ran;

    SECTION("queued when it stops")
    {
        {
            ThreadPoolExecutor pool { 1 };
            for ([[maybe_unused]] auto const index: { 0, 1, 2, 3 })
                RunOn(Job { .pool = &pool, .arrived = &ran });
        } // joins, draining what is left
        CHECK(ran.Count() == 4);
    }

    SECTION("submitted after it stopped")
    {
        ThreadPoolExecutor pool { 1 };
        pool.Stop();
        RunOn(Job { .pool = &pool, .arrived = &ran });
        // Resumed on this thread rather than dropped, so it still completed.
        CHECK(ran.Count() == 1);
    }
}

TEST_CASE("A pool asked for no threads still runs its work", "[async][threadpool]")
{
    // Zero would be a pool that accepts handles and resumes none of them, which is
    // a leak wearing the shape of an idle pool.
    Arrivals ran;
    ThreadPoolExecutor pool { 0 };
    CHECK(pool.Threads() == 1);

    RunOn(Job { .pool = &pool, .arrived = &ran });
    CHECK(ran.WaitFor(1));
}
