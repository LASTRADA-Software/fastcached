// SPDX-License-Identifier: Apache-2.0
#include "DashboardLoop.hpp"
#include "StatsSource.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The keystrokes that mean *leave*.
    ///
    /// A table rather than an `if` ladder, so a fourth spelling is a row. `ESC` is here
    /// because an operator reaches for it and a dashboard that ignores it feels stuck;
    /// it is safe only because this view sends no query whose reply begins with `ESC`
    /// -- the DA1 exchange is over before the loop starts, which is stage 2's one-shot
    /// probe, not a thing that can arrive mid-run.
    constexpr auto QuitKeys = std::to_array<std::string_view>({ "q", "Q", "\x03", "\x1b" });

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

    /// Record a sample that produced no reading, by either route.
    ///
    /// **The one place a failure is recorded.** A `SampleFailed`, and a `Sample` its reader
    /// could not read, both arrive here. They used to be two inline assignments that happened
    /// to agree, under a test comment saying they went through one helper; a failure now
    /// carries an outcome too, and *happened to agree* is how one route would record it and
    /// the other would not.
    ///
    /// The outcome is kept only while nothing has been read: one reading makes the run a
    /// success for good (§9.17), and a failure after that is a gap rather than a verdict.
    /// @param exit The run so far.
    /// @param why What the failure means as an outcome.
    void RecordFailure(DashboardExit& exit, Outcome why) noexcept
    {
        exit.model.runLength = 0;
        if (exit.outcome != Outcome::Affirmative)
            exit.outcome = why;
    }

    /// Fold one accepted reading into the model.
    /// @param model What to update.
    /// @param reading What the session's reader made of the sample.
    /// @param at When the sample was taken.
    void AcceptReading(DashboardModel& model, SampleReading reading, TimePoint at)
    {
        auto stamp = ReadingStamp { .at = at, .source = std::move(reading.source) };
        auto const continues =
            model.runLength > 0 && model.latestStamp.has_value() && ContinuesRun(*model.latestStamp, stamp);
        model.runLength = continues ? model.runLength + 1 : 1;
        model.previous = std::move(model.latest);
        model.latest = std::move(reading.value);
        model.latestStamp = std::move(stamp);
        ++model.samples;
    }

} // namespace

SampleReading ReadStatsSample(DashboardEvent const& event)
{
    auto answer = ChooseStats(event.attempts);
    if (answer.outcome != Outcome::Affirmative)
        return SampleReading { .outcome = answer.outcome, .value = {}, .source = {} };

    // Copied out BEFORE the value moves, since the field points into it.
    auto const* named = FindField(answer.value, StatsSourceFieldName);
    auto source = named == nullptr ? std::string {} : named->value.lexical;
    return SampleReading { .outcome = Outcome::Affirmative, .value = std::move(answer.value), .source = std::move(source) };
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
                sink->Present(view->Frame(exit.model));
                ++exit.model.frames;
                break;

            case DashboardEventKind::Sample: {
                auto reading = reader(event);
                if (reading.outcome != Outcome::Affirmative)
                {
                    RecordFailure(exit, reading.outcome);
                    break;
                }

                AcceptReading(exit.model, std::move(reading), event.at);
                // One reading is enough to make the run a success forever after. A source
                // lost later is drawn as gaps, not turned into a failure -- §9.17, and the
                // direction a healthy-path test cannot see.
                exit.outcome = Outcome::Affirmative;
                if (limits.samples != 0 && exit.model.samples >= limits.samples)
                {
                    exit.stop = DashboardStop::SampleBudget;
                    events->Close();
                    co_return exit;
                }
                break;
            }

            case DashboardEventKind::SampleFailed:
                RecordFailure(exit, event.outcome);
                break;

            case DashboardEventKind::Key:
                if (IsQuitKey(event.keys))
                {
                    exit.stop = DashboardStop::Quit;
                    events->Close();
                    co_return exit;
                }
                break;

            case DashboardEventKind::Resize:
                exit.model.columns = event.columns;
                exit.model.rows = event.rows;
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
