// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "DashboardEvent.hpp"
#include "DashboardLoop.hpp"
#include "LiveSubscriber.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Platform/StopSignal.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// @file LiveEventSource.hpp
/// The production `IDashboardEventSource`: a live-stats subscription, a terminal and a stop
/// request, merged into one ordered stream on one reactor.
///
/// **Every producer is a coroutine parked on the reactor, and nothing on the reactor
/// sleeps.** The stream runs `TakeFrame`'s two hops off the reactor and back for every frame,
/// waits on a `DeadlineTimer` before subscribing again, the terminal's own source parks on
/// whatever it reads, and a stop request blocks on a waiter thread of its own before it hops
/// back. So a keystroke that lands while a read is still on the pool is delivered while that
/// read is still on the pool: nothing here waits for one producer before hearing another, which
/// is the property a dashboard that must answer `q` while a node is slow exists to have.
///
/// **The server keeps the cadence, not this source** (#1399). A reading arrives when the node
/// pushes one; nothing here polls. What this source decides is what to do when a stream ends:
/// follow a `NotLeader` to the leader it names, subscribe again after one interval, or end the
/// session when nothing has been read and waiting cannot help.

/// How many `NotLeader` redirects one subscription follows before it counts as failed.
///
/// Two, as a lease and a registration bound theirs: a leader that moved while the redirect was in
/// flight is one more hop, and a pair of nodes each naming the other stale leader is not a leader
/// at all. The failed subscription is a gap, and the next attempt starts again at `--addr`.
inline constexpr int MaxLeaderRedirects = 2;

/// The `Detached` a source delivers when its stream cannot be read and waiting cannot help.
///
/// Named, because the other `Detached` -- the one a read of a CLOSED source answers -- has the same kind:
/// a case that looks only at the kind cannot tell a session that ended itself from one its caller closed.
inline constexpr std::string_view StreamFinishedNote = "the live-stats stream cannot be read";

/// The `Detached` a read of a closed source answers.
inline constexpr std::string_view SessionClosedNote = "the live-stats session was closed";

/// What a `LiveEventSource` is built from.
///
/// Pointers are borrowed and must outlive the source's drain, not merely the source: a read
/// already on the pool when the session ends is still inside `subscription` there, and nothing
/// can call it back -- closing the source asks the subscription to LEAVE, and the read returns
/// once the node has closed the stream. The terminal and the stop request are OWNED, because
/// nothing but this source reads them and closing them is part of closing the session -- and
/// each is released the moment nothing waits on it any more, whatever a read on the pool is
/// doing, which is what restores the terminal and the signal disposition.
struct LiveSourceParts
{
    /// Where every event is delivered, and whose clock paces the re-subscriptions. Close, Next and
    /// Drained are called on its thread; so is every resumption this source performs.
    IReactor* reactor { nullptr };

    /// The stream every reading arrives on. Opened, read and re-opened only on `pool`; left from
    /// `Close()`.
    ILiveSubscription* subscription { nullptr };

    /// What the session watches, as a `SUBSCRIBE` names it.
    CompileCacheWire::LiveSubject subject { CompileCacheWire::LiveSubject::Cache };

    /// Where the first subscription dials, and every one after a stream that did not end in a redirect.
    Endpoint endpoint {};

    /// What a `fleet` subscription presents as the dashboard credential; empty for none.
    std::string dashboardToken {};

    /// Where the blocking dial and every blocking read run.
    IExecutor* pool { nullptr };

    /// What a reading is stamped with, and when an outstanding read started.
    ///
    /// Its own part rather than `reactor->Clock()`, because a reading is stamped ON THE POOL the
    /// moment `Read()` returns: a clock refreshed once per reactor turn would hand that thread a
    /// stale reading, and every rate would be divided by a wrong duration. `SteadyClock` in production.
    IClock* clock { nullptr };

    /// The cadence asked of the server, and the wait before subscribing again after a stream ends.
    ///
    /// The wait is the operator's own interval rather than a backoff of this source's invention: a
    /// daemon that restarts is then one gap in the history, exactly as a missed reading is.
    std::chrono::milliseconds interval {};

    /// Key, Resize and Detached, or null for a run with no terminal. It must resume its
    /// `Next()` on `reactor`.
    ///
    /// Null is a whole mode rather than a missing part: a run whose output is not a
    /// terminal takes its samples with only `stop` able to end it early, and a terminal
    /// that detaches ends the session only where there is one.
    std::unique_ptr<IDashboardEventSource> terminal {};

    /// Where `terminal`'s frames are presented, or null for a run with no terminal.
    ///
    /// **Owned here, beside the events it presents over, and released immediately before them**
    /// -- when the terminal goes away on its own as much as when the session is closed. A presenter
    /// the composition held instead outlived a terminal that detached: the events were destroyed
    /// the moment their `Detached` was read, which restores the operator's screen, while a sample
    /// and its tick already queued ahead of that `Detached` still drew a frame onto it. The loop
    /// presents through `Frames()`, which drops a frame once this is gone. Declared after
    /// `terminal`, so a destroyed source releases it first too.
    std::unique_ptr<IFrameSink> frames {};

    /// An operator's stop request, or null where none is composed.
    ///
    /// A run with no terminal is the one that hears Ctrl-C as a signal; a terminal in raw
    /// mode sends it as a key. A signal is delivered as `StopRequested`, never as a `Key`
    /// spelling the byte nobody typed, and the loop quits on either.
    std::unique_ptr<IStopSignal> stop {};

    /// Where `stop`'s blocking wait runs, when there is a `stop`: see `IStopSignal::Stopped`
    /// for why it is never `pool`.
    IExecutor* stopWaiter { nullptr };
};

/// Reads a subscription and forwards a terminal and a stop request, as one stream.
///
/// **A `Tick` follows every sample and every resize**, and that is the whole tick policy.
/// A frame is drawn on a tick, so a piped run -- which has no terminal, hence no resize --
/// writes exactly one frame per reading, and a terminal is redrawn when its geometry
/// changes. There is no separate render cadence: a frame is drawn from `DashboardModel`,
/// which only a sample outcome and a resize change, so a tick after anything else would
/// redraw the frame already on screen.
class LiveEventSource final: public IDashboardEventSource
{
  public:
    /// Subscribe at once, and start every other producer that has something to read.
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
    /// terminal, cancels the stop request's wait, wakes a stream waiting to subscribe again on
    /// the reactor's next turn rather than at its deadline, and LEAVES the subscription -- a
    /// half-close, so the node counts the watcher's goodbye and closes, which is what returns a
    /// read still on the pool. What that read brings back is dropped, and the source drains then.
    void Close() noexcept override;

    /// Resume once nothing this source started is still running.
    ///
    /// The session's end is not this source's end: the loop returns the moment an operator
    /// quits, while a read may still be inside `subscription` on the pool. Awaiting this, or
    /// seeing `IsDrained()`, is what makes destroying the subscription and the pool afterwards
    /// safe. One caller.
    /// @return A task completing when every producer has finished.
    [[nodiscard]] Task<void> Drained();

    /// Whether every producer has finished.
    ///
    /// **Safe from any thread**, unlike everything but `ReadOutstandingSince()`: this is
    /// what a drain waiting from outside the reactor reads.
    /// @return True once the last producer has ended.
    [[nodiscard]] bool IsDrained() const noexcept;

    /// When the dial or read now on the pool was started, or nullopt when none is out.
    ///
    /// **Safe from any thread**: this is what a drain waiting from outside the reactor reads
    /// to say how long a read it is about to abandon had been out. Measured on the source's
    /// `clock`, so a caller subtracting it from another clock's `Now()` needs that clock to
    /// count from the same epoch -- `SteadyClock` against `steady_clock` does.
    /// @return The start of the outstanding read, or nullopt.
    [[nodiscard]] std::optional<TimePoint> ReadOutstandingSince() const noexcept;

    /// Where the stream dials now: `--addr`, or the leader a refusal named.
    ///
    /// **Safe from any thread**, for `ReadOutstandingSince()`'s reader: an abandonment names the
    /// machine whose stream did not close, and after a follow that is not the one `--addr` names.
    /// @return The endpoint, as `host:port`.
    [[nodiscard]] std::string StreamingEndpoint() const;

    /// Where the loop presents a terminal session's frames, or null when this source was given no
    /// `frames`.
    ///
    /// Presents through `LiveSourceParts::frames` for as long as the terminal's events exist, and
    /// drops a frame after: once the terminal has gone the operator's own screen is back, and a
    /// frame there is output nobody asked for. Called on the reactor's thread, as every presenter
    /// is; valid for the source's lifetime.
    /// @return The sink, or null.
    [[nodiscard]] IFrameSink* Frames() noexcept;

    /// The producers' shared state; outlives the source while a producer still runs.
    ///
    /// Public only so the .cpp's producer coroutines can name it, the same reason and the
    /// same spelling as `DeadlineTimer::State`. Treat as private.
    struct State;

  private:
    std::shared_ptr<State> _state;
};

} // namespace FastCache::Cli
