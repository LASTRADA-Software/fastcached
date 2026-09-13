// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"
#include "LiveSourceRig.hpp"
#include "ScriptedDashboardEvents.hpp"
#include "TerminalCapabilities.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

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
    ScriptedAcquisition(IReactor& reactor, std::expected<TerminalCapabilities, std::string> answer, bool detachAtOnce):
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
    ScriptedInstaller(IReactor& reactor, std::string refusal):
        _reactor { reactor },
        _refusal { std::move(refusal) }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<IStopSignal>, std::string> Install() override
    {
        ++calls;
        if (!_refusal.empty())
            return std::unexpected(_refusal);
        return std::make_unique<ScriptedStopSignal>(_reactor);
    }

    int calls { 0 };

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
    explicit RecordingRungViews(bool throwing) noexcept:
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

/// Await a composed session into @p out, recording an exception rather than losing it.
/// @param parts What to compose from.
/// @param source Where the running source is kept.
/// @param out How it ended.
/// @param threw Set when it ended by an exception.
/// @return The task to submit.
[[nodiscard]] Task<void> ComposeInto(LiveSessionParts parts,
                                     std::optional<LiveEventSource>* source,
                                     std::optional<std::expected<LiveSessionRun, Answer>>* out,
                                     bool* threw)
{
    try
    {
        *out = co_await RunComposedSession(std::move(parts), source);
    }
    catch (std::runtime_error const&)
    {
        *threw = true;
    }
}

/// One composed session over a rig: its scripted collaborators, what it leaves behind, and
/// the task running it -- declared last, so it is destroyed first.
struct Composition
{
    /// @param rig The rig.
    /// @param terminal What an acquisition reports.
    /// @param faults How the collaborators misbehave.
    Composition(Rig& rig, std::expected<TerminalCapabilities, std::string> terminal, CompositionFaults faults = {}):
        rig { rig },
        terminals { rig.reactor, std::move(terminal), faults.detachAtOnce },
        stops { rig.reactor, std::move(faults.stopRefusal) },
        views { faults.throwingViews },
        terminalPool { rig.clock }
    {
    }

    Rig& rig;
    ScriptedAcquisition terminals;
    ScriptedInstaller stops;
    RecordingRungViews views;
    TestReactor terminalPool;
    std::optional<LiveEventSource> source;
    std::optional<std::expected<LiveSessionRun, Answer>> result;
    bool threw { false };
    Task<void> task {};

    /// Start the session on the rig's reactor.
    /// @param interactive Whether the standard streams are a terminal.
    /// @param samples The sample budget; 0 for none.
    void Start(bool interactive, std::size_t samples = 0)
    {
        auto parts = LiveSessionParts { .plan = LivePlan { .subject = LiveSubject::Cache,
                                                           .interval = Interval,
                                                           .samples = samples,
                                                           .endpoint = "10.0.0.4:6674" },
                                        .reactor = &rig.reactor,
                                        .gatherer = &rig.gatherer,
                                        .reader = &ReadStatsSample,
                                        .clock = &rig.clock,
                                        .samplePool = &rig.pool,
                                        .stopWaiter = &rig.stopWaiter,
                                        .terminalPool = &terminalPool,
                                        .sink = &rig.sink,
                                        .interactive = interactive,
                                        .terminals = &terminals,
                                        .stops = &stops,
                                        .views = &views };
        task = ComposeInto(std::move(parts), &source, &result, &threw);
        rig.reactor.Submit(task.Native());
    }

    /// Run the rig through @p rounds sample intervals.
    /// @param rounds How many intervals to let pass.
    void RunFor(int rounds)
    {
        rig.Settle();
        for ([[maybe_unused]] auto const round: std::views::iota(0, rounds))
        {
            rig.clock.Advance(Interval);
            rig.Settle();
        }
    }

    /// @return The run, when the session ran and ended.
    [[nodiscard]] LiveSessionRun const* Run() const
    {
        return result.has_value() && result->has_value() ? &**result : nullptr;
    }

    /// @return How the session stopped, or `Last` when it did not run or has not ended.
    [[nodiscard]] DashboardStop Stop() const
    {
        auto const* const run = Run();
        return run != nullptr ? run->exit.stop : DashboardStop::Last;
    }

    /// @return The refusal, when the session was refused.
    [[nodiscard]] Answer const* Refusal() const
    {
        return result.has_value() && !result->has_value() ? &result->error() : nullptr;
    }

    /// Close the source if there is one, run everything out, and check the drain landed and
    /// nothing is left parked.
    void Finish()
    {
        if (source.has_value())
            source->Close();
        rig.Settle();
        CHECK((!source.has_value() || source->IsDrained()));
        CHECK(rig.reactor.PendingTimers() == 0);
        CHECK(rig.pool.PendingSubmissions() == 0);
    }
};

/// The capabilities of a terminal that reported Sixel.
constexpr auto SixelTerminal =
    TerminalCapabilities { .sixel = SixelAnswer::Advertised, .encoding = TerminalTextEncoding::Utf8 };

/// A drain wait whose time passes only when the drain sleeps, and which can let the rig run
/// after a chosen number of sleeps -- so a source drains for real, mid-drain.
///
/// **No real time passes**: the drain's ceiling is measured on this clock, so a five-second
/// ceiling costs a loop of five hundred iterations and nothing else.
class SteppedDrainWait final: public IDrainWait
{
  public:
    /// @param settle The rig to settle after @p settleAfter sleeps; null to settle nothing.
    /// @param settleAfter How many sleeps pass before it is.
    explicit SteppedDrainWait(Rig* settle = nullptr, int settleAfter = 0) noexcept:
        _settle { settle },
        _settleAfter { settleAfter }
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
        if (_settle != nullptr && _sleeps == _settleAfter)
            _settle->Settle();
    }

    [[nodiscard]] int Sleeps() const noexcept
    {
        return _sleeps;
    }

  private:
    Rig* _settle;
    int _settleAfter;
    TimePoint _now {};
    int _sleeps { 0 };
};

/// The bound every drain case uses.
constexpr auto Bound = DrainBound { .ceiling = std::chrono::seconds { 5 }, .poll = std::chrono::milliseconds { 10 } };

} // namespace

TEST_CASE("a session decides how it draws once however many frames it draws", "[cli][live][session]")
{
    // §9.12. Counted over MANY frames, because once and once-per-frame agree on a session of
    // one: a view asked for again per frame would pass a single-frame run.
    Rig rig;
    Composition composition { rig, SixelTerminal };
    composition.Start(false, 5);
    composition.RunFor(5);

    CHECK(composition.Stop() == DashboardStop::SampleBudget);
    CHECK(rig.sink.frames >= 2);
    CHECK(composition.views.asked == std::vector<RenderRung> { RenderRung::Piped });

    composition.Finish();
}

TEST_CASE("an interactive session acquires its terminal on the terminal pool and draws on the rung it reported",
          "[cli][live][session]")
{
    Rig rig;
    Composition composition { rig, SixelTerminal };
    composition.Start(true);
    rig.Settle();

    CHECK(composition.terminals.calls == 1);
    // Its own pool, never the sample pool: a read that waits for a keystroke would starve sampling.
    CHECK(composition.terminals.askedPool == &composition.terminalPool);
    CHECK(composition.terminals.askedResumeOn == &rig.reactor);
    CHECK(composition.stops.calls == 0);
    CHECK(composition.views.asked == std::vector<RenderRung> { RenderRung::Sixel });
    CHECK(rig.gatherer.Calls() == 1);

    if (composition.terminals.spoken != nullptr)
        composition.terminals.spoken->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.Settle();
    CHECK(composition.Stop() == DashboardStop::Quit);
    CHECK(composition.terminals.release.released);

    composition.Finish();
}

TEST_CASE("a session with no terminal acquires none and takes its whole budget even when a terminal would detach",
          "[cli][live][session]")
{
    // §9 D5: a run that does not draw on a terminal never composes one. The acquisition here
    // would hand out a terminal that goes away before its first read, which ends a session that
    // listens to it -- so N samples prove it was never acquired, not just never counted.
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .detachAtOnce = true } };
    composition.Start(false, 3);
    composition.RunFor(3);

    CHECK(composition.terminals.calls == 0);
    CHECK(composition.stops.calls == 1);
    CHECK(composition.views.asked == std::vector<RenderRung> { RenderRung::Piped });
    CHECK(rig.gatherer.Calls() == 3);
    CHECK(composition.Stop() == DashboardStop::SampleBudget);

    composition.Finish();
}

TEST_CASE("a terminal that goes away ends an interactive session and no gather starts after it", "[cli][live][session]")
{
    // The control for the case above, and the runner's obligation from the source's review: the
    // loop ends on `Detached`, the source is closed for it, and the cadence starts nothing more
    // however long the clock runs. The view was still decided, once, before that first event.
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .detachAtOnce = true } };
    composition.Start(true, 3);
    rig.Settle();
    auto const gathersAtDetach = rig.gatherer.Calls();
    composition.RunFor(3);

    CHECK(composition.Stop() == DashboardStop::SourceDetached);
    CHECK(gathersAtDetach <= 1);
    CHECK(rig.gatherer.Calls() == gathersAtDetach);
    CHECK(composition.views.asked.size() == 1);
    // Closed by the runner, not by the test: the source has drained with nobody else closing anything.
    CHECK((composition.source.has_value() && composition.source->IsDrained()));

    composition.Finish();
}

TEST_CASE("an interactive terminal that cannot be acquired refuses the session naming why", "[cli][live][session]")
{
    // Never a silent fall back to piped output: an interactive run whose shape changed under it
    // is the script-breaking change `CliFormat.hpp` rules out.
    Rig rig;
    Composition composition { rig, std::unexpected(std::string { "stdin is not a terminal" }) };
    composition.Start(true);
    rig.Settle();

    auto const* const refusal = composition.Refusal();
    CHECK((refusal != nullptr && refusal->outcome == Outcome::Refused));
    CHECK((refusal != nullptr && !refusal->advisories.empty()
           && refusal->advisories.front().contains("stdin is not a terminal")));
    CHECK(composition.stops.calls == 0);
    CHECK(composition.views.asked.empty());
    CHECK_FALSE(composition.source.has_value());
    CHECK(rig.gatherer.Calls() == 0);

    composition.Finish();
}

TEST_CASE("a session whose stop request cannot be installed runs anyway and says so", "[cli][live][session]")
{
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .stopRefusal = "no handler slot" } };
    composition.Start(false, 1);
    rig.Settle();

    CHECK(composition.Stop() == DashboardStop::SampleBudget);
    auto const* const run = composition.Run();
    CHECK((run != nullptr && !run->remarks.empty() && run->remarks.front().contains("no handler slot")));

    composition.Finish();
}

TEST_CASE("a session that ends by an exception still closes its source", "[cli][live][session]")
{
    // The source drains only after a close. A loop that ended by throwing, with nobody closing
    // the source, would leave the cadence sampling and the drain waiting forever.
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .throwingViews = true } };
    composition.Start(false);
    rig.Settle();

    CHECK(composition.threw);
    // Drained with the clock unmoved: the throw closed the source, and nobody else did.
    CHECK((composition.source.has_value() && composition.source->IsDrained()));
    CHECK(rig.gatherer.Calls() == 1);

    composition.Finish();
}

TEST_CASE("a sample that never returns is abandoned at the ceiling naming the endpoint and its age", "[cli][live][session]")
{
    Rig rig;
    LiveEventSource source { rig.Parts() };
    // The first gather is out, started at the clock's epoch -- where the drain's clock starts too.
    rig.reactor.Drain();
    CHECK(rig.pool.PendingSubmissions() == 1);
    source.Close();

    auto wait = SteppedDrainWait {};
    auto const abandonment = DrainSession(source, "10.0.0.4:6674", Bound, wait);
    CHECK((abandonment.has_value() && abandonment->contains("10.0.0.4:6674")));
    CHECK((abandonment.has_value() && abandonment->contains("5000 ms")));

    rig.Settle();
    CloseAndDrain(rig, source);
}

TEST_CASE("a sample that returns inside the bound drains and abandons nothing", "[cli][live][session]")
{
    // The control for the case above: the same drain, and the sample returns on the third look
    // -- the rig really runs, and the source really drains. Nothing is abandoned, and the drain
    // stopped looking when it did.
    Rig rig;
    LiveEventSource source { rig.Parts() };
    rig.reactor.Drain();
    source.Close();

    auto wait = SteppedDrainWait { &rig, 3 };
    CHECK_FALSE(DrainSession(source, "10.0.0.4:6674", Bound, wait).has_value());
    CHECK(wait.Sleeps() == 3);
    CHECK(rig.gatherer.Calls() == 1);

    CloseAndDrain(rig, source);
}

TEST_CASE("abandoning a session with no sample out says so rather than inventing an age", "[cli][live][session]")
{
    auto const withSample = DescribeAbandonment("10.0.0.4:6674", std::chrono::milliseconds { 1234 });
    CHECK(withSample.contains("1234 ms"));

    auto const withoutSample = DescribeAbandonment("10.0.0.4:6674", std::nullopt);
    CHECK(withoutSample.contains("no sample"));
    CHECK_FALSE(withoutSample.contains(" ms"));
}
