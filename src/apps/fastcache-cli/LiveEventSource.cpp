// SPDX-License-Identifier: Apache-2.0
#include "DashboardSampler.hpp"
#include "LiveEventSource.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <core/Ranges.hpp>
#include <core/async/AsyncQueue.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/net/DeadlineTimer.hpp>

namespace FastCache::Cli
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Presents through a source's presenter while it exists, and drops frames after.
    class GatedFrames final: public IFrameSink
    {
      public:
        /// @param presenter The source's presenter slot; outlives this.
        explicit GatedFrames(std::unique_ptr<IFrameSink> const* presenter) noexcept:
            _presenter { presenter }
        {
        }

        void PresentPlaced(DashboardFrame const& frame) override
        {
            if (*_presenter != nullptr)
                (*_presenter)->PresentPlaced(frame);
        }

      private:
        std::unique_ptr<IFrameSink> const* _presenter;
    };

    /// How one stream ended, which decides what the source does next.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class StreamEnd : std::uint8_t
    {
        Retry,  ///< Subscribe again at `--addr`, one interval from now.
        Follow, ///< Subscribe again at once, at the leader a refusal named.
        Finish, ///< End the session: nothing was read, and waiting cannot change the answer.
        Closed, ///< The source was closed while the stream ran.
        Last,
    };
} // namespace

/// Shared by the source and its producers.
///
/// Every member is read and written on the reactor's thread only -- the producers resume
/// there before touching any of it, and `LiveSourceParts::reactor` states the same of the
/// caller -- except the two that are atomic, which a drain reads from another thread, and
/// `parts.subscription`, which a read uses on the pool between two hand-offs and which
/// `Close()` may only LEAVE, the one call it allows from another thread.
struct LiveEventSource::State
{
    explicit State(LiveSourceParts from):
        parts { std::move(from) },
        events { *parts.reactor, core::async::AsyncQueueOptions {} },
        finished { *parts.reactor, core::async::AsyncQueueOptions {} },
        due { *parts.reactor, core::async::AsyncQueueOptions {} },
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
    /// The stream awaits each read before asking for another, and the forwarders await each
    /// read before asking for the next, so none can outrun a consumer that draws a frame per
    /// tick. And what a bound would displace is a sample the budget counts or a quit key --
    /// either one lost silently is a session that miscounts or cannot be left.
    core::async::AsyncQueue<DashboardEvent> events;

    /// Closed when the last producer ends; nothing is ever pushed to it.
    ///
    /// A queue rather than a flag because `Drained()` has to PARK on it, and the queue is
    /// this tree's one way to park a coroutine until something else says so.
    core::async::AsyncQueue<std::monostate> finished;

    /// Where the stream waits before subscribing again: the timer pushes when the wait is up,
    /// and `Close()` closes it.
    ///
    /// **A `core::net::DeadlineTimer` feeding this queue rather than a `SleepUntil`**, because a sleep can
    /// be taken back only by whoever holds the sleeping frame's handle. Closing the queue wakes
    /// the stream on the reactor's next turn, it leaves through its own tail like every other
    /// exit, and its timer's destructor takes the pending deadline off the heap.
    core::async::AsyncQueue<std::monostate> due;

    /// Whether the source was given a presenter at all, which `Frames()` answers by.
    bool presents;

    /// What the loop presents through: `parts.frames` while it exists.
    GatedFrames frames;

    int producers { 0 };

    /// Whether any reading has been delivered, which decides whether a refusal ends the session.
    bool readSomething { false };

    /// Consecutive `NotLeader` redirects followed without a grant.
    int redirects { 0 };

    /// Whether the last producer has ended, for a drain that cannot park on `finished`.
    std::atomic<bool> drained { false };

    /// When the outstanding dial or read started, in the source clock's ticks, or `NoRead`.
    std::atomic<core::platform::SteadyTimePoint::rep> readSince { NoRead };

    /// Guards `streaming`, which a drain reads from another thread.
    mutable std::mutex streamingGuard;

    /// The endpoint the stream dials now: `--addr`, or a leader it followed.
    std::string streaming;

    /// `readSince` when nothing is out. No real reading of a steady clock is this.
    static constexpr auto NoRead = std::numeric_limits<core::platform::SteadyTimePoint::rep>::min();

    /// Whether the session is closed.
    /// @return True once `Close()` has run.
    [[nodiscard]] bool Closed() const noexcept
    {
        return events.isClosed();
    }

    /// Queue @p event for `Next()`.
    /// @param event What happened.
    void Deliver(DashboardEvent event)
    {
        (void) events.push(std::move(event));
    }

    /// Queue a sample outcome for `Next()`, and the frame it owes.
    /// @param event A `Sample` or a `SampleFailed`.
    void DeliverSample(DashboardEvent event)
    {
        Deliver(std::move(event));
        Deliver(DashboardEvent { .kind = DashboardEventKind::Tick });
    }

    /// Queue a failed sample and the frame it owes.
    /// @param at When the failure was observed.
    /// @param outcome What it means as an outcome.
    /// @param note Why, for a person.
    void DeliverFailure(core::platform::SteadyTimePoint at, Outcome outcome, std::string note)
    {
        DeliverSample(DashboardEvent {
            .kind = DashboardEventKind::SampleFailed, .at = at, .outcome = outcome, .note = std::move(note) });
    }

    /// The request every subscription sends.
    ///
    /// **The dashboard credential rides a FLEET request only.** A node checks it for that subject
    /// alone, and a secret sent where nothing checks it is a secret handed to whatever answers
    /// `--addr` -- or to any address a redirect names -- for nothing.
    /// @return It.
    [[nodiscard]] Wire::SubscribeRequest Request() const
    {
        return Wire::SubscribeRequest {
            .subject = parts.subject,
            .cadenceMillis = static_cast<std::uint32_t>(parts.interval.count()),
            .dashboardToken = parts.subject == Wire::LiveSubject::Fleet ? parts.dashboardToken : std::string {},
        };
    }

    /// Name the endpoint the stream now dials, for a drain that reads it from another thread.
    /// @param where The endpoint.
    void StreamingAt(Endpoint const& where)
    {
        auto const lock = std::scoped_lock { streamingGuard };
        streaming = EndpointText(where);
    }

    /// Mark a dial or read as out, for `ReadOutstandingSince()`.
    void MarkOut() noexcept
    {
        readSince.store(parts.clock->now().time_since_epoch().count(), std::memory_order_release);
    }

    /// Mark that nothing is out any more.
    void MarkBack() noexcept
    {
        readSince.store(NoRead, std::memory_order_release);
    }

    /// One producer has ended; the last one says so to both kinds of waiter.
    void ProducerEnded() noexcept
    {
        if (--producers != 0)
            return;
        drained.store(true, std::memory_order_release);
        finished.close();
    }
};

namespace
{
    /// A re-subscription is due: say so to the queue the stream waits on.
    /// @param due The source's `due` queue.
    void SubscriptionDue(void* due)
    {
        (void) static_cast<core::async::AsyncQueue<std::monostate>*>(due)->push(std::monostate {});
    }

    /// What a refusal of a subscription leaves the session to do.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class RefusalCourse : std::uint8_t
    {
        Wait,         ///< A later attempt can outlive it: a gap, and a retry after an interval.
        Caller,       ///< About this caller: ends a session that has read nothing, a gap in one that has.
        Incompatible, ///< This client and that node cannot stream at all: ends the session whatever was read.
        Last,
    };

    /// One refusal code a subscription treats other than as `Caller`.
    struct StreamRefusalRow
    {
        Wire::ErrorCode code; ///< What the node refused with.
        Outcome outcome;      ///< What the gap it draws means.
        RefusalCourse course; ///< What the session does next.
        std::string_view why; ///< Appended to the note, naming what the operator can change; empty for nothing.
    };

    /// The refusals that are not about the caller. Every other code is `Caller`, `Refused`: the safe
    /// default, because a code nobody enumerated read as *wait* would retry forever against a node
    /// that has said no.
    constexpr auto StreamRefusalRows = std::to_array<StreamRefusalRow>({
        // `NotLeader` naming an address never reaches this table: it is followed. Naming nobody, no
        // leader is known yet, and one will be.
        { .code = Wire::ErrorCode::NotLeader, .outcome = Outcome::Unreachable, .course = RefusalCourse::Wait, .why = "" },
        { .code = Wire::ErrorCode::EndpointBusy, .outcome = Outcome::Unreachable, .course = RefusalCourse::Wait, .why = "" },
        { .code = Wire::ErrorCode::UnsupportedVersion,
          .outcome = Outcome::Protocol,
          .course = RefusalCourse::Incompatible,
          .why = "; this client and that node speak different 0xFC wire versions, so upgrade them together" },
        { .code = Wire::UnimplementedVerb,
          .outcome = Outcome::Protocol,
          .course = RefusalCourse::Incompatible,
          .why = "; that node is older than live-stats subscriptions, so upgrade it" },
    });

    /// The remedy an otherwise unlisted refusal names, as the tail of its note; empty for none.
    ///
    /// **What THIS client can change, never a restatement of the node's detail**, which the note already
    /// carries. A fleet refusal therefore turns on whether the request carried the dashboard credential:
    /// without one, where to pass it; with one, that it was not accepted -- which is true both of a wrong
    /// secret and of a leader that names no `--dashboard-token-file` and streams to its own machine only,
    /// two cases one code cannot tell apart and the node's detail does.
    /// @param code What the node refused with.
    /// @param request What was asked.
    /// @return The remedy.
    [[nodiscard]] std::string_view CallerRemedy(Wire::ErrorCode code, Wire::SubscribeRequest const& request) noexcept
    {
        if (code == Wire::ErrorCode::NotAMember)
            return "; add this machine to that node's --fleet-member list";
        if (code != Wire::ErrorCode::Unauthenticated)
            return {};
        if (request.subject != Wire::LiveSubject::Fleet)
            return "; present the credential with --token-file";
        return DashboardCredentialRemedy(!request.dashboardToken.empty());
    }

    /// A stream's failure, named by where it was: `<endpoint>: <why>`.
    ///
    /// For an account that does not name the endpoint itself -- a read that broke, an end, a frame this
    /// client cannot read -- so a remark says whose stream it was, which after a leader redirect is not
    /// `--addr`. A dial's and an AUTH refusal's accounts already name it and are delivered as they are.
    /// @param where Where the stream was.
    /// @param why What happened.
    /// @return The note.
    [[nodiscard]] std::string AtEndpoint(Endpoint const& where, std::string_view why)
    {
        return std::format("{}: {}", EndpointText(where), why);
    }

    /// Decide what a refusal means for the session, and say so in the history.
    ///
    /// **A `NotLeader` naming an address is an instruction, not a failure**: the stream follows it at
    /// once, bounded by `MaxLeaderRedirects`. A refusal a later attempt can outlive -- no leader known
    /// yet, a node at its subscription cap -- is a gap and a retry. Anything else is about this
    /// client's address or credential: it ends a session that has read nothing, and is a gap in one
    /// that has, because a node that revoked a watcher a minute ago may admit it again (§9.17).
    /// @param state The source.
    /// @param at When the refusal arrived.
    /// @param frame The refusal.
    /// @param where Who refused; replaced by the leader it named when this returns `Follow`.
    /// @return What the source does next.
    [[nodiscard]] StreamEnd Refused(LiveEventSource::State* state,
                                    core::platform::SteadyTimePoint at,
                                    LiveFrame const& frame,
                                    Endpoint* where)
    {
        auto const code = frame.code.value_or(Wire::ErrorCode::MalformedFrame);
        // `DecideLeaderHop`, the rule `fleet` follows too. A failed subscription is a gap, and the next
        // attempt starts again at `--addr`.
        auto const hop = DecideLeaderHop(code, frame.note, state->redirects);
        switch (hop.kind)
        {
            case LeaderHopKind::Follow:
                ++state->redirects;
                *where = hop.next;
                return StreamEnd::Follow;
            case LeaderHopKind::Exhausted:
                state->DeliverFailure(at,
                                      Outcome::Unreachable,
                                      std::format("followed {} leader redirects without a stream; the last named {}",
                                                  MaxLeaderRedirects,
                                                  hop.named));
                return StreamEnd::Retry;
            case LeaderHopKind::NotARedirect:
                break;
        }

        auto const* const row =
            core::findIfOrNull(StreamRefusalRows, [code](StreamRefusalRow const& each) { return each.code == code; });
        auto const outcome = row != nullptr ? row->outcome : Outcome::Refused;
        auto const course = row != nullptr ? row->course : RefusalCourse::Caller;
        auto const why = row != nullptr ? row->why : CallerRemedy(code, state->Request());
        state->DeliverFailure(
            at, outcome, std::format("{} refused the subscription: {}{}", EndpointText(*where), frame.note, why));
        switch (course)
        {
            case RefusalCourse::Wait:
                return StreamEnd::Retry;
            case RefusalCourse::Caller:
                return state->readSomething ? StreamEnd::Retry : StreamEnd::Finish;
            case RefusalCourse::Incompatible:
            case RefusalCourse::Last:
                break;
        }
        return StreamEnd::Finish;
    }

    /// Deliver a reading, stamped when its frame arrived.
    /// @param state The source.
    /// @param at When the frame arrived.
    /// @param frame The reading.
    /// @param where Where the stream answers.
    /// @param cadence What its grant said the server keeps.
    void DeliverReading(LiveEventSource::State* state,
                        core::platform::SteadyTimePoint at,
                        LiveFrame frame,
                        Endpoint const& where,
                        std::optional<std::chrono::milliseconds> cadence)
    {
        state->readSomething = true;
        state->DeliverSample(DashboardEvent { .kind = DashboardEventKind::Sample,
                                              .at = at,
                                              .reading = std::move(frame.reading),
                                              .document = std::move(frame.document),
                                              .where = EndpointText(where),
                                              .cadence = cadence,
                                              .nodeStatus = std::move(frame.nodeStatus) });
    }

    /// Open one stream at @p where and read it until it ends.
    ///
    /// Every frame is read by `TakeFrame`, off the reactor and back, so the terminal and the stop
    /// request are heard while a read is out.
    /// @param state The source; the stream holds it alive.
    /// @param where Where to subscribe; replaced by the leader when this returns `Follow`.
    /// @return How the stream ended.
    core::async::Task<StreamEnd> ReadStream(LiveEventSource::State* state, Endpoint* where)
    {
        auto const& parts = state->parts;

        state->StreamingAt(*where);
        state->MarkOut();
        auto const opened = co_await OpenStream(parts.subscription, *where, state->Request(), parts.pool, parts.reactor);
        state->MarkBack();
        if (state->Closed())
            co_return StreamEnd::Closed;
        if (!opened.has_value())
        {
            state->DeliverFailure(parts.clock->now(), Outcome::Unreachable, opened.error().detail);
            co_return StreamEnd::Retry;
        }

        auto cadence = std::optional<std::chrono::milliseconds> {};
        while (!state->Closed())
        {
            state->MarkOut();
            auto outcome = co_await TakeFrame(parts.subscription, parts.clock, parts.pool, parts.reactor);
            state->MarkBack();
            // Closed while the read was on the pool: what it brought back describes a session that
            // has already ended.
            if (state->Closed())
                co_return StreamEnd::Closed;
            if (!outcome.frame.has_value())
            {
                state->DeliverFailure(
                    outcome.takenAt, Outcome::Unreachable, AtEndpoint(*where, outcome.frame.error().detail));
                co_return StreamEnd::Retry;
            }

            auto frame = ReadLiveFrame(parts.subject, *outcome.frame);
            switch (frame.kind)
            {
                case LiveFrameKind::Granted:
                    cadence = std::chrono::milliseconds { frame.granted.grantedCadenceMillis };
                    state->redirects = 0;
                    // A snapshot comes every cadence whether or not anything changed, so that long a
                    // silence is a stream that has died, not a quiet one.
                    parts.subscription->ExpectEvery(*cadence);
                    break;
                case LiveFrameKind::Reading:
                    DeliverReading(state, outcome.takenAt, std::move(frame), *where, cadence);
                    break;
                case LiveFrameKind::Event:
                case LiveFrameKind::Gap:
                    // The same tick's reading follows an event, and a gap is the interval between two
                    // readings the fold already measures: neither is news on its own.
                    break;
                case LiveFrameKind::Ended:
                    state->DeliverFailure(outcome.takenAt, Outcome::Unreachable, AtEndpoint(*where, frame.note));
                    co_return StreamEnd::Retry;
                case LiveFrameKind::Refused:
                    co_return Refused(state, outcome.takenAt, frame, where);
                case LiveFrameKind::Unreadable:
                case LiveFrameKind::Last:
                    state->DeliverFailure(outcome.takenAt, Outcome::Protocol, AtEndpoint(*where, frame.note));
                    co_return StreamEnd::Finish;
            }
        }
        co_return StreamEnd::Closed;
    }

    /// Subscribe, read, and subscribe again after an interval; until closed or finished.
    /// @param shared The source's state; held so it outlives the source if need be.
    core::async::DetachedTask RunStream(std::shared_ptr<LiveEventSource::State> shared)
    {
        auto const& parts = shared->parts;
        co_await core::async::ResumeOn { *parts.reactor };

        auto where = parts.endpoint;
        while (!shared->Closed())
        {
            auto const end = co_await ReadStream(shared.get(), &where);
            if (end == StreamEnd::Closed)
                break;
            if (end == StreamEnd::Finish)
            {
                shared->Deliver(
                    DashboardEvent { .kind = DashboardEventKind::Detached, .note = std::string { StreamFinishedNote } });
                break;
            }
            if (end == StreamEnd::Follow)
                continue;

            // A stream that failed starts again where the operator pointed, not at a leader it once
            // followed: that leader may be the one that went away. And with its redirects counted
            // afresh, or a session that once ran out of them could never follow one again.
            where = parts.endpoint;
            shared->redirects = 0;
            // `Close()` wakes this through `due`, so the timer never needs to look at anything
            // before its deadline -- and core-cpp's parks once rather than polling.
            auto const timer = core::net::DeadlineTimer {
                *parts.reactor, parts.reactor->clock().now() + parts.interval, &SubscriptionDue, &shared->due
            };
            if (!(co_await shared->due.pop()).has_value())
                break;
        }
        shared->ProducerEnded();
    }

    /// Forward what the terminal says, until closed or until the terminal goes away.
    /// @param shared The source's state; held so it outlives the source if need be.
    core::async::DetachedTask RunTerminal(std::shared_ptr<LiveEventSource::State> shared)
    {
        co_await core::async::ResumeOn { *shared->parts.reactor };

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
        // restores the terminal, and a read still on the pool is no reason to leave an
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
    core::async::DetachedTask RunStopWatch(std::shared_ptr<LiveEventSource::State> shared)
    {
        auto& parts = shared->parts;
        auto const wake = co_await parts.stop->Stopped(parts.stopWaiter, parts.reactor);

        // Released as soon as nothing waits on it, which is what puts the previous signal
        // disposition back: a read stuck on the pool is no reason to keep Ctrl-C redirected
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
    // Zero would ask the server for its floor and re-subscribe back to back after a failure --
    // never blocking the reactor, and never letting the endpoint rest either. Admission refuses
    // anything below a subject's floor, so this is a caller's mistake, not input.
    assert(_state->parts.interval > std::chrono::milliseconds::zero());
    assert(_state->parts.subscription != nullptr);

    ++_state->producers;
    RunStream(_state);
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

core::async::Task<DashboardEvent> LiveEventSource::Next()
{
    auto const state = _state;
    auto event = co_await state->events.pop();
    if (!event.has_value())
        co_return DashboardEvent { .kind = DashboardEventKind::Detached, .note = std::string { SessionClosedNote } };
    co_return *std::move(event);
}

void LiveEventSource::Close() noexcept
{
    auto& state = *_state;
    if (state.Closed())
        return;
    state.events.close();
    state.due.close();
    // The one call allowed while a read is on the pool: a half-close, which the node answers by
    // closing, and that close is what returns the read.
    state.parts.subscription->Leave();
    if (state.parts.terminal != nullptr)
        state.parts.terminal->Close();
    if (state.parts.stop != nullptr)
        state.parts.stop->Cancel();
}

bool LiveEventSource::IsDrained() const noexcept
{
    return _state->drained.load(std::memory_order_acquire);
}

std::optional<core::platform::SteadyTimePoint> LiveEventSource::ReadOutstandingSince() const noexcept
{
    auto const since = _state->readSince.load(std::memory_order_acquire);
    if (since == State::NoRead)
        return std::nullopt;
    return core::platform::SteadyTimePoint { core::platform::SteadyTimePoint::duration { since } };
}

std::string LiveEventSource::StreamingEndpoint() const
{
    auto const lock = std::scoped_lock { _state->streamingGuard };
    return _state->streaming.empty() ? EndpointText(_state->parts.endpoint) : _state->streaming;
}

IFrameSink* LiveEventSource::Frames() noexcept
{
    return _state->presents ? &_state->frames : nullptr;
}

core::async::Task<void> LiveEventSource::Drained()
{
    auto const state = _state;
    // Nothing is ever pushed, so this resumes exactly when the last producer closes it.
    (void) co_await state->finished.pop();
}

} // namespace FastCache::Cli
