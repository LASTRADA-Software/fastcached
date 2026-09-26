// SPDX-License-Identifier: Apache-2.0
//
// The test tree's one wait (`src/tests/BoundedWait.hpp`), tested here beside the drain it runs on,
// the way `Net/SocketDecorator_test.cpp` tests its shared fake.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include <core/async/Task.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/FrameSentinel.hpp>
#include <tests/Unwrap.hpp>

using Catch::Matchers::ContainsSubstring;
using FastCache::Testing::AwaitUntil;
using FastCache::Testing::FrameCounters;
using FastCache::Testing::FrameSentinel;
using FastCache::Testing::OffThreadWaits;
using FastCache::Testing::ReactorWaitOptions;
using FastCache::Testing::ReadWait;
using FastCache::Testing::Unwrap;
using FastCache::Testing::WaitHangGuard;
using FastCache::Testing::WaitOptions;
using FastCache::Testing::WaitOutcome;
using FastCache::Testing::WaitReading;
using FastCache::Testing::WaitRest;
using FastCache::Testing::WaitUntil;
using FastCache::Testing::WaitUntilOutcome;

using namespace std::chrono_literals;

// `ReadWait` is a classifier, and a classifier nothing drives through an outcome cannot be trusted to
// report it: each reading has its own case over a synthesised record, thresholds' edges included, so
// no reading is known only from a run that waited out the hang guard.

TEST_CASE("A test wait shorter than the reading window is too short to tell a stall from a slow thread", "[core][wait]")
{
    // The shape a wait clock that counts instead of measuring produces: no real time at all.
    CHECK(ReadWait({ .elapsed = 0ms, .changes = 0, .sinceLastChange = 0ms }) == WaitReading::TooShort);
    // Just inside the window, whatever the state did.
    CHECK(ReadWait({ .elapsed = 999ms, .changes = 0, .sinceLastChange = 999ms }) == WaitReading::TooShort);
    CHECK(ReadWait({ .elapsed = 999ms, .changes = 5, .sinceLastChange = 0ms }) == WaitReading::TooShort);
}

TEST_CASE("A test wait whose state changed within the last window was still moving", "[core][wait]")
{
    // #1433's cycling neuter's measured record: 18 changes, the last 542 ms before the guard.
    CHECK(ReadWait({ .elapsed = 10000ms, .changes = 18, .sinceLastChange = 542ms }) == WaitReading::Moving);
    // Both edges at once: a wait of exactly the window is long enough to read, and quiet of exactly
    // the window still counts as moving.
    CHECK(ReadWait({ .elapsed = 1000ms, .changes = 1, .sinceLastChange = 1000ms }) == WaitReading::Moving);
    CHECK(ReadWait({ .elapsed = 10000ms, .changes = 3, .sinceLastChange = 1000ms }) == WaitReading::Moving);
}

TEST_CASE("A test wait quiet for more than half of it was stalled", "[core][wait]")
{
    // #1433's stalled neuter's measured record: quiet for 9995 ms of 10000.
    CHECK(ReadWait({ .elapsed = 10000ms, .changes = 2, .sinceLastChange = 9995ms }) == WaitReading::Stalled);
    // Never changed at all.
    CHECK(ReadWait({ .elapsed = 10000ms, .changes = 0, .sinceLastChange = 10000ms }) == WaitReading::Stalled);
    // One millisecond past half.
    CHECK(ReadWait({ .elapsed = 10000ms, .changes = 1, .sinceLastChange = 5001ms }) == WaitReading::Stalled);
}

TEST_CASE("A test wait that moved and then went quiet for at most half of it is inconclusive", "[core][wait]")
{
    // Exactly half: not MORE than half, so not stalled.
    CHECK(ReadWait({ .elapsed = 10000ms, .changes = 4, .sinceLastChange = 5000ms }) == WaitReading::QuietAfterMoving);
    // One millisecond past the window.
    CHECK(ReadWait({ .elapsed = 10000ms, .changes = 7, .sinceLastChange = 1001ms }) == WaitReading::QuietAfterMoving);
}

TEST_CASE("A test wait that already holds returns at its first look and runs no step", "[core][wait]")
{
    auto steps = 0;
    auto const options =
        WaitOptions { .step = [&steps] { ++steps; }, .context = {}, .bound = WaitHangGuard, .rest = WaitRest };
    CHECK(WaitUntil("something already true", [] { return true; }, [] { return std::string {}; }, options));
    CHECK(steps == 0);
}

TEST_CASE("A test wait that runs out answers false only after its bound of real time", "[core][wait]")
{
    // What the helper DECIDED -- false -- and that it decided it no earlier than the bound, measured on
    // the monotonic clock: a wait that gave up by counting polls could answer false far sooner.
    auto const bound = 30ms;
    auto const started = std::chrono::steady_clock::now();
    auto const held = WaitUntil(
        "something that never happens",
        [] { return false; },
        [] { return std::string { "unchanged" }; },
        WaitOptions { .step = {}, .context = {}, .bound = bound, .rest = WaitRest });
    auto const elapsed = std::chrono::steady_clock::now() - started;
    CHECK_FALSE(held);
    CHECK(elapsed >= bound);
}

TEST_CASE("A step that throws reaches the case as that exception rather than ending the binary", "[core][wait]")
{
    CHECK_THROWS_WITH(WaitUntil(
                          "a step that throws",
                          [] { return false; },
                          [] { return std::string {}; },
                          WaitOptions { .step = [] { throw std::runtime_error { "the step threw" }; },
                                        .context = {},
                                        .bound = WaitHangGuard,
                                        .rest = WaitRest }),
                      Catch::Matchers::Equals("the step threw"));
}

TEST_CASE("A test wait that runs out says what it waited for, what it found, and how to read it", "[core][wait]")
{
    // The account is the reason the helper exists rather than a bool: asserted on its words, since a
    // wait that answered false silently would pass every case above.
    auto const outcome = WaitUntilOutcome(
        "a reply that never comes",
        [] { return false; },
        [] { return std::string { "no reply yet" }; },
        WaitOptions {
            .step = {}, .context = [] { return std::string { "one peer connected" }; }, .bound = 30ms, .rest = WaitRest });
    CHECK_FALSE(outcome.reached);
    CHECK(outcome.elapsed >= 30ms);
    CHECK_THAT(outcome.account,
               ContainsSubstring("WaitUntil gave up waiting for a reply that never comes after")
                   && ContainsSubstring("State at the end: no reply yet; one peer connected.")
                   && ContainsSubstring("It changed 0 time(s)") && ContainsSubstring("INCONCLUSIVE: too short a wait"));

    auto const reached = WaitUntilOutcome("something already true", [] { return true; }, [] { return std::string {}; });
    CHECK(reached.reached);
    CHECK(reached.account.empty());
}

TEST_CASE("A wait on a helper thread hands its account to the case instead of asserting there", "[core][wait]")
{
    // What reached and what ran out, recorded on the helper and asserted here, after the join: a Catch2
    // message from the helper would race this thread's.
    auto const bounded = WaitOptions { .step = {}, .context = {}, .bound = 30ms, .rest = WaitRest };
    auto waits = OffThreadWaits {};
    auto everything = OffThreadWaits {};
    std::atomic<bool> firstReached { false };
    std::atomic<bool> secondReached { true };
    {
        std::jthread const helper { [&] {
            firstReached.store(waits.Wait(
                                   "a flag already set", [] { return true; }, [] { return std::string {}; }, bounded),
                               std::memory_order_relaxed);
            secondReached.store(
                waits.Wait(
                    "a flag nobody sets", [] { return false; }, [] { return std::string { "unset" }; }, bounded),
                std::memory_order_relaxed);
            std::ignore = everything.Wait("a flag already set", [] { return true; }, [] { return std::string {}; }, bounded);
        } };
    }
    CHECK(firstReached.load(std::memory_order_relaxed));
    CHECK_FALSE(secondReached.load(std::memory_order_relaxed));
    CHECK_FALSE(waits.AllReached());
    // The control: a holder whose waits all reached answers true, so the false above is the wait that ran out.
    CHECK(everything.AllReached());
}

// `AwaitUntil`, the same wait for a coroutine on a reactor (#1453). Driven on a `core::net::testing::TestLoop` over a
// `core::platform::ManualClock`, so the bound and the reading are shown to follow the REACTOR's clock: every case below
// takes no real time worth measuring.

namespace
{

/// Await @p flag on @p reactor and publish what the wait found.
/// @param reactor  Where the wait parks.
/// @param flag     What is waited for; must outlive the task.
/// @param options  The bound and the rest.
/// @param sentinel Counted when this frame dies.
/// @param out      Where the outcome is written; must outlive the task.
core::async::DetachedTask AwaitFlag(core::net::EventLoop* reactor,
                                    std::atomic<bool> const* flag,
                                    ReactorWaitOptions options,
                                    FrameSentinel sentinel,
                                    std::optional<WaitOutcome>* out)
{
    (void) sentinel;
    *out = co_await AwaitUntil(
        reactor,
        "the flag to be set",
        [flag] { return flag->load(std::memory_order_acquire); },
        [flag] { return std::string { flag->load(std::memory_order_acquire) ? "set" : "unset" }; },
        std::move(options));
}

} // namespace

TEST_CASE("A reactor wait that already holds returns at its first look and parks nothing", "[core][wait]")
{
    // Declared before the reactor: a case that ends with the coroutine still parked frees it as the
    // reactor dies, and the frame's sentinel, flag and outcome must still be alive then.
    FrameCounters counters;
    std::atomic<bool> const flag { true };
    std::optional<WaitOutcome> outcome;
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };

    AwaitFlag(&reactor, &flag, ReactorWaitOptions {}, FrameSentinel { &counters }, &outcome);

    REQUIRE(outcome.has_value());
    CHECK(Unwrap(outcome).reached);
    CHECK(Unwrap(outcome).account.empty());
    CHECK(reactor.pendingSubmissions() == 0);
    CHECK(reactor.pendingTimers() == 0);
    CHECK(counters.destroyed.load() == 1);
}

TEST_CASE("A reactor wait takes a reactor turn between looks and ends when its condition holds", "[core][wait]")
{
    // Declared before the reactor: a case that ends with the coroutine still parked frees it as the
    // reactor dies, and the frame's sentinel, flag and outcome must still be alive then.
    FrameCounters counters;
    std::atomic<bool> flag { false };
    std::optional<WaitOutcome> outcome;
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };

    AwaitFlag(&reactor, &flag, ReactorWaitOptions {}, FrameSentinel { &counters }, &outcome);
    REQUIRE_FALSE(outcome.has_value());
    // Parked on the reactor rather than spinning the thread: one submission per look.
    CHECK(reactor.pendingSubmissions() == 1);
    std::ignore = reactor.tick();
    std::ignore = reactor.tick();
    REQUIRE_FALSE(outcome.has_value());

    flag.store(true, std::memory_order_release);
    std::ignore = reactor.tick();

    REQUIRE(outcome.has_value());
    CHECK(Unwrap(outcome).reached);
    CHECK(reactor.pendingSubmissions() == 0);
    CHECK(counters.destroyed.load() == 1);
}

TEST_CASE("A reactor wait runs out on the reactor's clock and says what it found", "[core][wait]")
{
    // Three seconds of MANUAL time and no real time to speak of: a wait bounded or read on the host's
    // clock would still be waiting here, or would read TOO SHORT. STALLED is only reachable on the
    // reactor's clock.
    // Declared before the reactor: a case that ends with the coroutine still parked frees it as the
    // reactor dies, and the frame's sentinel, flag and outcome must still be alive then.
    FrameCounters counters;
    std::atomic<bool> const flag { false };
    std::optional<WaitOutcome> outcome;
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };

    AwaitFlag(&reactor,
              &flag,
              ReactorWaitOptions { .context = [] { return std::string { "nobody sets it" }; }, .bound = 3s, .rest = 100ms },
              FrameSentinel { &counters },
              &outcome);
    // Sleeping between looks parks on the reactor's timers, not its submissions.
    CHECK(reactor.pendingTimers() == 1);
    for ([[maybe_unused]] auto const step: std::views::iota(0, 100))
    {
        if (outcome.has_value())
            break;
        clock.advance(100ms);
        std::ignore = reactor.tick();
    }

    REQUIRE(outcome.has_value());
    CHECK_FALSE(Unwrap(outcome).reached);
    CHECK(Unwrap(outcome).elapsed == 3s);
    // Fifteen looks in thirty ticks, not thirty: core-cpp's loop fires a due deadline at the END of
    // a turn and resumes it at the START of the next (its guarantee G2, one place resumes), so each
    // 100 ms rest costs this drive two ticks where fastcached's `TestReactor` fired and resumed in one.
    CHECK_THAT(Unwrap(outcome).account,
               ContainsSubstring("AwaitUntil gave up waiting for the flag to be set after 3000 ms on the reactor's clock "
                                 "and 15 turn(s).")
                   && ContainsSubstring("State at the end: unset; nobody sets it.") && ContainsSubstring("STALLED"));
    CHECK(reactor.pendingTimers() == 0);
    CHECK(counters.destroyed.load() == 1);
}

TEST_CASE("A reactor torn down while a coroutine waits in AwaitUntil frees that coroutine's chain once", "[core][wait]")
{
    // The helper is a `core::async::Task` awaited from the caller's frame, so what the reactor holds at teardown is the
    // helper's frame, not the caller's. It is freed from the chain's ROOT (`Detail::UnownedRootOf`, #1025),
    // which is what makes delegating the loop to a helper safe -- `SleepUntil.hpp` points here for that.
    // ASan/UBSan builds would also report a double free or a leak.
    auto const rest = GENERATE(core::platform::SteadyDuration::zero(), core::platform::SteadyDuration { 1h });
    INFO("rest between looks: " << std::chrono::duration_cast<std::chrono::milliseconds>(rest).count() << " ms");
    FrameCounters counters;
    std::atomic<bool> const flag { false };
    std::optional<WaitOutcome> outcome;
    {
        core::platform::ManualClock clock;
        core::net::testing::TestLoop reactor { clock };
        AwaitFlag(&reactor,
                  &flag,
                  ReactorWaitOptions { .context = {}, .bound = 2h, .rest = rest },
                  FrameSentinel { &counters },
                  &outcome);
        REQUIRE(reactor.pendingSubmissions() + reactor.pendingTimers() == 1);
        CHECK(counters.destroyed.load() == 0);
    }
    CHECK_FALSE(outcome.has_value());
    CHECK(counters.destroyed.load() == 1);
}
