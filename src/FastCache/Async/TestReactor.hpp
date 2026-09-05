// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/ReactorWorkerIdentity.hpp>
#include <FastCache/Core/Clock.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
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

    void Stop() noexcept override;
    void Submit(std::coroutine_handle<> handle) override;
    void Schedule(TimePoint deadline, std::coroutine_handle<> handle) override;
    [[nodiscard]] bool CancelPending(std::coroutine_handle<> handle) noexcept override;
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

    /// Min-heap entry; public so anonymous-namespace helpers in the .cpp
    /// can name the type. The fields are not part of the public API
    /// surface — treat as Detail.
    struct ScheduledEntry
    {
        TimePoint deadline {};
        std::uint64_t sequence { 0 }; ///< Tie-breaker so equal deadlines fire FIFO.
        std::coroutine_handle<> handle {};
    };

  private:
    void FireExpiredTimers();

    IClock& _clock;

    /// Guards `_ready`, `_timers` and `_nextSequence`.
    ///
    /// `IReactor` documents Submit and Schedule as safe to call from any thread,
    /// and this double did not honour that -- it touched a bare deque and a bare
    /// vector. Nothing noticed while every producer was the test's own thread,
    /// and the primitives this reactor now has to exercise (a resolver handing a
    /// result back from a worker, a queue pushed from a producer thread) are
    /// precisely the ones whose headline property is that they cross threads. A
    /// test double that cannot be used the way its interface is documented is a
    /// test double that quietly forces every such case onto a real reactor.
    ///
    /// It is never held across a `resume()`: a resumed coroutine may call
    /// Submit, which is the same reason the platform reactors drain into a local
    /// before resuming anything.
    mutable std::mutex _mutex;

    std::atomic<bool> _stopped { false };

  protected:
    void RunLoop() override;

  private:
    std::uint64_t _nextSequence { 0 };
    std::deque<std::coroutine_handle<>> _ready;
    std::vector<ScheduledEntry> _timers; ///< Min-heap by (deadline, sequence).
};

} // namespace FastCache
