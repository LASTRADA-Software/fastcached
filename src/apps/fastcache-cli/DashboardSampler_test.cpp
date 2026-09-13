// SPDX-License-Identifier: Apache-2.0
#include "DashboardSampler.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <optional>
#include <thread>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::Unwrap;

namespace
{

/// A gatherer that records which thread asked it and blocks while it does.
///
/// It blocks on purpose. A gatherer that returned instantly would let a broken
/// implementation -- one that never left the reactor -- finish before anything could
/// observe the difference, so the case would pass for the wrong reason.
class RecordingGatherer final: public IStatsGatherer
{
  public:
    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        calledOn = std::this_thread::get_id();
        ++calls;
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        return { StatsAttempt { .origin = StatsOrigin::Info, .asked = true, .record = std::nullopt, .note = "probe" } };
    }

    std::thread::id calledOn {};
    std::size_t calls { 0 };
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
};

/// Take one reading, writing the result where the caller can read it.
///
/// Pointers and a named coroutine, for the reason the production signatures use
/// them: a lambda coroutine loses its closure at the first suspension, and this one
/// suspends immediately.
/// @param gatherer The ladder.
/// @param clock What stamps the reading.
/// @param pool Where the gather runs.
/// @param resumeOn Where to come back.
/// @param out Where to put the result.
/// @return The task to submit.
[[nodiscard]] Task<void> TakeOnce(
    IStatsGatherer* gatherer, IClock* clock, IExecutor* pool, IExecutor* resumeOn, std::optional<SampleOutcome>* out)
{
    *out = co_await TakeSample(gatherer, clock, pool, resumeOn);
}

/// A gatherer during which time passes, by a known amount.
///
/// It does not block: the property it serves is WHEN the stamp is read, not whether the
/// reactor stays free, and a gather that returns promptly keeps the case about one thing.
class TimedGatherer final: public IStatsGatherer
{
  public:
    TimedGatherer(ManualClock& clock, Duration during):
        _clock { clock },
        _during { during }
    {
    }

    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        _clock.Advance(_during);
        return { StatsAttempt { .origin = StatsOrigin::Info, .asked = true, .record = std::nullopt, .note = "probe" } };
    }

  private:
    ManualClock& _clock;
    Duration _during;
};

/// An executor that makes the queue take time before it hands work on.
///
/// Stands in for the reactor's own latency: the delay between a continuation being posted
/// and the reactor running it, which is real and which must not become part of a reading.
class SlowQueue final: public IExecutor
{
  public:
    SlowQueue(ManualClock& clock, Duration delay, IExecutor& inner):
        _clock { clock },
        _delay { delay },
        _inner { inner }
    {
    }

    void Submit(std::coroutine_handle<> handle) override
    {
        _clock.Advance(_delay);
        _inner.Submit(handle);
    }

    void Submit(ParkedWork work) override
    {
        _clock.Advance(_delay);
        _inner.Submit(work);
    }

  private:
    ManualClock& _clock;
    Duration _delay;
    IExecutor& _inner;
};

/// Record which thread an executor runs work on.
/// @param pool The executor to hop onto.
/// @param where Where to record the thread id.
/// @param ran Set once the hop has happened.
/// @return The task to submit.
[[nodiscard]] Task<void> MarkThread(IExecutor* pool, std::atomic<std::thread::id>* where, std::atomic<bool>* ran)
{
    co_await ResumeOn { *pool };
    where->store(std::this_thread::get_id(), std::memory_order_release);
    ran->store(true, std::memory_order_release);
}

} // namespace

TEST_CASE("a stats reading is gathered off the reactor and delivered back onto it", "[cli][dashboard][sampler]")
{
    // `Gather()` is synchronous and does socket I/O, so on the reactor it would stall
    // ticks, input and quit together. The hop is the fix and the hop back is what makes
    // the fix safe to use.
    //
    // WHAT DISTINGUISHES: the THREAD IDENTITIES. A sample arriving proves nothing -- an
    // implementation that dropped both hops and called `Gather()` inline would produce
    // exactly the same reading, and every assertion about the reading would still pass.
    // So this case asserts three things about WHERE, and the reading only incidentally.
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto gatherer = RecordingGatherer {};

    auto const driverThread = std::this_thread::get_id();
    auto result = std::optional<SampleOutcome> {};
    auto task = TakeOnce(&gatherer, &clock, &pool, &reactor, &result);

    reactor.Submit(task.Native());
    reactor.Drain();

    // The gather is now parked on the pool, blocking. The reactor has run out of work,
    // which is itself the point: the loop is FREE while a reading is outstanding.
    while (!gatherer.entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    CHECK(!result.has_value());

    gatherer.release.store(true, std::memory_order_release);

    // Resumption comes back through the reactor, so it only happens when the reactor
    // runs. Draining until it does is what proves the return hop exists at all: without
    // it the task would have completed on the pool thread with no reactor turn.
    while (!result.has_value())
        reactor.Drain();

    REQUIRE(result.has_value());
    CHECK(gatherer.calls == 1);
    CHECK(Unwrap(result).gatheredOn != driverThread); // it left the reactor
    CHECK(Unwrap(result).gatheredOn == gatherer.calledOn);
    CHECK(Unwrap(result).resumedOn == driverThread); // and came back
    CHECK(Unwrap(result).resumedOn != result->gatheredOn);
    CHECK(Unwrap(result).attempts.size() == 1);
}

TEST_CASE("the sampler's thread identities are not equal by construction", "[cli][dashboard][sampler]")
{
    // The control on the case above, and it is not decoration. Every assertion there is
    // about two ids differing, and a harness whose driver thread happened to BE a pool
    // thread would satisfy them while proving nothing. This states the premise the other
    // case rests on: the thread driving the reactor is not one of the pool's.
    //
    // Measured rather than assumed, because it is a property of the HARNESS, and a
    // premise nobody wrote down is the one that quietly stops holding.
    auto pool = ThreadPoolExecutor { 1 };
    auto poolThread = std::atomic<std::thread::id> {};
    auto ran = std::atomic<bool> { false };

    auto marker = MarkThread(&pool, &poolThread, &ran);
    pool.Submit(marker.Native());

    while (!ran.load(std::memory_order_acquire))
        std::this_thread::yield();

    CHECK(poolThread.load(std::memory_order_acquire) != std::this_thread::get_id());
}

TEST_CASE("a reading is stamped after its gather returns and before the hop back", "[cli][dashboard][sampler]")
{
    // The stamp is a rate's denominator, so WHERE it is read is the whole property: before
    // the gather it would exclude the time the sources took to answer, and after the hop
    // back it would include however long the reactor queue took to resume the caller.
    //
    // WHAT DISTINGUISHES: time passes on BOTH sides of the correct line -- 5 s inside the
    // gather and 7 s in the queue -- so each misplacement produces its own wrong answer.
    // Stamped before the gather reads the start; stamped after the hop reads 12 s. Only the
    // right line reads exactly 5 s, and an assertion of "later than the start" would pass
    // for the second mistake.
    constexpr auto InsideGather = Duration { std::chrono::seconds { 5 } };
    constexpr auto InQueue = Duration { std::chrono::seconds { 7 } };

    auto clock = ManualClock {};
    auto const start = clock.Now();
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto gatherer = TimedGatherer { clock, InsideGather };
    auto queue = SlowQueue { clock, InQueue, reactor };

    auto result = std::optional<SampleOutcome> {};
    auto task = TakeOnce(&gatherer, &clock, &pool, &queue, &result);
    reactor.Submit(task.Native());
    while (!result.has_value())
        reactor.Drain();

    REQUIRE(result.has_value());
    CHECK(Unwrap(result).takenAt == start + InsideGather);
    // And the queue delay really happened, or the case above would pass on a line that
    // cannot tell "after the hop" from "before it".
    CHECK(clock.Now() == start + InsideGather + InQueue);
}
