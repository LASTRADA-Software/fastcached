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
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <tests/BoundedWait.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::DrainUntil;
using FastCache::Testing::OffThreadWaits;
using FastCache::Testing::Unwrap;
using FastCache::Testing::WaitUntil;

namespace
{

/// A stream whose reads record which thread asked and block while they do.
///
/// It blocks on purpose. A read that returned instantly would let a broken implementation -- one that
/// never left the reactor -- finish before anything could observe the difference, so the case would
/// pass for the wrong reason. **Bounded** (#1446): a case that fails before releasing it must end red,
/// not with the pool's join waiting on a read nothing will release.
class RecordingSubscription final: public ILiveSubscription
{
  public:
    [[nodiscard]] std::expected<void, ExchangeError> Open(Endpoint const& /*where*/,
                                                          CompileCacheWire::SubscribeRequest const& /*request*/) override
    {
        return {};
    }

    [[nodiscard]] std::expected<NodeReply, ExchangeError> Read() override
    {
        calledOn = std::this_thread::get_id();
        ++calls;
        entered.store(true, std::memory_order_release);
        std::ignore = waits.WaitForFlag(
            "the case to release the read", release, [] { return std::string { "the read is still held" }; });
        return NodeReply { .status = CompileCacheWire::Status::Ok };
    }

    void ExpectEvery(std::chrono::milliseconds /*cadence*/) override {}

    void Leave() noexcept override {}

    std::thread::id calledOn {};
    std::size_t calls { 0 };
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    OffThreadWaits waits; ///< The read's wait for `release`, run on the pool thread.
};

/// Take one frame, writing the result where the caller can read it.
///
/// Pointers and a named coroutine, for the reason the production signatures use them: a lambda
/// coroutine loses its closure at the first suspension, and this one suspends immediately.
/// @param subscription The stream.
/// @param clock What stamps the frame.
/// @param pool Where the read runs.
/// @param resumeOn Where to come back.
/// @param out Where to put the result.
/// @return The task to submit.
[[nodiscard]] Task<void> TakeOnce(
    ILiveSubscription* subscription, IClock* clock, IExecutor* pool, IExecutor* resumeOn, std::optional<FrameOutcome>* out)
{
    *out = co_await TakeFrame(subscription, clock, pool, resumeOn);
}

/// A stream during whose read time passes, by a known amount.
///
/// It does not block: the property it serves is WHEN the stamp is read, not whether the reactor stays
/// free, and a read that returns promptly keeps the case about one thing.
class TimedSubscription final: public ILiveSubscription
{
  public:
    TimedSubscription(ManualClock& clock, Duration during):
        _clock { clock },
        _during { during }
    {
    }

    [[nodiscard]] std::expected<void, ExchangeError> Open(Endpoint const& /*where*/,
                                                          CompileCacheWire::SubscribeRequest const& /*request*/) override
    {
        return {};
    }

    [[nodiscard]] std::expected<NodeReply, ExchangeError> Read() override
    {
        _clock.Advance(_during);
        return NodeReply { .status = CompileCacheWire::Status::Ok };
    }

    void ExpectEvery(std::chrono::milliseconds /*cadence*/) override {}

    void Leave() noexcept override {}

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

TEST_CASE("a stream frame is read off the reactor and delivered back onto it", "[cli][dashboard][sampler]")
{
    // `Read()` is synchronous and does socket I/O, so on the reactor it would stall
    // ticks, input and quit together. The hop is the fix and the hop back is what makes
    // the fix safe to use.
    //
    // WHAT DISTINGUISHES: the THREAD IDENTITIES. A frame arriving proves nothing -- an
    // implementation that dropped both hops and called `Read()` inline would produce
    // exactly the same frame, and every assertion about the frame would still pass.
    // So this case asserts three things about WHERE, and the frame only incidentally.
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto subscription = RecordingSubscription {};

    auto const driverThread = std::this_thread::get_id();
    auto result = std::optional<FrameOutcome> {};
    auto task = TakeOnce(&subscription, &clock, &pool, &reactor, &result);

    reactor.Submit(task.Native());
    reactor.Drain();

    // The read is now parked on the pool, blocking. The reactor has run out of work,
    // which is itself the point: the loop is FREE while a frame is outstanding. A CHECK,
    // not a REQUIRE: the read is released below either way, so a failure here cannot leave
    // the pool's join waiting on it.
    auto const state = [&subscription, &result] {
        return std::format("read entered {}, released {}, frame back {}",
                           subscription.entered.load(std::memory_order_acquire),
                           subscription.release.load(std::memory_order_acquire),
                           result.has_value());
    };
    CHECK(WaitUntil(
        "the read to enter on the pool thread",
        [&subscription] { return subscription.entered.load(std::memory_order_acquire); },
        state));
    CHECK(!result.has_value());

    subscription.release.store(true, std::memory_order_release);

    // Resumption comes back through the reactor, so it only happens when the reactor
    // runs. Draining until it does is what proves the return hop exists at all: without
    // it the task would have completed on the pool thread with no reactor turn.
    //
    // Twice the guard, because the read's own wait for the release is one guard long: a read
    // nothing released gives up first, the frame still comes back, and the red below then names
    // the release -- measured the other way round, both waits ended within a millisecond of each
    // other and the frame's won, naming the symptom instead.
    REQUIRE(DrainUntil(
        reactor,
        "the frame to come back through the reactor",
        [&result] { return result.has_value(); },
        state,
        2 * FastCache::Testing::WaitHangGuard));
    // The read has returned, so its own wait has concluded: first, while its account is attached.
    CHECK(subscription.waits.AllReached());

    REQUIRE(result.has_value());
    CHECK(subscription.calls == 1);
    CHECK(Unwrap(result).readOn != driverThread); // it left the reactor
    CHECK(Unwrap(result).readOn == subscription.calledOn);
    CHECK(Unwrap(result).resumedOn == driverThread); // and came back
    CHECK(Unwrap(result).resumedOn != Unwrap(result).readOn);
    CHECK(Unwrap(result).frame.has_value());
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
    //
    // DECLARATION ORDER IS LOAD-BEARING, and the next tidy-up must not reorder it. `marker`
    // completes on the POOL thread, and `ran` is its last statement rather than its end: the
    // frame is still being written on the way to `final_suspend` when the wait below returns.
    // Declared BEFORE `pool`, the task is destroyed AFTER the pool has joined its thread, so the
    // frame is freed once nothing is inside it. The other way round, `~Task` frees the frame
    // while the pool thread is still in it -- a data race under TSan and a heap-use-after-free
    // when it lands. An `optional` only because the task needs the pool's address, which does
    // not exist yet where the task has to be declared.
    auto poolThread = std::atomic<std::thread::id> {};
    auto ran = std::atomic<bool> { false };
    auto marker = std::optional<Task<void>> {};
    auto pool = ThreadPoolExecutor { 1 };

    pool.Submit(marker.emplace(MarkThread(&pool, &poolThread, &ran)).Native());

    REQUIRE(WaitUntil(
        "the marker to run on the pool thread",
        [&ran] { return ran.load(std::memory_order_acquire); },
        [] { return std::string { "the marker has not run" }; }));

    CHECK(poolThread.load(std::memory_order_acquire) != std::this_thread::get_id());
}

TEST_CASE("a frame is stamped after its read returns and before the hop back", "[cli][dashboard][sampler]")
{
    // The stamp is a rate's denominator, so WHERE it is read is the whole property: before
    // the read it would exclude the time the frame took to arrive, and after the hop back
    // it would include however long the reactor queue took to resume the caller.
    //
    // WHAT DISTINGUISHES: time passes on BOTH sides of the correct line -- 5 s inside the
    // read and 7 s in the queue -- so each misplacement produces its own wrong answer.
    // Stamped before the read reads the start; stamped after the hop reads 12 s. Only the
    // right line reads exactly 5 s, and an assertion of "later than the start" would pass
    // for the second mistake.
    constexpr auto InsideRead = Duration { std::chrono::seconds { 5 } };
    constexpr auto InQueue = Duration { std::chrono::seconds { 7 } };

    auto clock = ManualClock {};
    auto const start = clock.Now();
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };
    auto subscription = TimedSubscription { clock, InsideRead };
    auto queue = SlowQueue { clock, InQueue, reactor };

    auto result = std::optional<FrameOutcome> {};
    auto task = TakeOnce(&subscription, &clock, &pool, &queue, &result);
    reactor.Submit(task.Native());
    REQUIRE(DrainUntil(
        reactor,
        "the stamped frame to come back through the slow queue",
        [&result] { return result.has_value(); },
        [&clock, start] {
            return std::format("{} ms of manual time passed",
                               std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start).count());
        }));

    REQUIRE(result.has_value());
    CHECK(Unwrap(result).takenAt == start + InsideRead);
    // And the queue delay really happened, or the case above would pass on a line that
    // cannot tell "after the hop" from "before it".
    CHECK(clock.Now() == start + InsideRead + InQueue);
}
