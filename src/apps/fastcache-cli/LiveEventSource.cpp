// SPDX-License-Identifier: Apache-2.0
#include "DashboardSampler.hpp"
#include "LiveEventSource.hpp"

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <utility>
#include <variant>

namespace FastCache::Cli
{

/// Shared by the source and its two producers.
///
/// Every member is read and written on the reactor's thread only -- the producers resume
/// there before touching any of it, and `LiveSourceParts::reactor` states the same of the
/// caller -- which is why the flags are plain rather than atomic.
struct LiveEventSource::State
{
    explicit State(LiveSourceParts parts):
        reactor { parts.reactor },
        gatherer { parts.gatherer },
        pool { parts.pool },
        interval { parts.interval },
        terminal { std::move(parts.terminal) },
        events { *parts.reactor, AsyncQueueOptions {} },
        finished { *parts.reactor, AsyncQueueOptions {} }
    {
    }

    IReactor* reactor;
    IStatsGatherer* gatherer;
    IExecutor* pool;
    std::chrono::milliseconds interval;
    std::unique_ptr<IDashboardEventSource> terminal;

    /// What `Next()` hands out.
    ///
    /// **Unbounded, because every producer is self-limiting and a drop would be a lie.**
    /// The cadence waits for one sample before starting the next, and the forwarder awaits
    /// each terminal read before asking for another, so neither can outrun a consumer that
    /// draws a frame per tick. And what a bound would displace is a sample the budget
    /// counts or a quit key -- either one lost silently is a session that miscounts or
    /// cannot be left.
    AsyncQueue<DashboardEvent> events;

    /// Closed when the last producer ends; nothing is ever pushed to it.
    ///
    /// A queue rather than a flag because `Drained()` has to PARK on it, and the queue is
    /// this tree's one way to park a coroutine until something else says so.
    AsyncQueue<std::monostate> finished;

    /// The cadence coroutine's own frame, so `Close()` can take it off the timer heap.
    std::coroutine_handle<> cadence {};

    /// Whether `cadence` is parked between samples right now -- the only moment it is on
    /// the timer heap, and so the only moment `CancelPending` can retract it.
    bool sleeping { false };

    bool closed { false };
    int producers { 0 };

    /// Queue @p event for `Next()`.
    /// @param event What happened.
    void Deliver(DashboardEvent event)
    {
        (void) events.Push(std::move(event));
    }

    /// One producer has ended; the last one resumes `Drained()`.
    void ProducerEnded() noexcept
    {
        if (--producers == 0)
            finished.Close();
    }
};

namespace
{
    /// Leave the coroutine's own handle in @p slot without suspending.
    ///
    /// The same shape `DeadlineTimer` uses to let its owner retract a parked wait: a
    /// coroutine cannot hand out its own handle any other way.
    struct CaptureHandle
    {
        std::coroutine_handle<>* slot;

        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const noexcept
        {
            *slot = handle;
            return false;
        }

        void await_resume() const noexcept {}
    };

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
        co_await CaptureHandle { &shared->cadence };
        co_await ResumeOn { *shared->reactor };

        auto deadline = shared->reactor->Clock().Now();
        while (!shared->closed)
        {
            auto sample = co_await TakeSample(shared->gatherer, shared->pool, shared->reactor);
            // Closed while the gather was on the pool: nobody is listening, and the reading
            // describes a session that has already ended.
            if (shared->closed)
                break;

            shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = std::move(sample.attempts) });
            shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Tick });

            deadline = std::max(deadline + shared->interval, shared->reactor->Clock().Now());
            shared->sleeping = true;
            co_await SleepUntil { .reactor = shared->reactor, .deadline = deadline };
            shared->sleeping = false;
        }
        shared->ProducerEnded();
    }

    /// Forward what the terminal says, until closed or until the terminal goes away.
    /// @param shared The source's state; held so it outlives the source if need be.
    DetachedTask RunTerminal(std::shared_ptr<LiveEventSource::State> shared)
    {
        co_await ResumeOn { *shared->reactor };

        while (!shared->closed)
        {
            auto event = co_await shared->terminal->Next();
            // Closed while the read was outstanding: this is the `Detached` that `Close()`
            // asked the terminal for, not news for the loop.
            if (shared->closed)
                break;

            auto const kind = event.kind;
            shared->Deliver(std::move(event));

            // A terminal that has gone has nothing left to show a frame on, so its
            // `Detached` ends the session: delivered, and nothing more read.
            if (kind == DashboardEventKind::Detached)
                break;
            if (kind == DashboardEventKind::Resize)
                shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Tick });
        }
        shared->ProducerEnded();
    }
} // namespace

LiveEventSource::LiveEventSource(LiveSourceParts parts):
    _state { std::make_shared<State>(std::move(parts)) }
{
    ++_state->producers;
    RunCadence(_state);
    if (_state->terminal != nullptr)
    {
        ++_state->producers;
        RunTerminal(_state);
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
    if (state.closed)
        return;
    state.closed = true;
    state.events.Close();
    if (state.terminal != nullptr)
        state.terminal->Close();

    // A cadence between samples is on the timer heap until its deadline -- a whole
    // interval, five seconds for `fleet` -- and `Drained()` would wait that out for
    // nothing. Retracted, it is freed here, and its end is counted here because the code
    // after its `co_await` will never run.
    if (state.sleeping && state.reactor->CancelPending(state.cadence))
    {
        state.sleeping = false;
        state.cadence.destroy();
        state.ProducerEnded();
    }
}

Task<void> LiveEventSource::Drained()
{
    auto const state = _state;
    // Nothing is ever pushed, so this resumes exactly when the last producer closes it.
    auto const ended = co_await state->finished.Pop();
    static_cast<void>(ended);
}

} // namespace FastCache::Cli
