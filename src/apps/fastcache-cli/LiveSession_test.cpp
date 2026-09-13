// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"
#include "LiveSourceRig.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
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

namespace
{

/// A drain wait whose time passes only when the drain sleeps, and which can let the source
/// finish draining after a chosen number of sleeps.
///
/// **No real time passes**: the drain's ceiling is measured on this clock, so a five-second
/// ceiling costs a loop of five hundred iterations and nothing else.
class SteppedDrainWait final: public IDrainWait
{
  public:
    /// @param drained Set after @p landsAfter sleeps, standing in for the reactor; null when
    ///        nothing ever lands.
    /// @param landsAfter How many sleeps pass before it does.
    explicit SteppedDrainWait(std::atomic<bool>* drained = nullptr, int landsAfter = 0) noexcept:
        _drained { drained },
        _landsAfter { landsAfter }
    {
    }

    [[nodiscard]] TimePoint Now() const noexcept override
    {
        return _now;
    }

    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        _now += requested;
        ++_sleeps;
        if (_drained != nullptr && _sleeps == _landsAfter)
            _drained->store(true);
    }

    [[nodiscard]] int Sleeps() const noexcept
    {
        return _sleeps;
    }

  private:
    std::atomic<bool>* _drained;
    int _landsAfter;
    TimePoint _now {};
    int _sleeps { 0 };
};

/// The bound every drain case uses.
constexpr auto Bound = DrainBound { .ceiling = std::chrono::seconds { 5 }, .poll = std::chrono::milliseconds { 10 } };

} // namespace

TEST_CASE("a sample that never returns is abandoned at the ceiling with the exit code the session earned",
          "[cli][live][session]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };
    // The first gather is out, started at the clock's epoch -- where the drain's clock starts too.
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);
    source.Close();

    auto const drained = std::atomic<bool> { false };

    auto answered = SteppedDrainWait {};
    auto const afterReadings = DrainSession(drained, source, Outcome::Affirmative, "10.0.0.4:6674", Bound, answered);
    CHECK(afterReadings.exitCode == 0);
    CHECK((afterReadings.abandonment.has_value() && afterReadings.abandonment->contains("10.0.0.4:6674")));
    CHECK((afterReadings.abandonment.has_value() && afterReadings.abandonment->contains("5000 ms")));

    // The exit code is the session's, not the drain's: one that never had a reading is still 3.
    auto unanswered = SteppedDrainWait {};
    auto const withoutReadings = DrainSession(drained, source, Outcome::Unreachable, "10.0.0.4:6674", Bound, unanswered);
    CHECK(withoutReadings.exitCode == 3);
    CHECK(withoutReadings.abandonment.has_value());

    rig.Settle();
    CloseAndDrain(rig, source);
}

TEST_CASE("a sample that returns inside the bound drains and leaves through the ordinary path", "[cli][live][session]")
{
    // The control for the case above: the same drain, and the source finishes draining on the
    // third look. Nothing is abandoned, and the drain stopped looking when it did.
    Rig rig;
    LiveEventSource source { rig.Parts() };
    rig.reactor.Drain();
    source.Close();

    auto drained = std::atomic<bool> { false };
    auto wait = SteppedDrainWait { &drained, 3 };
    auto const ending = DrainSession(drained, source, Outcome::Affirmative, "10.0.0.4:6674", Bound, wait);
    CHECK(ending.exitCode == 0);
    CHECK_FALSE(ending.abandonment.has_value());
    CHECK(wait.Sleeps() == 3);

    rig.Settle();
    CloseAndDrain(rig, source);
}

TEST_CASE("abandoning a session with no sample out says so rather than inventing an age", "[cli][live][session]")
{
    auto const ending = DecideSessionEnding(DrainResult::Ceiling, Outcome::Affirmative, "10.0.0.4:6674", std::nullopt);
    CHECK(ending.exitCode == 0);
    CHECK((ending.abandonment.has_value() && ending.abandonment->contains("no sample")));
    CHECK((ending.abandonment.has_value() && !ending.abandonment->contains(" ms")));
}
