// SPDX-License-Identifier: Apache-2.0
//
// What a reactor owes the coroutines it is holding when it goes away
// ([#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
//
// A reactor stopped with work parked resumed none of it and freed none of it: `Stop()`
// set a flag, `RunLoop()` returned, and the timer heap and submit queue were destroyed
// as containers of non-owning handles. Every frame in them, and everything reachable
// from it, leaked -- reported by LeakSanitizer as an INDIRECT-ONLY set, which is what a
// `Task` chain looks like when it refers to itself through its own continuations.
//
// **These cases do not rely on a sanitizer noticing.** A leak reported only by LSan is a
// red once in N runs and reads as a flake; every case here counts destructions with a
// sentinel and asserts the number, so it fails for its own reason on every platform.
//
// **Both directions, and never both at once.** Freeing everything at teardown is as
// wrong as freeing nothing: a timer that reaches its deadline must be RESUMED, and a
// handle whose frame something else owns must be LEFT ALONE. That second case is not
// hypothetical -- `TestReactor::Stop short-circuits the loop` parks a `Task` local it
// never starts and destroys it before the reactor, and a reactor that freed what it
// merely borrows would double free there. It is `WorkSomebodyElseOwnsIsLeftAlone`
// below, and it is why the answer to *destroy or resume* is neither on its own:
// the reactor frees exactly the chains that `Detail::ParkedWorkFor` says nothing owns.
//
// **Every property runs on both reactors this build has.** `PlatformReactor` is the
// derivation rather than a third name: it is `EpollReactor` on Linux, `IocpReactor` on
// Windows and `KqueueReactor` on macOS, so each CI leg covers its own. The other two
// cannot be compiled here at all, and the thing that stops a fifth backend inheriting
// the defect is the type system -- `Submit(ParkedWork)` and `Schedule(TimePoint,
// ParkedWork)` are pure virtual, so a new reactor is handed the question rather than
// left to not know about it.
#include <FastCache/Async/ParkedWork.hpp>
#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <thread>
#include <tuple>
#include <utility>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// What each case counts. Atomic because a platform reactor resumes on its own thread.
struct Counters
{
    std::atomic<int> parked { 0 };    ///< Coroutines that reached their suspend point.
    std::atomic<int> completed { 0 }; ///< Coroutine bodies that ran to their end.
    std::atomic<int> destroyed { 0 }; ///< Frames freed, counted by the sentinel each carries.
    std::atomic<int> reentered { 0 }; ///< Times a dying frame called back into the reactor.
};

/// A coroutine-frame sentinel: one per frame under test, counted when the frame dies.
///
/// Passed BY VALUE into every coroutine here, which is both the project's coroutine
/// rule and what puts it in the frame -- a body local would not exist in a lazy `Task`
/// that has never started, and one of these cases is precisely about such a task.
/// Move-aware, so the caller's temporary being destroyed at the end of the call
/// expression does not count as the frame dying.
class FrameSentinel
{
  public:
    /// @param counters Where the destruction is tallied; never null.
    explicit FrameSentinel(Counters* counters) noexcept:
        _counters { counters }
    {
    }

    FrameSentinel(FrameSentinel&& other) noexcept:
        _counters { std::exchange(other._counters, nullptr) }
    {
    }

    FrameSentinel(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel&&) = delete;

    ~FrameSentinel()
    {
        if (_counters != nullptr)
            _counters->destroyed.fetch_add(1, std::memory_order_acq_rel);
    }

  private:
    Counters* _counters;
};

/// A detached chain that parks on a timer nothing will ever reach.
/// @param reactor  The reactor to park on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask ParkOnTimerForever(IReactor* reactor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    co_await SleepUntil { .reactor = reactor, .deadline = reactor->Clock().Now() + 1h };
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// A detached chain that parks on a timer that DOES arrive.
/// @param reactor  The reactor to park on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
/// @param delay    How far out the deadline is.
DetachedTask ParkOnTimerBriefly(IReactor* reactor, FrameSentinel sentinel, Counters* counters, Duration delay)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    co_await SleepUntil { .reactor = reactor, .deadline = reactor->Clock().Now() + delay };
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// The innermost frame of a nested chain: it is what the reactor actually holds.
/// @param reactor  The reactor to park on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> ParkInner(IReactor* reactor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    co_await SleepUntil { .reactor = reactor, .deadline = reactor->Clock().Now() + 1h };
    co_return;
}

/// The middle frame: it owns `ParkInner`'s frame through its awaiter and is itself
/// owned by the root's.
/// @param reactor  The reactor the chain parks on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> ParkMiddle(IReactor* reactor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    co_await ParkInner(reactor, FrameSentinel { counters }, counters);
    co_return;
}

/// A detached chain three frames deep, the shape #1025 was reported on.
/// @param reactor  The reactor the chain parks on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask ParkNested(IReactor* reactor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    co_await ParkMiddle(reactor, FrameSentinel { counters }, counters);
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// A detached chain parked on the SUBMIT side rather than the timer heap.
/// @param reactor  The reactor to post to.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask ParkOnSubmit(IReactor* reactor, FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    co_await ResumeOn { *reactor };
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// A frame member that asks the reactor to take a handle back as it dies.
///
/// It stands in for `DeadlineTimer`, whose `~DeadlineTimer` calls `Disarm()` into
/// `IReactor::CancelPending`. That re-entry is what makes the ORDER of a reactor's own
/// teardown observable: `CancelPending` searches both parked containers, so a chain
/// freed after one of them has been destroyed reads freed storage.
///
/// The handle it cancels is deliberately one the reactor never had -- a miss is what
/// forces BOTH containers to be searched, where a hit would stop at the first.
class ReentrantDisarm
{
  public:
    /// @param reactor  Asked to cancel as this dies; never null.
    /// @param handle   A live handle this reactor does not hold, so the search misses.
    /// @param counters Where the re-entry is recorded.
    ReentrantDisarm(IReactor* reactor, std::coroutine_handle<> handle, Counters* counters) noexcept:
        _reactor { reactor },
        _handle { handle },
        _counters { counters }
    {
    }

    ReentrantDisarm(ReentrantDisarm&& other) noexcept:
        _reactor { std::exchange(other._reactor, nullptr) },
        _handle { other._handle },
        _counters { other._counters }
    {
    }

    ReentrantDisarm(ReentrantDisarm const&) = delete;
    ReentrantDisarm& operator=(ReentrantDisarm const&) = delete;
    ReentrantDisarm& operator=(ReentrantDisarm&&) = delete;

    ~ReentrantDisarm()
    {
        if (_reactor == nullptr)
            return;
        std::ignore = _reactor->CancelPending(_handle);
        _counters->reentered.fetch_add(1, std::memory_order_acq_rel);
    }

  private:
    IReactor* _reactor;
    std::coroutine_handle<> _handle;
    Counters* _counters;
};

/// A detached chain parked on the submit side whose frame re-enters the reactor as it
/// is freed.
/// @param reactor  The reactor to post to, and the one the disarm re-enters.
/// @param sentinel Counted when this frame dies.
/// @param disarm   Runs `CancelPending` on this reactor as this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask ParkOnSubmitWithDisarm(IReactor* reactor, FrameSentinel sentinel, ReentrantDisarm disarm, Counters* counters)
{
    (void) sentinel;
    (void) disarm;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    co_await ResumeOn { *reactor };
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// A lazy task carrying a sentinel, for the cases that need a frame rather than a chain.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> ImmediateWithSentinel(FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// A lazy task whose frame belongs to whoever holds the `Task`, never to a reactor.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> OwnedElsewhere(FrameSentinel sentinel, Counters* counters)
{
    (void) sentinel;
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// The deterministic double, driven by hand.
struct ManualDriver
{
    ManualClock clock;
    TestReactor reactor { clock };

    [[nodiscard]] IReactor& Reactor() noexcept
    {
        return reactor;
    }

    /// Turn the loop, letting the clock reach `window`, until `predicate` holds.
    /// @param window    How far the deadline under test is out.
    /// @param predicate What the caller is waiting for.
    /// @return Whether it became true.
    template <typename Predicate>
    [[nodiscard]] bool RunUntil(Duration window, Predicate predicate)
    {
        // Stepped rather than jumped so a wait that polls in sub-steps still makes
        // progress, and bounded so a regression fails rather than hangs.
        for (auto step = 0; step < 100 && !predicate(); ++step)
        {
            clock.Advance(window / 10 + 1ms);
            std::ignore = reactor.Drain();
        }
        return predicate();
    }

    /// Stop anything this driver started. Nothing runs on its own here.
    void Quiesce() noexcept {}
};

/// This platform's real reactor: epoll on Linux, IOCP on Windows, kqueue on macOS.
struct PlatformDriver
{
    SteadyClock clock;
    PlatformReactor reactor { clock };
    std::thread worker;

    PlatformDriver() = default;
    PlatformDriver(PlatformDriver const&) = delete;
    PlatformDriver(PlatformDriver&&) = delete;
    PlatformDriver& operator=(PlatformDriver const&) = delete;
    PlatformDriver& operator=(PlatformDriver&&) = delete;

    ~PlatformDriver()
    {
        // Joined before `reactor` is destroyed, which member order alone would not give:
        // reverse declaration order runs this body first, and a still-running loop would
        // then be freed underneath itself.
        Quiesce();
    }

    [[nodiscard]] IReactor& Reactor() noexcept
    {
        return reactor;
    }

    /// Run the loop on its own thread until `predicate` holds, then stop and join.
    /// @param window    Unused here; the real clock reaches the deadline on its own.
    /// @param predicate What the caller is waiting for.
    /// @return Whether it became true inside the bound.
    template <typename Predicate>
    [[nodiscard]] bool RunUntil(Duration /*window*/, Predicate predicate)
    {
        worker = std::thread { [this] { reactor.Run(); } };

        // Bounded, and generous rather than tuned: what is being waited for is one
        // timer on an idle reactor, so a slow runner is the only thing that can make
        // this long.
        auto const deadline = std::chrono::steady_clock::now() + 10s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);

        auto const settled = predicate();
        Quiesce();
        return settled;
    }

    /// Stop the loop and join its thread, if one was started.
    void Quiesce() noexcept
    {
        if (!worker.joinable())
            return;
        reactor.Stop();
        worker.join();
    }
};

/// A coroutine parked on a timer when the reactor goes away is freed exactly once.
/// @tparam Driver Which reactor to exercise.
template <typename Driver>
void AbandonedTimerIsFreedExactlyOnce()
{
    Counters counters;
    {
        Driver driver;
        ParkOnTimerForever(&driver.Reactor(), FrameSentinel { &counters }, &counters);

        // Parked, asserted rather than assumed: a `DetachedTask` runs eagerly to its
        // first suspend, and a body that had NOT suspended would have completed.
        REQUIRE(counters.parked.load() == 1);
        REQUIRE(counters.completed.load() == 0);
        REQUIRE(counters.destroyed.load() == 0);

        driver.Reactor().Stop();
        driver.Quiesce();

        // Stopping is not freeing, and that is the defect stated as an assertion:
        // `Stop()` sets a flag and the heap keeps the frame exactly where it was.
        REQUIRE(counters.destroyed.load() == 0);
    }

    CHECK(counters.destroyed.load() == 1);
    // Freed rather than resumed: nothing ran the rest of the body, which is the whole
    // reason this is safe to do at teardown.
    CHECK(counters.completed.load() == 0);
}

/// A timer that reaches its deadline is resumed, and is not then freed a second time.
/// @tparam Driver Which reactor to exercise.
template <typename Driver>
void AnElapsedTimerIsResumedAndNotFreedTwice()
{
    Counters counters;
    {
        Driver driver;
        ParkOnTimerBriefly(&driver.Reactor(), FrameSentinel { &counters }, &counters, 20ms);
        REQUIRE(counters.parked.load() == 1);
        REQUIRE(counters.completed.load() == 0);

        REQUIRE(driver.RunUntil(20ms, [&counters] { return counters.completed.load() == 1; }));

        // Resumed, ran to its end, and a `DetachedTask` frees its own frame there.
        CHECK(counters.destroyed.load() == 1);
    }

    // And the reactor did not free it again on the way out. Without this half the
    // teardown could free everything it holds and every other case would still pass.
    CHECK(counters.destroyed.load() == 1);
    CHECK(counters.completed.load() == 1);
}

/// An abandoned chain is freed from its ROOT, so every frame in it goes.
/// @tparam Driver Which reactor to exercise.
template <typename Driver>
void AnAbandonedChainIsFreedFromItsRoot()
{
    Counters counters;
    {
        Driver driver;
        ParkNested(&driver.Reactor(), FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked.load() == 1);
        REQUIRE(counters.destroyed.load() == 0);

        driver.Reactor().Stop();
        driver.Quiesce();
        REQUIRE(counters.destroyed.load() == 0);
    }

    // Three frames -- the detached root, the task it awaits, the task that parks --
    // each freed once. Freeing the frame the reactor HOLDS would give one: the two
    // above it are reachable only through each other, which is exactly the
    // indirect-only leak set #1025 was reported with.
    CHECK(counters.destroyed.load() == 3);
    CHECK(counters.completed.load() == 0);
}

/// The submit queue is covered too, not only the timer heap.
/// @tparam Driver Which reactor to exercise.
template <typename Driver>
void AnAbandonedSubmissionIsFreedExactlyOnce()
{
    Counters counters;
    {
        // Never started, so the posted resumption is never dequeued -- which is the
        // state a surface posting its own shutdown onto a stopped reactor is in.
        Driver driver;
        ParkOnSubmit(&driver.Reactor(), FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked.load() == 1);
        REQUIRE(counters.completed.load() == 0);
        REQUIRE(counters.destroyed.load() == 0);
    }

    CHECK(counters.destroyed.load() == 1);
    CHECK(counters.completed.load() == 0);
}

/// A submission that IS dequeued is resumed, and is not then freed a second time.
///
/// The submit-side mirror of the elapsed-timer case, and on IOCP it is the only thing
/// that drives the reconciliation at all: a posted completion packet has no entry to
/// carry its chain root, so `IocpReactor` keeps a side table and gives the chain back
/// at the one place a `KeyResumeCoroutine` packet is dequeued. A case that only ever
/// abandons submissions never reaches that line.
/// @tparam Driver Which reactor to exercise.
template <typename Driver>
void AResumedSubmissionIsNotFreedTwice()
{
    Counters counters;
    {
        Driver driver;
        ParkOnSubmit(&driver.Reactor(), FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked.load() == 1);
        REQUIRE(counters.completed.load() == 0);

        REQUIRE(driver.RunUntil(1ms, [&counters] { return counters.completed.load() == 1; }));
        CHECK(counters.destroyed.load() == 1);
    }

    CHECK(counters.destroyed.load() == 1);
    CHECK(counters.completed.load() == 1);
}

/// Work whose frame something else owns is left alone, however long it sits there.
/// @tparam Driver Which reactor to exercise.
template <typename Driver>
void WorkSomebodyElseOwnsIsLeftAlone()
{
    Counters counters;
    {
        // A lazy task, never started: its frame and its by-value parameters exist, and
        // the `Task` object is what frees them. This is the arrangement
        // `TestReactor::Stop short-circuits the loop` has had all along.
        auto owned = OwnedElsewhere(FrameSentinel { &counters }, &counters);
        {
            Driver driver;
            driver.Reactor().Submit(owned.Native());
        }

        // The reactor was destroyed holding this handle and did not touch it. A
        // teardown that freed what it merely borrows would have freed it here, and the
        // line below would then be a use-after-free rather than a failed check.
        CHECK(counters.destroyed.load() == 0);
        CHECK(counters.completed.load() == 0);
    }

    CHECK(counters.destroyed.load() == 1);
}

} // namespace

TEST_CASE("A coroutine parked on a reactor timer at stop is freed exactly once", "[async][reactor][teardown]")
{
    SECTION("on the test double")
    {
        AbandonedTimerIsFreedExactlyOnce<ManualDriver>();
    }
    SECTION("on this platform's reactor")
    {
        AbandonedTimerIsFreedExactlyOnce<PlatformDriver>();
    }
}

TEST_CASE("A reactor timer that elapses is resumed rather than freed", "[async][reactor][teardown]")
{
    SECTION("on the test double")
    {
        AnElapsedTimerIsResumedAndNotFreedTwice<ManualDriver>();
    }
    SECTION("on this platform's reactor")
    {
        AnElapsedTimerIsResumedAndNotFreedTwice<PlatformDriver>();
    }
}

TEST_CASE("An abandoned await chain is freed from its root rather than the frame parked", "[async][reactor][teardown]")
{
    SECTION("on the test double")
    {
        AnAbandonedChainIsFreedFromItsRoot<ManualDriver>();
    }
    SECTION("on this platform's reactor")
    {
        AnAbandonedChainIsFreedFromItsRoot<PlatformDriver>();
    }
}

TEST_CASE("A coroutine parked on a reactor submission at stop is freed exactly once", "[async][reactor][teardown]")
{
    SECTION("on the test double")
    {
        AnAbandonedSubmissionIsFreedExactlyOnce<ManualDriver>();
    }
    SECTION("on this platform's reactor")
    {
        AnAbandonedSubmissionIsFreedExactlyOnce<PlatformDriver>();
    }
}

TEST_CASE("A reactor submission that is dequeued is resumed rather than freed", "[async][reactor][teardown]")
{
    SECTION("on the test double")
    {
        AResumedSubmissionIsNotFreedTwice<ManualDriver>();
    }
    SECTION("on this platform's reactor")
    {
        AResumedSubmissionIsNotFreedTwice<PlatformDriver>();
    }
}

TEST_CASE("A parked entry that cannot resume frees the chain it owns", "[async][reactor][teardown]")
{
    // `Parked::Resume()` takes the work out before it decides whether it can resume, so
    // a handle it declines to resume would have its owned chain root DROPPED rather than
    // freed -- a silent leak on the one path this type exists to close.
    //
    // **Driven at the primitive, because no reactor reaches this branch today.** A
    // chain whose `abandon` is set is owned by nobody, so nothing else can have resumed
    // it to completion while the reactor held it; the branch is written for the contract
    // -- *resumed or freed, never neither* -- rather than for a caller that exists. A
    // case routed through a reactor would assert the unreachability instead of the rule.
    Counters spent;
    Counters owned;

    // Run to its final suspend, so it is a handle `Resume()` will decline.
    auto done = ImmediateWithSentinel(FrameSentinel { &spent }, &spent);
    done.Native().resume();
    REQUIRE(done.IsReady());

    auto victim = ImmediateWithSentinel(FrameSentinel { &owned }, &owned);
    {
        // `Release()` is what makes this entry the chain's only owner, which is the
        // arrangement `ParkedWork::abandon` describes.
        Detail::Parked parked { ParkedWork { .resume = done.Native(), .abandon = victim.Release() } };
        REQUIRE(owned.destroyed.load() == 0);

        parked.Resume();
        CHECK(owned.destroyed.load() == 1);
    }

    // Freed once, by the resume that declined -- not again when the entry died, and not
    // by having been run.
    CHECK(owned.destroyed.load() == 1);
    CHECK(owned.completed.load() == 0);
}

TEST_CASE("A reactor frees parked chains while both its containers are still alive", "[async][reactor][teardown]")
{
    // Finding: freeing during MEMBER destruction is not the same as freeing in the
    // destructor body. Members die in reverse declaration order, so one parked container
    // outlives the other -- and freeing a chain re-enters the reactor, because a frame
    // holding a `DeadlineTimer` runs `Disarm()` into `CancelPending`, which searches
    // BOTH containers. A chain freed from the later container therefore reads storage
    // the earlier one has already released.
    //
    // Asserted through ASan rather than a value: the failure is a read of freed memory,
    // so the case passing at all IS the assertion, and `reentered` is what stops it
    // passing for the wrong reason (a destructor that never ran asserts nothing).
    Counters counters;

    // Owned by this test and never given to the reactor, so `CancelPending` MISSES and
    // is forced to search both containers rather than stopping at the first.
    auto absent = ImmediateWithSentinel(FrameSentinel { &counters }, &counters);

    {
        ManualDriver driver;

        // A plain abandoned timer, so the timer container holds a real allocation. An
        // empty vector has no buffer to read, and the defect would then be invisible.
        ParkOnTimerForever(&driver.Reactor(), FrameSentinel { &counters }, &counters);

        // And the chain that re-enters, parked on the SUBMIT side -- the container that
        // dies second, so it is the one whose freeing would reach the other's corpse.
        ParkOnSubmitWithDisarm(&driver.Reactor(),
                               FrameSentinel { &counters },
                               ReentrantDisarm { &driver.Reactor(), absent.Native(), &counters },
                               &counters);

        REQUIRE(counters.parked.load() == 2);
        REQUIRE(counters.reentered.load() == 0);
    }

    // Both chains freed, and the re-entrant destructor did run -- so the search it
    // performed happened against containers this reactor had not yet let die.
    CHECK(counters.destroyed.load() == 2);
    CHECK(counters.reentered.load() == 1);
    CHECK(counters.completed.load() == 0);
}

TEST_CASE("A reactor leaves parked work whose frame something else owns alone", "[async][reactor][teardown]")
{
    SECTION("on the test double")
    {
        WorkSomebodyElseOwnsIsLeftAlone<ManualDriver>();
    }
    SECTION("on this platform's reactor")
    {
        WorkSomebodyElseOwnsIsLeftAlone<PlatformDriver>();
    }
}
