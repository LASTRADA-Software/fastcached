// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "DashboardRig.hpp"
#include "FleetDocument.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
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

    // And a FAILED sample spends the budget as a reading does: `--samples=N` counts samples
    // taken, so a flapping source ends at N as well. The control for the all-failing case below:
    // one reading among the N keeps the run `Affirmative`.
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
    CHECK(withGap.outcome == Outcome::Affirmative);
    CHECK(withGap.model.attempts == 2);
    CHECK(withGap.model.samples == 1);
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

/// A reader that hands over a document naming the sample's body, as a parsing reader does.
/// @param event The sample.
/// @return A reading carrying a document whose `kpi` table holds the body, or a failure.
[[nodiscard]] SampleReading HandingReader(DashboardEvent const& event)
{
    if (!event.document.has_value() || !event.document->has_value())
        return SampleReading { .outcome = Outcome::Unreachable, .value = {}, .source = {} };
    auto document = FleetDocument {};
    document.sections.at(static_cast<std::size_t>(FleetSection::Kpi)) =
        TableValue({ "body" }, { { TextCell(event.document->value()) } });
    return SampleReading { .outcome = Outcome::Affirmative,
                           .value = ScalarValue(TextCell(event.document->value())),
                           .source = "fleet.txt",
                           .note = {},
                           .document = std::make_shared<FleetDocument const>(std::move(document)) };
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

TEST_CASE("the sample that meets the budget is drawn before the run stops", "[cli][dashboard]")
{
    // `--samples=N` must show N readings. The frame owed for the Nth arrives as a `Tick`
    // AFTER that sample, and the budget stop returns before it can be consumed -- so the
    // stop has to draw it.
    //
    // WHAT DISTINGUISHES: the count AND what the last frame shows. A count alone passes for a
    // fix that presents a stale frame; the last frame must hold sample N, not N-1.
    auto const tick = DashboardEvent { .kind = DashboardEventKind::Tick };
    auto sample = [](std::int64_t value, int seconds) {
        return DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(seconds), .attempts = Reading(value) };
    };

    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const exit = Drive({ sample(10, 1), tick, sample(20, 2), tick, sample(30, 3), tick, sample(40, 4), tick },
                            DashboardLimits { .samples = 3 },
                            view,
                            sink);

    CHECK(exit.stop == DashboardStop::SampleBudget);
    CHECK(sink.frames.size() == 3);
    CHECK(exit.model.frames == 3);
    REQUIRE(!view.seen.empty());
    CHECK(view.seen.back().samples == 3);
    CHECK(LatestField(view.seen.back(), "curr_connections") == "30");

    // The controls: only the stop that CONSUMED a sample owes a frame. A source running out,
    // and an operator quitting, stop without one -- or "draw on every stop" would pass above.
    auto detachedView = RecordingView {};
    auto detachedSink = CollectingSink {};
    auto const detached =
        Drive({ sample(10, 1), tick, sample(20, 2), tick }, DashboardLimits {}, detachedView, detachedSink);
    CHECK(detached.stop == DashboardStop::SourceDetached);
    CHECK(detachedSink.frames.size() == 2);

    auto quitView = RecordingView {};
    auto quitSink = CollectingSink {};
    auto const quit = Drive({ sample(10, 1), tick, DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" } },
                            DashboardLimits {},
                            quitView,
                            quitSink);
    CHECK(quit.stop == DashboardStop::Quit);
    CHECK(quitSink.frames.size() == 1);
}

TEST_CASE("a stop request ends the run as a quit and is not a keystroke", "[cli][dashboard]")
{
    // A stop that did not arrive as a keystroke -- a signal on a piped run, where no raw-mode
    // terminal decodes a Ctrl-C byte -- is its own event rather than a `Key` carrying bytes
    // nobody typed.
    //
    // WHAT DISTINGUISHES: the request carries no keys at all, so a loop that judged it through
    // `IsQuitKey` would ignore it, draw the trailing tick and end `SourceDetached`. And the
    // outcome is the run's rather than the stop's: one reading makes it `Affirmative`.
    auto const tick = DashboardEvent { .kind = DashboardEventKind::Tick };
    auto const stop = DashboardEvent { .kind = DashboardEventKind::StopRequested };
    auto const sample = DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) };

    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const stopped = Drive({ sample, tick, stop, tick }, DashboardLimits {}, view, sink);
    CHECK(stopped.stop == DashboardStop::Quit);
    CHECK(stopped.outcome == Outcome::Affirmative);
    CHECK(sink.frames.size() == 1);

    // The control: a stop before anything was read is still a quit and is NOT exit 0, or "a
    // stop is a success" passes above. Which non-zero outcome it is belongs to the exit-code
    // rules, not to this case.
    auto earlyView = RecordingView {};
    auto earlySink = CollectingSink {};
    auto const early = Drive({ stop, tick }, DashboardLimits {}, earlyView, earlySink);
    CHECK(early.stop == DashboardStop::Quit);
    CHECK(early.outcome != Outcome::Affirmative);
    CHECK(earlySink.frames.empty());

    // And the keystroke route stays: a raw-mode terminal decodes a real Ctrl-C as a `Key`, so
    // the two routes coexist and neither absorbs the other.
    auto keyView = RecordingView {};
    auto keySink = CollectingSink {};
    auto const keyed = Drive({ sample, tick, DashboardEvent { .kind = DashboardEventKind::Key, .keys = "\x03" }, tick },
                             DashboardLimits {},
                             keyView,
                             keySink);
    CHECK(keyed.stop == DashboardStop::Quit);
    CHECK(keyed.outcome == Outcome::Affirmative);
    CHECK(keySink.frames.size() == 1);
}

namespace
{

/// The field every `Reading` carries.
constexpr std::string_view Counter = "curr_connections";

/// A `Sample` carrying one `Counter` reading, taken @p seconds in.
/// @param value What it reads.
/// @param seconds When it was taken.
/// @return The event.
[[nodiscard]] DashboardEvent SampleAt(std::int64_t value, int seconds)
{
    return DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(seconds), .attempts = Reading(value) };
}

/// A `SampleFailed`, taken @p seconds in.
/// @param seconds When it was attempted.
/// @return The event.
[[nodiscard]] DashboardEvent FailedAt(int seconds)
{
    return DashboardEvent { .kind = DashboardEventKind::SampleFailed, .at = At(seconds), .outcome = Outcome::Refused };
}

/// Drive @p script and hand back the model the run ended with.
/// @param script The events.
/// @return The final model.
[[nodiscard]] DashboardModel FinalModel(std::vector<DashboardEvent> script)
{
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    return Drive(std::move(script), DashboardLimits {}, view, sink).model;
}

/// The rates of `Counter` over @p script's final history.
/// @param script The events.
/// @return One rate per history entry.
[[nodiscard]] std::vector<std::optional<double>> RatesOf(std::vector<DashboardEvent> script)
{
    return CounterRateSeries(FinalModel(std::move(script)).history, Counter);
}

} // namespace

TEST_CASE("both routes to a failed reading leave an entry with no reading in the history", "[cli][dashboard]")
{
    // A `SampleFailed` and a `Sample` its reader could not read are two routes to one outcome,
    // and the history is a thing each must update. WHAT DISTINGUISHES: an empty entry at EACH
    // route's own position. Neutering either route drops its entry and shifts every later
    // index, so each has its own assertion rather than one count both could satisfy.
    auto const model =
        FinalModel({ SampleAt(10, 1),
                     FailedAt(2),
                     SampleAt(20, 3),
                     DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(4), .attempts = NothingAnswered() },
                     SampleAt(30, 5) });

    REQUIRE(model.history.size() == 5);
    CHECK(model.history[0].reading.has_value());
    CHECK(!model.history[1].reading.has_value()); // SampleFailed
    CHECK(model.history[2].reading.has_value());
    CHECK(!model.history[3].reading.has_value()); // nothing answered
    CHECK(model.history[4].reading.has_value());
}

TEST_CASE("a gap and a steady counter are different cells", "[cli][dashboard]")
{
    // §9.2 at the model level: an absent rate and a rate of zero must stay two different
    // things, or no renderer downstream could draw a space for one and a floor glyph for the
    // other however it chose its glyphs.
    //
    // WHAT DISTINGUISHES: absent on the gapped side AND a present zero on the steady side.
    // Asserting only that the gap is absent passes under a series that never reports a rate.
    auto const gapped = RatesOf({ SampleAt(10, 1), FailedAt(2), SampleAt(10, 3) });
    auto const steady = RatesOf({ SampleAt(10, 1), SampleAt(10, 2), SampleAt(10, 3) });
    REQUIRE(gapped.size() == 3);
    REQUIRE(steady.size() == 3);

    CHECK(!gapped[1].has_value()); // the interval into the failure
    CHECK(!gapped[2].has_value()); // and the one out of it
    REQUIRE(steady[1].has_value());
    CHECK(Unwrap(steady[1]) == 0.0);
    REQUIRE(steady[2].has_value());
    CHECK(Unwrap(steady[2]) == 0.0);
}

TEST_CASE("a counter that went down is a gap and the interval after it is a rate again", "[cli][dashboard]")
{
    // §9.3. A decrease is a restart: not a negative rate, and not a zero.
    //
    // WHAT DISTINGUISHES: the decreasing interval is ABSENT -- which fails both the naive
    // subtraction (-50) and the clamp (0) -- and the interval after it is PRESENT, which fails
    // a series that treats a restart as the end of the run.
    auto const rates = RatesOf({ SampleAt(100, 1), SampleAt(50, 2), SampleAt(60, 3) });
    REQUIRE(rates.size() == 3);
    CHECK(!rates[1].has_value());
    REQUIRE(rates[2].has_value());
    CHECK(Unwrap(rates[2]) == 10.0);
}

TEST_CASE("one reading has no rate and a second reading has one", "[cli][dashboard]")
{
    // §9.1 at the model level. Both directions in one case, because a series that reports a
    // rate from nothing passes a check on the second model alone.
    auto const one = RatesOf({ SampleAt(10, 1) });
    REQUIRE(one.size() == 1);
    CHECK(!one[0].has_value());

    auto const two = RatesOf({ SampleAt(10, 1), SampleAt(25, 2) });
    REQUIRE(two.size() == 2);
    CHECK(!two[0].has_value());
    REQUIRE(two[1].has_value());
    CHECK(Unwrap(two[1]) == 15.0);
}

TEST_CASE("a rate is divided by the elapsed time the fold measured", "[cli][dashboard]")
{
    // A rate over an interval nobody measured is a claim about the sampler's schedule, not the
    // server. WHAT DISTINGUISHES: two intervals with the SAME change and different lengths give
    // different rates. A series returning the change passes neither; one dividing by a nominal
    // second passes the second interval and fails the first.
    auto const rates = RatesOf({ SampleAt(10, 1), SampleAt(30, 5), SampleAt(50, 6) });
    REQUIRE(rates.size() == 3);
    REQUIRE(rates[1].has_value());
    CHECK(Unwrap(rates[1]) == 5.0);
    REQUIRE(rates[2].has_value());
    CHECK(Unwrap(rates[2]) == 20.0);
}

TEST_CASE("an interval the run did not continue has no rate though the counter rose", "[cli][dashboard]")
{
    // The run rule decides the history's intervals, not a second rule in the series. Every
    // counter here rises, so a decrease cannot be why an interval is absent -- only a change of
    // source or a stamp no later than the one before can.
    //
    // WHAT DISTINGUISHES: the interval at the break is absent AND the interval after it, from
    // the same source with a later stamp, is present again.
    auto const switched = RatesOf(
        { DashboardEvent {
              .kind = DashboardEventKind::Sample, .at = At(1), .attempts = ReadingFrom(StatsOrigin::Metrics, 10) },
          DashboardEvent {
              .kind = DashboardEventKind::Sample, .at = At(2), .attempts = ReadingFrom(StatsOrigin::Metrics, 20) },
          DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(3), .attempts = ReadingFrom(StatsOrigin::Info, 30) },
          DashboardEvent {
              .kind = DashboardEventKind::Sample, .at = At(4), .attempts = ReadingFrom(StatsOrigin::Info, 40) } });
    REQUIRE(switched.size() == 4);
    REQUIRE(switched[1].has_value());
    CHECK(Unwrap(switched[1]) == 10.0);
    CHECK(!switched[2].has_value()); // the switch
    REQUIRE(switched[3].has_value());
    CHECK(Unwrap(switched[3]) == 10.0);

    auto const stalled = RatesOf({ SampleAt(10, 5), SampleAt(20, 5), SampleAt(30, 6) });
    REQUIRE(stalled.size() == 3);
    CHECK(!stalled[1].has_value()); // the same instant
    REQUIRE(stalled[2].has_value());
    CHECK(Unwrap(stalled[2]) == 10.0);
}

TEST_CASE("the newest history entry agrees with BrokenRun at every frame", "[cli][dashboard]")
{
    // The history's intervals and `runLength` are two representations of one decision. This is
    // the case that keeps them one: at every frame, the newest entry carries a measured interval
    // exactly when `BrokenRun()` says a rate may be drawn. The script walks every way a run
    // breaks -- both failure routes, a change of source and a stalled stamp.
    auto const tick = DashboardEvent { .kind = DashboardEventKind::Tick };
    auto metrics = [](std::int64_t value, int seconds) {
        return DashboardEvent { .kind = DashboardEventKind::Sample,
                                .at = At(seconds),
                                .attempts = ReadingFrom(StatsOrigin::Metrics, value) };
    };

    auto view = RecordingView {};
    auto sink = CollectingSink {};
    (void) Drive({ tick, SampleAt(10, 1),
                   tick, SampleAt(20, 2),
                   tick, FailedAt(3),
                   tick, SampleAt(30, 4),
                   tick, SampleAt(40, 5),
                   tick, DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(6), .attempts = NothingAnswered() },
                   tick, SampleAt(50, 7),
                   tick, SampleAt(60, 8),
                   tick, metrics(70, 9),
                   tick, metrics(80, 10),
                   tick, metrics(90, 10),
                   tick, metrics(100, 11),
                   tick },
                 DashboardLimits {},
                 view,
                 sink);

    auto measured = std::size_t { 0 };
    auto broken = std::size_t { 0 };
    for (auto const& model: view.seen)
    {
        auto const newestMeasured = !model.history.empty() && model.history.back().elapsed.has_value();
        INFO("history " << model.history.size() << " runLength " << model.runLength);
        CHECK(newestMeasured == !model.BrokenRun());
        if (newestMeasured)
            ++measured;
        else
            ++broken;
    }

    // The control. Equality holds vacuously over a run broken at every frame, so the script
    // must have produced both kinds -- or the loop above tested one arm.
    REQUIRE(view.seen.size() == 13);
    CHECK(measured == 5);
    CHECK(broken == 8);
}

TEST_CASE("the history keeps its bound by dropping the oldest samples", "[cli][dashboard]")
{
    // Bounded so a long session holds the same memory as a short one. WHAT DISTINGUISHES: WHICH
    // end goes. A history that dropped its newest sample would hold the right count and a
    // sparkline frozen at the moment it filled.
    constexpr auto Extra = std::size_t { 3 };
    auto script = std::vector<DashboardEvent> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, HistoryCapacity + Extra))
        script.push_back(SampleAt(static_cast<std::int64_t>(index), static_cast<int>(index) + 1));

    auto const model = FinalModel(std::move(script));

    REQUIRE(model.history.size() == HistoryCapacity);
    auto const* first = FindField(Unwrap(model.history.front().reading), Counter);
    auto const* last = FindField(Unwrap(model.history.back().reading), Counter);
    REQUIRE(first != nullptr);
    REQUIRE(last != nullptr);
    CHECK(first->value.lexical == std::to_string(Extra));
    CHECK(last->value.lexical == std::to_string(HistoryCapacity + Extra - 1));

    // The oldest entry kept continued its run, but the reading it was measured against has been
    // dropped: no rate can be claimed for it, and the one after it has one.
    CHECK(model.history.front().elapsed.has_value());
    auto const rates = CounterRateSeries(model.history, Counter);
    REQUIRE(rates.size() == HistoryCapacity);
    CHECK(!rates.front().has_value());
    REQUIRE(rates[1].has_value());
    CHECK(Unwrap(rates[1]) == 1.0);
}

TEST_CASE("a run whose every sample fails still ends at its sample budget", "[cli][dashboard]")
{
    // Measured before this case existed: `live-stats --samples=3` against an endpoint whose every
    // sample failed never ended. §9.16 bounds a run by samples TAKEN, and §9.17's never-answered
    // exit code is reachable only if a failure spends the budget.
    //
    // WHAT DISTINGUISHES: the run stops at the budget with `SampleBudget` -- not by the script
    // running out, which the rig reports as `SourceDetached` rather than hanging -- carries the
    // MOST RECENT failure's outcome, and draws the frame the last sample was owed. Both failure
    // routes spend it: a `SampleFailed` and a `Sample` its reader could not read.
    auto view = RecordingView {};
    auto sink = CollectingSink {};
    auto const exit =
        Drive({ DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable },
                DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = NothingAnswered() },
                DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Refused },
                DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable },
                DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable } },
              DashboardLimits { .samples = 3 },
              view,
              sink);

    CHECK(exit.stop == DashboardStop::SampleBudget);
    CHECK(exit.outcome == Outcome::Refused);
    CHECK(exit.model.attempts == 3);
    CHECK(exit.model.samples == 0);
    CHECK(sink.frames.size() == 1);
}

TEST_CASE("a sample its reader could not read says why", "[cli][dashboard]")
{
    // Whoever reports a failed sample needs the decision's account, and must not run the decision
    // again to get it. WHAT DISTINGUISHES: the note carries the conclusion AND the source's own
    // reason, and a reading carries none -- or "always say something" passes the first half.
    auto const failed =
        ReadStatsSample(DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = NothingAnswered() });
    CHECK(failed.outcome == Outcome::Unreachable);
    CHECK(failed.note.contains("no stats source answered"));
    CHECK(failed.note.contains("refused"));

    auto const read =
        ReadStatsSample(DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(1), .attempts = Reading(10) });
    CHECK(read.outcome == Outcome::Affirmative);
    CHECK(read.note.empty());
}

namespace
{

/// A node status naming @p version, and nothing else anyone reads here.
/// @param version The node's compiled-in version.
/// @return The status.
[[nodiscard]] CompileCacheWire::NodeStatusFields StatusNamed(std::string version)
{
    auto status = CompileCacheWire::NodeStatusFields {};
    status.version = std::move(version);
    return status;
}

} // namespace

TEST_CASE("the model's node status is what the newest sample carried and nothing older", "[cli][dashboard]")
{
    // A status block is a live view, so the status it draws is the newest sample's. WHAT
    // DISTINGUISHES: a sample that carried NO status leaves the model's EMPTY -- an "update when
    // present" rule keeps `first` there, which is a view of the past -- and a FAILED sample
    // replaces it too, since a node can answer its status while its counters cannot be read.
    auto const tick = DashboardEvent { .kind = DashboardEventKind::Tick };
    auto withStatus = [](int seconds, std::int64_t value, std::string version) {
        return DashboardEvent { .kind = DashboardEventKind::Sample,
                                .at = At(seconds),
                                .attempts = Reading(value),
                                .nodeStatus = StatusNamed(std::move(version)) };
    };

    auto view = RecordingView {};
    auto sink = CollectingSink {};
    (void) Drive({ tick,
                   withStatus(1, 10, "first"),
                   tick,
                   DashboardEvent { .kind = DashboardEventKind::Sample, .at = At(2), .attempts = Reading(20) },
                   tick,
                   DashboardEvent { .kind = DashboardEventKind::SampleFailed,
                                    .nodeStatus = StatusNamed("while-failing"),
                                    .outcome = Outcome::Refused },
                   tick,
                   withStatus(3, 30, "third"),
                   tick },
                 DashboardLimits {},
                 view,
                 sink);

    auto const versionAt = [&view](std::size_t frame) {
        auto const& status = view.seen.at(frame).nodeStatus;
        return status.has_value() ? status->version : std::string { "<absent>" };
    };
    REQUIRE(view.seen.size() == 5);
    CHECK(versionAt(0) == "<absent>"); // nothing taken yet
    CHECK(versionAt(1) == "first");
    CHECK(versionAt(2) == "<absent>");      // the sample carried none: not `first`
    CHECK(versionAt(3) == "while-failing"); // a failed sample replaces it too
    CHECK(versionAt(4) == "third");
}

TEST_CASE("the model's fleet document is the newest reading's and nothing older", "[cli][dashboard]")
{
    // A fleet panel draws the newest document, so the model holds exactly that. WHAT DISTINGUISHES:
    // a sample whose reader REFUSED it leaves the model's document null -- a rule keeping the last one
    // draws a fleet from before the gap -- and so does a `SampleFailed`, since the same helper replaces
    // it on both routes.
    auto const tick = DashboardEvent { .kind = DashboardEventKind::Tick };
    auto const fleet = [](int seconds, std::string body) {
        return DashboardEvent { .kind = DashboardEventKind::Sample,
                                .at = At(seconds),
                                .document = std::expected<std::string, AdminError> { std::move(body) } };
    };

    auto view = RecordingView {};
    auto sink = CollectingSink {};
    (void) Drive(
        { tick,
          fleet(1, "first"),
          tick,
          DashboardEvent { .kind = DashboardEventKind::Sample,
                           .at = At(2),
                           .document =
                               std::expected<std::string, AdminError> {
                                   std::unexpect, AdminError { .kind = AdminFailure::Refused, .detail = "follower" } } },
          tick,
          fleet(3, "third"),
          tick,
          DashboardEvent { .kind = DashboardEventKind::SampleFailed, .outcome = Outcome::Unreachable },
          tick,
          fleet(5, "fifth"),
          tick },
        DashboardLimits {},
        view,
        sink,
        &HandingReader);

    auto const bodyAt = [&view](std::size_t frame) {
        auto const& document = view.seen.at(frame).latestDocument;
        if (document == nullptr)
            return std::string { "<null>" };
        auto const* table = document->Section(FleetSection::Kpi);
        return table == nullptr || table->rows.empty() ? std::string { "<no kpi>" } : table->rows.front().front().lexical;
    };
    REQUIRE(view.seen.size() == 6);
    CHECK(bodyAt(0) == "<null>"); // nothing taken yet
    CHECK(bodyAt(1) == "first");
    CHECK(bodyAt(2) == "<null>"); // refused by its reader: not `first`
    CHECK(bodyAt(3) == "third");
    CHECK(bodyAt(4) == "<null>"); // a failed sample replaces it too
    CHECK(bodyAt(5) == "fifth");
}

namespace
{

/// A view that watches the model's fleet document without keeping it alive.
///
/// **Weak, deliberately.** `RecordingView` copies the whole model, and a copied `shared_ptr` is
/// itself an owner -- a view recording that way would keep every document alive and could never see
/// one released.
class DocumentWatchingView final: public IDashboardView
{
  public:
    [[nodiscard]] std::string Frame(DashboardModel const& model) override
    {
        seen.emplace_back(model.latestDocument);
        return {};
    }

    std::vector<std::weak_ptr<FleetDocument const>> seen {};
};

} // namespace

TEST_CASE("after many fleet samples exactly one document is alive, and the history holds none", "[cli][dashboard]")
{
    // History exists for trends and rates. WHAT DISTINGUISHES: every document an earlier frame drew
    // has been RELEASED once a later sample replaced it -- a fold that kept documents in its history
    // entries would keep each of them alive -- while the newest is alive and owned by the model alone.
    constexpr auto Samples = 6;
    auto script = std::vector<DashboardEvent> {};
    for (auto const second: std::views::iota(1, Samples + 1))
    {
        script.push_back(
            DashboardEvent { .kind = DashboardEventKind::Sample,
                             .at = At(second),
                             .document = std::expected<std::string, AdminError> { std::format("body-{}", second) } });
        script.push_back(DashboardEvent { .kind = DashboardEventKind::Tick });
    }

    auto view = DocumentWatchingView {};
    auto sink = CollectingSink {};
    auto const exit = Drive(std::move(script), DashboardLimits {}, view, sink, &HandingReader);

    REQUIRE(view.seen.size() == static_cast<std::size_t>(Samples));
    for (auto const frame: std::views::iota(std::size_t { 0 }, view.seen.size() - 1))
    {
        INFO("frame " << frame);
        CHECK(view.seen[frame].expired());
    }
    CHECK_FALSE(view.seen.back().expired());
    CHECK(exit.model.latestDocument.use_count() == 1);
    CHECK(exit.model.history.size() == static_cast<std::size_t>(Samples));
}

namespace
{

/// A view that places one image over the model's geometry, to watch the loop carry it.
class PlacingView final: public IDashboardView
{
  public:
    [[nodiscard]] std::string Frame(DashboardModel const& model) override
    {
        return PlacedFrame(model).text;
    }

    [[nodiscard]] DashboardFrame PlacedFrame(DashboardModel const& model) override
    {
        cellPixelsSeen.push_back(model.cellPixels);
        return DashboardFrame { .text = std::format("cols={}", model.columns),
                                .placements = { FramePlacement { .row = 2,
                                                                 .column = 3,
                                                                 .cellsWide = static_cast<std::size_t>(model.columns),
                                                                 .cellsHigh = 1,
                                                                 .sixel = "body" } } };
    }

    std::vector<std::optional<CellPixelSize>> cellPixelsSeen {};
};

} // namespace

TEST_CASE("a frame's images reach the sink with its text, and a cell size rides each resize", "[cli][dashboard]")
{
    // WHAT DISTINGUISHES: the placement a view made arrives at the sink beside its text -- a loop
    // presenting `Frame` alone would drop it -- and the model's cell size is the LAST resize's, so a
    // resize that carried none clears the size an earlier one reported.
    auto view = PlacingView {};
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize,
                                    .columns = 40,
                                    .rows = 10,
                                    .cellPixels = CellPixelSize { .width = 9, .height = 18 } },
                   DashboardEvent { .kind = DashboardEventKind::Tick },
                   DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 50, .rows = 10 },
                   DashboardEvent { .kind = DashboardEventKind::Tick } },
                 DashboardLimits {},
                 view,
                 sink);

    REQUIRE(sink.frames.size() == 2);
    REQUIRE(sink.placements.size() == 2);
    CHECK(sink.frames[0] == "cols=40");
    REQUIRE(sink.placements[0].size() == 1);
    CHECK(sink.placements[0][0].cellsWide == 40);
    CHECK(sink.placements[0][0].sixel == "body");

    REQUIRE(view.cellPixelsSeen.size() == 2);
    REQUIRE(view.cellPixelsSeen[0].has_value());
    CHECK(Unwrap(view.cellPixelsSeen[0]).width == 9);
    CHECK(Unwrap(view.cellPixelsSeen[0]).height == 18);
    CHECK_FALSE(view.cellPixelsSeen[1].has_value());
}

TEST_CASE("a frame presented as text reaches a sink's one door as a frame with no images", "[cli][dashboard]")
{
    // `Present` is not a second door: a sink implements `PresentPlaced` alone, and text handed to
    // `Present` arrives there, whole and with nothing placed over it.
    auto sink = CollectingSink {};
    auto& door = static_cast<IFrameSink&>(sink);
    door.Present("rows\nmore rows");
    REQUIRE(sink.frames.size() == 1);
    CHECK(sink.frames[0] == "rows\nmore rows");
    REQUIRE(sink.placements.size() == 1);
    CHECK(sink.placements[0].empty());
}
