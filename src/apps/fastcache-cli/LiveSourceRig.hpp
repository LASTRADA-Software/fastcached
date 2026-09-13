// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file LiveSourceRig.hpp
/// The deterministic rig every `live-stats` session test runs a `LiveEventSource` on: a
/// session reactor, a second one standing in for each pool, one manual clock, and the fakes
/// the source composes. Shared because the source's own cases and the session runner's
/// drive the same object, and a second copy of a fixture is a second place for it to be
/// wrong.

#include "DashboardLoop.hpp"
#include "LiveEventSource.hpp"
#include "ScriptedExchange.hpp"
#include "ScriptedStopSignal.hpp"

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

namespace FastCache::Cli::Testing
{

using FastCache::Testing::Unwrap;

/// The interval every case samples at.
inline constexpr auto Interval = std::chrono::milliseconds { 2000 };

/// A reading `ChooseStats` accepts, so a sample counts against a budget.
/// @return One answered attempt.
[[nodiscard]] inline std::vector<StatsAttempt> Reading()
{
    return { StatsAttempt { .origin = StatsOrigin::Info,
                            .asked = true,
                            .record = RecordValue({ Field { .name = "curr_connections", .value = TextCell("10") } }),
                            .note = {} } };
}

/// A round in which the source was asked and did not answer: what a dead connection gives.
/// @return The attempts.
[[nodiscard]] inline std::vector<StatsAttempt> NothingAnswered()
{
    return { StatsAttempt { .origin = StatsOrigin::Info, .asked = true, .record = std::nullopt, .note = "closed" } };
}

/// A gatherer whose connection dies after its first answer, as a daemon's does when it restarts.
class DyingGatherer final: public IStatsGatherer
{
  public:
    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        return ++_calls == 1 ? Reading() : NothingAnswered();
    }

    /// @return How many gathers it served.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    int _calls { 0 };
};

/// A node status naming @p version, which is all these cases tell statuses apart by.
/// @param version The version it names.
/// @return The status.
[[nodiscard]] inline CompileCacheWire::NodeStatusFields NodeStatusNamed(std::string version)
{
    auto status = CompileCacheWire::NodeStatusFields {};
    status.version = std::move(version);
    return status;
}

/// A node that answers every status read with a status naming the read: `read-1`, `read-2`, ...
///
/// Numbered so a case can tell a status read with THIS sample from one remembered from an earlier.
class CountingNodeStatus final: public INodeStatusReader
{
  public:
    [[nodiscard]] std::optional<CompileCacheWire::NodeStatusFields> ReadNodeStatus() override
    {
        return NodeStatusNamed(std::format("read-{}", ++_reads));
    }

    /// @return How many statuses were read.
    [[nodiscard]] int Reads() const noexcept
    {
        return _reads.load();
    }

  private:
    std::atomic<int> _reads { 0 };
};

/// What one scripted dial opened: fixed attempts, and a status naming the dial.
class ScriptedDialed final: public IDialedStats
{
  public:
    /// @param attempts What every gather reports.
    /// @param name What every status read's version says.
    ScriptedDialed(std::vector<StatsAttempt> attempts, std::string name):
        _attempts { std::move(attempts) },
        _name { std::move(name) }
    {
    }

    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        return _attempts;
    }

    [[nodiscard]] std::optional<CompileCacheWire::NodeStatusFields> ReadNodeStatus() override
    {
        return NodeStatusNamed(_name);
    }

  private:
    std::vector<StatsAttempt> _attempts;
    std::string _name;
};

/// Dials gatherers from a script, counting the dials.
class ScriptedDialer final: public IStatsDialer
{
  public:
    /// @param answers Whether each dial's gatherer answers; the last is repeated.
    explicit ScriptedDialer(std::vector<bool> answers):
        _answers { std::move(answers) }
    {
    }

    [[nodiscard]] std::unique_ptr<IDialedStats> Dial() override
    {
        auto const answers = _answers[std::min(_dials, _answers.size() - 1)];
        ++_dials;
        return std::make_unique<ScriptedDialed>(answers ? Reading() : NothingAnswered(), std::format("dialled-{}", _dials));
    }

    /// @return How many dials were made.
    [[nodiscard]] std::size_t Dials() const noexcept
    {
        return _dials;
    }

  private:
    std::vector<bool> _answers;
    std::size_t _dials { 0 };
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

        void Present(std::string_view frame) override
        {
            ++_record->frames;
            _record->last = frame;
        }

      private:
        PresenterRecord* _record;
    };

    TerminalRelease const* events { nullptr }; ///< What became of the events it presented over.
    std::size_t frames { 0 };                  ///< How many frames were presented.
    std::string last {};                       ///< The newest frame.
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
        _release->closedFirst = _events.IsClosed();
    }

    /// The terminal goes away by itself: its reader gets `Detached` while nobody has closed it.
    void GoAway()
    {
        (void) _events.Push(DashboardEvent { .kind = DashboardEventKind::Detached, .note = "the terminal went away" });
    }

    /// Say something at the terminal.
    /// @param event A Key or a Resize.
    void Say(DashboardEvent event)
    {
        (void) _events.Push(std::move(event));
    }

    [[nodiscard]] Task<DashboardEvent> Next() override
    {
        auto const event = co_await _events.Pop();
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

/// An admin surface answering one scripted document to every fetch, and remembering each path.
///
/// Read and written on whichever rig reactor runs the fetch; the rig drains them on one thread.
class ScriptedDocument final: public IAdminDocument
{
  public:
    /// @param answer What every fetch returns.
    explicit ScriptedDocument(std::expected<std::string, AdminError> answer):
        _answer { std::move(answer) }
    {
    }

    [[nodiscard]] std::expected<std::string, AdminError> FetchAdmin(std::string_view path) override
    {
        _asked.emplace_back(path);
        return _answer;
    }

    /// Every path this was asked for, in order.
    /// @return The paths.
    [[nodiscard]] std::vector<std::string> const& Asked() const noexcept
    {
        return _asked;
    }

  private:
    std::expected<std::string, AdminError> _answer;
    std::vector<std::string> _asked;
};

/// A view whose frame is the sample count, which is all these cases read.
class CountView final: public IDashboardView
{
  public:
    [[nodiscard]] std::string Frame(DashboardModel const& model) override
    {
        return std::to_string(model.samples);
    }
};

/// Counts presented frames.
class CountSink final: public IFrameSink
{
  public:
    void Present(std::string_view /*frame*/) override
    {
        ++frames;
    }

    std::size_t frames { 0 };
};

/// A reactor for the session, one more standing in for each pool, one clock for all of them.
///
/// **The pool is a `TestReactor` so a case decides when a gather RUNS.** A real pool
/// would run it at once, and "a keystroke arrives while a sample is outstanding" would
/// then be a race rather than an input.
struct Rig
{
    ManualClock clock {};
    TestReactor reactor { clock };
    TestReactor pool { clock };
    TestReactor stopWaiter { clock };
    ScriptedGatherer gatherer { Reading() };
    CountingNodeStatus status {};
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

    /// A source over this rig, with no terminal.
    /// @return The parts.
    [[nodiscard]] LiveSourceParts Parts()
    {
        return LiveSourceParts { .reactor = &reactor,
                                 .gatherer = &gatherer,
                                 .status = nullptr,
                                 .admin = nullptr,
                                 .document = {},
                                 .dialer = nullptr,
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

    /// Run everything runnable, gathers included, until nothing is.
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

/// The next event, when one is already due without running a gather.
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
    rig.reactor.Submit(task.Native());
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
    rig.reactor.Submit(task.Native());
    rig.Settle();
    CHECK(drained);
    CHECK(rig.reactor.PendingTimers() == 0);
    CHECK(rig.reactor.PendingSubmissions() == 0);
    CHECK(rig.pool.PendingSubmissions() == 0);
}

} // namespace FastCache::Cli::Testing
