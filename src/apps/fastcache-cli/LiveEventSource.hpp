// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"
#include "StatsSource.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Platform/StopSignal.hpp>

#include <chrono>
#include <memory>
#include <optional>

namespace FastCache::Cli
{

/// @file LiveEventSource.hpp
/// The production `IDashboardEventSource`: a sampling cadence and a terminal, merged into
/// one ordered stream on one reactor.
///
/// **Every producer is a coroutine parked on the reactor, never a thread and never a
/// sleep.** The cadence waits on a `DeadlineTimer` between samples, a sample runs
/// `TakeSample`'s two hops off the reactor and back, and the terminal's own source parks
/// on whatever it reads. So a keystroke that lands while a sample is still on the pool is
/// delivered while that sample is still on the pool: nothing here waits for one producer
/// before hearing another, which is the property a dashboard that must answer `q` during
/// a slow scrape exists to have.

/// What a `LiveEventSource` is built from.
///
/// Pointers are borrowed and must outlive the source's `Drained()`, not merely the source:
/// a sample already on the pool when the session ends still reads `gatherer` there, and
/// nothing can call it back. The terminal is OWNED, because nothing but this source reads
/// it and closing it is part of closing the session -- and it is released, restoring the
/// terminal, the moment nothing reads it any more, whatever a sample on the pool is doing.
struct LiveSourceParts
{
    /// Where every event is delivered, and whose clock paces the samples. Close, Next and
    /// Drained are called on its thread; so is every resumption this source performs.
    IReactor* reactor { nullptr };

    /// What one sample asks.
    IStatsGatherer* gatherer { nullptr };

    /// Where the blocking gather runs.
    IExecutor* pool { nullptr };

    /// Time from one sample's start to the next one's. A sample slower than this is never
    /// overlapped: the next starts when it returns, and the ones its lateness skipped are
    /// not taken in a burst afterwards.
    std::chrono::milliseconds interval {};

    /// Key, Resize and Detached, or null for a run with no terminal. It must resume its
    /// `Next()` on `reactor`.
    ///
    /// Null is a whole mode rather than a missing part: a run whose output is not a
    /// terminal takes its samples with only `stop` able to end it early, and a terminal
    /// that detaches ends the session only where there is one.
    std::unique_ptr<IDashboardEventSource> terminal {};

    /// An operator's stop request, or null where none is composed.
    ///
    /// A run with no terminal is the one that hears Ctrl-C as a signal; a terminal in raw
    /// mode sends it as a key. So a stop is delivered as that same key, and the loop has one
    /// way to be quit whichever route Ctrl-C took.
    IStopSignal* stop { nullptr };

    /// Where `stop`'s blocking wait runs, when there is a `stop`.
    ///
    /// **Its own thread, never `pool`**: the wait holds its thread for the whole session, so
    /// a one-thread sample pool lent to it would never sample again.
    IExecutor* stopWaiter { nullptr };
};

/// Samples on a cadence and forwards a terminal and a stop request, as one stream.
///
/// **A `Tick` follows every sample and every resize**, and that is the whole tick policy.
/// A frame is drawn on a tick, so a piped run -- which has no terminal, hence no resize --
/// writes exactly one frame per sample, and a terminal is redrawn when its geometry
/// changes. There is no separate render cadence: a frame is drawn from `DashboardModel`,
/// which only a sample outcome and a resize change, so a tick after anything else would
/// redraw the frame already on screen.
class LiveEventSource final: public IDashboardEventSource
{
  public:
    /// Start sampling at once and, when there is a terminal, start reading it.
    ///
    /// Nothing runs inline: both producers begin on the reactor's next turn, so the
    /// constructor never re-enters its caller.
    /// @param parts What to sample, where, how often, and what else to listen to.
    explicit LiveEventSource(LiveSourceParts parts);

    /// Closes the source. Awaiting `Drained()` first is the caller's obligation, and not
    /// one a destructor can discharge: a sample on the pool cannot be recalled.
    ~LiveEventSource() override;

    LiveEventSource(LiveEventSource const&) = delete;
    LiveEventSource(LiveEventSource&&) = delete;
    LiveEventSource& operator=(LiveEventSource const&) = delete;
    LiveEventSource& operator=(LiveEventSource&&) = delete;

    [[nodiscard]] Task<DashboardEvent> Next() override;

    /// Stop both producers.
    ///
    /// Discards what is queued, resumes an outstanding `Next()` with `Detached`, closes the
    /// terminal, and wakes a cadence waiting between samples on the reactor's next turn
    /// rather than at its deadline. A sample already on the pool cannot be taken back; it
    /// is dropped when it returns, and `Drained()` resumes then.
    void Close() noexcept override;

    /// Resume once nothing this source started is still running.
    ///
    /// The session's end is not this source's end: the loop returns the moment an operator
    /// quits, while a sample may still be reading `gatherer` on the pool. Awaiting this is
    /// what makes destroying the gatherer and the pool afterwards safe. One caller.
    /// @return A task completing when both producers have finished.
    [[nodiscard]] Task<void> Drained();

    /// When the sample now on the pool was started, or nullopt when none is out.
    ///
    /// **Safe from any thread**, unlike everything else here: this is what a caller waiting
    /// for the drain from outside the reactor reads to say how long a sample it is about to
    /// abandon had been out. Measured on the reactor's clock.
    /// @return The start of the outstanding sample, or nullopt.
    [[nodiscard]] std::optional<TimePoint> SampleOutstandingSince() const noexcept;

    /// The producers' shared state; outlives the source while a producer still runs.
    ///
    /// Public only so the .cpp's producer coroutines can name it, the same reason and the
    /// same spelling as `DeadlineTimer::State`. Treat as private.
    struct State;

  private:
    std::shared_ptr<State> _state;
};

} // namespace FastCache::Cli
