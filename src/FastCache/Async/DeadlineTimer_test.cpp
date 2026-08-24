// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

namespace
{

/// Counts how many times a timer fired. A counter rather than a flag, because
/// "fired once" and "fired at all" are different claims and the first is what a
/// timer built on an uncancellable Schedule has to prove.
struct FireCount
{
    int value { 0 };
};

void CountFire(void* state) noexcept
{
    static_cast<FireCount*>(state)->value += 1;
}

} // namespace

TEST_CASE("A deadline fires exactly once, on the reactor", "[async][deadline]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FireCount fired;

    FastCache::DeadlineTimer timer { reactor, clock.Now() + 100ms, &CountFire, &fired, 25ms };
    reactor.Drain();
    CHECK(fired.value == 0);
    CHECK_FALSE(timer.IsSettled());

    clock.Advance(100ms);
    reactor.Drain();
    CHECK(fired.value == 1);
    CHECK(timer.IsSettled());

    // Advancing further must not fire it again: the settled flag is claimed
    // before the callback runs, so a late wake-up finds nothing to do.
    clock.Advance(1s);
    reactor.Drain();
    CHECK(fired.value == 1);
}

TEST_CASE("A disarmed deadline never fires, and its frame is reclaimed", "[async][deadline]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FireCount fired;

    FastCache::DeadlineTimer timer { reactor, clock.Now() + 30s, &CountFire, &fired, 50ms };
    reactor.Drain();
    timer.Disarm();

    // Within ONE poll interval, not after the 30s deadline. This is the whole
    // reason the wait is bounded and re-armed instead of parked once: a dial that
    // settles in 2ms against a 30s budget would otherwise leave a frame on the
    // reactor's timer heap for the rest of the budget, and `IReactor::Run`
    // returns with that heap exactly as it was -- a coroutine nobody resumes and
    // nobody ever frees.
    clock.Advance(50ms);
    reactor.Drain();
    CHECK(fired.value == 0);
    CHECK(reactor.PendingTimers() == 0);
    CHECK(reactor.PendingSubmissions() == 0);

    clock.Advance(30s);
    reactor.Drain();
    CHECK(fired.value == 0);
}

TEST_CASE("Disarming after the fire is a no-op", "[async][deadline]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FireCount fired;

    FastCache::DeadlineTimer timer { reactor, clock.Now() + 10ms, &CountFire, &fired, 5ms };
    clock.Advance(10ms);
    reactor.Drain();
    REQUIRE(fired.value == 1);

    // The ordinary shape at a call site: an operation completes, disarms
    // unconditionally, and neither knows nor cares whether it won the race.
    timer.Disarm();
    timer.Disarm();
    CHECK(fired.value == 1);
    CHECK(timer.IsSettled());
}

TEST_CASE("Destroying a timer disarms it", "[async][deadline]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FireCount fired;

    {
        FastCache::DeadlineTimer const timer { reactor, clock.Now() + 100ms, &CountFire, &fired, 25ms };
        reactor.Drain();
    }

    // The callback holds a pointer to `fired`, which is still alive here -- but
    // in production that state is typically the operation's own frame, so a fire
    // after the owner has gone is a use-after-free. The shared state outliving
    // the owner is what makes the late wake-up safe; the disarm is what makes it
    // silent.
    clock.Advance(100ms);
    reactor.Drain();
    CHECK(fired.value == 0);
    CHECK(reactor.PendingTimers() == 0);
}

TEST_CASE("A deadline already in the past fires on the next turn, not inline", "[async][deadline]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FireCount fired;

    FastCache::DeadlineTimer const timer { reactor, clock.Now() - 1s, &CountFire, &fired, 25ms };

    // Not during construction: a caller must never be re-entered from its own
    // constructor, because the object it is building does not exist yet.
    CHECK(fired.value == 0);

    reactor.Drain();
    CHECK(fired.value == 1);
}
