// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <core/platform/Clock.hpp>

namespace FastCache
{

/// How a bounded drain spends the gap between two tests of its predicate.
///
/// Two operations rather than one, and both of them ambient: the wait has to
/// *block* (so an `core::platform::IClock` alone cannot serve), and it has to know how much
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
    [[nodiscard]] virtual core::platform::SteadyTimePoint Now() const noexcept = 0;

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
    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept override
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
/// `core::platform::defaultSystemWallClock()` in `Clock.hpp`; tests pass their own.
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

/// The exit status of a process that ended because a bounded drain ran out -- the ONE value
/// every abandoned drain in this tree exits with, the compile drain's (#239) included.
///
/// Distinct from every ordinary failure, so a supervisor's log tells "stopped with work still
/// running" from a crash. 75 is `EX_TEMPFAIL` from `sysexits.h` -- not a standard this project
/// otherwise uses, but the closest thing to a shared vocabulary for "this was not clean, and
/// retrying is reasonable", and unambiguous beside a compiler's own exit codes.
inline constexpr int AbandonedDrainExitCode = 75;

/// What a teardown does with work still outstanding when `DrainWithin` answers `Ceiling`.
///
/// **There is no safe way to carry on**, which is why this is a seam rather than a branch.
/// Outstanding work borrows members of the object being torn down, so returning frees them
/// underneath it, and destroying the work instead frees code another thread is still running.
/// Production therefore ends the process -- `std::_Exit`, which runs no destructor that could
/// reach either -- the shape `CompileCapacity::Drain` gives an abandoned compile drain (#239).
/// A test supplies an implementation that RETURNS, to watch the ceiling being reached; what
/// the caller does then is part of the caller's own contract.
class IDrainAbandonment
{
  public:
    IDrainAbandonment() = default;
    IDrainAbandonment(IDrainAbandonment const&) = delete;
    IDrainAbandonment(IDrainAbandonment&&) = delete;
    IDrainAbandonment& operator=(IDrainAbandonment const&) = delete;
    IDrainAbandonment& operator=(IDrainAbandonment&&) = delete;
    virtual ~IDrainAbandonment() = default;

    /// Abandon the work a drain gave up on. Production does not return.
    virtual void Abandon() noexcept = 0;
};

/// Production `IDrainAbandonment`: says so on stderr, then ends the process with
/// `AbandonedDrainExitCode`.
///
/// **The line is not duplicate logging, and deleting it as such is the hazard.** Its readers are
/// two, and only one of them is served today:
///
///   * The OPERATOR gets the drain's own `ILogger` line, which names the outstanding count and the
///     bound. That is the diagnostic #239 added.
///   * The SUPERVISOR -- systemd, the SCM, `ctest`, a shell -- gets nothing. It sees status 75 and
///     no text, which is the very shape #239 exists to remove: *an unbounded drain hands the ending
///     to the supervisor, which answers `SIGKILL` with no diagnostic.* Ending ourselves with
///     nothing on stderr is that defect wearing our own name.
///
/// The second reader is why this is here and why it stays in PRODUCTION rather than being a test
/// affordance. It is also what makes an abandoned TEST binary legible at all: a test's `ILogger` is
/// a `CapturingLogger` nobody reads, so before this line a case that reached the ceiling did not
/// fail -- it vanished, taking every case after it in that process, with two lines of banner and no
/// explanation anywhere a person looks (#297).
///
/// **The trailing newline is load-bearing.** `std::_Exit` runs no `atexit` handler and flushes
/// nothing, deliberately -- that is the whole reason it is used here -- so delivery rests on a
/// property of the stream rather than of this code. C11 7.21.3p7 requires stderr to be *not fully
/// buffered*, which permits unbuffered OR line buffered, and a message ending in `\n` is delivered
/// under either. Measured separately, and NOT the same claim: on glibc the text survives even
/// without the newline, to a pipe and to a file, because glibc leaves stderr unbuffered -- that is
/// this platform, not the guarantee. So do not drop the newline, do not "fix" this with an
/// `fflush` that cannot matter, and do not move the write after the `_Exit` it precedes.
class EndProcessOnAbandonedDrain final: public IDrainAbandonment
{
  public:
    [[noreturn]] void Abandon() noexcept override
    {
        std::fputs("fastcached: a bounded drain gave up with work still running and is ending this process "
                   "(status 75). If this is a TEST binary, the case did not fail -- it vanished, and so did every "
                   "case after it: construct with an IDrainAbandonment that RETURNS (see Core/BoundedDrain.hpp).\n",
                   stderr);

        // `_Exit`, not `exit`: static destructors would run the same teardown this is avoiding.
        std::_Exit(AbandonedDrainExitCode);
    }
};

/// Process-singleton `EndProcessOnAbandonedDrain`. Mirrors `DefaultDrainWait()`; tests pass
/// their own.
/// @return Reference to a singleton EndProcessOnAbandonedDrain with static storage.
[[nodiscard]] inline IDrainAbandonment& DefaultDrainAbandonment() noexcept
{
    static EndProcessOnAbandonedDrain instance;
    return instance;
}

} // namespace FastCache
