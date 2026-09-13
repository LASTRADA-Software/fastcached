// SPDX-License-Identifier: Apache-2.0
#include "LiveSourceRig.hpp"
#include "LiveStats.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("a terminal's presenter goes before its events, and a frame after the terminal went away is dropped",
          "[cli][live][source]")
{
    // A sample and its tick can be queued ahead of the terminal's `Detached`: the loop draws them
    // after the events are gone and the operator's own screen is back. That frame must not land.
    Rig rig;
    auto presented = PresenterRecord { .events = &rig.terminalRelease };
    auto parts = rig.SpokenParts();
    parts.frames = std::make_unique<PresenterRecord::Sink>(&presented);
    LiveEventSource source { std::move(parts) };
    auto* const frames = source.Frames();
    REQUIRE(frames != nullptr);
    rig.Settle();

    frames->Present("while the terminal is there");
    CHECK(presented.frames == 1);

    rig.terminal->GoAway();
    rig.Settle();
    REQUIRE(rig.terminalRelease.released);
    CHECK(presented.released);
    CHECK_FALSE(presented.afterEvents);

    frames->Present("after it went away");
    CHECK(presented.frames == 1);
    CHECK(presented.last == "while the terminal is there");

    CloseAndDrain(rig, source);
}

TEST_CASE("a source with no presenter hands the loop none", "[cli][live][source]")
{
    Rig rig;
    LiveEventSource source { rig.SpokenParts() };
    CHECK(source.Frames() == nullptr);
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

TEST_CASE("a stop request arrives as StopRequested and never as a keystroke", "[cli][live][source]")
{
    // The loop quits on both, so a case driving the loop cannot tell them apart: this reads the
    // event itself. A `Key` would carry bytes nobody typed.
    Rig rig;
    LiveEventSource source { rig.StoppableParts() };
    rig.Settle();
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Sample);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    rig.stop->Fire();
    rig.reactor.Drain();
    auto const stop = NextDue(rig, source);
    CHECK(KindOf(stop) == DashboardEventKind::StopRequested);
    CHECK((stop.has_value() && stop->keys.empty()));

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

TEST_CASE("a sample carries the moment it was taken rather than the moment it was asked for", "[cli][live][source]")
{
    // A rate is a change over the time it TOOK. The gather is handed off at the epoch and runs
    // 700 ms later, so a stamp taken where it was asked for would be wrong by exactly that.
    Rig rig;
    LiveEventSource source { rig.Parts() };
    rig.reactor.Drain();
    rig.clock.Advance(std::chrono::milliseconds { 700 });
    rig.Settle();

    auto const sample = NextDue(rig, source);
    CHECK(KindOf(sample) == DashboardEventKind::Sample);
    CHECK((sample.has_value() && sample->at == TimePoint {} + std::chrono::milliseconds { 700 }));

    CloseAndDrain(rig, source);
}

TEST_CASE("a document session fetches its whole document on the pool, stamped when it answered, and gathers nothing",
          "[cli][live][source][document]")
{
    // Fleet's own column, so the case follows the table: the WHOLE document, never one section.
    auto const path = std::string { LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Fleet)].document };
    REQUIRE_FALSE(path.empty());
    CHECK_FALSE(path.contains('?'));

    Rig rig;
    auto const body = std::string { "# kpi\nkpi\tvalue\tunit\tof\n" };
    ScriptedDocument admin { body };
    auto parts = rig.Parts();
    parts.admin = &admin;
    parts.document = path;
    LiveEventSource source { std::move(parts) };

    // Handed off on the reactor's turn and fetched only when the pool runs, 700 ms later: a
    // fetch on the reactor would already have happened, and a stamp taken where it was asked
    // for would be wrong by exactly that.
    rig.reactor.Drain();
    CHECK(admin.Asked().empty());
    rig.clock.Advance(std::chrono::milliseconds { 700 });
    rig.Settle();

    auto const sample = NextDue(rig, source);
    REQUIRE(KindOf(sample) == DashboardEventKind::Sample);
    CHECK(admin.Asked() == std::vector<std::string> { path });
    CHECK(Unwrap(sample).at == TimePoint {} + std::chrono::milliseconds { 700 });
    CHECK(Unwrap(sample).attempts.empty());
    REQUIRE(Unwrap(sample).document.has_value());
    CHECK(Unwrap(Unwrap(sample).document) == body);
    CHECK(rig.gatherer.Calls() == 0);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    CloseAndDrain(rig, source);
}

TEST_CASE("a document fetch that fails is still a sample, carrying the failure for the reader",
          "[cli][live][source][document]")
{
    // Which outcome a refusal is -- the leader declining, a follower naming the leader -- is the
    // reader's decision, made with the server's own words. A source that turned it into a gap
    // here would pick an outcome for it, and lose the words.
    Rig rig;
    ScriptedDocument admin { std::unexpected(
        AdminError { .kind = AdminFailure::Refused, .detail = "this node is not the leader; ask 10.0.0.9:7071" }) };
    auto parts = rig.Parts();
    parts.admin = &admin;
    parts.document = "/fleet.txt";
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    auto const sample = NextDue(rig, source);
    REQUIRE(KindOf(sample) == DashboardEventKind::Sample);
    REQUIRE(Unwrap(sample).document.has_value());
    auto const& fetched = Unwrap(Unwrap(sample).document);
    REQUIRE_FALSE(fetched.has_value());
    CHECK(fetched.error().kind == AdminFailure::Refused);
    CHECK(fetched.error().detail.contains("10.0.0.9:7071"));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    CloseAndDrain(rig, source);
}

TEST_CASE("a gather that could ask nothing is a failed sample rather than an empty reading", "[cli][live][source]")
{
    Rig rig;
    ScriptedGatherer asksNothing { {} };
    auto parts = rig.Parts();
    parts.gatherer = &asksNothing;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    auto const failed = NextDue(rig, source);
    CHECK(KindOf(failed) == DashboardEventKind::SampleFailed);
    CHECK((failed.has_value() && failed->outcome == Outcome::Unreachable));
    CHECK((failed.has_value() && failed->at == rig.clock.Now()));
    // Still a frame owed: a gap is drawn.
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    CloseAndDrain(rig, source);
}

namespace
{

/// Whether @p event is a sample the ladder read a record from.
/// @param event The event.
/// @return True for a sample with an answer in it.
[[nodiscard]] bool Answered(std::optional<DashboardEvent> const& event)
{
    return event.has_value() && event->kind == DashboardEventKind::Sample
           && ReadStatsSample(*event).outcome == Outcome::Affirmative;
}

/// Take the next sample: let the interval pass, run the gather, and read the sample and its tick.
/// @param rig The rig.
/// @param source The source.
/// @return The sample.
[[nodiscard]] std::optional<DashboardEvent> NextSample(Rig& rig, LiveEventSource& source)
{
    rig.clock.Advance(Interval);
    rig.Settle();
    auto sample = NextDue(rig, source);
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    return sample;
}

} // namespace

TEST_CASE("a failed sample re-dials before the next, so a daemon restart is one gap", "[cli][live][source][redial]")
{
    // §6.4. The connection dies after the first answer; without a re-dial every sample after it
    // fails for as long as the session runs, which is a monitor that cannot show the restart.
    Rig rig;
    DyingGatherer dying;
    ScriptedDialer dialer { { true } };
    auto parts = rig.Parts();
    parts.gatherer = &dying;
    parts.dialer = &dialer;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    CHECK(Answered(NextDue(rig, source)));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(dialer.Dials() == 0);

    CHECK_FALSE(Answered(NextSample(rig, source)));
    // Not re-dialled in the gap itself: the dial waits for the next sample's turn on the cadence.
    CHECK(dialer.Dials() == 0);

    CHECK(Answered(NextSample(rig, source)));
    CHECK(Answered(NextSample(rig, source)));
    CHECK(dialer.Dials() == 1);
    CHECK(dying.Calls() == 2);

    CloseAndDrain(rig, source);
}

TEST_CASE("a healthy session dials nothing", "[cli][live][source][redial]")
{
    // The control for the case above: the connections a session starts with serve every sample
    // for as long as they answer.
    Rig rig;
    ScriptedDialer dialer { { true } };
    auto parts = rig.Parts();
    parts.dialer = &dialer;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    CHECK(Answered(NextDue(rig, source)));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK(Answered(NextSample(rig, source)));
    CHECK(Answered(NextSample(rig, source)));
    CHECK(dialer.Dials() == 0);
    CHECK(rig.gatherer.Calls() == 3);

    CloseAndDrain(rig, source);
}

TEST_CASE("a daemon still down is re-dialled once per sample and never in a loop of its own", "[cli][live][source][redial]")
{
    Rig rig;
    DyingGatherer dying;
    ScriptedDialer dialer { { false } };
    auto parts = rig.Parts();
    parts.gatherer = &dying;
    parts.dialer = &dialer;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    CHECK(Answered(NextDue(rig, source)));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK_FALSE(Answered(NextSample(rig, source)));
    CHECK_FALSE(Answered(NextSample(rig, source)));
    CHECK_FALSE(Answered(NextSample(rig, source)));
    CHECK(dialer.Dials() == 2);

    // Everything runnable has run and the clock has not moved: nothing dials between samples.
    rig.Settle();
    CHECK(dialer.Dials() == 2);

    CloseAndDrain(rig, source);
}

TEST_CASE("a node sample carries the node's status, read afresh with every sample", "[cli][live][source][status]")
{
    // Per sample and never remembered: a status block repeating the session's first answer is a
    // live view of the past, so the second sample must carry the second read.
    Rig rig;
    auto parts = rig.Parts();
    parts.status = &rig.status;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    auto const first = NextDue(rig, source);
    REQUIRE(KindOf(first) == DashboardEventKind::Sample);
    REQUIRE(Unwrap(first).nodeStatus.has_value());
    CHECK(Unwrap(Unwrap(first).nodeStatus).version == "read-1");
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);

    auto const second = NextSample(rig, source);
    REQUIRE(KindOf(second) == DashboardEventKind::Sample);
    REQUIRE(Unwrap(second).nodeStatus.has_value());
    CHECK(Unwrap(Unwrap(second).nodeStatus).version == "read-2");
    CHECK(rig.status.Reads() == 2);

    CloseAndDrain(rig, source);
}

TEST_CASE("a failed node sample still carries the status read with it", "[cli][live][source][status]")
{
    // The model REPLACES its status with every sample taken, a failed one included, so a failure
    // that dropped the status would blank a block the node was still answering for.
    Rig rig;
    ScriptedGatherer asksNothing { {} };
    auto parts = rig.Parts();
    parts.gatherer = &asksNothing;
    parts.status = &rig.status;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    auto const failed = NextDue(rig, source);
    REQUIRE(KindOf(failed) == DashboardEventKind::SampleFailed);
    REQUIRE(Unwrap(failed).nodeStatus.has_value());
    CHECK(Unwrap(Unwrap(failed).nodeStatus).version == "read-1");

    CloseAndDrain(rig, source);
}

TEST_CASE("a session whose samples carry no node status asks for none", "[cli][live][source][status]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };
    rig.Settle();

    auto const sample = NextDue(rig, source);
    REQUIRE(KindOf(sample) == DashboardEventKind::Sample);
    CHECK_FALSE(Unwrap(sample).nodeStatus.has_value());
    CHECK(rig.status.Reads() == 0);

    CloseAndDrain(rig, source);
}

TEST_CASE("after a re-dial the node status is read over what the dial opened", "[cli][live][source][redial][status]")
{
    // The status and the counters are one connection. Re-dialled for the counters and still asking
    // the dead connection for the status would draw a status block of gaps over a recovered session.
    Rig rig;
    DyingGatherer dying;
    ScriptedDialer dialer { { true } };
    auto parts = rig.Parts();
    parts.gatherer = &dying;
    parts.status = &rig.status;
    parts.dialer = &dialer;
    LiveEventSource source { std::move(parts) };
    rig.Settle();

    CHECK(Answered(NextDue(rig, source)));
    CHECK(KindOf(NextDue(rig, source)) == DashboardEventKind::Tick);
    CHECK_FALSE(Answered(NextSample(rig, source)));

    auto const over = NextSample(rig, source);
    CHECK(Answered(over));
    CHECK(dialer.Dials() == 1);
    REQUIRE((over.has_value() && over->nodeStatus.has_value()));
    CHECK(Unwrap(Unwrap(over).nodeStatus).version == "dialled-1");
    CHECK(rig.status.Reads() == 2);

    CloseAndDrain(rig, source);
}
