// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"
#include "LiveSourceRig.hpp"
#include "ScriptedDashboardEvents.hpp"
#include "TerminalCapabilities.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
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

namespace
{

/// A terminal acquisition that answers from a script, counting what it was asked.
///
/// It parks through the reactor before answering, as the real one resumes on `resumeOn` after
/// a hop, so a composition that raced ahead of the acquisition would be caught.
class ScriptedAcquisition final: public ITerminalAcquisition
{
  public:
    /// @param reactor Where the answer parks.
    /// @param answer What the terminal reports, or why it cannot be acquired.
    /// @param detachAtOnce Whether the acquired terminal goes away before its first read.
    ScriptedAcquisition(IReactor& reactor,
                        std::expected<TerminalCapabilities, std::string> answer,
                        bool detachAtOnce = false):
        _reactor { reactor },
        _answer { std::move(answer) },
        _detachAtOnce { detachAtOnce }
    {
    }

    [[nodiscard]] Task<std::expected<StartedTerminal, std::string>> Acquire(IExecutor* pool, IExecutor* resumeOn) override
    {
        ++calls;
        askedPool = pool;
        askedResumeOn = resumeOn;
        co_await ParkAwaiter { .reactor = _reactor };
        if (!_answer.has_value())
            co_return std::unexpected(_answer.error());
        auto terminal = std::make_unique<SpokenTerminal>(_reactor, &release);
        spoken = terminal.get();
        if (_detachAtOnce)
            terminal->GoAway();
        co_return StartedTerminal { .capabilities = *_answer, .events = std::move(terminal) };
    }

    int calls { 0 };
    IExecutor* askedPool { nullptr };
    IExecutor* askedResumeOn { nullptr };
    SpokenTerminal* spoken { nullptr };
    TerminalRelease release {};

  private:
    IReactor& _reactor;
    std::expected<TerminalCapabilities, std::string> _answer;
    bool _detachAtOnce;
};

/// A stop-signal installer that hands out scripted signals, or refuses.
class ScriptedInstaller final: public IStopSignalInstaller
{
  public:
    /// @param reactor Where an installed signal's wait parks.
    /// @param refusal Why installing fails, or empty when it succeeds.
    explicit ScriptedInstaller(IReactor& reactor, std::string refusal = {}):
        _reactor { reactor },
        _refusal { std::move(refusal) }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<IStopSignal>, std::string> Install() override
    {
        ++calls;
        if (!_refusal.empty())
            return std::unexpected(_refusal);
        auto signal = std::make_unique<ScriptedStopSignal>(_reactor);
        installed = signal.get();
        return signal;
    }

    int calls { 0 };
    ScriptedStopSignal* installed { nullptr };

  private:
    IReactor& _reactor;
    std::string _refusal;
};

/// A view that throws on its first frame.
class ThrowingView final: public IDashboardView
{
  public:
    [[nodiscard]] std::string Frame(DashboardModel const& /*model*/) override
    {
        throw std::runtime_error { "the view could not draw" };
    }
};

/// Hands out a view per rung and records which rungs were asked for.
class RecordingRungViews final: public IRungViews
{
  public:
    /// @param throwing Whether the views it hands out throw.
    explicit RecordingRungViews(bool throwing = false) noexcept:
        _throwing { throwing }
    {
    }

    [[nodiscard]] std::unique_ptr<IDashboardView> For(RenderRung rung) override
    {
        asked.push_back(rung);
        if (_throwing)
            return std::make_unique<ThrowingView>();
        return std::make_unique<CountView>();
    }

    std::vector<RenderRung> asked {};

  private:
    bool _throwing;
};

/// How a composition's collaborators misbehave, when a case wants them to.
struct CompositionFaults
{
    bool detachAtOnce { false };  ///< An acquired terminal goes away before its first read.
    std::string stopRefusal {};   ///< Why installing the stop request fails; empty when it does not.
    bool throwingViews { false }; ///< Every view throws on its first frame.
};

/// The collaborators one composed session is built from, over a rig.
struct Composition
{
    /// @param rig The rig.
    /// @param terminal What an acquisition reports.
    /// @param faults How the collaborators misbehave.
    Composition(Rig& rig, std::expected<TerminalCapabilities, std::string> terminal, CompositionFaults faults = {}):
        terminals { rig.reactor, std::move(terminal), faults.detachAtOnce },
        stops { rig.reactor, std::move(faults.stopRefusal) },
        views { faults.throwingViews }
    {
    }

    ScriptedAcquisition terminals;
    ScriptedInstaller stops;
    RecordingRungViews views;

    /// The parts for a session over @p rig.
    /// @param rig The rig.
    /// @param terminalPoolReactor The executor standing in for the terminal's own pool.
    /// @param interactive Whether the standard streams are a terminal.
    /// @param samples The sample budget; 0 for none.
    /// @return The parts.
    [[nodiscard]] LiveSessionParts Parts(Rig& rig,
                                         TestReactor& terminalPoolReactor,
                                         bool interactive,
                                         std::size_t samples = 0)
    {
        return LiveSessionParts { .plan = LivePlan { .subject = LiveSubject::Cache,
                                                     .interval = Interval,
                                                     .samples = samples,
                                                     .endpoint = "10.0.0.4:6674" },
                                  .reactor = &rig.reactor,
                                  .gatherer = &rig.gatherer,
                                  .samplePool = &rig.pool,
                                  .stopWaiter = &rig.stopWaiter,
                                  .terminalPool = &terminalPoolReactor,
                                  .sink = &rig.sink,
                                  .interactive = interactive,
                                  .terminals = &terminals,
                                  .stops = &stops,
                                  .views = &views };
    }
};

/// Await a composed session into @p out, recording an exception rather than losing it.
/// @param parts What to compose from.
/// @param hold What outlives it.
/// @param out How it ended.
/// @param threw Set when it ended by an exception.
/// @return The task to submit.
[[nodiscard]] Task<void> ComposeInto(LiveSessionParts parts,
                                     LiveSessionHold* hold,
                                     std::optional<std::expected<LiveSessionRun, Answer>>* out,
                                     bool* threw)
{
    try
    {
        *out = co_await RunComposedSession(std::move(parts), hold);
    }
    catch (std::runtime_error const&)
    {
        *threw = true;
    }
}

/// Close a composed session's source if there is one, run everything out, and check the drain
/// landed and nothing is left parked.
/// @param rig The rig.
/// @param hold The session's hold.
void FinishComposed(Rig& rig, LiveSessionHold& hold)
{
    if (hold.source.has_value())
        hold.source->Close();
    rig.Settle();
    CHECK((!hold.source.has_value() || hold.drained.load()));
    CHECK(rig.reactor.PendingTimers() == 0);
    CHECK(rig.pool.PendingSubmissions() == 0);
}

/// The capabilities of a terminal that reported Sixel.
constexpr auto SixelTerminal =
    TerminalCapabilities { .sixel = SixelAnswer::Advertised, .encoding = TerminalTextEncoding::Utf8 };

} // namespace

TEST_CASE("an interactive session acquires its terminal on the terminal pool and draws on the rung it reported",
          "[cli][live][session]")
{
    Rig rig;
    TestReactor terminalPool { rig.clock };
    Composition composition { rig, SixelTerminal };
    LiveSessionHold hold;

    auto result = std::optional<std::expected<LiveSessionRun, Answer>> {};
    auto threw = false;
    auto task = ComposeInto(composition.Parts(rig, terminalPool, true), &hold, &result, &threw);
    rig.reactor.Submit(task.Native());
    rig.Settle();

    CHECK(composition.terminals.calls == 1);
    // Its own pool, never the sample pool: a read that waits for a keystroke would starve sampling.
    CHECK(composition.terminals.askedPool == &terminalPool);
    CHECK(composition.terminals.askedResumeOn == &rig.reactor);
    CHECK(composition.stops.calls == 0);
    CHECK(composition.views.asked == std::vector<RenderRung> { RenderRung::Sixel });
    CHECK(rig.gatherer.Calls() == 1);

    if (composition.terminals.spoken != nullptr)
        composition.terminals.spoken->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.Settle();
    CHECK((result.has_value() && result->has_value() && (*result)->exit.stop == DashboardStop::Quit));
    CHECK(composition.terminals.release.released);

    FinishComposed(rig, hold);
}

TEST_CASE("a session with no terminal acquires none and takes its whole budget even when a terminal would detach",
          "[cli][live][session]")
{
    // §9 D5: a run that does not draw on a terminal never composes one. The acquisition here
    // would hand out a terminal that goes away before its first read, which ends a session that
    // listens to it -- so N samples prove it was never acquired, not just never counted.
    Rig rig;
    TestReactor terminalPool { rig.clock };
    Composition composition { rig, SixelTerminal, CompositionFaults { .detachAtOnce = true } };
    LiveSessionHold hold;

    auto result = std::optional<std::expected<LiveSessionRun, Answer>> {};
    auto threw = false;
    auto task = ComposeInto(composition.Parts(rig, terminalPool, false, 3), &hold, &result, &threw);
    rig.reactor.Submit(task.Native());
    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
    {
        rig.Settle();
        rig.clock.Advance(Interval);
    }
    rig.Settle();

    CHECK(composition.terminals.calls == 0);
    CHECK(composition.stops.calls == 1);
    CHECK(hold.stop != nullptr);
    CHECK(composition.views.asked == std::vector<RenderRung> { RenderRung::Piped });
    CHECK(rig.gatherer.Calls() == 3);
    CHECK((result.has_value() && result->has_value() && (*result)->exit.stop == DashboardStop::SampleBudget));

    FinishComposed(rig, hold);
}

TEST_CASE("a terminal that goes away ends an interactive session and no gather starts after it", "[cli][live][session]")
{
    // The control for the case above, and the runner's obligation from the source's review: the
    // loop ends on `Detached`, the source is closed for it, and the cadence starts nothing more
    // however long the clock runs.
    Rig rig;
    TestReactor terminalPool { rig.clock };
    Composition composition { rig, SixelTerminal, CompositionFaults { .detachAtOnce = true } };
    LiveSessionHold hold;

    auto result = std::optional<std::expected<LiveSessionRun, Answer>> {};
    auto threw = false;
    auto task = ComposeInto(composition.Parts(rig, terminalPool, true, 3), &hold, &result, &threw);
    rig.reactor.Submit(task.Native());
    rig.Settle();
    auto const gathersAtDetach = rig.gatherer.Calls();
    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
    {
        rig.clock.Advance(Interval);
        rig.Settle();
    }

    CHECK((result.has_value() && result->has_value() && (*result)->exit.stop == DashboardStop::SourceDetached));
    CHECK(gathersAtDetach <= 1);
    CHECK(rig.gatherer.Calls() == gathersAtDetach);
    // Closed by the runner, not by the test: the drain has landed with nobody else closing anything.
    CHECK(hold.drained.load());

    FinishComposed(rig, hold);
}

TEST_CASE("an interactive terminal that cannot be acquired refuses the session naming why", "[cli][live][session]")
{
    // Never a silent fall back to piped output: an interactive run whose shape changed under it
    // is the script-breaking change `CliFormat.hpp` rules out.
    Rig rig;
    TestReactor terminalPool { rig.clock };
    Composition composition { rig, std::unexpected(std::string { "stdin is not a terminal" }) };
    LiveSessionHold hold;

    auto result = std::optional<std::expected<LiveSessionRun, Answer>> {};
    auto threw = false;
    auto task = ComposeInto(composition.Parts(rig, terminalPool, true), &hold, &result, &threw);
    rig.reactor.Submit(task.Native());
    rig.Settle();

    CHECK((result.has_value() && !result->has_value() && result->error().outcome == Outcome::Refused));
    CHECK((result.has_value() && !result->has_value() && !result->error().advisories.empty()
           && result->error().advisories.front().contains("stdin is not a terminal")));
    CHECK(composition.stops.calls == 0);
    CHECK(composition.views.asked.empty());
    CHECK_FALSE(hold.source.has_value());
    CHECK(rig.gatherer.Calls() == 0);

    FinishComposed(rig, hold);
}

TEST_CASE("a session whose stop request cannot be installed runs anyway and says so", "[cli][live][session]")
{
    Rig rig;
    TestReactor terminalPool { rig.clock };
    Composition composition { rig, SixelTerminal, CompositionFaults { .stopRefusal = "no handler slot" } };
    LiveSessionHold hold;

    auto result = std::optional<std::expected<LiveSessionRun, Answer>> {};
    auto threw = false;
    auto task = ComposeInto(composition.Parts(rig, terminalPool, false, 1), &hold, &result, &threw);
    rig.reactor.Submit(task.Native());
    rig.Settle();

    CHECK((result.has_value() && result->has_value() && (*result)->exit.stop == DashboardStop::SampleBudget));
    CHECK((result.has_value() && result->has_value() && !(*result)->remarks.empty()
           && (*result)->remarks.front().contains("no handler slot")));
    CHECK(hold.stop == nullptr);

    FinishComposed(rig, hold);
}

TEST_CASE("a session that ends by an exception still closes its source", "[cli][live][session]")
{
    // `Drained()` completes only after a close. A loop that ended by throwing, with nobody
    // closing the source, would leave the cadence sampling and the drain waiting forever.
    Rig rig;
    TestReactor terminalPool { rig.clock };
    Composition composition { rig, SixelTerminal, CompositionFaults { .throwingViews = true } };
    LiveSessionHold hold;

    auto result = std::optional<std::expected<LiveSessionRun, Answer>> {};
    auto threw = false;
    auto task = ComposeInto(composition.Parts(rig, terminalPool, false), &hold, &result, &threw);
    rig.reactor.Submit(task.Native());
    rig.Settle();

    CHECK(threw);
    // Drained with the clock unmoved: the throw closed the source, and nobody else did.
    CHECK(hold.drained.load());
    CHECK(rig.gatherer.Calls() == 1);

    FinishComposed(rig, hold);
}
