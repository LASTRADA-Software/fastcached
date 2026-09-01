// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <thread>

namespace FastCache
{

/// How a bounded drain spends the gap between two tests of its predicate.
///
/// Two operations rather than one, and both of them ambient: the wait has to
/// *block* (so an `IClock` alone cannot serve), and it has to know how much
/// real time that blocking actually cost (so a bare `sleep_for` cannot either).
/// Keeping them on one seam is what makes the bound testable — see
/// `DrainWithin` for why a test that cannot separate the two is a test that
/// cannot see the defect this exists to prevent.
class IDrainWait
{
  public:
    IDrainWait() = default;
    IDrainWait(IDrainWait const&) = delete;
    IDrainWait(IDrainWait&&) = delete;
    IDrainWait& operator=(IDrainWait const&) = delete;
    IDrainWait& operator=(IDrainWait&&) = delete;
    virtual ~IDrainWait() = default;

    /// @return Current steady-clock time. Must be monotonic and thread-safe.
    [[nodiscard]] virtual TimePoint Now() const noexcept = 0;

    /// Block the calling thread for approximately @p requested.
    ///
    /// "Approximately" is the whole point: an implementation may sleep for
    /// considerably longer than asked, and a caller may not assume otherwise.
    /// @param requested Cadence the caller would like between two polls.
    virtual void Sleep(std::chrono::milliseconds requested) noexcept = 0;
};

/// Production `IDrainWait`: `std::this_thread::sleep_for` over
/// `std::chrono::steady_clock`.
///
/// On Windows the scheduler's timer granularity is ~15 ms, so a 5 ms request
/// here routinely costs three times that. That is not a bug in this class; it
/// is the reason `DrainWithin` measures instead of counting.
class ThreadDrainWait final: public IDrainWait
{
  public:
    [[nodiscard]] TimePoint Now() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }

    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        std::this_thread::sleep_for(requested);
    }
};

/// Process-singleton `ThreadDrainWait`, so a production shutdown path does not
/// have to carry a seam it has no reason to vary. Mirrors
/// `DefaultSystemWallClock()` in `Clock.hpp`; tests pass their own.
/// @return Reference to a singleton ThreadDrainWait with static storage.
[[nodiscard]] inline IDrainWait& DefaultDrainWait() noexcept
{
    static ThreadDrainWait instance;
    return instance;
}

/// How long a bounded drain waits, and how often it looks.
///
/// A struct rather than two parameters because both are durations of the same
/// type, so a transposed pair would compile and would turn a five-second
/// ceiling into a ten-millisecond one. The defaults are what every shutdown
/// path in this tree wants; a caller states a field only to depart from them.
struct DrainBound
{
    /// Upper bound on how long the drain keeps waiting.
    std::chrono::milliseconds ceiling { std::chrono::seconds { 5 } };

    /// Requested gap between two tests of the predicate.
    std::chrono::milliseconds poll { 10 };
};

/// What a bounded drain found when it stopped waiting. Two outcomes, named,
/// because "the work finished" and "the ceiling ran out" are the two halves of
/// a shutdown diagnostic and a caller that cannot tell them apart logs the
/// wrong one.
enum class DrainResult : std::uint8_t
{
    /// The predicate reported nothing outstanding.
    Drained,

    /// The ceiling elapsed with work still outstanding.
    Ceiling,
};

/// Wait until nothing is outstanding, or until the ceiling elapses.
///
/// The one bounded drain in this tree: every shutdown path that waits on
/// detached work goes through it. Such work — a reactor loop, a connection
/// coroutine, a peer sender, an admin request — borrows members held on the
/// object being torn down, so the teardown must not return while one of them is
/// still running; and it must not wait forever either, because a stuck peer
/// would then turn a stop into a hang and hand the ending to a supervisor that
/// answers `SIGKILL` with no diagnostic.
///
/// **The ceiling is measured, never counted.** Accumulating the *requested*
/// poll — `waited += poll` — states a bound and enforces `poll_actual / poll`
/// times it: on Windows, where a 5 ms request costs ~15 ms, a loop that reads
/// as a five-second ceiling waits fifteen. That defect shipped twice here, and
/// both copies carried a comment citing a correct implementation they had
/// reimplemented rather than called. Which is why this is a function.
///
/// The predicate is tested first, so a ceiling of zero is one look and no
/// sleep, and work that finishes exactly at the deadline reports `Drained`.
/// The bound is on when this stops *asking*: the final sleep may overrun it by
/// up to one poll plus whatever the platform's timer granularity adds, so a
/// caller wanting a hard real-time guarantee does not want this function.
///
/// @param busy   Returns true while work is still outstanding. Called from the
///               draining thread, so it must be safe against whatever the
///               outstanding work touches — in practice an acquire load.
/// @param bound  Ceiling and poll cadence; see `DrainBound`.
/// @param wait   Where blocking and time come from. Defaults to the process
///               `ThreadDrainWait`; tests inject one that can make a request
///               and its cost disagree.
/// @return `Drained` if `busy` reported false, `Ceiling` if it did not in time.
template <std::predicate Predicate>
[[nodiscard]] DrainResult DrainWithin(Predicate busy, DrainBound bound = {}, IDrainWait& wait = DefaultDrainWait())
{
    auto const deadline = wait.Now() + bound.ceiling;
    while (busy())
    {
        if (wait.Now() >= deadline)
            return DrainResult::Ceiling;
        wait.Sleep(bound.poll);
    }
    return DrainResult::Drained;
}

} // namespace FastCache
