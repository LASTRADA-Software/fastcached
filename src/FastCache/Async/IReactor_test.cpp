// SPDX-License-Identifier: Apache-2.0
//
// The OBLIGATION half of #668's teardown rule.
//
// `IReactor` gained `Running()`, `IsOnWorkerThread()` and the
// `TeardownIsSerialisedWithDispatch()` derived from them, so every reactor can be
// asked the question that used to be answerable only of `IocpReactor`. That is the
// QUERY half, and on its own it is a false-safe: a reactor that never claimed the
// worker thread would answer "nobody is running" forever, the rule would be `true`
// unconditionally, and every guard built on it -- `reactor-teardown-canary` included
// -- would stay green while checking nothing.
//
// The four reactors in the tree each claimed correctly by hand. This case is about
// the FIFTH, which is what conventions lose to: it defines a reactor that implements
// nothing but `RunLoop()`, exactly as a new backend would, and asserts the claim
// happened anyway. If somebody makes `Run()` virtual again, or moves the claim back
// into the reactors, this goes red without anyone having to remember why.
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <coroutine>
#include <string>
#include <thread>
#include <tuple>

#include <tests/BoundedWait.hpp>

using namespace FastCache;

namespace
{

/// A reactor written the way a new backend would be: it implements the loop and
/// nothing else. It deliberately does NOT touch `ReactorWorkerIdentity` -- that is
/// the whole point, and adding it here would make this case prove nothing.
class BareReactor: public IReactor
{
  public:
    void stop() noexcept override
    {
        _stop.store(true, std::memory_order_release);
    }

    void submit(std::coroutine_handle<> /*handle*/) override {}
    void schedule(TimePoint /*deadline*/, std::coroutine_handle<> /*handle*/) override {}

    // The owning forms are pure on `IExecutor`/`IReactor`, so a fifth backend cannot
    // reach `Run()` without having been shown the question #1025 is about: what happens
    // to work it never resumes. This one parks nothing, so both are empty here -- and
    // that is a decision with a reason beside it rather than an omission.
    void submit(ParkedWork /*work*/) override {}
    void schedule(TimePoint /*deadline*/, ParkedWork /*work*/) override {}

    [[nodiscard]] bool cancelPending(std::coroutine_handle<> /*handle*/) noexcept override
    {
        return false;
    }

    [[nodiscard]] IClock& clock() noexcept override
    {
        return _clock;
    }

    /// What the loop observed about ITSELF, from inside, on its own thread. The
    /// whole point of the case is that a reactor implementing only `RunLoop()` sees
    /// these as true without having written a line of the claim.
    [[nodiscard]] bool SawRunning() const noexcept
    {
        return _sawRunning.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool SawOnWorker() const noexcept
    {
        return _sawOnWorker.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool SawTeardownSafe() const noexcept
    {
        return _sawTeardownSafe.load(std::memory_order_acquire);
    }

    /// Whether the loop has been entered, so the test can wait rather than sleep.
    [[nodiscard]] bool Entered() const noexcept
    {
        return _entered.load(std::memory_order_acquire);
    }

    /// @return The loop's wait for `Stop()`, asserted by the case once the loop's thread is joined.
    [[nodiscard]] Testing::OffThreadWaits const& Waits() const noexcept
    {
        return _waits;
    }

  protected:
    void RunLoop() override
    {
        // Asked from inside the loop, on the loop's own thread.
        _sawRunning.store(running(), std::memory_order_relaxed);
        _sawOnWorker.store(isOnWorkerThread(), std::memory_order_relaxed);
        _sawTeardownSafe.store(teardownIsSerialisedWithDispatch(), std::memory_order_relaxed);
        _entered.store(true, std::memory_order_release);
        // Bounded (#1446): a case that never stops the loop ends it red, not in a join that never returns.
        // Twice the guard, because the case's own wait for this loop to be entered is one guard long:
        // measured with both at one guard, a case whose wait ran out found the loop already given up
        // and gone, and three more assertions went red for a reason that was not theirs.
        std::ignore = _waits.WaitForFlag(
            "the case to stop the loop",
            _stop,
            [] { return std::string { "still running" }; },
            Testing::WaitOptions {
                .step = {}, .context = {}, .bound = 2 * Testing::WaitHangGuard, .rest = Testing::WaitRest });
    }

  private:
    SteadyClock _clock;
    Testing::OffThreadWaits _waits;
    std::atomic<bool> _stop { false };
    std::atomic<bool> _sawRunning { false };
    std::atomic<bool> _sawOnWorker { false };
    std::atomic<bool> _sawTeardownSafe { false };
    std::atomic<bool> _entered { false };
};

} // namespace

TEST_CASE("A reactor that implements only the loop still claims its worker thread", "[async][reactor][teardown]")
{
    BareReactor reactor;

    // Before anything runs, nothing is running -- and teardown is therefore safe
    // from any thread, which is the legitimate "stopped" arm of the rule.
    CHECK_FALSE(reactor.running());
    CHECK(reactor.teardownIsSerialisedWithDispatch());

    std::thread worker { [&reactor] { reactor.run(); } };
    // A CHECK, so a loop that never entered still reaches the Stop and the join below.
    CHECK(Testing::WaitUntil(
        "the loop to be entered on its thread", [&reactor] { return reactor.Entered(); }, [] { return std::string {}; }));

    // The claim happened without `BareReactor` writing a line of it. This is the
    // assertion the whole obligation exists for.
    CHECK(reactor.SawRunning());
    CHECK(reactor.SawOnWorker());
    CHECK(reactor.SawTeardownSafe());

    // And from THIS thread, with that loop alive, the rule refuses -- which is the
    // violation `reactor-teardown-canary` drives to an assert. A reactor that had
    // forgotten to claim would answer `true` here, and that is the false-safe.
    CHECK(reactor.running());
    CHECK_FALSE(reactor.isOnWorkerThread());
    CHECK_FALSE(reactor.teardownIsSerialisedWithDispatch());

    reactor.stop();
    worker.join();
    CHECK(reactor.Waits().AllReached());

    // Claim released on the way out, or every later teardown would be refused
    // forever by a reactor that has finished.
    CHECK_FALSE(reactor.running());
    CHECK(reactor.teardownIsSerialisedWithDispatch());
}
