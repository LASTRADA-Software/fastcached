// SPDX-License-Identifier: Apache-2.0
#include "LiveSourceRig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <ranges>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using namespace std::chrono_literals;

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
    LiveEventSource source { rig.SpokenParts() };

    // The reactor has run and the pool has not: the first gather is handed off and has
    // not happened, which is the window this case is about.
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);

    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "x" });
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
    LiveEventSource source { rig.SpokenParts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.Submit(run.Native());
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);

    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.reactor.Drain();

    // The operator was not made to wait for the scrape: the loop has returned, and the
    // gather has still not run.
    CHECK(StopOf(exit) == DashboardStop::Quit);
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
    CHECK(rig.sink.frames == 0);
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

    // Drained, and the timer gone from the heap, with the clock where it was and without
    // another sample: the close is heard on the reactor's next turn, not at the deadline.
    CloseAndDrain(rig, source);
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
    LiveEventSource source { rig.SpokenParts() };
    rig.Settle();
    (void) NextDue(rig, source);
    (void) NextDue(rig, source);

    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40 });
    auto const resize = NextDue(rig, source);
    CHECK(KindOf(resize) == DashboardEventKind::Resize);
    CHECK((resize.has_value() && resize->columns == 120 && resize->rows == 40));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    // A key changes nothing a frame is drawn from, so it earns no tick.
    rig.terminal->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "x" });
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
    LiveEventSource source { rig.SpokenParts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.Submit(run.Native());
    rig.Settle();
    CHECK_FALSE(exit.has_value());

    rig.terminal->Close();
    rig.Settle();
    CHECK(StopOf(exit) == DashboardStop::SourceDetached);

    CloseAndDrain(rig, source);
}

TEST_CASE("a run with no terminal takes its whole sample budget", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits { .samples = 3 }, &exit);
    rig.reactor.Submit(run.Native());
    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
    {
        rig.Settle();
        rig.clock.Advance(Interval);
    }
    rig.Settle();

    CHECK(StopOf(exit) == DashboardStop::SampleBudget);
    CHECK(rig.gatherer.Calls() == 3);

    CloseAndDrain(rig, source);
}

TEST_CASE("a stop request after a sample ends a session with no terminal as answered", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.Submit(run.Native());
    rig.Settle();
    CHECK(rig.gatherer.Calls() == 1);
    CHECK_FALSE(exit.has_value());

    rig.stop->Fire();
    rig.reactor.Drain();
    // Released when its watch ended, which is what restores the disposition -- not when the
    // source goes.
    CHECK(rig.stopReleased);

    // Quit, and a session that had a reading to show: the operator ended a run that
    // worked, which is exit 0 rather than a failure they have to explain to a script.
    CHECK(StopOf(exit) == DashboardStop::Quit);
    CHECK((exit.has_value() && ExitCodeOf(exit->outcome) == 0));

    CloseAndDrain(rig, source);
}

TEST_CASE("a stop request before any sample does not end a session as answered", "[cli][live][source]")
{
    // The control for the case above: a stop is not an answer by itself. Quit before a
    // reading arrived is a session that showed nothing, and exit 0 would claim otherwise.
    Rig rig;
    auto parts = rig.StoppableParts();
    rig.stop->Fire();
    LiveEventSource source { std::move(parts) };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.Submit(run.Native());
    rig.reactor.Drain();

    CHECK(StopOf(exit) == DashboardStop::Quit);
    CHECK((exit.has_value() && ExitCodeOf(exit->outcome) != 0));
    CHECK(rig.gatherer.Calls() == 0);

    CloseAndDrain(rig, source);
}

TEST_CASE("a session with a stop signal keeps sampling until a stop arrives and drains once closed", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };

    auto exit = std::optional<DashboardExit> {};
    auto run = RunOver(&source, &rig.view, &rig.sink, DashboardLimits {}, &exit);
    rig.reactor.Submit(run.Native());
    rig.Settle();
    rig.clock.Advance(Interval);
    rig.Settle();

    // Nothing fired: the watch is waiting, not ending the session and not asking twice.
    CHECK_FALSE(exit.has_value());
    CHECK(rig.gatherer.Calls() == 2);
    CHECK(rig.stop->Waits() == 1);
    CHECK_FALSE(rig.stopReleased);

    // And a close answers that wait, so the drain does not hang on a thread nobody woke, and
    // the signal is released for it.
    CloseAndDrain(rig, source);
    CHECK(rig.stopReleased);
    CHECK(StopOf(exit) == DashboardStop::SourceDetached);
}

TEST_CASE("a stop watch that fails ends the session saying why", "[cli][live][source]")
{
    // A session nothing could interrupt with Ctrl-C is not one to keep running.
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };
    rig.Settle();
    (void) NextDue(rig, source);
    (void) NextDue(rig, source);

    rig.stop->Fail();
    auto const ended = NextDue(rig, source);
    CHECK(KindOf(ended) == DashboardEventKind::Detached);
    CHECK((ended.has_value() && ended->note.contains("Ctrl-C")));

    CloseAndDrain(rig, source);
}

TEST_CASE("a terminal is released once nothing reads it and not when a stuck sample returns", "[cli][live][source]")
{
    // Releasing the terminal is what restores it. A sample that never comes back must not
    // leave an operator's terminal in raw mode behind a process that is about to give up on
    // that sample.
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);

    source.Close();
    rig.reactor.Drain();
    CHECK(rig.terminalRelease.released);
    CHECK(rig.terminalRelease.closedFirst);
    // The sample is still out, and its start is readable from anywhere.
    CHECK(source.SampleOutstandingSince() == rig.clock.Now());

    rig.Settle();
    CHECK_FALSE(source.SampleOutstandingSince().has_value());
    CloseAndDrain(rig, source);
}

TEST_CASE("a terminal that goes away on its own is closed before it is released", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };
    rig.Settle();

    // Detached with nobody having closed anything: the forwarder ends and releases the
    // terminal, which its contract says must be closed first -- and nobody else closed it.
    rig.terminal->GoAway();
    rig.reactor.Drain();
    CHECK(rig.terminalRelease.released);
    CHECK(rig.terminalRelease.closedFirst);

    CloseAndDrain(rig, source);
}
