// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(_WIN32)
    #include <FastCache/Async/IocpReactor.hpp>
#elif defined(__linux__)
    #include <FastCache/Async/EpollReactor.hpp>
#elif defined(__APPLE__)
    #include <FastCache/Async/KqueueReactor.hpp>
#endif

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace
{

// The same platform selection ReactorServerLoop.cpp makes, for the same reason:
// `CachedClock` is only correct because whoever owns the event loop refreshes
// it, and every platform's loop has to hold up its end of that bargain.
#if defined(_WIN32)
using PlatformReactor = FastCache::IocpReactor;
#elif defined(__linux__)
using PlatformReactor = FastCache::EpollReactor;
#elif defined(__APPLE__)
using PlatformReactor = FastCache::KqueueReactor;
#else
    #error "No reactor implementation for this platform"
#endif

/// Where the source clock starts, and what a reactor that never refreshes would
/// be stuck reporting forever.
constexpr auto StartTime = FastCache::TimePoint { 1s };

/// Where the source clock jumps to, from inside the reactor's own thread.
constexpr auto AdvancedTime = FastCache::TimePoint { 60s };

/// Move the source clock forward on one loop iteration, then read the cached
/// clock back on the next one.
///
/// Both halves run on the reactor thread, which is what makes the assertion
/// deterministic without a sleep: the advance happens *after* the iteration that
/// resumed this coroutine took its sample, so the only thing that can publish
/// the new value is the loop refreshing on the way round again.
///
/// Parameters are pointers rather than references because a coroutine copies
/// its parameters into the frame, and a reference parameter copies the
/// *reference* — leaving the frame pointing at whatever the caller's argument
/// was, which for a temporary is already gone by the first resumption
/// (cppcoreguidelines-avoid-reference-coroutine-parameters). These particular
/// arguments outlive the frame, but the rule is enforced rather than judged.
///
/// @param reactor  Reactor driving this coroutine; also the one being stopped.
/// @param source   Upstream clock the `CachedClock` under test samples.
/// @param observed Receives the cached clock's value as seen on the second pass.
FastCache::DetachedTask AdvanceThenObserve(FastCache::IReactor* reactor,
                                           FastCache::ManualClock* source,
                                           std::atomic<FastCache::Duration::rep>* observed)
{
    co_await FastCache::ResumeOn { *reactor };
    source->SetNow(AdvancedTime);

    co_await FastCache::ResumeOn { *reactor };
    observed->store(reactor->Clock().Now().time_since_epoch().count(), std::memory_order_release);

    reactor->Stop();
    co_return;
}

} // namespace

TEST_CASE("The platform reactor refreshes its clock every loop iteration", "[reactor][clock][cached-clock]")
{
    // Without this, nothing in the suite exercises IClock::Refresh() from a
    // loop: the reactor tests inject SteadyClock or ManualClock, for both of
    // which Refresh() is a no-op by design. Deleting the `_clock.Refresh()` call
    // from Epoll/Kqueue/IocpReactor::Run therefore used to leave every test
    // green while freezing the daemon's clock at the value CachedClock sampled
    // in its constructor — TTLs that never expire, `expired_unfetched` stuck at
    // zero, and a cache serving dead entries forever. That is the regression
    // this pins.
    FastCache::ManualClock source { StartTime };
    FastCache::CachedClock cached { source };
    PlatformReactor reactor { cached };

    REQUIRE(cached.Now() == StartTime);

    std::atomic<FastCache::Duration::rep> observed { 0 };
    AdvanceThenObserve(&reactor, &source, &observed);

    // The coroutine ran to its first ResumeOn on this thread, so a submission is
    // already queued and the loop below has work waiting the moment it starts.
    // The coroutine stops the reactor itself, so Run() returns on its own.
    reactor.Run();

    REQUIRE(FastCache::TimePoint { FastCache::Duration { observed.load(std::memory_order_acquire) } } == AdvancedTime);
    REQUIRE(cached.Now() == AdvancedTime);
}

TEST_CASE("A reactor given a plain SteadyClock is unaffected by the refresh call", "[reactor][clock][cached-clock]")
{
    // The refresh is unconditional — the reactor cannot know which IClock it was
    // handed — so the default no-op implementation has to be safe on the loop's
    // hot path as well as in isolation. A reactor that ran at all here is the
    // assertion; a Refresh() that disturbed a direct-reading clock would show up
    // as a non-monotonic sample.
    FastCache::SteadyClock clock;
    PlatformReactor reactor { clock };

    auto const before = clock.Now();

    std::atomic<FastCache::Duration::rep> observed { 0 };
    FastCache::ManualClock unused { StartTime }; // not read by this reactor
    AdvanceThenObserve(&reactor, &unused, &observed);
    reactor.Run();

    auto const sampled = FastCache::TimePoint { FastCache::Duration { observed.load(std::memory_order_acquire) } };
    REQUIRE(sampled >= before);
    REQUIRE(clock.Now() >= sampled);
}
