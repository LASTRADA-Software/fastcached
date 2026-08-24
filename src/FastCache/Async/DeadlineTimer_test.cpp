// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
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

TEST_CASE("A disarmed deadline leaves nothing parked on the reactor", "[async][deadline]")
{
    // The property behind a leak rather than a wrong answer, so nothing else here
    // can catch it: a disarmed timer used to stay on the timer wheel until its next
    // poll, and a reactor destroyed inside that window never freed the frame. One
    // per dial and one per cache exchange -- invisible until the sanitizer preset
    // was fixed to actually build with sanitizers, at which point an ASan
    // `fastcache-cc` exited non-zero and every compile through it failed.
    //
    // Asserted as "nothing is parked", which is the observable form of "the frame
    // is gone": `Disarm()` takes the handle back off the reactor with
    // `CancelPending` and destroys it, rather than leaving it to retire a poll
    // interval later.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FireCount fired;

    {
        FastCache::DeadlineTimer const timer { reactor, clock.Now() + 1h, &CountFire, &fired, 25ms };
        reactor.Drain();
        CHECK(reactor.PendingTimers() == 1);
    }

    // Nothing left anywhere, and nothing needed to run for that to be true: the
    // caller that disarms is typically about to stop the reactor, so a fix that
    // merely re-queued the frame would leak exactly as before.
    CHECK(reactor.PendingTimers() == 0);
    CHECK(reactor.PendingSubmissions() == 0);
    reactor.Drain();
    CHECK(fired.value == 0);
}

TEST_CASE("CancelPending hands the handle back exactly once", "[async][reactor]")
{
    // The ownership half of the contract, which is what makes the race decidable:
    // the caller may resume or destroy the handle only if this call is the one that
    // took it. A second call must therefore say no -- otherwise two owners resume
    // the same coroutine, which is the defect this facility exists to avoid rather
    // than one it may introduce.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    // Nothing this reactor holds: a handle it never saw is not the caller's to take.
    CHECK_FALSE(reactor.CancelPending(std::coroutine_handle<> {}));

    FireCount fired;
    FastCache::DeadlineTimer timer { reactor, clock.Now() + 1h, &CountFire, &fired, 25ms };
    reactor.Drain();
    REQUIRE(reactor.PendingTimers() == 1);

    timer.Disarm();
    CHECK(reactor.PendingTimers() == 0);
    // Idempotent, and the second call must not reach the reactor at all: the frame
    // it would name has already been destroyed, so asking again with that handle is
    // exactly the double-free this contract exists to prevent.
    timer.Disarm();
    reactor.Drain();
    CHECK(reactor.PendingSubmissions() == 0);
    CHECK(fired.value == 0);
}

TEST_CASE("A deadline fires when a task on the same reactor moves the clock past it", "[async][deadline]")
{
    // The drive pattern every other case here avoids, and the one production uses:
    // the clock moves from a coroutine ON the reactor rather than from the test
    // thread, and the loop is `Run()` -- which stops at the first turn that resumes
    // nothing -- rather than `Drain()`, which the test thread calls again after each
    // advance. The difference matters because a bounded poll only makes progress on
    // turns that happen, so a timer that re-armed after the last advance would be
    // left on the wheel here and fire nowhere.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    FireCount fired;

    auto const advance = [](FastCache::TestReactor* loop, FastCache::ManualClock* moving) -> FastCache::DetachedTask {
        co_await FastCache::ResumeOn { *loop };
        moving->Advance(10s);
        co_return;
    };

    FastCache::DeadlineTimer const timer { reactor, clock.Now() + 1s, &CountFire, &fired, 25ms };
    advance(&reactor, &clock);
    reactor.Run();

    INFO("advanced=" << std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now().time_since_epoch()).count()
                     << "ms pendingSubmissions=" << reactor.PendingSubmissions()
                     << " pendingTimers=" << reactor.PendingTimers());
    CHECK(clock.Now() > FastCache::TimePoint {});
    CHECK(fired.value == 1);
}
