// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Platform/StopSignal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <future>
#include <optional>

#if !defined(_WIN32)
    #include <csignal>
#endif

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// How long a case waits for a wait that must end. Generous, because a waiter thread that
/// is slow to be scheduled is not the defect; one that never wakes is.
constexpr auto WakeBound = std::chrono::seconds { 10 };

/// Resumes whatever it is handed on the calling thread, at once.
///
/// The caller's side of the two hops, standing in for a reactor: these cases are about
/// the WAIT, and a reactor would add a second thread nobody is asking about.
class InlineExecutor final: public IExecutor
{
  public:
    void Submit(std::coroutine_handle<> handle) override
    {
        handle.resume();
    }

    void Submit(ParkedWork work) override
    {
        work.resume.resume();
    }
};

/// Await @p signal and hand the answer to @p answer.
/// @param signal What to wait for.
/// @param waiter Where the blocking wait runs.
/// @param resumeOn Where the answer is delivered.
/// @param answer Receives why the wait ended.
/// @return The task to start.
[[nodiscard]] Task<void> AwaitStop(IStopSignal* signal,
                                   IExecutor* waiter,
                                   IExecutor* resumeOn,
                                   std::promise<StopWake>* answer)
{
    answer->set_value(co_await signal->Stopped(waiter, resumeOn));
}

/// Wait for @p signal on a dedicated thread, doing @p meanwhile once the wait has started.
///
/// **The waiter thread is joined before the task is destroyed.** The answer is handed over
/// from inside the coroutine, so the test thread can see it while the waiter is still
/// finishing that same frame; destroying the frame then would race it. And a wait that did
/// not end in time is cancelled before the join, so a red case stays a red case rather
/// than a hang.
/// @param signal What to wait for.
/// @param meanwhile What to do once the wait is under way.
/// @return Why the wait ended, or nullopt when it did not end within the bound.
template <typename Meanwhile>
[[nodiscard]] std::optional<StopWake> WaitWhile(IStopSignal& signal, Meanwhile meanwhile)
{
    auto answer = std::promise<StopWake> {};
    auto answered = answer.get_future();
    auto resumeHere = InlineExecutor {};
    auto task = Task<void> {};
    auto result = std::optional<StopWake> {};
    {
        auto waiter = ThreadPoolExecutor { 1 };
        task = AwaitStop(&signal, &waiter, &resumeHere, &answer);
        task.Native().resume();
        meanwhile();
        if (answered.wait_for(WakeBound) == std::future_status::ready)
            result = answered.get();
        else
        {
            signal.Cancel();
            answered.wait();
        }
    }
    return result;
}

} // namespace

TEST_CASE("a cancel wakes a stop signal's waiter as cancelled", "[platform][stop-signal]")
{
    auto installed = InstallStopSignal();
    REQUIRE(installed.has_value());
    auto& signal = **installed;

    auto const wake = WaitWhile(signal, [&signal] { signal.Cancel(); });
    CHECK(wake == StopWake::Cancelled);

    // Sticky: a wait that starts after the cancel answers at once, rather than blocking a
    // thread for a session that has already ended.
    CHECK(WaitWhile(signal, [] {}) == StopWake::Cancelled);
}

TEST_CASE("a second stop signal is refused while the first is installed", "[platform][stop-signal]")
{
    // A disposition is process-wide, so a second install would silently take the first
    // one's signal away.
    {
        auto first = InstallStopSignal();
        REQUIRE(first.has_value());
        auto const second = InstallStopSignal();
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().contains("already installed"));
    }

    // And the refusal is about the one ALIVE, not about having ever installed one.
    CHECK(InstallStopSignal().has_value());
}

// POSIX only. The Windows twin would drive `GenerateConsoleCtrlEvent`, which reaches every
// process attached to the console -- ctest, its parallel siblings, the shell -- and a
// process started without one attached cannot receive it at all. Under ctest neither is a
// test of this code, so the Windows arm is covered by the two cases above and by nothing
// that would report a pass it did not earn.
#if !defined(_WIN32)

namespace
{

/// How often the disposition that was there BEFORE the stop signal has run.
std::atomic<int> priorHandlerRuns { 0 };

/// The disposition a case installs first, so "restored" is a claim about a handler it can
/// name and watch run, not about whatever the test runner happened to leave.
/// @param number The signal number; unused.
void PriorHandler(int number)
{
    static_cast<void>(number);
    priorHandlerRuns.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("SIGINT reaches a waiting stop signal and the previous disposition returns after it", "[platform][stop-signal]")
{
    struct sigaction original {};
    struct sigaction prior {};
    prior.sa_handler = &PriorHandler;
    static_cast<void>(::sigemptyset(&prior.sa_mask));
    REQUIRE(::sigaction(SIGINT, &prior, &original) == 0);
    priorHandlerRuns.store(0);

    {
        auto installed = InstallStopSignal();
        REQUIRE(installed.has_value());

        struct sigaction during {};
        static_cast<void>(::sigaction(SIGINT, nullptr, &during));
        CHECK(during.sa_handler != &PriorHandler);

        // The real route: a signal, a handler, a byte in a pipe, a waiter thread waking.
        auto const wake = WaitWhile(**installed, [] { static_cast<void>(::raise(SIGINT)); });
        CHECK(wake == StopWake::Stopped);
        CHECK(priorHandlerRuns.load() == 0);
    }

    // Gone, and what was there before is back -- not merely "something other than ours":
    // the prior handler RUNS when the signal comes again.
    struct sigaction after {};
    static_cast<void>(::sigaction(SIGINT, nullptr, &after));
    CHECK(after.sa_handler == &PriorHandler);
    static_cast<void>(::raise(SIGINT));
    CHECK(priorHandlerRuns.load() == 1);

    static_cast<void>(::sigaction(SIGINT, &original, nullptr));
}

#endif
