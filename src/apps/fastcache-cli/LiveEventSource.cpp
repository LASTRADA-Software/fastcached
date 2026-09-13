// SPDX-License-Identifier: Apache-2.0
#include "DashboardSampler.hpp"
#include "LiveEventSource.hpp"

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/ResumeOn.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace FastCache::Cli
{

namespace
{
    /// One document sample, and when it was taken.
    struct DocumentOutcome
    {
        std::expected<std::string, AdminError> document { std::unexpected(AdminError {}) }; ///< What the fetch produced.
        TimePoint takenAt {};                                                               ///< When it answered.
    };

    /// Fetch one admin document off the reactor and come back: `TakeSample`'s two hops.
    ///
    /// The same shape for the same reasons, and stamped in the same place -- on the pool, the
    /// moment the fetch returns -- so a rate over documents is divided by the time the fetches took.
    /// @param admin What to ask.
    /// @param path The document; by value, for the coroutine frame.
    /// @param clock What stamps it; reads its source on every call.
    /// @param pool Where the blocking fetch runs.
    /// @param resumeOn Where the caller resumes.
    /// @return The document, and when it arrived.
    Task<DocumentOutcome> TakeDocument(
        IAdminDocument* admin, std::string path, IClock* clock, IExecutor* pool, IExecutor* resumeOn)
    {
        auto outcome = DocumentOutcome {};
        co_await ResumeOn { *pool };
        outcome.document = admin->FetchAdmin(path);
        outcome.takenAt = clock->Now();
        co_await ResumeOn { *resumeOn };
        co_return outcome;
    }

    /// Asks the gatherer it holds, dialling a new one first when the last round failed.
    ///
    /// Read and written only by the gathers, which run one at a time on the pool: the cadence
    /// never starts a sample before the previous one has come back.
    class RedialingGatherer final: public IStatsGatherer
    {
      public:
        /// @param first What to ask until a round fails.
        /// @param status What to ask for the node's status until then; null for never.
        /// @param dialer What dials a replacement; null for never.
        RedialingGatherer(IStatsGatherer* first, INodeStatusReader* status, IStatsDialer* dialer) noexcept:
            _current { first },
            _status { status },
            _dialer { dialer },
            _readsStatus { status != nullptr }
        {
        }

        [[nodiscard]] std::vector<StatsAttempt> Gather() override
        {
            if (_lastFailed && _dialer != nullptr)
            {
                _dialed = _dialer->Dial();
                _current = _dialed.get();
                if (_readsStatus)
                    _status = _dialed.get();
            }
            // In the same hop and BEFORE the counters: `TakeSample` stamps the moment `Gather()`
            // returns, and every rate is divided by the time between two stamps, so a round trip
            // read after the counters would put its own jitter into every rate's denominator.
            if (_status != nullptr)
                _lastStatus = _status->ReadNodeStatus();
            auto attempts = _current->Gather();
            // Failed by the reader's own decision rather than by a second reading of the
            // attempts: a round the ladder cannot choose a record from is the round that
            // renders as a gap, and exactly that round is the one worth a re-dial.
            _lastFailed = ChooseStats(attempts).outcome != Outcome::Affirmative;
            return attempts;
        }

        /// The status the last gather read, handed over once.
        ///
        /// Written on the pool and taken on the reactor after `TakeSample` has hopped back, which
        /// the executor's hand-off orders; the cadence never starts a gather before taking this.
        /// @return The status, or nullopt when none was read.
        [[nodiscard]] std::optional<CompileCacheWire::NodeStatusFields> TakeStatus() noexcept
        {
            return std::exchange(_lastStatus, std::nullopt);
        }

      private:
        IStatsGatherer* _current;
        INodeStatusReader* _status;
        IStatsDialer* _dialer;
        std::unique_ptr<IDialedStats> _dialed {};
        std::optional<CompileCacheWire::NodeStatusFields> _lastStatus {};
        bool _readsStatus;
        bool _lastFailed { false };
    };
} // namespace

namespace
{
    /// Presents through a source's presenter while it exists, and drops frames after.
    class GatedFrames final: public IFrameSink
    {
      public:
        /// @param presenter The source's presenter slot; outlives this.
        explicit GatedFrames(std::unique_ptr<IFrameSink> const* presenter) noexcept:
            _presenter { presenter }
        {
        }

        void Present(std::string_view frame) override
        {
            if (*_presenter != nullptr)
                (*_presenter)->Present(frame);
        }

      private:
        std::unique_ptr<IFrameSink> const* _presenter;
    };
} // namespace

/// Shared by the source and its producers.
///
/// Every member is read and written on the reactor's thread only -- the producers resume
/// there before touching any of it, and `LiveSourceParts::reactor` states the same of the
/// caller -- except the two that are atomic, which a drain reads from another thread, and
/// `gatherer`, which a sample uses on the pool between two hand-offs and never at once with
/// the reactor.
struct LiveEventSource::State
{
    explicit State(LiveSourceParts from):
        parts { std::move(from) },
        events { *parts.reactor, AsyncQueueOptions {} },
        finished { *parts.reactor, AsyncQueueOptions {} },
        due { *parts.reactor, AsyncQueueOptions {} },
        gatherer { parts.gatherer, parts.status, parts.dialer },
        presents { parts.frames != nullptr },
        frames { &parts.frames }
    {
    }

    /// What the source was built from, held whole. Declared first: the queues are
    /// constructed from its reactor.
    LiveSourceParts parts;

    /// What `Next()` hands out, and whether the session is closed: only `Close()` closes it.
    ///
    /// **Unbounded, because every producer is self-limiting and a drop would be a lie.**
    /// The cadence waits for one sample before starting the next, and the forwarders await
    /// each read before asking for another, so none can outrun a consumer that draws a frame
    /// per tick. And what a bound would displace is a sample the budget counts or a quit
    /// key -- either one lost silently is a session that miscounts or cannot be left.
    AsyncQueue<DashboardEvent> events;

    /// Closed when the last producer ends; nothing is ever pushed to it.
    ///
    /// A queue rather than a flag because `Drained()` has to PARK on it, and the queue is
    /// this tree's one way to park a coroutine until something else says so.
    AsyncQueue<std::monostate> finished;

    /// Where the cadence waits between samples: the timer pushes when the next sample is
    /// due, and `Close()` closes it.
    ///
    /// **The wait is a `DeadlineTimer` feeding this queue rather than a `SleepUntil`**,
    /// because a sleep can be taken back only by whoever holds the sleeping frame's handle
    /// -- which the coroutine cannot hand out without copying `DeadlineTimer`'s own
    /// machinery. Here closing the queue wakes the cadence on the reactor's next turn, it
    /// leaves through its own tail like every other exit, and its timer's destructor takes
    /// the pending deadline off the heap.
    AsyncQueue<std::monostate> due;

    /// What every sample asks: `parts.gatherer`, re-dialled through `parts.dialer` after a
    /// failure. Held here because a sample still on the pool when the source is destroyed is
    /// still inside it, and this state outlives the source for exactly that long.
    RedialingGatherer gatherer;

    /// Whether the source was given a presenter at all, which `Frames()` answers by.
    bool presents;

    /// What the loop presents through: `parts.frames` while it exists.
    GatedFrames frames;

    int producers { 0 };

    /// Whether the last producer has ended, for a drain that cannot park on `finished`.
    std::atomic<bool> drained { false };

    /// When the outstanding sample started, in the reactor clock's ticks, or `NoSample`.
    std::atomic<TimePoint::rep> sampleSince { NoSample };

    /// `sampleSince` when no sample is out. No real reading of a steady clock is this.
    static constexpr auto NoSample = std::numeric_limits<TimePoint::rep>::min();

    /// Whether the session is closed.
    /// @return True once `Close()` has run.
    [[nodiscard]] bool Closed() const noexcept
    {
        return events.IsClosed();
    }

    /// Queue @p event for `Next()`.
    /// @param event What happened.
    void Deliver(DashboardEvent event)
    {
        (void) events.Push(std::move(event));
    }

    /// One producer has ended; the last one says so to both kinds of waiter.
    void ProducerEnded() noexcept
    {
        if (--producers != 0)
            return;
        drained.store(true, std::memory_order_release);
        finished.Close();
    }
};

namespace
{
    /// A cadence deadline has come: say so to the queue the cadence waits on.
    /// @param due The source's `due` queue.
    void SampleDue(void* due)
    {
        (void) static_cast<AsyncQueue<std::monostate>*>(due)->Push(std::monostate {});
    }

    /// Take one sample through whichever door the session samples, as the event it becomes.
    ///
    /// One step for both doors, so the cadence around it -- the stamp of when a sample is out,
    /// the tick, the deadline -- is written once.
    /// @param state The source's state; the cadence holds it alive.
    /// @return The `Sample` or `SampleFailed` to deliver.
    Task<DashboardEvent> SampleOnce(LiveEventSource::State* state)
    {
        auto const& parts = state->parts;
        if (!parts.document.empty())
        {
            auto fetched = co_await TakeDocument(parts.admin, parts.document, parts.clock, parts.pool, parts.reactor);
            // Success or failure, the document is the reader's to interpret: a refusal carries the
            // leader's own words, and the reader decides which outcome that is.
            co_return DashboardEvent { .kind = DashboardEventKind::Sample,
                                       .at = fetched.takenAt,
                                       .document = std::move(fetched.document) };
        }

        auto sample = co_await TakeSample(&state->gatherer, parts.clock, parts.pool, parts.reactor);
        auto status = state->gatherer.TakeStatus();
        if (sample.attempts.empty())
            // Nothing could be asked at all. A failure the loop counts and draws as a gap -- never
            // a reading with nothing in it, which the reader would have to guess about.
            co_return DashboardEvent { .kind = DashboardEventKind::SampleFailed,
                                       .at = sample.takenAt,
                                       .nodeStatus = std::move(status),
                                       .outcome = Outcome::Unreachable,
                                       .note = "no stats source could be asked" };
        co_return DashboardEvent { .kind = DashboardEventKind::Sample,
                                   .at = sample.takenAt,
                                   .attempts = std::move(sample.attempts),
                                   .nodeStatus = std::move(status) };
    }

    /// Sample, deliver, park until the next deadline; until closed.
    ///
    /// **The deadline is the later of the cadence grid and now.** On the grid, a sample
    /// that took 300 ms of a 2 s interval does not push every later one 300 ms back. Never
    /// before now, so a sample slower than the interval is followed by the next at once
    /// rather than by one per interval it overran: a burst of back-dated samples would
    /// all read the same moment and make a rate out of nothing.
    /// @param shared The source's state; held so it outlives the source if need be.
    DetachedTask RunCadence(std::shared_ptr<LiveEventSource::State> shared)
    {
        auto const& parts = shared->parts;
        co_await ResumeOn { *parts.reactor };

        auto deadline = parts.reactor->Clock().Now();
        while (!shared->Closed())
        {
            shared->sampleSince.store(parts.clock->Now().time_since_epoch().count(), std::memory_order_release);
            auto sample = co_await SampleOnce(shared.get());
            shared->sampleSince.store(LiveEventSource::State::NoSample, std::memory_order_release);
            // Closed while the sample was on the pool: the closed queue refuses the reading,
            // which describes a session that has already ended, and the closed `due` ends
            // the loop below without a wait.
            shared->Deliver(std::move(sample));
            shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Tick });

            deadline = std::max(deadline + parts.interval, parts.reactor->Clock().Now());
            // A zero poll interval: `Close()` wakes this through `due`, so the timer never
            // needs to look at anything before its deadline.
            auto const timer = DeadlineTimer { *parts.reactor, deadline, &SampleDue, &shared->due, Duration::zero() };
            if (!(co_await shared->due.Pop()).has_value())
                break;
        }
        shared->ProducerEnded();
    }

    /// Forward what the terminal says, until closed or until the terminal goes away.
    /// @param shared The source's state; held so it outlives the source if need be.
    DetachedTask RunTerminal(std::shared_ptr<LiveEventSource::State> shared)
    {
        co_await ResumeOn { *shared->parts.reactor };

        while (!shared->Closed())
        {
            auto event = co_await shared->parts.terminal->Next();
            // Closed while the read was outstanding: this is the `Detached` that `Close()`
            // asked the terminal for, not news for the loop.
            if (shared->Closed())
                break;

            auto const kind = event.kind;
            shared->Deliver(std::move(event));

            // A terminal that has gone has nothing more to read. Its `Detached` is delivered
            // like any other event, and what it means for the session is the loop's call.
            if (kind == DashboardEventKind::Detached)
                break;
            if (kind == DashboardEventKind::Resize)
                shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Tick });
        }
        // Released here, on the reactor, as soon as nothing reads it: destroying it is what
        // restores the terminal, and a sample still on the pool is no reason to leave an
        // operator's terminal in raw mode. Closed first, because a terminal that went away
        // on its own was never closed, and its contract asks for that before destruction. Its
        // presenter goes first: nothing may be drawn once the operator's screen is back.
        shared->parts.frames.reset();
        shared->parts.terminal->Close();
        shared->parts.terminal.reset();
        shared->ProducerEnded();
    }

    /// Wait for a stop request and tell the loop what it means.
    ///
    /// **Awaited, never polled**: the blocking wait is on `stopWaiter`, and this resumes on
    /// the reactor only once the signal or `Close()` has said something.
    /// @param shared The source's state; held so it outlives the source if need be.
    DetachedTask RunStopWatch(std::shared_ptr<LiveEventSource::State> shared)
    {
        auto& parts = shared->parts;
        auto const wake = co_await parts.stop->Stopped(parts.stopWaiter, parts.reactor);

        // Released as soon as nothing waits on it, which is what puts the previous signal
        // disposition back: a sample stuck on the pool is no reason to keep Ctrl-C redirected
        // at a watch that has ended.
        parts.stop.reset();

        // The wait ended because `Close()` cancelled it, or it said something nobody is left
        // to hear.
        if (shared->Closed() || wake == StopWake::Cancelled)
        {
            shared->ProducerEnded();
            co_return;
        }

        if (wake == StopWake::Stopped)
            shared->Deliver(DashboardEvent { .kind = DashboardEventKind::StopRequested });
        else
            // With the handler installed, Ctrl-C no longer ended the process by itself, and
            // nothing was waiting to hear it -- so a session that went on would be one Ctrl-C
            // could not end. Ending it says why instead.
            shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Detached,
                                             .note = "stopped watching for Ctrl-C: waiting for the stop request failed" });
        shared->ProducerEnded();
    }
} // namespace

LiveEventSource::LiveEventSource(LiveSourceParts parts):
    _state { std::make_shared<State>(std::move(parts)) }
{
    // Zero would make every deadline `now`, and the cadence would gather back to back --
    // never blocking the reactor, and never letting the endpoint rest either. Admission
    // refuses anything below a subject's floor, so this is a caller's mistake, not input.
    assert(_state->parts.interval > std::chrono::milliseconds::zero());

    ++_state->producers;
    RunCadence(_state);
    if (_state->parts.terminal != nullptr)
    {
        ++_state->producers;
        RunTerminal(_state);
    }
    if (_state->parts.stop != nullptr)
    {
        assert(_state->parts.stopWaiter != nullptr);
        ++_state->producers;
        RunStopWatch(_state);
    }
}

LiveEventSource::~LiveEventSource()
{
    Close();
}

Task<DashboardEvent> LiveEventSource::Next()
{
    auto const state = _state;
    auto event = co_await state->events.Pop();
    if (!event.has_value())
        co_return DashboardEvent { .kind = DashboardEventKind::Detached, .note = "the live-stats session was closed" };
    co_return *std::move(event);
}

void LiveEventSource::Close() noexcept
{
    auto& state = *_state;
    if (state.Closed())
        return;
    state.events.Close();
    state.due.Close();
    if (state.parts.terminal != nullptr)
        state.parts.terminal->Close();
    if (state.parts.stop != nullptr)
        state.parts.stop->Cancel();
}

bool LiveEventSource::IsDrained() const noexcept
{
    return _state->drained.load(std::memory_order_acquire);
}

std::optional<TimePoint> LiveEventSource::SampleOutstandingSince() const noexcept
{
    auto const since = _state->sampleSince.load(std::memory_order_acquire);
    if (since == State::NoSample)
        return std::nullopt;
    return TimePoint { TimePoint::duration { since } };
}

IFrameSink* LiveEventSource::Frames() noexcept
{
    return _state->presents ? &_state->frames : nullptr;
}

Task<void> LiveEventSource::Drained()
{
    auto const state = _state;
    // Nothing is ever pushed, so this resumes exactly when the last producer closes it.
    (void) co_await state->finished.Pop();
}

} // namespace FastCache::Cli
