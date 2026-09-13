// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

/// A reading from a named source that `ChooseStats` will accept.
/// @param origin Which source it came from.
/// @param value What the one metric reads.
/// @return One attempt carrying it.
[[nodiscard]] std::vector<StatsAttempt> ReadingFrom(StatsOrigin origin, std::int64_t value)
{
    auto attempts = Reading(value);
    attempts.front().origin = origin;
    return attempts;
}

/// A steady time @p seconds after the clock's epoch.
/// @param seconds How far in.
/// @return The time point.
[[nodiscard]] TimePoint At(int seconds)
{
    return TimePoint { std::chrono::seconds { seconds } };
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
                                   SampleReader reader,
                                   IDashboardView* view,
                                   IFrameSink* sink,
                                   DashboardLimits limits,
                                   std::optional<DashboardExit>* out)
{
    *out = co_await RunDashboard(events, reader, view, sink, limits);
}

/// Drive the loop to completion on a deterministic reactor.
/// @param script The events, in order.
/// @param limits What bounds the run.
/// @param view The view to draw through.
/// @param sink Where frames go.
/// @param reader What reads a `Sample`; the stats ladder's own, as a `cache` session binds it.
/// @return How it ended.
[[nodiscard]] DashboardExit Drive(std::vector<DashboardEvent> script,
                                  DashboardLimits limits,
                                  RecordingView& view,
                                  CollectingSink& sink,
                                  SampleReader reader = &ReadStatsSample)
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto events = ScriptedDashboardEvents { reactor, std::move(script) };

    auto result = std::optional<DashboardExit> {};
    auto task = DriveOnce(&events, reader, &view, &sink, limits, &result);

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
    auto task = DriveOnce(&events, &ReadStatsSample, &view, &sink, DashboardLimits {}, &result);

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
        DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) },
        DashboardEvent { .kind = DashboardEventKind::Tick },
        DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 100, .rows = 40 },
        DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(2), .attempts = Reading(20) },
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
                              DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) },
                              DashboardEvent { .kind = DashboardEventKind::Tick },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(2), .attempts = Reading(20) },
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
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(2), .attempts = Reading(20) },
                   DashboardEvent { .kind = DashboardEventKind::Tick },
                   DashboardEvent { .kind = DashboardEventKind::SampleFailed, .note = "refused" },
                   DashboardEvent { .kind = DashboardEventKind::Tick },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(3), .attempts = Reading(30) },
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
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(4), .attempts = Reading(10) },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(5), .attempts = Reading(20) },
                   DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(6), .attempts = Reading(30) },
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
    auto const exit =
        Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = NothingAnswered() },
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
    auto const exit = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) },
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
    auto const exit = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(2), .attempts = Reading(20) },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(3), .attempts = Reading(30) },
                              DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(4), .attempts = Reading(40) } },
                            DashboardLimits { .samples = 2 },
                            view,
                            sink);

    CHECK(exit.stop == DashboardStop::SampleBudget);
    CHECK(exit.model.samples == 2);

    // And a FAILED round does not spend the budget, or a flapping source ends the run
    // early while reporting it completed normally.
    auto second = RecordingView {};
    auto secondSink = CollectingSink {};
    auto const withGap =
        Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(5), .attempts = Reading(10) },
                DashboardEvent { .kind = DashboardEventKind::SampleFailed, .note = "gone" },
                DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(6), .attempts = Reading(20) } },
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
                              DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) },
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

namespace
{

/// A reader that ignores the stats ladder and says something no ladder would.
/// @param event The sample; unused, which is the point.
/// @return A reading marked as this reader's.
[[nodiscard]] SampleReading MarkerReader(DashboardEvent const& event)
{
    (void) event;
    auto record = Value {};
    record.shape = Shape::Record;
    record.fields.push_back(Field { .name = "marker", .value = TextCell("from-the-reader") });
    return SampleReading { .outcome = Outcome::Affirmative, .value = std::move(record), .source = "marker" };
}

/// A `fleet` session's reader, reduced to what this file needs: the document's body.
/// @param event The sample.
/// @return The body as a one-field record, or the fetch's failure as an outcome.
[[nodiscard]] SampleReading DocumentReader(DashboardEvent const& event)
{
    if (!event.document.has_value() || !event.document->has_value())
        return SampleReading { .outcome = Outcome::Unreachable, .value = {}, .source = {} };
    auto record = Value {};
    record.shape = Shape::Record;
    record.fields.push_back(Field { .name = "body", .value = TextCell(event.document->value()) });
    return SampleReading { .outcome = Outcome::Affirmative, .value = std::move(record), .source = "fleet.txt" };
}

/// A reader for which every sample was answered and declined.
/// @param event The sample; unused.
/// @return A refusal.
[[nodiscard]] SampleReading RefusingReader(DashboardEvent const& event)
{
    (void) event;
    return SampleReading { .outcome = Outcome::Refused, .value = {}, .source = {} };
}

/// The text a record holds for @p name, or empty.
/// @param model The model whose latest reading to read.
/// @param name The field.
/// @return Its text.
[[nodiscard]] std::string LatestField(DashboardModel const& model, std::string_view name)
{
    if (!model.latest.has_value())
        return {};
    auto const* field = FindField(Unwrap(model.latest), name);
    return field == nullptr ? std::string {} : field->value.lexical;
}

} // namespace

TEST_CASE("a reading from a different source breaks the run and the next from that source continues it", "[cli][dashboard]")
{
    // A change measured across two sources subtracts one vocabulary from another, so it is
    // not a rate. WHAT DISTINGUISHES: the frame after the switch is broken, the frame after
    // THAT is not, and a run with no switch is not broken at the same point -- without the
    // last, "a third reading is always broken" would satisfy the first two.
    auto const tick = DashboardEvent { .kind = DashboardEventKind::Tick };
    auto sample = [](StatsOrigin origin, std::int64_t value, int seconds) {
        return DashboardEvent { .kind = DashboardEventKind::Sample,
                                .at = At(seconds),
                                .attempts = ReadingFrom(origin, value) };
    };

    auto view = RecordingView {};
    auto sink = CollectingSink {};
    (void) Drive({ sample(StatsOrigin::Metrics, 10, 1),
                   tick,
                   sample(StatsOrigin::Metrics, 20, 2),
                   tick,
                   sample(StatsOrigin::Info, 30, 3),
                   tick,
                   sample(StatsOrigin::Info, 40, 4),
                   tick },
                 DashboardLimits {},
                 view,
                 sink);
    REQUIRE(view.seen.size() == 4);
    CHECK(view.seen[0].BrokenRun());
    CHECK(!view.seen[1].BrokenRun());
    CHECK(view.seen[2].BrokenRun()); // the switch
    CHECK(!view.seen[3].BrokenRun());

    auto same = RecordingView {};
    auto sameSink = CollectingSink {};
    (void) Drive({ sample(StatsOrigin::Metrics, 10, 1),
                   sample(StatsOrigin::Metrics, 20, 2),
                   sample(StatsOrigin::Metrics, 30, 3),
                   tick },
                 DashboardLimits {},
                 same,
                 sameSink);
    REQUIRE(same.seen.size() == 1);
    CHECK(!same.seen[0].BrokenRun());
}

TEST_CASE("a reading stamped no later than the one before breaks the run", "[cli][dashboard]")
{
    // An elapsed time of zero or less cannot divide anything. WHAT DISTINGUISHES: equal
    // stamps break, an EARLIER stamp breaks, and a later one after that continues -- plus a
    // strictly increasing run that is not broken at the point the equal stamp was, so "a
    // second reading is always broken" cannot pass.
    auto const tick = DashboardEvent { .kind = DashboardEventKind::Tick };
    auto sample = [](std::int64_t value, int seconds) {
        return DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(seconds), .attempts = Reading(value) };
    };

    auto view = RecordingView {};
    auto sink = CollectingSink {};
    (void) Drive({ sample(10, 5), tick, sample(20, 5), tick, sample(30, 4), tick, sample(40, 6), tick },
                 DashboardLimits {},
                 view,
                 sink);
    REQUIRE(view.seen.size() == 4);
    CHECK(view.seen[1].BrokenRun());  // same instant
    CHECK(view.seen[2].BrokenRun());  // earlier than the one before
    CHECK(!view.seen[3].BrokenRun()); // later again

    auto forward = RecordingView {};
    auto forwardSink = CollectingSink {};
    (void) Drive({ sample(10, 5), sample(20, 6), tick }, DashboardLimits {}, forward, forwardSink);
    REQUIRE(forward.seen.size() == 1);
    CHECK(!forward.seen[0].BrokenRun());
}

TEST_CASE("a run that only ever failed ends with the outcome its failures carried", "[cli][dashboard]")
{
    // §9.17's never-answered half: exit 3 and exit 4 have different remedies, so the failures
    // must say which. WHAT DISTINGUISHES: a refused run and an unreachable run end
    // DIFFERENTLY -- collapsing both to one outcome fails exactly one of the two -- and both
    // routes carry it, the `SampleFailed` event and a `Sample` its reader declined.
    auto view = RecordingView {};
    auto sink = CollectingSink {};

    auto const refused = Drive({ DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Refused },
                                 DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Refused } },
                               DashboardLimits {},
                               view,
                               sink);
    CHECK(refused.outcome == Outcome::Refused);

    auto const unreachable =
        Drive({ DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable },
                DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable } },
              DashboardLimits {},
              view,
              sink);
    CHECK(unreachable.outcome == Outcome::Unreachable);

    auto const declined = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1) },
                                  DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(2) } },
                                DashboardLimits {},
                                view,
                                sink,
                                &RefusingReader);
    CHECK(declined.outcome == Outcome::Refused);

    // The control: one reading makes the run a success, and a refusal after it does not undo
    // that -- or "the last failure decides" would pass every assertion above.
    auto const recovered =
        Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) },
                DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Refused } },
              DashboardLimits {},
              view,
              sink);
    CHECK(recovered.outcome == Outcome::Affirmative);
}

TEST_CASE("the fold reads a sample only through the reader it was given", "[cli][dashboard]")
{
    // The subject is chosen by which reader is passed, so the fold must not have a reading of
    // its own to fall back on. WHAT DISTINGUISHES: a reading no stats ladder would produce --
    // a fold that still ran `ChooseStats` itself would hold `curr_connections` here, not the
    // marker.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const marked =
        Drive({ DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) } },
              DashboardLimits {},
              view,
              sink,
              &MarkerReader);
    CHECK(LatestField(marked.model, "marker") == "from-the-reader");
    CHECK(LatestField(marked.model, "curr_connections").empty());

    // And a fleet session streams: a sample carrying only a document, and no attempts at all,
    // is a reading. Under the fold that ran `ChooseStats` this was Unreachable, because an
    // empty attempt list chooses nothing.
    auto const fleet = Drive({ DashboardEvent { .kind = DashboardEventKind::Sample,
                                                .at = At(1),
                                                .document = std::expected<std::string, AdminError> { "machines 12" } } },
                             DashboardLimits {},
                             view,
                             sink,
                             &DocumentReader);
    CHECK(fleet.outcome == Outcome::Affirmative);
    CHECK(LatestField(fleet.model, "body") == "machines 12");
    REQUIRE(fleet.model.latestStamp.has_value());
    CHECK(Unwrap(fleet.model.latestStamp).source == "fleet.txt");
}
