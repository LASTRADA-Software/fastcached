// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;

namespace
{

/// A reading `ChooseStats` accepts, so every sample moves the model.
/// @return One answered attempt.
[[nodiscard]] std::vector<StatsAttempt> Reading()
{
    return { StatsAttempt { .origin = StatsOrigin::Info,
                            .asked = true,
                            .record = RecordValue({ Field { .name = "curr_connections", .value = TextCell("10") } }),
                            .note = {} } };
}

/// A view that counts the frames drawn through it.
class CountingView final: public IDashboardView
{
  public:
    /// @param frames Where to count; outlives the view.
    explicit CountingView(std::size_t* frames) noexcept:
        _frames { frames }
    {
    }

    [[nodiscard]] std::string Frame(DashboardModel const& /*model*/) override
    {
        ++*_frames;
        return "frame";
    }

  private:
    std::size_t* _frames;
};

/// A ladder that counts how often it is asked, and hands out views that count frames.
class CountingLadder final: public IViewLadder
{
  public:
    [[nodiscard]] std::unique_ptr<IDashboardView> Decide() override
    {
        ++decisions;
        return std::make_unique<CountingView>(&framesThroughDecidedViews);
    }

    std::size_t decisions { 0 };
    std::size_t framesThroughDecidedViews { 0 };
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

/// Await a whole session into @p out.
/// @param events The events.
/// @param ladder How it draws.
/// @param sink Where frames go.
/// @param out How it ended.
/// @return The task to submit.
[[nodiscard]] Task<void> RunInto(IDashboardEventSource* events,
                                 IViewLadder* ladder,
                                 IFrameSink* sink,
                                 std::optional<DashboardExit>* out)
{
    *out = co_await RunLiveSession(events, ladder, sink, DashboardLimits {});
}

/// Run a session over @p script to its end.
/// @param script The events; the scripted source answers `Detached` once they run out.
/// @param ladder How it draws.
/// @param sink Where frames go.
/// @return How it ended, or nullopt when it did not end.
[[nodiscard]] std::optional<DashboardExit> Session(std::vector<DashboardEvent> script,
                                                   CountingLadder& ladder,
                                                   CountSink& sink)
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto events = ScriptedDashboardEvents { reactor, std::move(script) };

    auto exit = std::optional<DashboardExit> {};
    auto task = RunInto(&events, &ladder, &sink, &exit);
    reactor.Submit(task.Native());
    reactor.Drain();
    return exit;
}

} // namespace

TEST_CASE("a session decides how it draws once however many frames it draws", "[cli][live][session]")
{
    // §9.12. Counted over MANY frames, because once and once-per-frame agree on a session
    // of one: a ladder asked again per frame would pass a single-frame script.
    constexpr auto frames = std::size_t { 5 };
    auto script = std::vector<DashboardEvent> {};
    for ([[maybe_unused]] auto const frame: std::views::iota(std::size_t { 0 }, frames))
    {
        script.push_back(DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = Reading() });
        script.push_back(DashboardEvent { .kind = DashboardEventKind::Tick });
    }

    CountingLadder ladder;
    CountSink sink;
    auto const exit = Session(std::move(script), ladder, sink);

    CHECK((exit.has_value() && exit->stop == DashboardStop::SourceDetached));
    CHECK(sink.frames == frames);
    CHECK(ladder.decisions == 1);
    // Every frame went through the view that one decision produced.
    CHECK(ladder.framesThroughDecidedViews == frames);
}

TEST_CASE("a session decides how it draws before its first event arrives", "[cli][live][session]")
{
    // The first event an interactive session sees is the terminal's opening `Resize`, and
    // its tick draws a frame: the view has to exist by then. So a session that ends before
    // drawing anything has still decided, exactly once.
    CountingLadder ladder;
    CountSink sink;
    auto const exit = Session({}, ladder, sink);

    CHECK((exit.has_value() && exit->stop == DashboardStop::SourceDetached));
    CHECK(sink.frames == 0);
    CHECK(ladder.decisions == 1);
}
