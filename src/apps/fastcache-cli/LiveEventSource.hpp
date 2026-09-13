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
/// The production `IDashboardEventSource`: a sampling cadence, a terminal and a stop
/// request, merged into one ordered stream on one reactor.
///
/// **Every producer is a coroutine parked on the reactor, and nothing on the reactor
/// sleeps.** The cadence waits on a `DeadlineTimer` between samples, a sample runs
/// `TakeSample`'s two hops off the reactor and back, the terminal's own source parks on
/// whatever it reads, and a stop request blocks on a waiter thread of its own before it hops
/// back. So a keystroke that lands while a sample is still on the pool is delivered while
/// that sample is still on the pool: nothing here waits for one producer before hearing
/// another, which is the property a dashboard that must answer `q` during a slow scrape
/// exists to have.

/// Opens fresh connections to the endpoint, and a gatherer over them.
///
/// **What lets a session survive the daemon restarting under it** (#134 §6.4). A gatherer is
/// built over connections opened once, which is right for one answer and not for a watch: a
/// daemon that restarts leaves those sockets dead, and every sample after it would fail for as
/// long as the session runs. So a sample that failed is followed by a re-dial before the next
/// one -- on the cadence, never in a loop of its own -- and the restart is one gap.
class IStatsDialer
{
  public:
    IStatsDialer() = default;
    IStatsDialer(IStatsDialer const&) = delete;
    IStatsDialer(IStatsDialer&&) = delete;
    IStatsDialer& operator=(IStatsDialer const&) = delete;
    IStatsDialer& operator=(IStatsDialer&&) = delete;
    virtual ~IStatsDialer() = default;

    /// Dial again. Runs where a gather runs, on the pool.
    /// @return A gatherer over new connections; never null. One whose connections could not be
    ///         opened says so from `Gather()`, as a gatherer built at startup does.
    [[nodiscard]] virtual std::unique_ptr<IStatsGatherer> Dial() = 0;
};

/// What a `LiveEventSource` is built from.
///
/// Pointers are borrowed and must outlive the source's drain, not merely the source: a
/// sample already on the pool when the session ends still reads `gatherer` there, and
/// nothing can call it back. The terminal and the stop request are OWNED, because nothing
/// but this source reads them and closing them is part of closing the session -- and each
/// is released the moment nothing waits on it any more, whatever a sample on the pool is
/// doing, which is what restores the terminal and the signal disposition.
struct LiveSourceParts
{
    /// Where every event is delivered, and whose clock paces the samples. Close, Next and
    /// Drained are called on its thread; so is every resumption this source performs.
    IReactor* reactor { nullptr };

    /// What the first sample asks, and every one after it until a sample fails.
    IStatsGatherer* gatherer { nullptr };

    /// What re-dials after a failed sample, or null for a source that never re-dials.
    ///
    /// **Asked only after a failure**, so the healthy path opens nothing: the connections
    /// `gatherer` holds serve every sample for as long as they answer. What it dials replaces
    /// `gatherer` from then on, and is owned by the source until its last producer has ended.
    IStatsDialer* dialer { nullptr };

    /// Where the blocking gather runs.
    IExecutor* pool { nullptr };

    /// What a sample is stamped with, and when an outstanding one started.
    ///
    /// Its own part rather than `reactor->Clock()`, because a sample is stamped ON THE POOL
    /// the moment `Gather()` returns: a clock refreshed once per reactor turn would hand that
    /// thread a stale reading, and every rate would be divided by a wrong duration. `SteadyClock`
    /// in production.
    IClock* clock { nullptr };

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
    std::unique_ptr<IStopSignal> stop {};

    /// Where `stop`'s blocking wait runs, when there is a `stop`: see `IStopSignal::Stopped`
    /// for why it is never `pool`.
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
    /// Start sampling at once, and start every other producer that has something to read.
    ///
    /// Nothing runs inline: the producers begin on the reactor's next turn, so the
    /// constructor never re-enters its caller.
    /// @param parts What to sample, where, how often, and what else to listen to.
    explicit LiveEventSource(LiveSourceParts parts);

    /// Closes the source. Waiting for the drain first is the caller's obligation, and not
    /// one a destructor can discharge: a sample on the pool cannot be recalled.
    ~LiveEventSource() override;

    LiveEventSource(LiveEventSource const&) = delete;
    LiveEventSource(LiveEventSource&&) = delete;
    LiveEventSource& operator=(LiveEventSource const&) = delete;
    LiveEventSource& operator=(LiveEventSource&&) = delete;

    [[nodiscard]] Task<DashboardEvent> Next() override;

    /// Stop every producer.
    ///
    /// Discards what is queued, resumes an outstanding `Next()` with `Detached`, closes the
    /// terminal, cancels the stop request's wait, and wakes a cadence waiting between samples
    /// on the reactor's next turn rather than at its deadline. A sample already on the pool
    /// cannot be taken back; it is dropped when it returns, and the source drains then.
    void Close() noexcept override;

    /// Resume once nothing this source started is still running.
    ///
    /// The session's end is not this source's end: the loop returns the moment an operator
    /// quits, while a sample may still be reading `gatherer` on the pool. Awaiting this, or
    /// seeing `IsDrained()`, is what makes destroying the gatherer and the pool afterwards
    /// safe. One caller.
    /// @return A task completing when every producer has finished.
    [[nodiscard]] Task<void> Drained();

    /// Whether every producer has finished.
    ///
    /// **Safe from any thread**, unlike everything but `SampleOutstandingSince()`: this is
    /// what a drain waiting from outside the reactor reads.
    /// @return True once the last producer has ended.
    [[nodiscard]] bool IsDrained() const noexcept;

    /// When the sample now on the pool was started, or nullopt when none is out.
    ///
    /// **Safe from any thread**: this is what a drain waiting from outside the reactor reads
    /// to say how long a sample it is about to abandon had been out. Measured on the
    /// source's `clock`, so a caller subtracting it from another clock's `Now()` needs that
    /// clock to count from the same epoch -- `SteadyClock` against `steady_clock` does.
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
