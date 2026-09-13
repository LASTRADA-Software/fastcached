// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/StopAwareWait.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <stop_token>
#include <thread>

using FastCache::WaitEnd;
using FastCache::WaitForStopOr;

using namespace std::chrono_literals;

namespace
{
/// Long enough that a wait sliced into twentieths sleeps three minutes before it looks at the token.
constexpr auto LongInterval = std::chrono::hours { 1 };

/// How soon a stop requested during that wait must be observed.
constexpr auto ObservedWithin = std::chrono::seconds { 10 };
} // namespace

TEST_CASE("a stop requested during a long wait ends it at once, not after a slice of it", "[core][stop]")
{
    // WHAT DISTINGUISHES: the wait is an hour long, and a stop requested while it is under way ends it
    // within seconds. A loop that sleeps in slices and polls the token between them also ends with
    // `Stopped` -- one slice late -- so the decision alone cannot tell the two apart, and the bound
    // below is what does: at an hour's twentieth, a slice is three minutes.
    //
    // Bounded on the steady clock through the future, never on a wall clock, which a VM host steps.
    // Everything the waiter touches is shared rather than borrowed, so a wait that does NOT end can be
    // left behind: joining it would turn this red into an hour's hang, and a detached thread that
    // borrowed this frame would read freed memory when it finally woke.
    auto source = std::stop_source {};
    auto entering = std::make_shared<std::latch>(1);
    auto ended = std::make_shared<std::promise<WaitEnd>>();
    auto result = ended->get_future();
    auto waiter = std::thread { [source, entering, ended] {
        entering->count_down();
        ended->set_value(WaitForStopOr(source.get_token(), LongInterval));
    } };

    entering->wait();
    // Long enough that the waiter is inside the wait rather than about to enter it: a stop requested
    // BEFORE the wait ends a sliced loop at once too, and the case would pass for the wrong reason.
    std::this_thread::sleep_for(100ms);
    (void) source.request_stop();

    auto const status = result.wait_for(ObservedWithin);
    if (status != std::future_status::ready)
        waiter.detach();
    else
        waiter.join();
    REQUIRE(status == std::future_status::ready);
    CHECK(result.get() == WaitEnd::Stopped);
}

TEST_CASE("a wait nobody stops lasts its interval and says so", "[core][stop]")
{
    // The control: without a stop the same wait ends `Elapsed`, and not before its interval.
    constexpr auto Interval = 50ms;
    auto source = std::stop_source {};
    auto const started = std::chrono::steady_clock::now();
    CHECK(WaitForStopOr(source.get_token(), Interval) == WaitEnd::Elapsed);
    CHECK(std::chrono::steady_clock::now() - started >= Interval);
}

TEST_CASE("a stop requested before the wait ends it without waiting", "[core][stop]")
{
    auto source = std::stop_source {};
    (void) source.request_stop();
    auto const started = std::chrono::steady_clock::now();
    CHECK(WaitForStopOr(source.get_token(), LongInterval) == WaitEnd::Stopped);
    CHECK(std::chrono::steady_clock::now() - started < ObservedWithin);
}
