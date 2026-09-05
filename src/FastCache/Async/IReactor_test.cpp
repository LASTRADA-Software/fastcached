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
#include <thread>

using namespace FastCache;

namespace
{

/// A reactor written the way a new backend would be: it implements the loop and
/// nothing else. It deliberately does NOT touch `ReactorWorkerIdentity` -- that is
/// the whole point, and adding it here would make this case prove nothing.
class BareReactor: public IReactor
{
  public:
    void Stop() noexcept override
    {
        _stop.store(true, std::memory_order_release);
    }

    void Submit(std::coroutine_handle<> /*handle*/) override {}
    void Schedule(TimePoint /*deadline*/, std::coroutine_handle<> /*handle*/) override {}

    [[nodiscard]] bool CancelPending(std::coroutine_handle<> /*handle*/) noexcept override
    {
        return false;
    }

    [[nodiscard]] IClock& Clock() noexcept override
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

  protected:
    void RunLoop() override
    {
        // Asked from inside the loop, on the loop's own thread.
        _sawRunning.store(Running(), std::memory_order_relaxed);
        _sawOnWorker.store(IsOnWorkerThread(), std::memory_order_relaxed);
        _sawTeardownSafe.store(TeardownIsSerialisedWithDispatch(), std::memory_order_relaxed);
        _entered.store(true, std::memory_order_release);
        while (!_stop.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

  private:
    SteadyClock _clock;
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
    CHECK_FALSE(reactor.Running());
    CHECK(reactor.TeardownIsSerialisedWithDispatch());

    std::thread worker { [&reactor] { reactor.Run(); } };
    while (!reactor.Entered())
        std::this_thread::yield();

    // The claim happened without `BareReactor` writing a line of it. This is the
    // assertion the whole obligation exists for.
    CHECK(reactor.SawRunning());
    CHECK(reactor.SawOnWorker());
    CHECK(reactor.SawTeardownSafe());

    // And from THIS thread, with that loop alive, the rule refuses -- which is the
    // violation `reactor-teardown-canary` drives to an assert. A reactor that had
    // forgotten to claim would answer `true` here, and that is the false-safe.
    CHECK(reactor.Running());
    CHECK_FALSE(reactor.IsOnWorkerThread());
    CHECK_FALSE(reactor.TeardownIsSerialisedWithDispatch());

    reactor.Stop();
    worker.join();

    // Claim released on the way out, or every later teardown would be refused
    // forever by a reactor that has finished.
    CHECK_FALSE(reactor.Running());
    CHECK(reactor.TeardownIsSerialisedWithDispatch());
}
