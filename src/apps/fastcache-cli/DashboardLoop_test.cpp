// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <optional>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using FastCache::Testing::Unwrap;

namespace
{

/// A view that writes down what it was asked to draw.
///
/// It renders the MODEL's distinctions rather than a picture, because this file tests
/// the fold and not the drawing: what a case needs to see is *which* model reached the
/// renderer, and a real panel would bury that under layout. The eventual Unicode and
/// ASCII rungs are separate implementations of the same seam and get their own cases.
class RecordingView final: public IDashboardView
{
  public:
    [[nodiscard]] std::string Frame(DashboardModel const& model) override
    {
        ++calls;
        seen.push_back(model);
        return std::format("latest={} previous={} broken={} samples={} cols={} rows={}",
                           model.latest.has_value(),
                           model.previous.has_value(),
                           model.BrokenRun(),
                           model.samples,
                           model.columns,
                           model.rows);
    }

    std::size_t calls { 0 };
    std::vector<DashboardModel> seen {};
};

/// Collects frames so a case can compare runs to each other.
class CollectingSink final: public IFrameSink
{
  public:
    void Present(std::string_view frame) override
    {
        frames.emplace_back(frame);
    }

    std::vector<std::string> frames {};
};

/// A reading that `ChooseStats` will accept.
/// @param value What the one metric reads.
/// @return One attempt carrying it.
[[nodiscard]] std::vector<StatsAttempt> Reading(std::int64_t value)
{
    auto record = Value {};
    record.shape = Shape::Record;
    record.fields.push_back(Field { .name = "curr_connections", .value = TextCell(std::to_string(value)) });
    return { StatsAttempt { .origin = StatsOrigin::Info, .asked = true, .record = std::move(record), .note = {} } };
}

/// A round in which every source was asked and none answered.
/// @return The attempts.
[[nodiscard]] std::vector<StatsAttempt> NothingAnswered()
{
    return { StatsAttempt { .origin = StatsOrigin::Info, .asked = true, .record = std::nullopt, .note = "refused" } };
}

/// Run the dashboard once, writing the result where the caller can read it.
///
/// A named coroutine over POINTERS rather than a capturing lambda: a lambda
/// coroutine destroys its closure at the first suspension, so every captured
/// reference dangles from then on -- and this coroutine suspends on its first
/// statement. The caller owns every argument for the whole run.
/// @param events The scripted source.
/// @param view What draws.
/// @param sink Where frames go.
/// @param limits What bounds the run.
/// @param out Where to put the result.
/// @return The task to submit.
[[nodiscard]] Task<void> DriveOnce(IDashboardEventSource* events,
                                   IDashboardView* view,
                                   IFrameSink* sink,
                                   DashboardLimits limits,
                                   std::optional<DashboardExit>* out)
{
    *out = co_await RunDashboard(events, view, sink, limits);
}

/// Drive the loop to completion on a deterministic reactor.
/// @param script The events, in order.
/// @param limits What bounds the run.
/// @param view The view to draw through.
/// @param sink Where frames go.
/// @return How it ended.
[[nodiscard]] DashboardExit Drive(std::vector<DashboardEvent> script,
                                  DashboardLimits limits,
                                  RecordingView& view,
                                  CollectingSink& sink)
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto events = ScriptedDashboardEvents { reactor, std::move(script) };

    auto result = std::optional<DashboardExit> {};
    auto task = DriveOnce(&events, &view, &sink, limits, &result);

    reactor.Submit(task.Native());
    reactor.Drain();

    REQUIRE(result.has_value());
    return Unwrap(result);
}

} // namespace

TEST_CASE("the dashboard's event source parks rather than resolving inline", "[cli][dashboard]")
{
    // THE property the whole fixture design rests on, and the one every other case here
    // would pass without. A source that answered synchronously would satisfy
    // `IDashboardEventSource` and make every ordering property vacuous -- a tick arriving
    // while a fetch is outstanding, a quit during a slow sample -- while appearing to
    // pass.
    //
    // WHAT DISTINGUISHES: the loop must be INCOMPLETE after the task is submitted and
    // only one turn of the reactor has run. A synchronous source finishes the whole
    // script inside `Submit`, so `result` would already be engaged and `PendingTimers`
    // would be zero. Asserting merely that the loop finishes passes either way.
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto events = ScriptedDashboardEvents { reactor,
                                            { DashboardEvent { .kind = DashboardEventKind::Tick },
                                              DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" } } };

    auto result = std::optional<DashboardExit> {};
    auto task = DriveOnce(&events, &view, &sink, DashboardLimits {}, &result);

    reactor.Submit(task.Native());
    CHECK(!result.has_value()); // nothing has run yet

    (void) reactor.Tick();
    CHECK(!result.has_value()); // the first Next() is PARKED, not answered
    CHECK(reactor.PendingTimers() >= 1);

    reactor.Drain();
    REQUIRE(result.has_value());
    CHECK(Unwrap(result).stop == DashboardStop::Quit);
}

TEST_CASE("the same event list renders byte-identical frames", "[cli][dashboard]")
{
    // The behavioural half of §9.11's ambient-state scan. It fails the moment the loop
    // reads a clock, an environment variable, a terminal or a socket that is not in the
    // list -- and it catches what the scan cannot, namely an ambient read reached
    // through a helper the scan's needles do not name.
    //
    // WHAT DISTINGUISHES: the two runs' frames compared to EACH OTHER, never to a golden
    // string. A golden comparison is rewritten by every rendering change and a check
    // everybody regenerates has stopped being a check.
    auto const script = std::vector<DashboardEvent> {
        DashboardEvent { .kind = DashboardEventKind::Tick },
        DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
        DashboardEvent { .kind = DashboardEventKind::Tick },
        DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 100, .rows = 40 },
        DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(20) },
        DashboardEvent { .kind = DashboardEventKind::Tick },
    };

    auto firstView = RecordingView {};
    auto firstSink = CollectingSink {};
    (void) Drive(script, DashboardLimits {}, firstView, firstSink);

    auto secondView = RecordingView {};
    auto secondSink = CollectingSink {};
    (void) Drive(script, DashboardLimits {}, secondView, secondSink);

    REQUIRE(firstSink.frames.size() == 3);
    CHECK(firstSink.frames == secondSink.frames);
}

TEST_CASE("the first frame has no rate and the second does", "[cli][dashboard]")
{
    // Both directions in one case: asserting only that rates eventually render passes
    // under an implementation that shows `0` on frame 1, which IS the defect -- a
    // dashboard that opens claiming zero throughput is worse than one that says it does
    // not know yet.
    //
    // WHAT DISTINGUISHES: frame 1 must have no PREVIOUS reading to measure against, and
    // frame 3 must have one. A case reading only the last frame cannot tell them apart.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const exit = Drive({ DashboardEvent { .kind = DashboardEventKind::Tick },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
                              DashboardEvent { .kind = DashboardEventKind::Tick },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(20) },
                              DashboardEvent { .kind = DashboardEventKind::Tick } },
                            DashboardLimits {},
                            view,
                            sink);

    REQUIRE(view.seen.size() == 3);
    CHECK(!view.seen[0].latest.has_value());   // nothing read yet
    CHECK(!view.seen[1].previous.has_value()); // one reading is not a rate
    CHECK(view.seen[1].BrokenRun());
    CHECK(view.seen[2].previous.has_value()); // two readings are
    CHECK(!view.seen[2].BrokenRun());
    CHECK(exit.outcome == Outcome::Affirmative);
}

TEST_CASE("a failed sample breaks the run and the reading after it is not a rate", "[cli][dashboard]")
{
    // §9.2 and §9.3: a rate must not span a gap. A renderer cannot infer this from the
    // two values, because a source that failed and a source that reported the same
    // numbers twice look identical in the readings alone -- which is why the run LENGTH is
    // model state rather than something the view recomputes.
    //
    // WHAT DISTINGUISHES: the frame AFTER the failure. Asserting only that the failing
    // frame is broken passes under an implementation that clears the flag on the very
    // next reading, which is the bug.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(20) },
                   DashboardEvent { .kind = DashboardEventKind::Tick },
                   DashboardEvent { .kind = DashboardEventKind::SampleFailed, .note = "refused" },
                   DashboardEvent { .kind = DashboardEventKind::Tick },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(30) },
                   DashboardEvent { .kind = DashboardEventKind::Tick } },
                 DashboardLimits {},
                 view,
                 sink);

    REQUIRE(view.seen.size() == 3);
    CHECK(!view.seen[0].BrokenRun()); // two good readings
    CHECK(view.seen[1].BrokenRun());  // the failure
    CHECK(view.seen[2].BrokenRun());  // and the reading after it is still not a pair

    // The control: a run with no failure in it is NOT broken at the same point, or
    // "always broken" would satisfy every assertion above.
    auto quiet = RecordingView {};
    auto quietSink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(20) },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(30) },
                   DashboardEvent { .kind = DashboardEventKind::Tick } },
                 DashboardLimits {},
                 quiet,
                 quietSink);
    REQUIRE(quiet.seen.size() == 1);
    CHECK(!quiet.seen[0].BrokenRun());
}

TEST_CASE("a reading nothing answered is a gap, not a sample", "[cli][dashboard]")
{
    // A `Sample` whose attempts all came back empty must land where `SampleFailed`
    // lands. Two routes, one outcome -- and they are folded through one helper precisely
    // so they cannot drift apart, which is what this case watches.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const exit = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = NothingAnswered() },
                              DashboardEvent { .kind = DashboardEventKind::Tick } },
                            DashboardLimits {},
                            view,
                            sink);

    REQUIRE(view.seen.size() == 1);
    CHECK(!view.seen[0].latest.has_value());
    CHECK(view.seen[0].BrokenRun());
    CHECK(view.seen[0].samples == 0);
    // Nothing ever answered, so the run did not succeed.
    CHECK(exit.outcome == Outcome::Unreachable);
}

TEST_CASE("one good sample then failures to the end is a SUCCESS", "[cli][dashboard]")
{
    // §9.17, and the direction a healthy-path test cannot see: a dashboard that showed
    // real data and then lost its source did its job. Zero successful samples is the
    // failure; one followed by nothing but failures is not.
    //
    // WHAT DISTINGUISHES: this case against the one above. They differ by a single good
    // reading at the front and must produce different outcomes; an implementation keying
    // on "did the LAST sample succeed" passes neither, and one keying on "did any fail"
    // passes only the wrong one.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const exit = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
                              DashboardEvent { .kind = DashboardEventKind::SampleFailed, .note = "gone" },
                              DashboardEvent { .kind = DashboardEventKind::SampleFailed, .note = "gone" },
                              DashboardEvent { .kind = DashboardEventKind::Tick } },
                            DashboardLimits {},
                            view,
                            sink);

    CHECK(exit.outcome == Outcome::Affirmative);
    CHECK(exit.model.samples == 1);
    CHECK(exit.model.BrokenRun());
}

TEST_CASE("a sample budget of N takes exactly N readings", "[cli][dashboard]")
{
    // §9.16, for `--samples=N`. The name carries no leading dash because a Catch2 case
    // name is an ARGUMENT and a leading `-` reads as an option rather than a test spec,
    // so the flag goes here, where it is prose. Measured the expensive way: this case
    // passed every local run because I selected it by TAG, and only the by-NAME ctest
    // registration could see it.
    //
    // The script holds more readings than the budget, so an off-by-one in either
    // direction shows: stopping at N-1 leaves a reading unconsumed that the count would
    // miss, and stopping at N+1 consumes one the budget forbade.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const exit = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(20) },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(30) },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(40) } },
                            DashboardLimits { .samples = 2 },
                            view,
                            sink);

    CHECK(exit.stop == DashboardStop::SampleBudget);
    CHECK(exit.model.samples == 2);

    // And a FAILED round does not spend the budget, or a flapping source ends the run
    // early while reporting it completed normally.
    auto second = RecordingView {};
    auto secondSink = CollectingSink {};
    auto const withGap = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
                                 DashboardEvent { .kind = DashboardEventKind::SampleFailed, .note = "gone" },
                                 DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(20) } },
                               DashboardLimits { .samples = 2 },
                               second,
                               secondSink);
    CHECK(withGap.stop == DashboardStop::SampleBudget);
    CHECK(withGap.model.samples == 2);
}

TEST_CASE("a quit arriving mid-fetch ends the run and closes the source", "[cli][dashboard]")
{
    // The interleaving that a four-seam design cannot express without racing something:
    // the key lands BETWEEN a reading and the tick that would have drawn it. With one
    // ordered list it is an input.
    //
    // WHAT DISTINGUISHES: the frame that never happened. A loop that drew before
    // checking the key would leave a third frame behind.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const exit = Drive({ DashboardEvent { .kind = DashboardEventKind::Tick },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading(10) },
                              DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" },
                              DashboardEvent { .kind = DashboardEventKind::Tick } },
                            DashboardLimits {},
                            view,
                            sink);

    CHECK(exit.stop == DashboardStop::Quit);
    CHECK(sink.frames.size() == 1);

    // A key that is not a quit key does NOT end the run, or "any key quits" passes the
    // assertion above.
    auto other = RecordingView {};
    auto otherSink = CollectingSink {};
    auto const kept = Drive({ DashboardEvent { .kind = DashboardEventKind::Key, .keys = "x" },
                              DashboardEvent { .kind = DashboardEventKind::Tick } },
                            DashboardLimits {},
                            other,
                            otherSink);
    CHECK(kept.stop == DashboardStop::SourceDetached);
    CHECK(otherSink.frames.size() == 1);
}

TEST_CASE("a resize is visible to the very next frame", "[cli][dashboard]")
{
    // Geometry is model state, so the frame after a resize must see it. If the loop
    // asked the terminal instead, this would still pass on a real terminal and fail here
    // -- which is the point of the size arriving as an event.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Tick },
                   DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 132, .rows = 50 },
                   DashboardEvent { .kind = DashboardEventKind::Tick } },
                 DashboardLimits {},
                 view,
                 sink);

    REQUIRE(view.seen.size() == 2);
    CHECK(view.seen[1].columns == 132);
    CHECK(view.seen[1].rows == 50);
    CHECK(view.seen[0].columns != view.seen[1].columns);
}

TEST_CASE("the quit keys are a set, not a spelling", "[cli][dashboard]")
{
    CHECK(IsQuitKey("q"));
    CHECK(IsQuitKey("Q"));
    CHECK(IsQuitKey("\x03")); // Ctrl-C
    CHECK(IsQuitKey("\x1b")); // ESC
    CHECK(!IsQuitKey("x"));
    CHECK(!IsQuitKey(""));
    // A prefix is not a match: a key sequence STARTING with ESC is an arrow key, and a
    // dashboard that quit on one would be unusable.
    CHECK(!IsQuitKey("\x1b[A"));
}
