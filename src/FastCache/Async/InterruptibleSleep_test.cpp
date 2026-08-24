// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Cancellation.hpp>
#include <FastCache/Async/InterruptibleSleep.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

#include <tests/Unwrap.hpp>

using namespace std::chrono_literals;

namespace
{

/// Drives one sleep and records how it ended, so a case can assert the reason
/// without awaiting from the test body.
FastCache::Task<void> Sleeper(FastCache::IReactor* reactor,
                              FastCache::CancellationToken token,
                              FastCache::TimePoint deadline,
                              FastCache::Duration wakeBound,
                              std::optional<FastCache::WakeReason>* out)
{
    *out = co_await FastCache::InterruptibleSleepUntil(reactor, token, deadline, wakeBound);
    co_return;
}

} // namespace

TEST_CASE("InterruptibleSleepUntil wakes at its deadline", "[async][sleep]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FastCache::CancellationSource source;

    std::optional<FastCache::WakeReason> reason;
    auto task = Sleeper(&reactor, source.Token(), clock.Now() + 100ms, 25ms, &reason);
    reactor.Submit(task.Native());
    reactor.Drain();
    CHECK_FALSE(reason.has_value());

    clock.Advance(100ms);
    reactor.Drain();
    REQUIRE(reason.has_value());
    CHECK(FastCache::Testing::Unwrap(reason) == FastCache::WakeReason::Deadline);

    // Nothing left behind either way -- this is the assertion the bounded poll
    // exists for. A version that parked one frame at the deadline would leave it
    // on the wheel, and a reactor stopped before it fired would never free it.
    CHECK(reactor.PendingTimers() == 0);
    CHECK(reactor.PendingSubmissions() == 0);
}

TEST_CASE("InterruptibleSleepUntil is interrupted well before its deadline", "[async][sleep]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FastCache::CancellationSource source;

    auto const started = clock.Now();
    std::optional<FastCache::WakeReason> reason;
    auto task = Sleeper(&reactor, source.Token(), started + 30s, 50ms, &reason);
    reactor.Submit(task.Native());
    reactor.Drain();

    source.Cancel();
    clock.Advance(50ms);
    reactor.Drain();

    REQUIRE(reason.has_value());
    CHECK(FastCache::Testing::Unwrap(reason) == FastCache::WakeReason::Cancelled);

    // The assertion that the wait was INTERRUPTED rather than waited out: the
    // clock never reached the deadline. `elapsed < deadline` is what a version
    // that merely happened to be fast would also satisfy, so the bound is what
    // is checked -- teardown lag must not depend on how long the caller asked to
    // wait, which is the whole property a plain SleepUntil cannot give.
    CHECK(clock.Now() - started == 50ms);
    CHECK(reactor.PendingTimers() == 0);
    CHECK(reactor.PendingSubmissions() == 0);
}

TEST_CASE("An already-cancelled token never touches the reactor", "[async][sleep]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FastCache::CancellationSource source;
    source.Cancel();

    std::optional<FastCache::WakeReason> reason;
    auto task = Sleeper(&reactor, source.Token(), clock.Now() + 30s, 50ms, &reason);
    reactor.Submit(task.Native());
    reactor.Drain();

    REQUIRE(reason.has_value());
    CHECK(FastCache::Testing::Unwrap(reason) == FastCache::WakeReason::Cancelled);
    // A stop issued during teardown must not park a frame on a wheel that is
    // about to stop turning.
    CHECK(reactor.PendingTimers() == 0);
}

TEST_CASE("A non-positive wake bound sleeps once rather than spinning", "[async][sleep]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FastCache::CancellationSource source;

    std::optional<FastCache::WakeReason> reason;
    auto task = Sleeper(&reactor, source.Token(), clock.Now() + 100ms, FastCache::Duration::zero(), &reason);
    reactor.Submit(task.Native());
    reactor.Drain();

    // Exactly one parked timer, not thousands: a zero-length step resolves as
    // already-ready, so a loop that took it literally would busy-spin the
    // reactor thread instead of waiting.
    CHECK(reactor.PendingTimers() == 1);

    clock.Advance(100ms);
    reactor.Drain();
    REQUIRE(reason.has_value());
    CHECK(FastCache::Testing::Unwrap(reason) == FastCache::WakeReason::Deadline);
}

TEST_CASE("A null reactor resolves immediately, as SleepUntil does", "[async][sleep]")
{
    FastCache::ManualClock clock;
    FastCache::CancellationSource source;

    std::optional<FastCache::WakeReason> reason;
    // No reactor means no timer wheel -- the in-memory transport's case. Driven
    // with SyncRun precisely because nothing here may suspend.
    FastCache::SyncRun(Sleeper(nullptr, source.Token(), clock.Now() + 30s, 50ms, &reason));

    REQUIRE(reason.has_value());
    CHECK(FastCache::Testing::Unwrap(reason) == FastCache::WakeReason::Deadline);
}
