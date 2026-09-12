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

    /// Fold one reading into the model.
    ///
    /// Split out because it is the whole of what a `Sample` does, and a reading that
    /// cannot be chosen must leave the model in the same state a `SampleFailed` does --
    /// two routes, one outcome, and writing it twice is how they drift apart.
    /// @param model What to update.
    /// @param attempts What each source said.
    /// @return True when a reading was accepted.
    [[nodiscard]] bool AcceptReading(DashboardModel& model, std::span<StatsAttempt const> attempts)
    {
        auto answer = ChooseStats(attempts);
        if (answer.outcome != Outcome::Affirmative)
        {
            // Nothing readable. The run resets whether the gatherer said so itself or
            // every source came back silent, because a rate drawn across this would be
            // an average over an interval nobody observed.
            model.runLength = 0;
            return false;
        }

        model.previous = std::move(model.latest);
        model.latest = std::move(answer.value);
        ++model.runLength;
        ++model.samples;
        return true;
    }

} // namespace

bool IsQuitKey(std::string_view keys) noexcept
{
    return std::ranges::find(QuitKeys, keys) != QuitKeys.end();
}

Task<DashboardExit> RunDashboard(IDashboardEventSource* events,
                                 IDashboardView* view,
                                 IFrameSink* sink,
                                 DashboardLimits limits)
{
    auto exit = DashboardExit { .stop = DashboardStop::SourceDetached, .outcome = Outcome::Unreachable, .model = {} };

    while (true)
    {
        auto const event = co_await events->Next();

        switch (event.kind)
        {
            case DashboardEventKind::Tick:
                // A frame is drawn from what is KNOWN, including before anything has
                // been read -- that first frame, with every rate absent, is §9.1's whole
                // point and an implementation that waited for a reading would skip it.
                sink->Present(view->Frame(exit.model));
                ++exit.model.frames;
                break;

            case DashboardEventKind::Sample:
                if (AcceptReading(exit.model, event.attempts))
                {
                    // One reading is enough to make the run a success forever after. A
                    // source lost later is drawn as gaps, not turned into a failure --
                    // §9.17, and the direction a healthy-path test cannot see.
                    exit.outcome = Outcome::Affirmative;
                    if (limits.samples != 0 && exit.model.samples >= limits.samples)
                    {
                        exit.stop = DashboardStop::SampleBudget;
                        events->Close();
                        co_return exit;
                    }
                }
                break;

            case DashboardEventKind::SampleFailed:
                exit.model.runLength = 0;
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
