// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "LiveEventSource.hpp"
#include "ScriptedExchange.hpp"

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

/// The interval every case samples at.
constexpr auto Interval = std::chrono::milliseconds { 2000 };

/// A reading `ChooseStats` accepts, so a sample counts against a budget.
/// @return One answered attempt.
[[nodiscard]] std::vector<StatsAttempt> Reading()
{
    auto record = Value {};
    record.shape = Shape::Record;
    record.fields.push_back(Field { .name = "curr_connections", .value = TextCell("10") });
    return { StatsAttempt { .origin = StatsOrigin::Info, .asked = true, .record = std::move(record), .note = {} } };
}

/// A terminal a case speaks for, one event at a time.
///
/// It parks on an empty queue exactly as a terminal read parks on an idle input, and it
/// resumes through the reactor, so a case PLACES a keystroke between two other events
/// rather than racing one in. Closing it is how a case makes the terminal go away.
class SpokenTerminal final: public IDashboardEventSource
{
  public:
    /// @param reactor Where a parked read is resumed.
    explicit SpokenTerminal(IReactor& reactor):
        _events { reactor, AsyncQueueOptions {} }
    {
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

/// A reactor for the session, a second one standing in for the pool, one clock for both.
///
/// **The pool is a `TestReactor` so a case decides when a gather RUNS.** A real pool
/// would run it at once, and "a keystroke arrives while a sample is outstanding" would
/// then be a race rather than an input.
struct Rig
{
    ManualClock clock {};
    TestReactor reactor { clock };
    TestReactor pool { clock };
    ScriptedGatherer gatherer { Reading() };

    /// A source over this rig.
    /// @param terminal The terminal, or null for a run with none.
    /// @return The parts.
    [[nodiscard]] LiveSourceParts Parts(std::unique_ptr<IDashboardEventSource> terminal = nullptr)
    {
        return LiveSourceParts {
            .reactor = &reactor, .gatherer = &gatherer, .pool = &pool, .interval = Interval, .terminal = std::move(terminal)
        };
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
[[nodiscard]] Task<void> TakeOne(IDashboardEventSource* source, std::optional<DashboardEvent>* into)
{
    *into = co_await source->Next();
}

/// Await the source's drain, then say so.
/// @param source What to wait for.
/// @param drained Set once it has.
/// @return The task to submit.
[[nodiscard]] Task<void> AwaitDrained(LiveEventSource* source, bool* drained)
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
[[nodiscard]] Task<void> RunOver(LiveEventSource* source,
                                 IDashboardView* view,
                                 IFrameSink* sink,
                                 DashboardLimits limits,
                                 std::optional<DashboardExit>* out)
{
    *out = co_await RunDashboard(source, view, sink, limits);
}

/// The kind of an event that may not have arrived.
/// @param event The event.
/// @return Its kind, or `Last` when there was none.
[[nodiscard]] DashboardEventKind KindOf(std::optional<DashboardEvent> const& event)
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
[[nodiscard]] std::optional<DashboardEvent> NextDue(Rig& rig, LiveEventSource& source)
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
void CloseAndDrain(Rig& rig, LiveEventSource& source)
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

} // namespace

TEST_CASE("a live source takes no second sample until its clock reaches the interval", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };

    rig.Settle();
    CHECK(rig.gatherer.Calls() == 1);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    // Everything runnable has run, and the clock has not moved: a source that sampled on
    // anything but its clock -- in a loop, or on every wake -- has asked again by now.
    auto next = std::optional<DashboardEvent> {};
    auto task = TakeOne(&source, &next);
    rig.reactor.Submit(task.Native());
    rig.Settle();
    CHECK_FALSE(next.has_value());
    CHECK(rig.gatherer.Calls() == 1);

    rig.clock.Advance(Interval - 1ms);
    rig.Settle();
    CHECK_FALSE(next.has_value());
    CHECK(rig.gatherer.Calls() == 1);

    rig.clock.Advance(1ms);
    rig.Settle();
    CHECK(rig.gatherer.Calls() == 2);
    CHECK(KindOf(next) == DashboardEventKind::Sample);

    CloseAndDrain(rig, source);
}

TEST_CASE("a keystroke that arrives while a sample is outstanding is delivered first", "[cli][live][source]")
{
    Rig rig;
    auto terminal = std::make_unique<SpokenTerminal>(rig.reactor);
    auto* const at = terminal.get();
    LiveEventSource source { rig.Parts(std::move(terminal)) };

    // The reactor has run and the pool has not: the first gather is handed off and has
    // not happened, which is the window this case is about.
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);

    at->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "x" });
    auto const key = NextDue(rig, source);
    CHECK(KindOf(key) == DashboardEventKind::Key);
    CHECK((key.has_value() && key->keys == "x"));
    // What it overtook really was still out: the gather had not run.
    CHECK(rig.gatherer.Calls() == 0);

    rig.Settle();
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);

    CloseAndDrain(rig, source);
}

TEST_CASE("quitting during a sample ends the dashboard at once and the source drains when the sample returns",
          "[cli][live][source]")
{
    Rig rig;
    auto terminal = std::make_unique<SpokenTerminal>(rig.reactor);
    auto* const at = terminal.get();
    LiveEventSource source { rig.Parts(std::move(terminal)) };
    CountView view;
    CountSink sink;

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &view, &sink, DashboardLimits {}, &exit);
    rig.reactor.Submit(run.Native());
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);

    at->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.reactor.Drain();

    // The operator was not made to wait for the scrape: the loop has returned, and the
    // gather has still not run.
    CHECK((exit.has_value() && exit->stop == DashboardStop::Quit));
    CHECK(rig.gatherer.Calls() == 0);

    // The SESSION is over and the SOURCE is not: the sample is still on the pool, reading
    // a gatherer the caller must therefore not destroy yet.
    auto drained = false;
    auto wait = AwaitDrained(&source, &drained);
    rig.reactor.Submit(wait.Native());
    rig.reactor.Drain();
    CHECK_FALSE(drained);

    rig.Settle();
    CHECK(drained);
    CHECK(rig.gatherer.Calls() == 1);
    // Returned into a closed session, the reading is dropped rather than drawn.
    CHECK(sink.frames == 0);
    CHECK(rig.reactor.PendingTimers() == 0);
    CHECK(rig.reactor.PendingSubmissions() == 0);
    CHECK(rig.pool.PendingSubmissions() == 0);

    // Whatever the checks above found, nothing may be left parked when the tasks go.
    source.Close();
    rig.Settle();
}

TEST_CASE("closing a source between samples retires its timer without waiting for the deadline", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };
    rig.Settle();
    CHECK(rig.reactor.PendingTimers() == 1);

    source.Close();
    // Retracted at the close, not at the deadline: `fleet` waits five seconds between
    // samples, and a quit that took that long to finish would read as a hang.
    CHECK(rig.reactor.PendingTimers() == 0);

    // Drained with the clock where it was.
    auto drained = false;
    auto wait = AwaitDrained(&source, &drained);
    rig.reactor.Submit(wait.Native());
    rig.Settle();
    CHECK(drained);
    CHECK(rig.gatherer.Calls() == 1);
}

TEST_CASE("a closed live source answers Detached, including to a read already waiting", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };
    rig.Settle();
    (void) NextDue(rig, source);
    (void) NextDue(rig, source);

    auto waiting = std::optional<DashboardEvent> {};
    auto task = TakeOne(&source, &waiting);
    rig.reactor.Submit(task.Native());
    rig.reactor.Drain();
    CHECK_FALSE(waiting.has_value());

    source.Close();
    rig.reactor.Drain();
    CHECK(KindOf(waiting) == DashboardEventKind::Detached);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Detached);

    CloseAndDrain(rig, source);
}

TEST_CASE("a sample slower than the interval is never overlapped and never made up in a burst", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };

    // The first gather is out, and three intervals pass before it returns.
    rig.reactor.Drain();
    rig.clock.Advance(3 * Interval);
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);
    CHECK(rig.gatherer.Calls() == 0);

    // Once it returns, the next follows at once -- and ONE follows, not one for each
    // interval the first overran. Both would read the same moment.
    rig.Settle();
    CHECK(rig.gatherer.Calls() == 2);
    CHECK(rig.reactor.PendingTimers() == 1);

    // And the cadence is still on its grid rather than restarted from the late return.
    rig.clock.Advance(Interval - 1ms);
    rig.Settle();
    CHECK(rig.gatherer.Calls() == 2);
    rig.clock.Advance(1ms);
    rig.Settle();
    CHECK(rig.gatherer.Calls() == 3);

    CloseAndDrain(rig, source);
}

TEST_CASE("a resize is followed by a tick, so the frame is redrawn at the new size", "[cli][live][source]")
{
    Rig rig;
    auto terminal = std::make_unique<SpokenTerminal>(rig.reactor);
    auto* const at = terminal.get();
    LiveEventSource source { rig.Parts(std::move(terminal)) };
    rig.Settle();
    (void) NextDue(rig, source);
    (void) NextDue(rig, source);

    at->Say(DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40 });
    auto const resize = NextDue(rig, source);
    CHECK(KindOf(resize) == DashboardEventKind::Resize);
    CHECK((resize.has_value() && resize->columns == 120 && resize->rows == 40));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    // A key changes nothing a frame is drawn from, so it earns no tick.
    at->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "x" });
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Key);
    auto after = std::optional<DashboardEvent> {};
    auto task = TakeOne(&source, &after);
    rig.reactor.Submit(task.Native());
    rig.reactor.Drain();
    CHECK_FALSE(after.has_value());

    CloseAndDrain(rig, source);
}

TEST_CASE("a terminal that goes away ends the session", "[cli][live][source]")
{
    Rig rig;
    auto terminal = std::make_unique<SpokenTerminal>(rig.reactor);
    auto* const at = terminal.get();
    LiveEventSource source { rig.Parts(std::move(terminal)) };
    CountView view;
    CountSink sink;

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &view, &sink, DashboardLimits {}, &exit);
    rig.reactor.Submit(run.Native());
    rig.Settle();
    CHECK_FALSE(exit.has_value());

    at->Close();
    rig.Settle();
    CHECK((exit.has_value() && exit->stop == DashboardStop::SourceDetached));

    CloseAndDrain(rig, source);
}

TEST_CASE("a run with no terminal takes its whole sample budget", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };
    CountView view;
    CountSink sink;

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &view, &sink, DashboardLimits { .samples = 3 }, &exit);
    rig.reactor.Submit(run.Native());
    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
    {
        rig.Settle();
        rig.clock.Advance(Interval);
    }
    rig.Settle();

    CHECK((exit.has_value() && exit->stop == DashboardStop::SampleBudget));
    CHECK(rig.gatherer.Calls() == 3);

    CloseAndDrain(rig, source);
}
