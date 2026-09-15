// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_message.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// How long a test waits for another thread before it calls that thread hung.
///
/// A hang guard, not a race: a thread that is merely slow gets there inside it on any host that
/// runs these suites at all, and one that never does fails at it -- by name, through `WaitUntil`.
inline constexpr auto WaitHangGuard = std::chrono::milliseconds { 10'000 };

/// How long a wait's reported state must be quiet before its account stops calling it moving --
/// and how short a wait is too short for the account to say anything about that at all.
inline constexpr auto ReadingWindow = std::chrono::milliseconds { 1000 };
// A real timeout waits the whole guard, so the too-short reading is reachable only through a wait
// clock that lies (a seam counting instead of measuring) or a bound a case set below the window.
static_assert(WaitHangGuard > ReadingWindow);

/// How long a wait's poll sleeps after its step, unless the case says otherwise: short, so a wait
/// costs little past the moment it could have ended.
inline constexpr auto WaitRest = std::chrono::microseconds { 100 };

/// What a wait that ran out observed: the record `ReadWait` decides on.
struct WaitReadings
{
    std::chrono::milliseconds elapsed;         ///< Time the wait spent, on the clock it was bounded by.
    int changes;                               ///< How many times the reported state changed.
    std::chrono::milliseconds sinceLastChange; ///< Time since it last changed, or since the start, on that clock.
};

/// What a wait's account concludes. Private to the tests: never stored or sent.
enum class WaitReading : std::uint8_t
{
    TooShort,         ///< Shorter than the window: a stall and a slow thread cannot be told apart.
    Moving,           ///< The state changed within the last window.
    Stalled,          ///< The state was quiet for more than half of the wait.
    QuietAfterMoving, ///< It moved, then went quiet for less: a backed-off cycle or a stuck thread.
    Last,
};

/// One reading and the words the account prints for it.
struct WaitReadingRow
{
    WaitReading reading;   ///< The reading this row describes.
    std::string_view text; ///< What the account says.
};

inline constexpr EnumTable<WaitReading, WaitReadingRow> WaitReadingTexts { {
    { .reading = WaitReading::TooShort, .text = "INCONCLUSIVE: too short a wait to tell a stall from a slow thread" },
    { .reading = WaitReading::Moving, .text = "still MOVING at the guard: slow, or spinning" },
    { .reading = WaitReading::Stalled, .text = "STALLED: nothing it reports moved for most of the wait" },
    { .reading = WaitReading::QuietAfterMoving,
      .text = "INCONCLUSIVE: it moved, then went quiet -- a backed-off cycle and a stuck thread both read so" },
} };
static_assert(RowsInEnumeratorOrder(WaitReadingTexts, &WaitReadingRow::reading));

/// Decide what a wait that ran out says about the threads it waited on.
///
/// **Four outcomes, because two would each claim the cases between them** (#1433). A wait shorter
/// than the window cannot tell a stall from a slow thread at all, and a state that moved and then
/// went quiet for part of the wait is what a backed-off cycle and a stuck thread BOTH look like. A
/// pure function over the record, so every outcome is driven by a case rather than by a hung run.
/// @param readings What the wait observed.
/// @return The reading.
[[nodiscard]] constexpr WaitReading ReadWait(WaitReadings const& readings) noexcept
{
    if (readings.elapsed < ReadingWindow)
        return WaitReading::TooShort;
    if (readings.sinceLastChange <= ReadingWindow)
        return WaitReading::Moving;
    if (readings.sinceLastChange * 2 > readings.elapsed)
        return WaitReading::Stalled;
    return WaitReading::QuietAfterMoving;
}

/// The drain seam a test wait runs on: time is the host's monotonic clock, and each poll runs one
/// step of the case's own -- a reactor tick, a `Drain()`, nothing -- then rests briefly so the thread
/// the case waits on can run.
///
/// The requested poll is not a duration here: the cadence is one step plus the rest.
///
/// **A step that throws is kept, not swallowed and not fatal.** `Sleep` is `noexcept`, so an
/// exception escaping it would end the whole test binary; the wait stops at it instead, and
/// `WaitUntilOutcome` rethrows it to the case, where Catch2 reports it as that case's failure.
class PollingDrainWait final: public IDrainWait
{
  public:
    /// @param step What each poll runs; empty runs nothing.
    /// @param rest How long each poll then sleeps -- longer where the host's timer is coarse.
    explicit PollingDrainWait(std::function<void()> step = {}, std::chrono::microseconds rest = WaitRest):
        _step { std::move(step) },
        _rest { rest }
    {
    }

    [[nodiscard]] TimePoint Now() const noexcept override
    {
        return DefaultDrainWait().Now();
    }

    void Sleep(std::chrono::milliseconds /*requested*/) noexcept override
    {
        if (_step)
        {
            try
            {
                _step();
            }
            catch (...)
            {
                _thrown = std::current_exception();
            }
        }
        ++_polls;
        std::this_thread::sleep_for(_rest);
    }

    /// @return How many polls the wait has run.
    [[nodiscard]] std::size_t Polls() const noexcept
    {
        return _polls;
    }

    /// @return What a step threw, or null.
    [[nodiscard]] std::exception_ptr Thrown() const noexcept
    {
        return _thrown;
    }

  private:
    std::function<void()> _step;
    std::chrono::microseconds _rest;
    std::size_t _polls { 0 };
    std::exception_ptr _thrown;
};

/// How a wait waits, beyond what it waits for.
struct WaitOptions
{
    std::function<void()> step;                      ///< Run once per poll (see `PollingDrainWait`); empty: none.
    std::function<std::string()> context;            ///< Printed once in an account and never tracked for change.
    std::chrono::milliseconds bound = WaitHangGuard; ///< Real time the wait may take.
    /// How long each poll sleeps after its step: longer where a busy poll would perturb what the case measures.
    std::chrono::microseconds rest = WaitRest;
};

/// What a wait found: whether it got there, and when it did not, the account of why.
struct WaitOutcome
{
    bool reached;                      ///< Whether the predicate held within the bound.
    std::chrono::milliseconds elapsed; ///< Time the wait took, on the clock it was bounded by.
    std::string account; ///< Empty when reached; otherwise what was waited for, what was found, and its reading.
};

/// The words that differ between a wait on a thread and a wait on a reactor: who gave up, on which
/// clock, and in what steps.
struct WaitVoice
{
    std::string_view waiter; ///< The helper that gave up.
    std::string_view clock;  ///< The clock its bound was read from, as the account phrases it.
    std::string_view steps;  ///< What it counts once per look.
};

/// `WaitUntil`'s voice: real time, in polls.
inline constexpr WaitVoice ThreadWaitVoice { .waiter = "WaitUntil", .clock = "of real time", .steps = "poll(s)" };

/// `AwaitUntil`'s voice: the reactor's clock, in turns.
inline constexpr WaitVoice ReactorWaitVoice { .waiter = "AwaitUntil",
                                              .clock = "on the reactor's clock",
                                              .steps = "turn(s)" };

/// The account of a wait that ran out, one wording for both helpers.
/// @param voice    Who gave up, on which clock, in what steps.
/// @param what     What was waited for, in words.
/// @param readings What the wait observed.
/// @param steps    How many looks it took.
/// @param seen     The state it last read.
/// @param context  The untracked context, when the case gave one.
/// @return The account.
[[nodiscard]] inline std::string GaveUpAccount(WaitVoice voice,
                                               std::string_view what,
                                               WaitReadings const& readings,
                                               std::size_t steps,
                                               std::string_view seen,
                                               std::function<std::string()> const& context)
{
    return std::format("{} gave up waiting for {} after {} ms {} and {} {}. State at the end: {}{}{}. It changed {} "
                       "time(s), and nothing changed in the last {} ms: {}.",
                       voice.waiter,
                       what,
                       readings.elapsed.count(),
                       voice.clock,
                       steps,
                       voice.steps,
                       seen,
                       context ? "; " : "",
                       context ? context() : std::string {},
                       readings.changes,
                       readings.sinceLastChange.count(),
                       WaitReadingTexts.at(static_cast<std::size_t>(ReadWait(readings))).text);
}

/// Wait until @p reached holds, for at most `options.bound` of REAL time, and say what was found.
///
/// **Bounded by time on a monotonic clock, never by a count of polls** (#1433, #1446). What a test
/// waits for is done by ANOTHER thread, and a count of requested sleeps is a race a loaded host loses
/// and a coarse timer stretches. Through `DrainWithin`, the tree's one bounded wait, which measures.
///
/// **A wait that ran out says what it waited for and what it found**: the real time and polls it
/// spent, @p state at the end, `options.context`, and how long @p state had been quiet, read as
/// MOVING, STALLED, or INCONCLUSIVE where the numbers cannot separate those.
///
/// Touches no Catch2 state, so any thread may run it; `WaitUntil` is the spelling for the case's own
/// thread and `OffThreadWaits` the one for a helper thread. A step that throws is rethrown here.
/// @param what    What the case waits for, in words.
/// @param reached True once it has happened.
/// @param state   What the threads involved have done so far, in words.
/// @param options The per-poll step, the untracked context, and the bound.
/// @return Whether @p reached held within the bound, and the account when it did not.
template <std::predicate Predicate, typename State>
[[nodiscard]] WaitOutcome WaitUntilOutcome(std::string_view what,
                                           Predicate reached,
                                           State state,
                                           WaitOptions const& options = {})
{
    PollingDrainWait polling { options.step, options.rest };
    // The account is timed on the host's clock directly, never through the seam the wait ran on: a
    // seam that counted instead of measuring then shows as a wait too short to read.
    auto const started = DefaultDrainWait().Now();
    std::string seen = state();
    auto lastChange = started;
    auto changes = 0;
    auto const busy = [&] {
        if (polling.Thrown() != nullptr || reached())
            return false;
        if (std::string now = state(); now != seen)
        {
            seen = std::move(now);
            lastChange = DefaultDrainWait().Now();
            ++changes;
        }
        return true;
    };
    auto const result =
        DrainWithin(busy, DrainBound { .ceiling = options.bound, .poll = std::chrono::milliseconds { 1 } }, polling);
    if (auto const thrown = polling.Thrown(); thrown != nullptr)
        std::rethrow_exception(thrown);
    auto const ended = DefaultDrainWait().Now();
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ended - started);
    if (result == DrainResult::Drained)
        return WaitOutcome { .reached = true, .elapsed = elapsed, .account = {} };
    auto const readings = WaitReadings {
        .elapsed = elapsed,
        .changes = changes,
        .sinceLastChange = std::chrono::duration_cast<std::chrono::milliseconds>(ended - lastChange),
    };
    return WaitOutcome {
        .reached = false,
        .elapsed = elapsed,
        .account = GaveUpAccount(ThreadWaitVoice, what, readings, polling.Polls(), seen, options.context),
    };
}

/// Hand a wait's outcome to the case: its account, when it ran out, attached to the case's next assertion.
///
/// `UNSCOPED_INFO`, since a scoped message would die here. Catch2 clears that message at the next
/// assertion whether it passes or fails, so assert the wait that ran out FIRST. The case's thread only.
/// @param outcome What a `WaitUntilOutcome` found.
/// @return Whether that wait reached what it waited for.
[[nodiscard]] inline bool Reached(WaitOutcome const& outcome)
{
    if (!outcome.reached)
        UNSCOPED_INFO(outcome.account);
    return outcome.reached;
}

/// Wait, on the case's own thread, until @p reached holds, for at most `options.bound` of REAL time.
///
/// `WaitUntilOutcome`, handed to the case through `Reached`.
/// @param what    What the case waits for, in words.
/// @param reached True once it has happened.
/// @param state   What the threads involved have done so far, in words.
/// @param options The per-poll step, the untracked context, the bound and the rest.
/// @return Whether @p reached held within the bound.
template <std::predicate Predicate, typename State>
[[nodiscard]] bool WaitUntil(std::string_view what, Predicate reached, State state, WaitOptions const& options = {})
{
    return Reached(WaitUntilOutcome(what, std::move(reached), std::move(state), options));
}

/// Wait, on the case's own thread, until @p reached holds, draining @p reactor once per poll.
///
/// For a case waiting on work another thread hands back THROUGH a reactor the case drives: the
/// hand-back runs only when the reactor does, so the drain is the step.
/// @param reactor Drained once per poll.
/// @param what    What the case waits for, in words.
/// @param reached True once it has happened.
/// @param state   What the threads involved have done so far, in words.
/// @param bound   Real time the wait may take.
/// @return Whether @p reached held within the bound.
template <typename Reactor, std::predicate Predicate, typename State>
[[nodiscard]] bool DrainUntil(
    Reactor& reactor, std::string_view what, Predicate reached, State state, std::chrono::milliseconds bound = WaitHangGuard)
{
    return WaitUntil(what,
                     std::move(reached),
                     std::move(state),
                     WaitOptions {
                         .step = [&reactor] { std::ignore = reactor.Drain(); },
                         .context = {},
                         .bound = bound,
                         .rest = WaitRest,
                     });
}

/// How a coroutine waits on its reactor, beyond what it waits for.
struct ReactorWaitOptions
{
    std::function<std::string()> context; ///< Printed once in an account and never tracked for change.
    Duration bound = std::chrono::duration_cast<Duration>(WaitHangGuard); ///< Reactor-clock time the wait may take.
    /// Zero yields one reactor turn between looks; anything else sleeps that long on the reactor's clock.
    Duration rest = Duration::zero();
};

/// Await, on @p reactor, until @p reached holds, for at most `options.bound` of the REACTOR's clock, and
/// say what was found (#1453).
///
/// **For a coroutine, which must not block the thread it runs on**: `WaitUntil` sleeps its thread, and a
/// coroutine on a reactor that did so would stall the very loop that has to make @p reached true. So
/// this parks between looks -- `ResumeOn` or `SleepFor`, both through the reactor, so a teardown frees
/// the chain from its root (`Detail::UnownedRootOf`, #1025) -- and reads its bound off `reactor->Clock()`,
/// the clock its sleeps already use: a `ManualClock` case bounds it on manual time.
///
/// The account and its reading are `WaitUntilOutcome`'s, in `ReactorWaitVoice`, with durations from the
/// reactor's clock. Touches no Catch2 state: keep the outcome with `OffThreadWaits::Keep` and assert
/// `AllReached()` on the case's thread.
///
/// **A wait that ran out must end what it was guarding.** The code after it assumed @p reached, so the
/// caller returns -- or releases what it holds -- instead of carrying on.
/// @param reactor The reactor the awaiting coroutine runs on; a pointer, as every coroutine here takes one:
///                a reference parameter is a dangling reference waiting for a caller that outlives it less.
/// @param what    What is waited for, in words.
/// @param reached True once it has happened.
/// @param state   What the parties involved have done so far, in words.
/// @param options The untracked context, the bound and the rest.
/// @return Whether @p reached held within the bound, and the account when it did not.
template <std::predicate Predicate, typename State>
[[nodiscard]] Task<WaitOutcome> AwaitUntil(
    IReactor* reactor, std::string what, Predicate reached, State state, ReactorWaitOptions options = {})
{
    auto const& clock = reactor->Clock();
    auto const started = clock.Now();
    std::string seen = state();
    auto lastChange = started;
    auto changes = 0;
    std::size_t turns = 0;
    while (!reached())
    {
        auto const now = clock.Now();
        if (now - started >= options.bound)
        {
            auto const readings = WaitReadings {
                .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started),
                .changes = changes,
                .sinceLastChange = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChange),
            };
            co_return WaitOutcome {
                .reached = false,
                .elapsed = readings.elapsed,
                .account = GaveUpAccount(ReactorWaitVoice, what, readings, turns, seen, options.context),
            };
        }
        if (std::string current = state(); current != seen)
        {
            seen = std::move(current);
            lastChange = now;
            ++changes;
        }
        ++turns;
        if (options.rest == Duration::zero())
            co_await ResumeOn { *reactor };
        else
            co_await SleepFor(*reactor, options.rest);
    }
    co_return WaitOutcome {
        .reached = true,
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - started),
        .account = {},
    };
}

/// Waits run somewhere other than the case's own thread -- a helper thread, or a coroutine on a reactor --
/// with the accounts of any that ran out carried to the case.
///
/// **Catch2's assertions and messages belong to the thread running the case**: a message from a
/// helper thread races the case's own Catch2 state, which inside the TSan scope is a reported race and
/// outside it a torn message. So a helper thread waits through `Wait`, and a coroutine keeps what
/// `AwaitUntil` found through `Keep`, neither touching Catch2 state; the case asserts `AllReached()` once
/// that thread is joined or that reactor has stopped -- as its FIRST assertion after, because a wait that
/// ran out is the reason every later assertion would fail, and Catch2 clears the accounts at the next
/// assertion.
///
/// A step that throws on a helper thread ends the binary, as any exception escaping a thread does: give
/// a wait there no step that can throw.
class OffThreadWaits
{
  public:
    /// `WaitUntilOutcome`, keeping the account when the wait ran out. Any thread.
    /// @param what    What the thread waits for, in words.
    /// @param reached True once it has happened.
    /// @param state   What the threads involved have done so far, in words.
    /// @param options The per-poll step, the untracked context, and the bound.
    /// @return Whether @p reached held within the bound.
    template <std::predicate Predicate, typename State>
    [[nodiscard]] bool Wait(std::string_view what, Predicate reached, State state, WaitOptions const& options = {})
    {
        return Keep(WaitUntilOutcome(what, std::move(reached), std::move(state), options));
    }

    /// Keep what a wait already run found -- `AwaitUntil` on a reactor -- with its account when it ran out. Any thread.
    /// @param outcome What the wait found.
    /// @return Whether that wait reached what it waited for.
    [[nodiscard]] bool Keep(WaitOutcome outcome)
    {
        if (!outcome.reached)
        {
            std::scoped_lock const lock { _mutex };
            _accounts.push_back(std::move(outcome.account));
        }
        return outcome.reached;
    }

    /// `Wait` for a flag another thread sets.
    /// @param what    What the flag says has happened, in words.
    /// @param flag    Set once it has.
    /// @param state   What the threads involved have done so far, in words.
    /// @param options The per-poll step, the untracked context, and the bound.
    /// @return Whether @p flag was set within the bound.
    template <typename State>
    [[nodiscard]] bool WaitForFlag(std::string_view what,
                                   std::atomic<bool> const& flag,
                                   State state,
                                   WaitOptions const& options = {})
    {
        return Wait(what, [&flag] { return flag.load(std::memory_order_acquire); }, std::move(state), options);
    }

    /// Attach every kept account to the case's next assertion. The case's thread, after the join.
    /// @return Whether every wait reached what it waited for.
    [[nodiscard]] bool AllReached() const
    {
        std::scoped_lock const lock { _mutex };
        for (auto const& account: _accounts)
            UNSCOPED_INFO(account);
        return _accounts.empty();
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::string> _accounts;
};

} // namespace FastCache::Testing
