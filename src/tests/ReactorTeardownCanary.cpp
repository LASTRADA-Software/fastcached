// SPDX-License-Identifier: Apache-2.0
//
// A program that MUST die.
//
// `IReactor::TeardownIsSerialisedWithDispatch()` is the tripwire for
// [#668](https://github.com/LASTRADA-Software/fastcached/issues/668): an object a
// reactor owns is destroyed either on that reactor's worker thread or with the
// reactor stopped, because clearing a pending awaitable anywhere else races the
// completion dispatch.
//
// **This canary exists because that rule had exactly ONE observer in the entire CI
// matrix**, and it was not a required context. The assertion lived in
// `IocpSocket.cpp`, `Running()`/`IsOnWorkerThread()` existed on `IocpReactor` and
// nowhere else, and `Windows-cl-debug` is the only leg that both builds Debug and
// runs `ctest`. So a violation was invisible on four of five legs and intermittent
// on the fifth -- two greens and one red on identical source -- which is a gate that
// teaches people to re-run rather than to look. Making the question askable of every
// reactor is what lets THIS program run everywhere, and that is the whole fix to the
// ticket's second half: the leg does not need to gate, because the check no longer
// needs the leg.
//
// **It drives the predicate through a REAL reactor on a REAL second thread**, not a
// hand-built stub. A stub would prove the `assert` fires and say nothing about
// whether a live reactor reports its worker identity correctly -- which is the half
// that rots, and the half that was missing on three reactors until #668.
//
// Every self-diagnosed problem exits **0** and says which one it was, so the gate
// can tell "the reactor never started" and "the predicate was never false" apart
// from "the guard refused". None of them may read as the guard working: **not having
// run is not a pass.** That is also why `scripts/reactor-teardown-gate.cmake` reads
// the assertion's own words rather than inverting an exit code -- a bare `WILL_FAIL`
// would accept a segfault, a missing shared library and a failed thread spawn alike.
//
// Registered only for Debug configurations (`src/tests/CMakeLists.txt`), because
// `assert` compiles out everywhere else -- the same shape, and for the same reason,
// as `read-slot-guard-canary` and `iterator-debug-canary`.

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/ReactorTeardown.hpp>
#include <FastCache/Core/Clock.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <print>
#include <thread>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// A bounded wait that says what it waited for, per `.agent/rules/testing.md`.
/// @param predicate What must become true.
/// @param what Named in the diagnostic when it does not.
/// @return True when it became true within the bound.
template <typename Predicate>
[[nodiscard]] bool WaitFor(Predicate predicate, char const* what)
{
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    std::println(stderr, "canary: timed out waiting for {}", what);
    return false;
}

} // namespace

int main()
{
    SteadyClock clock;
    PlatformReactor reactor { clock };

    // The reactor runs on a thread of its own, which is the arrangement every
    // production owner has: `NodeIoLoop` starts one, and every teardown that has
    // ever tripped this assertion happened on some OTHER thread while it turned.
    std::atomic<bool> entered { false };
    std::thread worker { [&reactor, &entered, &clock] {
        // Keeps `Run()` from returning the instant it finds nothing to do, so the
        // window this canary is about stays open long enough to observe. A timer
        // rather than a sleep, because the reactor must be genuinely dequeuing --
        // a parked thread would report `Running()` while nothing is dispatching,
        // which is the state that would make this canary lie.
        reactor.Schedule(clock.Now() + 5s, {});
        entered.store(true, std::memory_order_release);
        reactor.Run();
    } };

    if (!WaitFor([&reactor] { return reactor.Running(); }, "the reactor to enter Run()"))
    {
        reactor.Stop();
        worker.join();
        std::println(stderr, "canary: the reactor never started; nothing was tested");
        return 0; // Not a pass. The gate refuses this by name.
    }
    (void) entered.load(std::memory_order_acquire);

    // The arrangement, stated as the two facts rather than assumed from them: a
    // reactor that is running, and a thread that is not its worker. If either is
    // false the assertion below could not fire for a reason that has nothing to do
    // with the guard, and reporting THAT as a pass is the failure this whole file
    // exists to avoid.
    if (reactor.IsOnWorkerThread())
    {
        reactor.Stop();
        worker.join();
        std::println(stderr, "canary: main thread reports as the worker; the arrangement is wrong");
        return 0;
    }
    if (reactor.TeardownIsSerialisedWithDispatch())
    {
        reactor.Stop();
        worker.join();
        std::println(stderr, "canary: predicate answered SAFE with the reactor running off-thread");
        return 0;
    }

    std::println(stderr, "canary: arrangement established; asserting teardown from a non-worker thread");
    std::fflush(stderr);

    // MUST die here.
    Detail::AssertTeardownIsSerialisedWithDispatch(reactor);

    // Only reachable when the guard has been deleted, weakened, or compiled out of a
    // configuration this test should not have been registered for.
    std::println(stderr, "canary: SURVIVED -- the teardown guard did not refuse");
    reactor.Stop();
    worker.join();
    return 0;
}
