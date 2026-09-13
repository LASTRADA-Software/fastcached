// SPDX-License-Identifier: Apache-2.0
#include "DashboardSampler.hpp"
#include "LiveEventSource.hpp"

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/ResumeOn.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>
#include <variant>

namespace FastCache::Cli
{

/// Shared by the source and its two producers.
///
/// Every member is read and written on the reactor's thread only -- the producers resume
/// there before touching any of it, and `LiveSourceParts::reactor` states the same of the
/// caller -- which is why the count is plain rather than atomic.
struct LiveEventSource::State
{
    explicit State(LiveSourceParts from):
        parts { std::move(from) },
        events { *parts.reactor, AsyncQueueOptions {} },
        finished { *parts.reactor, AsyncQueueOptions {} },
        due { *parts.reactor, AsyncQueueOptions {} }
    {
    }

    /// What the source was built from, held whole. Declared first: the queues are
    /// constructed from its reactor.
    LiveSourceParts parts;

    /// What `Next()` hands out, and whether the session is closed: only `Close()` closes it.
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

    int producers { 0 };

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

    /// One producer has ended; the last one resumes `Drained()`.
    void ProducerEnded() noexcept
    {
        if (--producers == 0)
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
            auto sample = co_await TakeSample(parts.gatherer, parts.pool, parts.reactor);
            // Closed while the gather was on the pool: the closed queue refuses the reading,
            // which describes a session that has already ended, and the closed `due` ends
            // the loop below without a wait.
            shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = std::move(sample.attempts) });
            shared->Deliver(DashboardEvent { .kind = DashboardEventKind::Tick });

            deadline = std::max(deadline + parts.interval, parts.reactor->Clock().Now());
            // One sleep straight through to the deadline: a zero poll interval, because
            // nothing needs the timer to look at a flag -- `Close()` wakes this through the
            // queue, and the timer's destructor retracts what is still pending.
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
    // A cadence between samples would otherwise wait out a whole interval before seeing
    // the close, and `Drained()` with it.
    state.due.Close();
    if (state.parts.terminal != nullptr)
        state.parts.terminal->Close();
}

Task<void> LiveEventSource::Drained()
{
    auto const state = _state;
    // Nothing is ever pushed, so this resumes exactly when the last producer closes it.
    (void) co_await state->finished.Pop();
}

} // namespace FastCache::Cli
