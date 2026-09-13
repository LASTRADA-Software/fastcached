// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "StatsSource.hpp"

#include <FastCache/Core/NumericText.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <ranges>
#include <string_view>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// `Ctrl+C`: the quit key that stays one while a view `CapturesText`, so a session is always leavable.
    constexpr std::string_view InterruptKey = "\x03";

    /// The keystrokes that mean *leave*.
    ///
    /// A table rather than an `if` ladder, so a fourth spelling is a row. `ESC` is here
    /// because an operator reaches for it and a dashboard that ignores it feels stuck;
    /// it is safe only because this view sends no query whose reply begins with `ESC`
    /// -- the DA1 exchange is over before the loop starts, which is stage 2's one-shot
    /// probe, not a thing that can arrive mid-run.
    constexpr auto QuitKeys = std::to_array<std::string_view>({ "q", "Q", InterruptKey, "\x1b" });

    /// Whether a reading continues the run the reading before it belongs to.
    ///
    /// **The one rule a run obeys**, and each way it breaks is a way the change between two
    /// readings would describe an interval nobody measured:
    ///
    /// - a different SOURCE: two vocabularies, so their difference is a change in nothing;
    /// - a stamp NO LATER than the one before: an elapsed time no rate can be divided by,
    ///   which is what a loop-cached clock produces for two readings taken while its
    ///   reactor slept.
    ///
    /// A failure between the two breaks the run as well; that route never reaches here,
    /// because `RecordFailure` has already set the run to zero.
    /// @param before The reading the run so far ends with.
    /// @param after The reading being folded in.
    /// @return True when the interval between them can be measured.
    [[nodiscard]] bool ContinuesRun(ReadingStamp const& before, ReadingStamp const& after) noexcept
    {
        return after.source == before.source && after.at > before.at;
    }

    /// Append one sample to the history, dropping the oldest entries past the bound.
    /// @param model What to update.
    /// @param entry The sample.
    void RecordHistory(DashboardModel& model, HistoryEntry entry)
    {
        model.history.push_back(std::move(entry));
        while (model.history.size() > HistoryCapacity)
            model.history.pop_front();
    }

    /// Record a sample that produced no reading, by either route.
    ///
    /// **The one place a failure is recorded.** A `SampleFailed`, and a `Sample` its reader
    /// could not read, both arrive here. They used to be two inline assignments that happened
    /// to agree, under a test comment saying they went through one helper; a failure now
    /// carries an outcome and a history entry too, and *happened to agree* is how one route
    /// would record them and the other would not.
    ///
    /// The outcome is kept only while nothing has been read: one reading makes the run a
    /// success for good (§9.17), and a failure after that is a gap rather than a verdict.
    /// @param exit The run so far.
    /// @param why What the failure means as an outcome.
    void RecordFailure(DashboardExit& exit, Outcome why)
    {
        exit.model.runLength = 0;
        RecordHistory(exit.model, HistoryEntry {});
        if (exit.outcome != Outcome::Affirmative)
            exit.outcome = why;
    }

    /// Draw what is known and present it.
    ///
    /// Every frame the loop presents goes through here, so a frame is drawn and counted in
    /// one place however many reasons there are to owe one.
    /// @param view What draws.
    /// @param sink Where the frame goes.
    /// @param model What is known; its frame count advances.
    void PresentFrame(IDashboardView& view, IFrameSink& sink, DashboardModel& model)
    {
        sink.PresentPlaced(view.PlacedFrame(model));
        ++model.frames;
    }

    /// Fold one accepted reading into the model.
    /// @param model What to update.
    /// @param reading What the session's reader made of the sample.
    /// @param at When the sample was taken.
    void AcceptReading(DashboardModel& model, SampleReading reading, TimePoint at)
    {
        auto stamp = ReadingStamp { .at = at,
                                    .source = std::move(reading.source),
                                    .route = std::move(reading.route),
                                    .where = std::move(reading.where),
                                    .role = std::move(reading.role) };
        // The interval is decided ONCE, and the run length and the history are both written from
        // that one decision, so the history and `BrokenRun()` cannot come to disagree about it.
        auto const elapsed = model.runLength > 0 && model.latestStamp.has_value() && ContinuesRun(*model.latestStamp, stamp)
                                 ? std::optional<Duration> { stamp.at - model.latestStamp->at }
                                 : std::optional<Duration> {};
        model.runLength = elapsed.has_value() ? model.runLength + 1 : 1;
        RecordHistory(
            model,
            HistoryEntry {
                .reading = reading.value, .stats = reading.stats, .elapsed = elapsed, .points = std::move(reading.points) });
        model.previous = std::move(model.latest);
        model.latest = std::move(reading.value);
        model.stats = std::move(reading.stats);
        model.latestStamp = std::move(stamp);
        ++model.samples;
    }

    /// The rate of @p field over the interval from @p before to @p entry, per second.
    /// @param before The entry the interval starts at.
    /// @param entry The entry the interval ends at.
    /// @param field The counter.
    /// @param tier The tier asked about, or nullopt for the whole cache.
    /// @return The rate, or nullopt where `CounterRateSeries` says none can be claimed.
    [[nodiscard]] std::optional<double> RateInto(HistoryEntry const& before,
                                                 HistoryEntry const& entry,
                                                 ReadingField field,
                                                 std::optional<StorageTier> tier)
    {
        // Non-positive is refused here as well as by the fold, so a hand-built entry cannot
        // divide by zero: the fold never records one, and this function does not rely on that.
        if (!entry.elapsed.has_value() || entry.elapsed->count() <= 0)
            return std::nullopt;
        auto const from = NumberIn(before.stats, field, tier);
        auto const to = NumberIn(entry.stats, field, tier);
        if (!from.has_value() || !to.has_value() || *to < *from)
            return std::nullopt;
        return (*to - *from) / std::chrono::duration<double> { *entry.elapsed }.count();
    }

    /// Everything a sample taken does whatever it read: it spends the budget, and it replaces the
    /// node status and the fleet document with whatever it carried.
    ///
    /// One helper for both arms -- a `Sample` and a `SampleFailed` -- so neither route can count a
    /// sample without replacing both, or the reverse.
    /// @param model Where the count, the status and the document are kept.
    /// @param event The sample.
    /// @param document The document its reading carried; null for a failure or a reading without one.
    /// @param limits The budget.
    /// @return True when this sample spent the budget.
    [[nodiscard]] bool RecordSampleTaken(DashboardModel& model,
                                         DashboardEvent const& event,
                                         std::shared_ptr<FleetDocument const> document,
                                         DashboardLimits limits)
    {
        model.nodeStatus = event.nodeStatus;
        model.latestDocument = std::move(document);
        ++model.attempts;
        return limits.samples != 0 && model.attempts >= limits.samples;
    }

    /// End the run on its budget.
    ///
    /// The stop that consumed a sample owes that sample its frame, whether it was a reading or a
    /// failure. This is the loop's only draw not preceded by a `Tick`: the `Tick` owed for the
    /// last sample would arrive after the loop had already returned, so without it `--samples=N`
    /// presents N-1 frames and the Nth sample -- the one the budget was spent on -- is never
    /// shown. Drawn here rather than by whoever runs the loop, so frames are still presented from
    /// one place.
    /// @param exit The run so far.
    /// @param view What draws.
    /// @param sink Where the frame goes.
    /// @param events The source, closed.
    void StopOnBudget(DashboardExit& exit, IDashboardView& view, IFrameSink& sink, IDashboardEventSource& events)
    {
        PresentFrame(view, sink, exit.model);
        exit.stop = DashboardStop::SampleBudget;
        events.Close();
    }

} // namespace

std::optional<double> NumberIn(std::optional<StatsReading> const& reading,
                               ReadingField field,
                               std::optional<StorageTier> tier) noexcept
{
    return reading.has_value() && field.Names() ? field.read(*reading, tier) : std::nullopt;
}

std::vector<std::optional<double>> CounterRateSeries(std::deque<HistoryEntry> const& history,
                                                     ReadingField field,
                                                     std::optional<StorageTier> tier)
{
    auto series = std::vector<std::optional<double>> {};
    series.reserve(history.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, history.size()))
        series.push_back(index == 0 ? std::optional<double> {} : RateInto(history[index - 1], history[index], field, tier));
    return series;
}

SampleReading ReadStatsSample(DashboardEvent const& event)
{
    auto answer = ChooseStats(event.attempts);
    if (answer.outcome != Outcome::Affirmative)
    {
        // The advisories ARE the account: the conclusion, then what happened to each source. One
        // line, because whoever surfaces it writes one line per failed sample.
        auto note = std::string {};
        for (auto const& advisory: answer.advisories)
            note += (note.empty() ? "" : "; ") + advisory;
        return SampleReading { .outcome = answer.outcome, .value = {}, .source = {}, .note = std::move(note) };
    }

    // Copied out BEFORE the value moves, since the field points into it.
    auto const* named = FindField(answer.value, StatsSourceFieldName);
    auto source = named == nullptr ? std::string {} : named->value.lexical;
    // Where it answered is the WINNING attempt's, found by the name the decision reported rather than
    // decided a second time; the route is that source's own row.
    auto reading = SampleReading {
        .outcome = Outcome::Affirmative, .value = std::move(answer.value), .source = std::move(source), .note = {}
    };
    for (auto const& attempt: event.attempts)
    {
        auto const* row = DescriptorOf(attempt.origin);
        if (row != nullptr && row->name == reading.source)
        {
            reading.route = std::string { row->route };
            reading.where = attempt.where;
            // The live model, read out of the record in that source's own vocabulary, once.
            reading.stats = StatsReadingFromRecord(reading.value, attempt.origin);
        }
    }
    return reading;
}

bool IsQuitKey(std::string_view keys) noexcept
{
    return std::ranges::find(QuitKeys, keys) != QuitKeys.end();
}

Task<DashboardExit> RunDashboard(
    IDashboardEventSource* events, SampleReader reader, IDashboardView* view, IFrameSink* sink, DashboardLimits limits)
{
    auto exit = DashboardExit { .stop = DashboardStop::SourceDetached, .outcome = Outcome::Unreachable, .model = {} };

    while (true)
    {
        auto const event = co_await events->Next();

        switch (event.kind)
        {
            case DashboardEventKind::Tick:
                // A `Tick` means a frame is OWED, and the source decides when: the loop
                // keeps no cadence of its own. The frame is drawn from what is KNOWN, so
                // one owed before anything was read shows every rate absent (§9.1) rather
                // than waiting for a reading. Whether one is owed that early is the
                // source's call -- a fixture opening with a `Tick` is that fixture's script,
                // not a rule, and a piped run owes none until the first sample, because an
                // empty first line would break §9.13.
                PresentFrame(*view, *sink, exit.model);
                break;

            case DashboardEventKind::Sample: {
                auto reading = reader(event);
                // Only a reading's document is kept: a reader that refused a sample made no claim
                // about the fleet, whatever it had in hand.
                auto document = reading.outcome == Outcome::Affirmative ? std::move(reading.document)
                                                                        : std::shared_ptr<FleetDocument const> {};
                if (reading.outcome != Outcome::Affirmative)
                {
                    RecordFailure(exit, reading.outcome);
                }
                else
                {
                    AcceptReading(exit.model, std::move(reading), event.at);
                    // One reading is enough to make the run a success forever after. A source
                    // lost later is drawn as gaps, not turned into a failure -- §9.17, and the
                    // direction a healthy-path test cannot see.
                    exit.outcome = Outcome::Affirmative;
                }
                if (RecordSampleTaken(exit.model, event, std::move(document), limits))
                {
                    StopOnBudget(exit, *view, *sink, *events);
                    co_return exit;
                }
                break;
            }

            case DashboardEventKind::SampleFailed:
                RecordFailure(exit, event.outcome);
                if (RecordSampleTaken(exit.model, event, {}, limits))
                {
                    StopOnBudget(exit, *view, *sink, *events);
                    co_return exit;
                }
                break;

            case DashboardEventKind::Key:
                if (!IsQuitKey(event.keys) || (view->CapturesText() && event.keys != InterruptKey))
                {
                    // Owed now, not at the next `Tick`: the view changed, and nothing was read.
                    if (view->Key(event.keys))
                        PresentFrame(*view, *sink, exit.model);
                    break;
                }
                [[fallthrough]];

            case DashboardEventKind::StopRequested:
                // Two routes to one stop, and neither reads the other's fields: a quit KEY is
                // judged by its bytes, a stop REQUEST carries none. The run's outcome is not
                // touched -- a stop after one reading is still that reading's success.
                exit.stop = DashboardStop::Quit;
                events->Close();
                co_return exit;

            case DashboardEventKind::Resize:
                exit.model.columns = event.columns;
                exit.model.rows = event.rows;
                exit.model.cellPixels = event.cellPixels;
                break;

            case DashboardEventKind::Detached:
            case DashboardEventKind::Last:
                exit.stop = DashboardStop::SourceDetached;
                events->Close();
                co_return exit;
        }
    }
}

} // namespace FastCache::Cli
