// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <coroutine>
#include <cstdint>
#include <deque>
#include <vector>

namespace FastCache
{

/// Deterministic single-threaded reactor for use in tests. Pairs with
/// ManualClock so timer-driven behaviour (TTLs, deadlines) is reproducible.
///
/// Run() processes ready submissions, fires any timers whose deadline has
/// elapsed, and exits when there is nothing left to do (no submissions, no
/// timers) — there is no "wait for something to happen" semantic. Tests
/// typically Submit() the entrypoint, advance the clock, and call Run()
/// (or Drain()) to make progress.
class TestReactor: public IReactor
{
  public:
    /// Construct a TestReactor that uses the given clock for deadline checks.
    /// @param clock Backing clock. Owned by the caller — typically a ManualClock.
    explicit TestReactor(IClock& clock) noexcept;

    void Run() override;
    void Stop() noexcept override;
    void Submit(std::coroutine_handle<> handle) override;
    void Schedule(TimePoint deadline, std::coroutine_handle<> handle) override;
    [[nodiscard]] IClock& Clock() noexcept override;

    /// Resume every ready submission and every timer whose deadline has
    /// elapsed exactly once; do not loop. Returns the number of resumes
    /// performed. Used by tests that want to drive one "tick" at a time.
    /// @return Number of coroutines resumed in this tick.
    std::size_t Tick();

    /// Repeatedly call Tick() until both queues are empty.
    /// @return Total number of resumes performed.
    std::size_t Drain();

    /// @return Number of pending submissions.
    [[nodiscard]] std::size_t PendingSubmissions() const noexcept;

    /// @return Number of pending timers.
    [[nodiscard]] std::size_t PendingTimers() const noexcept;

  private:
    struct ScheduledEntry
    {
        TimePoint deadline;
        std::uint64_t sequence; ///< Tie-breaker so equal deadlines fire FIFO.
        std::coroutine_handle<> handle;
    };

    void FireExpiredTimers();

    IClock& _clock;
    bool _stopped { false };
    std::uint64_t _nextSequence { 0 };
    std::deque<std::coroutine_handle<>> _ready;
    std::vector<ScheduledEntry> _timers; ///< Min-heap by (deadline, sequence).
};

} // namespace FastCache
