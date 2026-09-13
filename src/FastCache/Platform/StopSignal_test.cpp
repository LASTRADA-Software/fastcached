// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Platform/StopSignal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <ranges>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <csignal>

    #include <fcntl.h>
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
/// @param bound How long the wait may take before it is cancelled.
/// @return Why the wait ended, or nullopt when it did not end within the bound.
template <typename Meanwhile>
[[nodiscard]] std::optional<StopWake> WaitWhile(IStopSignal& signal,
                                                Meanwhile meanwhile,
                                                std::chrono::milliseconds bound = WakeBound)
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
        if (answered.wait_for(bound) == std::future_status::ready)
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

TEST_CASE("cancels past the pipe's capacity stay one cancel, and do not end the process", "[platform][stop-signal]")
{
    // A cancel pipe is never read, so enough cancels fill it and the next write answers `EAGAIN`.
    // WHAT DISTINGUISHES: that answer is a wake already pending, not a failure -- a stop signal
    // treating it as one ends this process here. The count is past every host's pipe capacity
    // (64 KiB on Linux and on macOS, where a pipe can grow to it), so the full-pipe answer is
    // reached rather than hoped for.
    constexpr auto PastAnyPipeCapacity = std::size_t { 1 } << 17U;
    auto installed = InstallStopSignal();
    REQUIRE(installed.has_value());
    auto& signal = **installed;

    auto cancels = std::size_t { 0 };
    for ([[maybe_unused]] auto const each: std::views::iota(std::size_t { 0 }, PastAnyPipeCapacity))
    {
        signal.Cancel();
        ++cancels;
    }
    CHECK(cancels == PastAnyPipeCapacity);
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

TEST_CASE("the stop handler's end outlives an uninstall and the next install reuses it", "[platform][stop-signal]")
{
    // A handler already running when a signal is uninstalled may still write to the end it loaded,
    // so that end is created once and never closed: still open after the uninstall, and the one
    // the next install hands the handler again.
    auto first = InstallStopSignal();
    REQUIRE(first.has_value());
    auto const end = StopSignalHandlerEnd();
    REQUIRE(end != -1);
    first->reset();

#if defined(_WIN32)
    auto flags = DWORD { 0 };
    CHECK(::GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(end)), &flags) != FALSE);
#else
    CHECK(::fcntl(static_cast<int>(end), F_GETFD) != -1);
#endif

    auto second = InstallStopSignal();
    REQUIRE(second.has_value());
    CHECK(StopSignalHandlerEnd() == end);
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

TEST_CASE("SIGINTs past the stop pipe's capacity stay one stop, and do not end the process", "[platform][stop-signal]")
{
    // The handler's own arm of the full-pipe answer. The stop pipe is drained only at an install,
    // so a burst of SIGINT fills it; WHAT DISTINGUISHES is that the handler takes the full pipe as
    // a stop already pending -- one that took it as a failure ends the process inside the burst.
    constexpr auto PastAnyPipeCapacity = std::size_t { 1 } << 17U;
    auto installed = InstallStopSignal();
    REQUIRE(installed.has_value());

    auto raised = std::size_t { 0 };
    for ([[maybe_unused]] auto const each: std::views::iota(std::size_t { 0 }, PastAnyPipeCapacity))
    {
        static_cast<void>(::raise(SIGINT));
        ++raised;
    }
    CHECK(raised == PastAnyPipeCapacity);
    CHECK(WaitWhile(**installed, [] {}) == StopWake::Stopped);
}

TEST_CASE("a stop an earlier install heard is not the next install's, and the next still hears its own",
          "[platform][stop-signal]")
{
    // The handler's pipe outlives each install, so the byte a stop left in it must not reach the
    // next one as a stop nobody made -- and a stop made to the next one still arrives.
    struct sigaction original {};
    struct sigaction prior {};
    prior.sa_handler = &PriorHandler;
    static_cast<void>(::sigemptyset(&prior.sa_mask));
    REQUIRE(::sigaction(SIGINT, &prior, &original) == 0);

    {
        auto first = InstallStopSignal();
        REQUIRE(first.has_value());
        CHECK(WaitWhile(**first, [] { static_cast<void>(::raise(SIGINT)); }) == StopWake::Stopped);
    }
    {
        auto second = InstallStopSignal();
        REQUIRE(second.has_value());
        // Nothing asked this one to stop: its wait does not end on its own. A short bound is
        // enough, since a leftover byte would answer at once; it can only miss the defect, never
        // invent it.
        CHECK_FALSE(WaitWhile(**second, [] {}, std::chrono::milliseconds { 200 }).has_value());
    }
    {
        // A third, because the wait above was cancelled at its bound and a cancel is sticky.
        auto third = InstallStopSignal();
        REQUIRE(third.has_value());
        CHECK(WaitWhile(**third, [] { static_cast<void>(::raise(SIGINT)); }) == StopWake::Stopped);
    }

    static_cast<void>(::sigaction(SIGINT, &original, nullptr));
}

TEST_CASE("a SIGINT this process inherited as ignored stays ignored, and its stop signal never fires",
          "[platform][stop-signal]")
{
    // A background job or `nohup` starts with SIGINT ignored. Catching it would let a Ctrl-C meant
    // for the foreground end this process. So nothing is installed: the disposition is still
    // `SIG_IGN`, a SIGINT changes nothing, and only a cancel ends the wait -- a signal that can
    // never fire, not a failure to install one.
    struct sigaction original {};
    struct sigaction ignored {};
    ignored.sa_handler = SIG_IGN;
    static_cast<void>(::sigemptyset(&ignored.sa_mask));
    REQUIRE(::sigaction(SIGINT, &ignored, &original) == 0);

    {
        auto installed = InstallStopSignal();
        REQUIRE(installed.has_value());

        struct sigaction during {};
        static_cast<void>(::sigaction(SIGINT, nullptr, &during));
        CHECK(during.sa_handler == SIG_IGN);

        CHECK_FALSE(WaitWhile(
                        **installed, [] { static_cast<void>(::raise(SIGINT)); }, std::chrono::milliseconds { 200 })
                        .has_value());
        CHECK(WaitWhile(**installed, [&installed] { (*installed)->Cancel(); }) == StopWake::Cancelled);
    }

    struct sigaction after {};
    static_cast<void>(::sigaction(SIGINT, nullptr, &after));
    CHECK(after.sa_handler == SIG_IGN);
    static_cast<void>(::sigaction(SIGINT, &original, nullptr));
}

#endif
