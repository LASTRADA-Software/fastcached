// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file LiveSourceRig.hpp
/// The deterministic rig every `live-stats` session test runs a `LiveEventSource` on: a
/// session reactor, a second one standing in for each pool, one manual clock, a scripted
/// subscription, and the fakes the source composes. Shared because the source's own cases and the session runner's
/// drive the same object, and a second copy of a fixture is a second place for it to be
/// wrong.

#include "DashboardFrame.hpp"
#include "DashboardLoop.hpp"
#include "LiveEventSource.hpp"
#include "LiveSubscriber.hpp"
#include "ScriptedStopSignal.hpp"

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

namespace FastCache::Cli::Testing
{

using FastCache::Testing::Unwrap;

/// The interval every case asks the server for, and waits before subscribing again.
inline constexpr auto Interval = std::chrono::milliseconds { 2000 };

/// Where every rig's session subscribes first: its `--addr`.
inline constexpr auto RigHost = std::string_view { "127.0.0.1" };

/// The port half of `RigHost`'s endpoint.
inline constexpr std::uint16_t RigPort = 6674;

/// What a scripted stream answers a read past its last frame: the silence a real stream's idle bound ends.
inline constexpr auto SilentStream = std::string_view { "the stream went silent past its idle bound" };

/// The endpoint a rig's session subscribes at.
/// @return `RigHost:RigPort`.
[[nodiscard]] inline Endpoint RigEndpoint()
{
    return Endpoint { .host = std::string { RigHost }, .port = RigPort };
}

/// A reading a case can tell from another by one counter's value.
/// @param opened What `LiveSubscriptionsOpened` reads.
/// @return The reading, captured by the daemon's own capture.
[[nodiscard]] inline StatsReading ReadingOpened(std::uint64_t opened)
{
    AtomicMetricsSink sink;
    sink.Increment(IMetricsSink::Counter::LiveSubscriptionsOpened, opened);
    return CaptureStatsReading(sink, MetricsSnapshot {}, EverySurface);
}

/// A node status naming @p version, which is all these cases tell statuses apart by.
/// @param version The version it names.
/// @return The status.
[[nodiscard]] inline CompileCacheWire::NodeStatusFields NodeStatusNamed(std::string version)
{
    auto status = CompileCacheWire::NodeStatusFields {};
    status.version = std::move(version);
    return status;
}

/// @param payload A push's payload.
/// @return The reply a stream carries it in.
[[nodiscard]] inline NodeReply PushFrame(std::vector<std::byte> payload)
{
    return NodeReply {
        .status = CompileCacheWire::Status::Push, .payload = std::move(payload), .code = std::nullopt, .detail = {}
    };
}

/// A grant, in this build's layout for a stats subject and none for the fleet.
/// @param subject What was granted.
/// @param cadence The cadence the server keeps.
/// @return The grant, as a reply.
[[nodiscard]] inline NodeReply GrantFrame(CompileCacheWire::LiveSubject subject,
                                          std::chrono::milliseconds cadence = Interval)
{
    return PushFrame(CompileCacheWire::EncodeLiveSubscribed(CompileCacheWire::LiveSubscribedFields {
        .subject = subject,
        .grantedCadenceMillis = static_cast<std::uint32_t>(cadence.count()),
        .statsLayout = subject == CompileCacheWire::LiveSubject::Fleet ? std::uint64_t { 0 } : StatsReadingLayout,
        .endpoint = "rig.test:6674" }));
}

/// A cache snapshot carrying @p reading.
/// @param reading The reading.
/// @param tick The tick it was captured on.
/// @return The snapshot, as a reply.
[[nodiscard]] inline NodeReply CacheReadingFrame(StatsReading const& reading, std::uint64_t tick = 1)
{
    return PushFrame(CompileCacheWire::EncodeLiveSnapshot(tick, EncodeStatsReading(reading)));
}

/// A node snapshot: @p reading, then the node's status on the same tick.
/// @param reading The reading.
/// @param status The node's status.
/// @param tick The tick it was captured on.
/// @return The snapshot, as a reply.
[[nodiscard]] inline NodeReply NodeReadingFrame(StatsReading const& reading,
                                                CompileCacheWire::NodeStatusFields const& status,
                                                std::uint64_t tick = 1)
{
    auto const encoded = EncodeStatsReading(reading);
    auto const described = CompileCacheWire::EncodeNodeStatus(status);
    auto const body =
        WireFields::Encode({ std::span<std::byte const> { encoded }, std::span<std::byte const> { described } });
    return PushFrame(CompileCacheWire::EncodeLiveSnapshot(tick, body));
}

/// A fleet snapshot: the leader's document, whole.
/// @param document The document.
/// @param tick The tick it was captured on.
/// @return The snapshot, as a reply.
[[nodiscard]] inline NodeReply FleetDocumentFrame(std::string_view document, std::uint64_t tick = 1)
{
    return PushFrame(CompileCacheWire::EncodeLiveSnapshot(tick, CompileCacheWire::AsBytes(document)));
}

/// The refusal a server ends a stream with, or refuses one with.
/// @param code What it refused with.
/// @param detail What it said.
/// @return The refusal, as a reply.
[[nodiscard]] inline NodeReply RefusalFrame(CompileCacheWire::ErrorCode code, std::string detail)
{
    return NodeReply { .status = CompileCacheWire::Status::Error, .payload = {}, .code = code, .detail = std::move(detail) };
}

/// The orderly end a server closes a stream with.
/// @return The end, as a reply.
[[nodiscard]] inline NodeReply EndedFrame()
{
    return NodeReply { .status = CompileCacheWire::Status::Ok, .payload = {}, .code = std::nullopt, .detail = {} };
}

/// One scripted answer to a `Read`.
using ScriptedFrame = std::expected<NodeReply, ExchangeError>;

/// What one `Open` dials into: whether the dial succeeds, and what its stream then answers.
struct ScriptedStream
{
    std::optional<ExchangeError> refused {}; ///< Set for a dial that fails; the stream then answers nothing.
    std::vector<ScriptedFrame> frames {};    ///< Each read's answer, in order; past the last, `SilentStream`.
};

/// A cache stream: its grant, then one snapshot per reading, ticks counted from 1.
/// @param readings The readings, in order.
/// @return The stream.
[[nodiscard]] inline ScriptedStream CacheStream(std::vector<StatsReading> const& readings)
{
    auto stream = ScriptedStream { .refused = std::nullopt, .frames = { GrantFrame(CompileCacheWire::LiveSubject::Cache) } };
    auto tick = std::uint64_t { 0 };
    for (auto const& reading: readings)
        stream.frames.emplace_back(CacheReadingFrame(reading, ++tick));
    return stream;
}

/// A dial that fails, as a node that is down does.
/// @param why What the dial says.
/// @return The stream.
[[nodiscard]] inline ScriptedStream RefusedDial(std::string why = "connection refused")
{
    return ScriptedStream { .refused = ExchangeError { .kind = ExchangeFailure::Unreachable, .detail = std::move(why) },
                            .frames = {} };
}

/// A subscription answering from a script: one `ScriptedStream` per `Open`, the last repeated.
///
/// **Every call runs where the source runs it**: `Open` and `Read` on the rig's pool reactor, so a case
/// decides when a read HAPPENS by draining that reactor; `Leave` from `Close()`, on the session's. Both
/// reactors are drained on one thread, so only `Leave` needs to be safe from another, and it is atomic.
class ScriptedSubscription final: public ILiveSubscription
{
  public:
    /// @param streams What each dial opens; the last is repeated for every dial past it.
    explicit ScriptedSubscription(std::vector<ScriptedStream> streams):
        _streams { std::move(streams) }
    {
    }

    [[nodiscard]] std::expected<void, ExchangeError> Open(Endpoint const& where,
                                                          CompileCacheWire::SubscribeRequest const& request) override
    {
        _dialled.push_back(where);
        _requests.push_back(request);
        _current = _streams.empty() ? ScriptedStream {} : _streams[std::min(_opens, _streams.size() - 1)];
        ++_opens;
        _next = 0;
        if (_current.refused.has_value())
            return std::unexpected(*_current.refused);
        return {};
    }

    [[nodiscard]] std::expected<NodeReply, ExchangeError> Read() override
    {
        ++_reads;
        if (_current.refused.has_value() || _next >= _current.frames.size())
            return std::unexpected(
                ExchangeError { .kind = ExchangeFailure::Unreachable, .detail = std::string { SilentStream } });
        return _current.frames[_next++];
    }

    void ExpectEvery(std::chrono::milliseconds cadence) override
    {
        _expected.push_back(cadence);
    }

    void Leave() noexcept override
    {
        _leaves.fetch_add(1, std::memory_order_acq_rel);
    }

    /// @return How many dials were made.
    [[nodiscard]] std::size_t Opens() const noexcept
    {
        return _opens;
    }

    /// @return How many reads were made, across every stream.
    [[nodiscard]] std::size_t Reads() const noexcept
    {
        return _reads;
    }

    /// @return How many times the source left.
    [[nodiscard]] int Leaves() const noexcept
    {
        return _leaves.load(std::memory_order_acquire);
    }

    /// @return Where each dial went, in order.
    [[nodiscard]] std::vector<Endpoint> const& Dialled() const noexcept
    {
        return _dialled;
    }

    /// @return What each dial asked for, in order.
    [[nodiscard]] std::vector<CompileCacheWire::SubscribeRequest> const& Requests() const noexcept
    {
        return _requests;
    }

    /// @return Every silence bound the source set, in order.
    [[nodiscard]] std::vector<std::chrono::milliseconds> const& Expected() const noexcept
    {
        return _expected;
    }

  private:
    std::vector<Endpoint> _dialled {};
    std::vector<CompileCacheWire::SubscribeRequest> _requests {};
    std::vector<std::chrono::milliseconds> _expected {};
    std::vector<ScriptedStream> _streams;
    ScriptedStream _current {};
    std::size_t _opens { 0 };
    std::size_t _next { 0 };
    std::size_t _reads { 0 };
    std::atomic<int> _leaves { 0 };
};

/// What became of a terminal a source owned.
struct TerminalRelease
{
    bool released { false };    ///< It was destroyed, which is what restores a real one.
    bool closedFirst { false }; ///< It had been closed when it was, as its contract asks.
};

/// What a started terminal's presenter was given.
struct PresenterRecord
{
    /// A presenter that counts into a record the acquisition keeps.
    class Sink final: public IFrameSink
    {
      public:
        /// @param record Where it counts; outlives the sink.
        explicit Sink(PresenterRecord* record) noexcept:
            _record { record }
        {
        }

        Sink(Sink const&) = delete;
        Sink(Sink&&) = delete;
        Sink& operator=(Sink const&) = delete;
        Sink& operator=(Sink&&) = delete;

        ~Sink() override
        {
            _record->released = true;
            _record->afterEvents = _record->events != nullptr && _record->events->released;
        }

        void PresentPlaced(DashboardFrame const& frame) override
        {
            ++_record->frames;
            _record->last = frame.text;
            _record->placements = frame.placements;
        }

      private:
        PresenterRecord* _record;
    };

    TerminalRelease const* events { nullptr }; ///< What became of the events it presented over.
    std::size_t frames { 0 };                  ///< How many frames were presented.
    std::string last {};                       ///< The newest frame.
    std::vector<FramePlacement> placements {}; ///< The newest frame's images.
    bool released { false };                   ///< Whether the presenter was destroyed.
    bool afterEvents { false };                ///< Whether it was destroyed after the events were.
};

/// A terminal a case speaks for, one event at a time.
///
/// It parks on an empty queue exactly as a terminal read parks on an idle input, and it
/// resumes through the reactor, so a case PLACES a keystroke between two other events
/// rather than racing one in. Closing it is how a case makes the terminal go away.
class SpokenTerminal final: public IDashboardEventSource
{
  public:
    /// @param reactor Where a parked read is resumed.
    /// @param release Where to record this terminal's destruction, which is when production
    ///        restores one; null when the case does not ask.
    explicit SpokenTerminal(IReactor& reactor, TerminalRelease* release = nullptr):
        _events { reactor, AsyncQueueOptions {} },
        _release { release }
    {
    }

    SpokenTerminal(SpokenTerminal const&) = delete;
    SpokenTerminal(SpokenTerminal&&) = delete;
    SpokenTerminal& operator=(SpokenTerminal const&) = delete;
    SpokenTerminal& operator=(SpokenTerminal&&) = delete;

    ~SpokenTerminal() override
    {
        if (_release == nullptr)
            return;
        _release->released = true;
        _release->closedFirst = _events.isClosed();
    }

    /// The terminal goes away by itself: its reader gets `Detached` while nobody has closed it.
    void GoAway()
    {
        (void) _events.push(DashboardEvent { .kind = DashboardEventKind::Detached, .note = "the terminal went away" });
    }

    /// Say something at the terminal.
    /// @param event A Key or a Resize.
    void Say(DashboardEvent event)
    {
        (void) _events.push(std::move(event));
    }

    [[nodiscard]] Task<DashboardEvent> Next() override
    {
        auto const event = co_await _events.pop();
        if (!event.has_value())
            co_return DashboardEvent { .kind = DashboardEventKind::Detached, .note = "the terminal went away" };
        co_return Unwrap(event);
    }

    void Close() noexcept override
    {
        _events.Close();
    }

  private:
    AsyncQueue<DashboardEvent> _events;
    TerminalRelease* _release;
};

/// A view whose frame is the sample count, which is all these cases read.
class CountView final: public IDashboardView
{
  public:
    [[nodiscard]] DashboardFrame PlacedFrame(DashboardModel const& model) override
    {
        return DashboardFrame { .text = std::to_string(model.samples), .placements = {} };
    }
};

/// Counts presented frames.
class CountSink final: public IFrameSink
{
  public:
    void PresentPlaced(DashboardFrame const& /*frame*/) override
    {
        ++frames;
    }

    std::size_t frames { 0 };
};

/// A reactor for the session, one more standing in for each pool, one clock for all of them.
///
/// **The pool is a `TestReactor` so a case decides when a dial or a read RUNS.** A real pool
/// would run it at once, and "a keystroke arrives while a read is outstanding" would then be a
/// race rather than an input.
struct Rig
{
    ManualClock clock {};
    TestReactor reactor { clock };
    TestReactor pool { clock };
    TestReactor stopWaiter { clock };
    ScriptedSubscription subscription { { CacheStream({ ReadingOpened(1) }) } };
    CountView view {};
    CountSink sink {};

    /// The terminal `SpokenParts()` built; owned by the source it went into, so valid only
    /// until `terminalRelease.released`.
    SpokenTerminal* terminal { nullptr };

    /// What became of `terminal`.
    TerminalRelease terminalRelease {};

    /// The stop signal `StoppableParts()` built; owned by the source it went into, so valid
    /// only until `stopReleased`.
    ScriptedStopSignal* stop { nullptr };

    /// Whether the source has released `stop`.
    bool stopReleased { false };

    /// A cache source over this rig, with no terminal.
    /// @return The parts.
    [[nodiscard]] LiveSourceParts Parts()
    {
        return LiveSourceParts { .reactor = &reactor,
                                 .subscription = &subscription,
                                 .subject = CompileCacheWire::LiveSubject::Cache,
                                 .endpoint = RigEndpoint(),
                                 .dashboardToken = {},
                                 .pool = &pool,
                                 .clock = &clock,
                                 .interval = Interval,
                                 .terminal = nullptr,
                                 .frames = nullptr,
                                 .stop = nullptr,
                                 .stopWaiter = nullptr };
    }

    /// A source over this rig with no terminal and a stop signal a case fires through `stop`,
    /// which is the non-interactive composition.
    /// @return The parts.
    [[nodiscard]] LiveSourceParts StoppableParts()
    {
        auto signal = std::make_unique<ScriptedStopSignal>(reactor, &stopReleased);
        stop = signal.get();
        auto parts = Parts();
        parts.stop = std::move(signal);
        parts.stopWaiter = &stopWaiter;
        return parts;
    }

    /// A source over this rig, with a terminal a case speaks for through `terminal`.
    /// @return The parts.
    [[nodiscard]] LiveSourceParts SpokenParts()
    {
        auto spoken = std::make_unique<SpokenTerminal>(reactor, &terminalRelease);
        terminal = spoken.get();
        auto parts = Parts();
        parts.terminal = std::move(spoken);
        return parts;
    }

    /// Run everything runnable, dials and reads included, until nothing is.
    ///
    /// Terminates because every scripted stream ends: past its last frame a read answers
    /// `SilentStream`, and the source then waits one interval on the clock before dialling again.
    void Settle()
    {
        auto progressed = true;
        while (progressed)
            progressed = reactor.Drain() + pool.Drain() != 0;
    }
};

/// Await one event into @p into.
/// @param source What to ask.
/// @param into Where the answer goes.
/// @return The task to submit.
[[nodiscard]] inline Task<void> TakeOne(IDashboardEventSource* source, std::optional<DashboardEvent>* into)
{
    *into = co_await source->Next();
}

/// Await the source's drain, then say so.
/// @param source What to wait for.
/// @param drained Set once it has.
/// @return The task to submit.
[[nodiscard]] inline Task<void> AwaitDrained(LiveEventSource* source, bool* drained)
{
    co_await source->Drained();
    *drained = true;
}

/// Drive a whole dashboard over @p source.
/// @param source The events.
/// @param view What draws.
/// @param sink Where frames go.
/// @param limits The budget.
/// @param out How it ended.
/// @return The task to submit.
[[nodiscard]] inline Task<void> RunOver(IDashboardEventSource* source,
                                        IDashboardView* view,
                                        IFrameSink* sink,
                                        DashboardLimits limits,
                                        std::optional<DashboardExit>* out)
{
    *out = co_await RunDashboard(source, &ReadStatsSample, view, sink, limits);
}

/// How a session that may not have ended stopped.
/// @param exit The session's end, if it has one.
/// @return Its stop, or `Last` when it has not ended.
[[nodiscard]] inline DashboardStop StopOf(std::optional<DashboardExit> const& exit)
{
    return exit.has_value() ? exit->stop : DashboardStop::Last;
}

/// The kind of an event that may not have arrived.
/// @param event The event.
/// @return Its kind, or `Last` when there was none.
[[nodiscard]] inline DashboardEventKind KindOf(std::optional<DashboardEvent> const& event)
{
    return event.has_value() ? event->kind : DashboardEventKind::Last;
}

/// Whether @p event is the source ending ITSELF, never the `Detached` a closed source answers.
///
/// **`NextDue` closes a source that has nothing due**, and a read of a closed source is `Detached` too -- so a
/// case asserting only the kind passes for a source that armed a retry instead of finishing (K4 survived that
/// way). The note is what separates the two.
/// @param event The event.
/// @return Whether it is the source's own end.
[[nodiscard]] inline bool FinishedItself(std::optional<DashboardEvent> const& event)
{
    return event.has_value() && event->kind == DashboardEventKind::Detached && event->note == StreamFinishedNote;
}

/// The next event, when one is already due without running a dial or a read.
///
/// **Never returns with a read still parked**: a `Task` destroyed while suspended is
/// undefined, so an event that did not arrive closes the source and settles, and the
/// case sees `Detached` -- a failed expectation rather than a crash.
/// @param rig The rig.
/// @param source The source.
/// @return The event.
[[nodiscard]] inline std::optional<DashboardEvent> NextDue(Rig& rig, LiveEventSource& source)
{
    auto event = std::optional<DashboardEvent> {};
    auto task = TakeOne(&source, &event);
    rig.reactor.submit(task.handle());
    rig.reactor.Drain();
    if (!event.has_value())
    {
        source.Close();
        rig.Settle();
    }
    return event;
}

/// Close @p source and check it drained, leaving nothing parked on either reactor.
/// @param rig The rig.
/// @param source The source.
inline void CloseAndDrain(Rig& rig, LiveEventSource& source)
{
    source.Close();
    auto drained = false;
    auto task = AwaitDrained(&source, &drained);
    rig.reactor.submit(task.handle());
    rig.Settle();
    CHECK(drained);
    CHECK(rig.reactor.PendingTimers() == 0);
    CHECK(rig.reactor.PendingSubmissions() == 0);
    CHECK(rig.pool.PendingSubmissions() == 0);
}

} // namespace FastCache::Cli::Testing
