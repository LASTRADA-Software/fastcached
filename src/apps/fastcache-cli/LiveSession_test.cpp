// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"
#include "LiveSourceRig.hpp"
#include "ScriptedCellWidth.hpp"
#include "ScriptedDashboardEvents.hpp"
#include "TerminalCapabilities.hpp"

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Platform/StopSignal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
    #include <csignal>
#endif

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;

namespace
{

/// Every remark a session made, in order.
class RecordingRemarks final: public IRemarkSink
{
  public:
    void Remark(std::string_view line) override
    {
        auto const lock = std::scoped_lock { _mutex };
        _lines.emplace_back(line);
    }

    /// @return The remarks so far.
    [[nodiscard]] std::vector<std::string> Lines() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _lines;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::string> _lines;
};

/// A restore handle that counts its calls, and notes them in a log when it is given one.
class LoggingRestore final: public ITerminalRestore
{
  public:
    /// @param log Where each call is noted, or null.
    explicit LoggingRestore(std::vector<std::string>* log = nullptr) noexcept:
        _log { log }
    {
    }

    void RestoreNow() noexcept override
    {
        ++_calls;
        if (_log != nullptr)
            _log->emplace_back("restore");
    }

    /// @return How many times the terminal was put back through this handle.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls.load();
    }

  private:
    std::vector<std::string>* _log;
    std::atomic<int> _calls { 0 };
};

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
    /// @param noPresenter Whether the started terminal names nowhere to draw frames.
    ScriptedAcquisition(IReactor& reactor,
                        std::expected<TerminalCapabilities, std::string> answer,
                        bool detachAtOnce,
                        bool noPresenter = false):
        _reactor { reactor },
        _answer { std::move(answer) },
        _detachAtOnce { detachAtOnce },
        _noPresenter { noPresenter }
    {
    }

    [[nodiscard]] Task<std::expected<StartedTerminal, std::string>> Acquire(IExecutor* pool, IExecutor* resumeOn) override
    {
        ++_calls;
        _askedPool = pool;
        _askedResumeOn = resumeOn;
        co_await ParkAwaiter { .reactor = _reactor };
        if (!_answer.has_value())
            co_return std::unexpected(_answer.error());
        auto terminal = std::make_unique<SpokenTerminal>(_reactor, &_release);
        _spoken = terminal.get();
        if (_detachAtOnce)
            terminal->GoAway();
        _presented.events = &_release;
        co_return StartedTerminal { .capabilities = *_answer,
                                    .events = std::move(terminal),
                                    .restore = _restore,
                                    .frames =
                                        _noPresenter ? nullptr : std::make_unique<PresenterRecord::Sink>(&_presented) };
    }

    /// @return The restore handle every acquisition hands out.
    [[nodiscard]] std::shared_ptr<LoggingRestore> const& RestoreHandle() const noexcept
    {
        return _restore;
    }

    /// @return What the presenter it handed out was given, and whether it outlived the events.
    [[nodiscard]] PresenterRecord const& Presented() const noexcept
    {
        return _presented;
    }

    /// @return How many acquisitions were asked for.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

    /// @return The pool the last acquisition was told to block on.
    [[nodiscard]] IExecutor* AskedPool() const noexcept
    {
        return _askedPool;
    }

    /// @return Where the last acquisition was told to resume.
    [[nodiscard]] IExecutor* AskedResumeOn() const noexcept
    {
        return _askedResumeOn;
    }

    /// @return The terminal the last acquisition handed out, while it lives; null before one.
    [[nodiscard]] SpokenTerminal* Spoken() const noexcept
    {
        return _spoken;
    }

    /// @return What became of the terminal handed out.
    [[nodiscard]] TerminalRelease const& Release() const noexcept
    {
        return _release;
    }

  private:
    IReactor& _reactor;
    std::expected<TerminalCapabilities, std::string> _answer;
    bool _detachAtOnce;
    bool _noPresenter;
    int _calls { 0 };
    IExecutor* _askedPool { nullptr };
    IExecutor* _askedResumeOn { nullptr };
    SpokenTerminal* _spoken { nullptr };
    TerminalRelease _release {};
    PresenterRecord _presented {};
    std::shared_ptr<LoggingRestore> _restore { std::make_shared<LoggingRestore>() };
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
        ++_calls;
        if (!_refusal.empty())
            return std::unexpected(_refusal);
        return std::make_unique<ScriptedStopSignal>(_reactor);
    }

    /// @return How many installs were asked for.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    IReactor& _reactor;
    std::string _refusal;
    int _calls { 0 };
};

/// A view whose frame is the sample count, and which records the geometry every frame was drawn at.
class GeometryView final: public IDashboardView
{
  public:
    /// @param geometry Where each frame's columns and rows go; outlives the view.
    explicit GeometryView(std::vector<std::pair<int, int>>* geometry) noexcept:
        _geometry { geometry }
    {
    }

    [[nodiscard]] std::string Frame(DashboardModel const& model) override
    {
        _geometry->emplace_back(model.columns, model.rows);
        return std::to_string(model.samples);
    }

  private:
    std::vector<std::pair<int, int>>* _geometry;
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
    /// @param pipedOnly Whether only the `Piped` rung has a view.
    /// @param none Whether no rung has one.
    explicit RecordingRungViews(bool throwing, bool pipedOnly = false, bool none = false) noexcept:
        _throwing { throwing },
        _pipedOnly { pipedOnly },
        _none { none }
    {
    }

    [[nodiscard]] std::unique_ptr<IDashboardView> For(RenderRung rung,
                                                      LivePlan const& /*plan*/,
                                                      std::string_view /*address*/) override
    {
        _asked.push_back(rung);
        if (_none || (_pipedOnly && rung != RenderRung::Piped))
            return nullptr;
        if (_throwing)
            return std::make_unique<ThrowingView>();
        return std::make_unique<GeometryView>(&_geometry);
    }

    /// @return The columns and rows every frame was drawn at, in order.
    [[nodiscard]] std::vector<std::pair<int, int>> const& Geometry() const noexcept
    {
        return _geometry;
    }

    /// @return The rungs asked for, in order.
    [[nodiscard]] std::vector<RenderRung> const& Asked() const noexcept
    {
        return _asked;
    }

  private:
    bool _throwing;
    bool _pipedOnly;
    bool _none;
    std::vector<RenderRung> _asked {};
    std::vector<std::pair<int, int>> _geometry {};
};

/// How a composition's collaborators misbehave or differ from a cache session's, when a case wants them to.
struct CompositionFaults
{
    bool detachAtOnce { false };                ///< An acquired terminal goes away before its first read.
    std::string stopRefusal {};                 ///< Why installing the stop request fails; empty when it does not.
    bool throwingViews { false };               ///< Every view throws on its first frame.
    bool pipedViewsOnly { false };              ///< Only the `Piped` rung has a view.
    bool noViews { false };                     ///< No rung has a view, the `Piped` one included.
    bool noPresenter { false };                 ///< An acquired terminal names nowhere to draw frames.
    IStatsGatherer* gatherer { nullptr };       ///< What samples ask instead of the rig's gatherer, when set.
    IStatsDialer* dialer { nullptr };           ///< What re-dials after a failed sample; none when null.
    LiveSubject subject { LiveSubject::Cache }; ///< What the admitted plan watches.
    IAdminDocument* admin { nullptr };          ///< What a document sample asks; none when null.
    SampleReader reader { &ReadStatsSample };   ///< What a sample says.
};

/// Await a composed session into @p out, recording an exception rather than losing it.
/// @param parts What to compose from.
/// @param source Where the running source is kept.
/// @param restore Where the acquired terminal's restore handle is kept.
/// @param out How it ended.
/// @param threw Set when it ended by an exception.
/// @return The task to submit.
[[nodiscard]] Task<void> ComposeInto(LiveSessionParts parts,
                                     std::optional<LiveEventSource>* source,
                                     std::shared_ptr<ITerminalRestore>* restore,
                                     std::optional<std::expected<LiveSessionRun, Answer>>* out,
                                     bool* threw)
{
    try
    {
        *out = co_await RunComposedSession(std::move(parts), source, restore);
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
        terminals { rig.reactor, std::move(terminal), faults.detachAtOnce, faults.noPresenter },
        stops { rig.reactor, std::move(faults.stopRefusal) },
        views { faults.throwingViews, faults.pipedViewsOnly, faults.noViews },
        terminalPool { rig.clock },
        gatherer { faults.gatherer != nullptr ? faults.gatherer : &rig.gatherer },
        dialer { faults.dialer },
        subject { faults.subject },
        admin { faults.admin },
        reader { faults.reader }
    {
    }

    Rig& rig;
    ScriptedAcquisition terminals;
    ScriptedInstaller stops;
    RecordingRungViews views;
    TestReactor terminalPool;
    IStatsGatherer* gatherer;
    IStatsDialer* dialer;
    LiveSubject subject;
    IAdminDocument* admin;
    SampleReader reader;
    RecordingRemarks remarks;
    std::optional<LiveEventSource> source;
    std::shared_ptr<ITerminalRestore> restore;
    std::optional<std::expected<LiveSessionRun, Answer>> result;
    bool threw { false };
    Task<void> task {};

    /// Start the session on the rig's reactor.
    /// @param interactive Whether the standard streams are a terminal.
    /// @param samples The sample budget; 0 for none.
    void Start(bool interactive, std::size_t samples = 0)
    {
        auto parts = LiveSessionParts {
            .plan = LivePlan { .subject = subject, .interval = Interval, .samples = samples, .endpoint = "10.0.0.4:6674" },
            .address = "10.0.0.4:6674",
            .reactor = &rig.reactor,
            .gatherer = gatherer,
            .admin = admin,
            .dialer = dialer,
            .reader = reader,
            .clock = &rig.clock,
            .samplePool = &rig.pool,
            .stopWaiter = &rig.stopWaiter,
            .terminalPool = &terminalPool,
            .sink = &rig.sink,
            .remarks = &remarks,
            .interactive = interactive,
            .terminals = &terminals,
            .stops = &stops,
            .views = &views
        };
        task = ComposeInto(std::move(parts), &source, &restore, &result, &threw);
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
constexpr auto SixelTerminal = TerminalCapabilities { .sixel = SixelAnswer::Advertised,
                                                      .encoding = TerminalTextEncoding::Utf8,
                                                      .cellPixels = CellPixelSize { .width = 10, .height = 20 } };

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
    CHECK(composition.views.Asked() == std::vector<RenderRung> { RenderRung::Piped });

    composition.Finish();
}

TEST_CASE("an interactive session acquires its terminal on the terminal pool and draws on the rung it reported",
          "[cli][live][session]")
{
    Rig rig;
    Composition composition { rig, SixelTerminal };
    composition.Start(true);
    rig.Settle();

    CHECK(composition.terminals.Calls() == 1);
    // Its own pool, never the sample pool: a read that waits for a keystroke would starve sampling.
    CHECK(composition.terminals.AskedPool() == &composition.terminalPool);
    CHECK(composition.terminals.AskedResumeOn() == &rig.reactor);
    CHECK(composition.stops.Calls() == 0);
    CHECK(composition.views.Asked() == std::vector<RenderRung> { RenderRung::Sixel });
    CHECK(rig.gatherer.Calls() == 1);
    // The restore handle is kept apart from the events, where an abandonment can reach it, and
    // nothing in a session that is still running calls it.
    CHECK(composition.restore == composition.terminals.RestoreHandle());
    CHECK(composition.terminals.RestoreHandle()->Calls() == 0);

    if (composition.terminals.Spoken() != nullptr)
        composition.terminals.Spoken()->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.Settle();
    CHECK(composition.Stop() == DashboardStop::Quit);
    CHECK(composition.terminals.Release().released);

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

    CHECK(composition.terminals.Calls() == 0);
    CHECK(composition.stops.Calls() == 1);
    CHECK(composition.views.Asked() == std::vector<RenderRung> { RenderRung::Piped });
    CHECK(rig.gatherer.Calls() == 3);
    CHECK(composition.restore == nullptr);
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
    CHECK(composition.views.Asked().size() == 1);
    // Closed by the runner, not by the test: the source has drained with nobody else closing anything.
    CHECK((composition.source.has_value() && composition.source->IsDrained()));

    composition.Finish();
}

TEST_CASE("an interactive session whose rung has no view refuses by name and releases the terminal", "[cli][live][session]")
{
    // Never drawn through the piped view instead: a terminal session in record lines is the shape
    // change §1.6 rules out. The terminal it acquired is closed and released before the refusal,
    // and its presenter before that.
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .pipedViewsOnly = true } };
    composition.Start(true);
    rig.Settle();

    auto const* const refusal = composition.Refusal();
    CHECK((refusal != nullptr && refusal->outcome == Outcome::Local));
    CHECK((refusal != nullptr && AdvisoryText(*refusal).contains("live-stats cache")));
    CHECK((refusal != nullptr && AdvisoryText(*refusal).contains("no panel")));
    CHECK(composition.views.Asked() == std::vector<RenderRung> { RenderRung::Sixel });
    CHECK(composition.terminals.Release().released);
    CHECK(composition.terminals.Release().closedFirst);
    CHECK(composition.terminals.Presented().released);
    CHECK_FALSE(composition.terminals.Presented().afterEvents);
    CHECK(composition.terminals.Presented().frames == 0);
    CHECK_FALSE(composition.source.has_value());
    CHECK(rig.gatherer.Calls() == 0);

    composition.Finish();
}

TEST_CASE("an interactive session for a subject with no panel is refused before the terminal is acquired",
          "[cli][live][session]")
{
    Rig rig;
    ScriptedDocument admin { std::string { "# kpi" } };
    Composition composition {
        rig, SixelTerminal, CompositionFaults { .subject = LiveSubject::Fleet, .admin = &admin, .reader = &ReadFleetSample }
    };
    composition.Start(true);
    rig.Settle();

    auto const* const refusal = composition.Refusal();
    CHECK((refusal != nullptr && refusal->outcome == Outcome::Local));
    CHECK((refusal != nullptr && AdvisoryText(*refusal).contains("live-stats fleet")));
    // Nothing flashed: no raw mode, no queries, no alternate screen for a refusal.
    CHECK(composition.terminals.Calls() == 0);
    CHECK(composition.views.Asked().empty());
    CHECK(admin.Asked().empty());

    composition.Finish();
}

TEST_CASE("an interactive terminal started with nowhere to draw frames is refused and released, never drawn to the pipe",
          "[cli][live][session]")
{
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .noPresenter = true } };
    composition.Start(true);
    rig.Settle();

    auto const* const refusal = composition.Refusal();
    CHECK((refusal != nullptr && refusal->outcome == Outcome::Local));
    CHECK((refusal != nullptr && AdvisoryText(*refusal).contains("nowhere to draw")));
    CHECK(composition.terminals.Release().released);
    CHECK(composition.terminals.Release().closedFirst);
    CHECK(rig.sink.frames == 0);
    CHECK_FALSE(composition.source.has_value());

    composition.Finish();
}

TEST_CASE("a composition whose loop throws still left the terminal's restore handle where an abandonment finds it",
          "[cli][live][session]")
{
    // The reason the handle has its own slot: a loop that threw returns no run to carry it in, and the
    // abandonment that follows a stuck drain still has to put the terminal back.
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .throwingViews = true } };
    composition.Start(true);
    rig.Settle();

    CHECK(composition.threw);
    CHECK_FALSE(composition.result.has_value());
    CHECK(composition.restore == composition.terminals.RestoreHandle());

    composition.Finish();
}

TEST_CASE("a piped session whose subject has no record to stream refuses by name", "[cli][live][session]")
{
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .noViews = true } };
    composition.Start(false);
    rig.Settle();

    auto const* const refusal = composition.Refusal();
    CHECK((refusal != nullptr && refusal->outcome == Outcome::Local));
    CHECK((refusal != nullptr && AdvisoryText(*refusal).contains("no record to stream")));
    // Not the terminal's wording: there is no terminal, and redirecting the output is no remedy.
    CHECK((refusal != nullptr && !AdvisoryText(*refusal).contains("terminal")));
    CHECK_FALSE(composition.source.has_value());
    CHECK(rig.gatherer.Calls() == 0);

    composition.Finish();
}

namespace
{

/// The remarks a session makes over @p script, read by the stats ladder's reader.
/// @param script The events, in order.
/// @return The remarks.
[[nodiscard]] std::vector<std::string> RemarksOver(std::vector<DashboardEvent> script)
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto events = ScriptedDashboardEvents { reactor, std::move(script) };
    auto remarks = RecordingRemarks {};
    auto remarking = FailureRemarks { &events, &ReadStatsSample, &remarks };
    auto view = CountView {};
    auto sink = CountSink {};
    auto exit = std::optional<DashboardExit> {};
    auto task = RunOver(&remarking, &view, &sink, DashboardLimits {}, &exit);
    reactor.Submit(task.Native());
    reactor.Drain();
    REQUIRE(exit.has_value());
    return remarks.Lines();
}

/// A sample the source could not take, for @p reason.
/// @param reason Why.
/// @return The event.
[[nodiscard]] DashboardEvent FailedFor(std::string reason)
{
    return DashboardEvent { .kind = DashboardEventKind::SampleFailed,
                            .outcome = Outcome::Unreachable,
                            .note = std::move(reason) };
}

/// A sample that arrived carrying @p attempts.
/// @param attempts What each source said.
/// @return The event.
[[nodiscard]] DashboardEvent SampleOf(std::vector<StatsAttempt> attempts)
{
    return DashboardEvent { .kind = DashboardEventKind::Sample, .attempts = std::move(attempts) };
}

} // namespace

TEST_CASE("a remark is one line whatever its reason carried", "[cli][live][session][remark]")
{
    // A follower's 503 names its leader across several comment lines of its body.
    auto const remarks =
        RemarksOver({ FailedFor("/fleet.txt answered HTTP 503: # not the leader\n# leader: 10.0.0.9:7071\n") });
    REQUIRE(remarks.size() == 1);
    CHECK_FALSE(remarks[0].contains('\n'));
    CHECK(remarks[0].contains("# not the leader; # leader: 10.0.0.9:7071"));
    CHECK_FALSE(remarks[0].ends_with("; "));
}

TEST_CASE("a failing sample is remarked on once per reason, and again after a recovery", "[cli][live][session][remark]")
{
    // WHAT DISTINGUISHES: the repeat of a reason stays quiet (once per sample would say it twice),
    // a changed reason speaks (once per run would not), and a recovery re-arms it (a reason
    // remembered across a reading would not tell the second outage).
    auto const unread = SampleOf(NothingAnswered());
    auto const readerNote = ReadStatsSample(unread).note;
    REQUIRE_FALSE(readerNote.empty());

    auto const remarks = RemarksOver({ SampleOf(Reading()),
                                       FailedFor("the daemon is down"),
                                       FailedFor("the daemon is down"),
                                       FailedFor("the daemon refused"),
                                       SampleOf(Reading()),
                                       FailedFor("the daemon refused"),
                                       unread,
                                       unread });

    REQUIRE(remarks.size() == 4);
    CHECK(remarks[0].contains("the daemon is down"));
    CHECK(remarks[1].contains("the daemon refused"));
    CHECK(remarks[2].contains("the daemon refused"));
    // A sample the READER could not read says the reader's own account.
    CHECK(remarks[3].contains(readerNote));
}

TEST_CASE("a run whose samples all read is never remarked on", "[cli][live][session][remark]")
{
    CHECK(RemarksOver({ SampleOf(Reading()), SampleOf(Reading()), SampleOf(Reading()) }).empty());
}

TEST_CASE("a piped session remarks on a failing sample as it happens, and an interactive one at the end",
          "[cli][live][session][remark]")
{
    {
        // Piped: stderr is free, so the operator reading an absent row learns why at once.
        Rig rig;
        DyingGatherer dying;
        Composition composition { rig, SixelTerminal, CompositionFaults { .gatherer = &dying } };
        composition.Start(false, 3);
        composition.RunFor(3);

        CHECK(composition.Stop() == DashboardStop::SampleBudget);
        CHECK(composition.remarks.Lines().size() == 1);
        auto const* const run = composition.Run();
        REQUIRE(run != nullptr);
        CHECK(std::ranges::none_of(run->remarks, [](std::string const& line) { return line.contains("read nothing"); }));
        composition.Finish();
    }
    {
        // Interactive: stderr is the screen being drawn on, so the remark waits for the terminal to be back.
        Rig rig;
        DyingGatherer dying;
        Composition composition { rig, SixelTerminal, CompositionFaults { .gatherer = &dying } };
        composition.Start(true, 3);
        composition.RunFor(3);

        CHECK(composition.Stop() == DashboardStop::SampleBudget);
        CHECK(composition.remarks.Lines().empty());
        auto const* const run = composition.Run();
        REQUIRE(run != nullptr);
        CHECK(std::ranges::count_if(run->remarks, [](std::string const& line) { return line.contains("read nothing"); })
              == 1);
        composition.Finish();
    }
}

TEST_CASE("an interactive session draws every frame through the terminal's presenter and none to the pipe",
          "[cli][live][session]")
{
    // The alternate screen is the presenter's: a frame written to stdout as well would scroll the
    // operator's own screen underneath it, and a frame written only there would never be seen.
    Rig rig;
    Composition composition { rig, SixelTerminal };
    composition.Start(true);
    composition.RunFor(2);

    CHECK(composition.terminals.Presented().frames >= 2);
    CHECK(rig.sink.frames == 0);

    if (composition.terminals.Spoken() != nullptr)
        composition.terminals.Spoken()->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.Settle();
    CHECK(composition.Stop() == DashboardStop::Quit);
    composition.Finish();
    // Released, and before the events it presented over.
    CHECK(composition.terminals.Presented().released);
    CHECK_FALSE(composition.terminals.Presented().afterEvents);
    CHECK(composition.terminals.Release().released);
}

TEST_CASE("an interactive session's presenter is released before the terminal's events when the terminal goes away",
          "[cli][live][session]")
{
    Rig rig;
    Composition composition { rig, SixelTerminal, CompositionFaults { .detachAtOnce = true } };
    composition.Start(true);
    rig.Settle();

    CHECK(composition.Stop() == DashboardStop::SourceDetached);
    composition.Finish();
    CHECK(composition.terminals.Presented().released);
    CHECK_FALSE(composition.terminals.Presented().afterEvents);
    CHECK(composition.terminals.Release().released);
}

TEST_CASE("a resize reaches the next frame an interactive session draws", "[cli][live][session]")
{
    Rig rig;
    Composition composition { rig, SixelTerminal };
    composition.Start(true);
    rig.Settle();
    REQUIRE(composition.terminals.Spoken() != nullptr);

    composition.terminals.Spoken()->Say(DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 132, .rows = 43 });
    rig.Settle();

    REQUIRE_FALSE(composition.views.Geometry().empty());
    CHECK(composition.views.Geometry().front() != std::pair { 132, 43 });
    CHECK(composition.views.Geometry().back() == std::pair { 132, 43 });

    composition.terminals.Spoken()->Say(DashboardEvent { .kind = DashboardEventKind::Key, .keys = "q" });
    rig.Settle();
    composition.Finish();
}

TEST_CASE("the standard views draw the piped rung, and each subject's own panel on an interactive one",
          "[cli][live][session]")
{
    auto views = StandardRungViews { RenderOptions { .format = OutputFormat::Human }, &FakeCellWidth };
    auto const cache = LivePlan { .subject = LiveSubject::Cache,
                                  .interval = std::chrono::milliseconds { 2000 },
                                  .samples = 0,
                                  .endpoint = "a cache daemon" };
    auto node = cache;
    node.subject = LiveSubject::Node;
    auto fleet = cache;
    fleet.subject = LiveSubject::Fleet;

    CHECK(views.For(RenderRung::Piped, cache, "10.0.0.4:6674") != nullptr);
    CHECK(views.For(RenderRung::Piped, node, "10.0.0.4:6674") != nullptr);
    CHECK(views.For(RenderRung::Piped, fleet, "10.0.0.4:6674") != nullptr);

    // Which panel, told apart by its title, and the frame names where and how often it samples.
    auto const model = DashboardModel {};
    for (auto const rung: { RenderRung::Sixel, RenderRung::Unicode, RenderRung::Ascii })
    {
        auto const cacheView = views.For(rung, cache, "10.0.0.4:6674");
        auto const nodeView = views.For(rung, node, "10.0.0.5:6674");
        REQUIRE(cacheView != nullptr);
        REQUIRE(nodeView != nullptr);
        auto const cacheFrame = cacheView->Frame(model);
        auto const nodeFrame = nodeView->Frame(model);
        CHECK(cacheFrame.contains(CachePanel().title));
        CHECK_FALSE(cacheFrame.contains(NodePanel().title));
        CHECK(nodeFrame.contains(NodePanel().title));
        CHECK(cacheFrame.contains("10.0.0.4:6674"));
        CHECK(nodeFrame.contains("10.0.0.5:6674"));
        CHECK(cacheFrame.contains("every 2s"));

        // No panel for the fleet yet: refused by name, never drawn as records.
        CHECK(views.For(rung, fleet, "10.0.0.4:6674") == nullptr);
    }
}

TEST_CASE("a composed session re-dials after a failed sample and reaches its budget", "[cli][live][session][redial]")
{
    // The composition hands its dialer to the source: the connection dies after one reading, the
    // gap spends a sample of the budget, and the third sample reads only because it was taken
    // over a re-dial.
    Rig rig;
    DyingGatherer dying;
    ScriptedDialer dialer { { true } };
    Composition composition { rig, SixelTerminal, CompositionFaults { .gatherer = &dying, .dialer = &dialer } };
    composition.Start(false, 3);
    composition.RunFor(3);

    CHECK(composition.Stop() == DashboardStop::SampleBudget);
    CHECK(dialer.Dials() == 1);
    auto const* const run = composition.Run();
    REQUIRE(run != nullptr);
    // Two readings of three samples: the one before the connection died and the one over the re-dial.
    CHECK(run->exit.model.samples == 2);

    composition.Finish();
}

namespace
{

/// A reader for a document session that reads only whether a document arrived.
///
/// Stands in for the fleet reader this build does not have yet: the composition is the subject,
/// and it cannot tell one reader from another.
/// @param event The `Sample`.
/// @return A reading when the fetch produced a document; otherwise the failure.
[[nodiscard]] SampleReading ReadAnyDocument(DashboardEvent const& event)
{
    if (!event.document.has_value() || !event.document->has_value())
        return SampleReading { .outcome = Outcome::Unreachable, .value = {}, .source = {} };
    return SampleReading { .outcome = Outcome::Affirmative, .value = {}, .source = "fleet.txt" };
}

} // namespace

TEST_CASE("a composed fleet session samples its subject's document and never the stats gatherer",
          "[cli][live][session][document]")
{
    // The composition, not the source, decides which door a session's samples go through: it
    // reads the subject's column. Composed as `cache` this same case gathers every sample.
    Rig rig;
    ScriptedDocument admin { std::string { "# kpi" } };
    Composition composition {
        rig, SixelTerminal, CompositionFaults { .subject = LiveSubject::Fleet, .admin = &admin, .reader = &ReadAnyDocument }
    };
    composition.Start(false, 2);
    composition.RunFor(3);

    CHECK(composition.Stop() == DashboardStop::SampleBudget);
    auto const path = std::string { LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Fleet)].document };
    CHECK(admin.Asked() == std::vector<std::string>(2, path));
    CHECK(rig.gatherer.Calls() == 0);

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
    CHECK((refusal != nullptr && refusal->outcome == Outcome::Local));
    CHECK((refusal != nullptr && !refusal->advisories.empty()
           && refusal->advisories.front().contains("stdin is not a terminal")));
    // The remedy is on this machine, which is what `Local` says to a script and this says to a person.
    CHECK((refusal != nullptr && !refusal->advisories.empty() && refusal->advisories.front().contains("output redirected")));
    CHECK(composition.stops.Calls() == 0);
    CHECK(composition.views.Asked().empty());
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

namespace
{

/// An installer whose signal fires when a case says: Ctrl-C pressed at a chosen moment.
class StopOnDemandInstaller final: public IStopSignalInstaller
{
  public:
    /// @param reactor Where the signal's wait parks.
    explicit StopOnDemandInstaller(IReactor& reactor):
        _reactor { reactor }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<IStopSignal>, std::string> Install() override
    {
        ++_calls;
        auto signal = std::make_unique<ScriptedStopSignal>(_reactor);
        _installed = signal.get();
        return signal;
    }

    /// Press Ctrl-C. Only while the session still waits on the signal, which outlives any sample
    /// that is out: the source releases it only once its wait has returned.
    void Press()
    {
        auto* const installed = _installed.load();
        REQUIRE(installed != nullptr);
        installed->Fire();
    }

    /// @return How many installs were asked for.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls.load();
    }

  private:
    IReactor& _reactor;
    std::atomic<ScriptedStopSignal*> _installed { nullptr };
    std::atomic<int> _calls { 0 };
};

/// A gatherer whose every gather presses Ctrl-C and then waits for the case to open a gate: a
/// sample that is out when the operator stops, for as long as the case says.
///
/// Pressed FROM the gather so the order is fixed rather than raced: the stop cannot arrive
/// before the sample is out.
class GatedGatherer final: public IStatsGatherer
{
  public:
    /// @param stop What the gather presses, or null for a gather that only waits.
    explicit GatedGatherer(StopOnDemandInstaller* stop) noexcept:
        _stop { stop }
    {
    }

    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        ++_calls;
        if (_stop != nullptr)
            _stop->Press();
        _gate.acquire();
        return Reading();
    }

    /// Let every gather that is waiting, or will wait, return. Once: a semaphore released past
    /// its bound is undefined, and MSVC's ends the process for it.
    void Open()
    {
        if (!_opened.exchange(true))
            _gate.release(GateWidth);
    }

    /// @return How many gathers started.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls.load();
    }

  private:
    static constexpr auto GateWidth = 64;
    std::atomic<int> _calls { 0 };
    StopOnDemandInstaller* _stop;
    std::counting_semaphore<GateWidth> _gate { 0 };
    std::atomic<bool> _opened { false };
};

/// A connection that dies after one answer and is never re-dialled past a third ask: the third
/// ask presses Ctrl-C, so a session that failed to re-dial ends rather than waiting forever for
/// a budget only a re-dial could meet.
class BoundedDyingGatherer final: public IStatsGatherer
{
  public:
    /// @param stop What the third ask presses.
    explicit BoundedDyingGatherer(StopOnDemandInstaller* stop) noexcept:
        _stop { stop }
    {
    }

    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        auto const call = ++_calls;
        if (call == 3)
            _stop->Press();
        return call == 1 ? Reading() : NothingAnswered();
    }

  private:
    StopOnDemandInstaller* _stop;
    int _calls { 0 };
};

/// Frames as one stream, written on the reactor's thread and read on the case's.
class StreamingSink final: public IFrameSink
{
  public:
    void Present(std::string_view frame) override
    {
        auto const lock = std::scoped_lock { _mutex };
        _stream += frame;
    }

    /// @return Everything presented so far.
    [[nodiscard]] std::string Stream() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _stream;
    }

  private:
    mutable std::mutex _mutex;
    std::string _stream;
};

/// Close @p source on its reactor's thread, which is the only thread `Close()` may run on.
/// @param reactor Where to close it.
/// @param source The source, when one was composed.
/// @return The detached task.
DetachedTask CloseOnReactor(IReactor* reactor, std::optional<LiveEventSource>* source)
{
    co_await ResumeOn { *reactor };
    if (source->has_value())
        (*source)->Close();
}

/// How long a seat case waits for a session before closing it from outside.
///
/// Every case here ends its session by budget, refusal or Ctrl-C within a couple of seconds.
/// One that does not -- a mutant that never reads, a budget nothing can meet -- would otherwise
/// block the case's thread for the life of the run; closed, it fails on its assertions instead.
inline constexpr auto SeatSessionBound = std::chrono::seconds { 20 };

/// What `main` acquires, for real: a platform reactor on a thread of its own, three one-thread
/// pools and a steady clock -- so the session's blocking wait and its drain run on the case's
/// thread exactly as they run on `main`'s.
///
/// **Torn down in the order `main` owes**: open the gate so a stuck sample can return, wait for
/// the source to drain, stop the reactor and join it, and only then destroy the source. A
/// destructor rather than a closing statement, so a failed `REQUIRE` unwinds through it instead
/// of leaving a reactor thread nobody stops.
struct RunningSeat
{
    /// @param render The `--format` asked for.
    /// @param streamsInteractive Whether the standard streams are a terminal.
    /// @param stoppedFromGather Whether the stop request is the one the gated gather presses.
    RunningSeat(RenderOptions render, bool streamsInteractive, bool stoppedFromGather = false):
        render { std::move(render) },
        streamsInteractive { streamsInteractive },
        stoppedFromGather { stoppedFromGather }
    {
        thread = std::jthread { [this] { reactor.Run(); } };
    }

    RunningSeat(RunningSeat const&) = delete;
    RunningSeat(RunningSeat&&) = delete;
    RunningSeat& operator=(RunningSeat const&) = delete;
    RunningSeat& operator=(RunningSeat&&) = delete;

    ~RunningSeat()
    {
        gated.Open();
        stuck.Open();
        if (source.has_value())
        {
            auto wait = ThreadDrainWait {};
            CHECK_FALSE(DrainSession(*source, "teardown", DrainBound {}, wait).has_value());
        }
        reactor.Stop();
        thread.join();
        source.reset();
    }

    /// The seat `main` would hand over.
    /// @param bound How long the drain may take.
    /// @return The seat.
    [[nodiscard]] LiveSessionSeat Seat(DrainBound bound = {})
    {
        auto* installer = static_cast<IStopSignalInstaller*>(&stops);
        if (stopsOverride != nullptr)
            installer = stopsOverride;
        else if (stoppedFromGather)
            installer = &onDemand;
        return LiveSessionSeat { .reactor = &reactor,
                                 .address = "10.0.0.4:6674",
                                 .clock = &clock,
                                 .dialer = dialer,
                                 .samplePool = &samplePool,
                                 .stopWaiter = &stopWaiter,
                                 .terminalPool = &terminalPool,
                                 .sink = &sink,
                                 .remarks = &remarks,
                                 .streamsInteractive = streamsInteractive,
                                 .render = render,
                                 .terminals = terminalsOverride != nullptr ? terminalsOverride : &terminals,
                                 .stops = installer,
                                 .views = &views,
                                 .drainWait = &drainWait,
                                 .drainBound = bound,
                                 .source = &source };
    }

    /// @return What the pipe received.
    [[nodiscard]] std::string Stream() const
    {
        return sink.Stream();
    }

    /// Run a session on this seat, closing it from outside if it outlives `SeatSessionBound`.
    ///
    /// **A session that never ends must fail rather than hang**: the call blocks this thread
    /// until the session drains, so a watchdog closes the source on the reactor at the bound, and
    /// the case then fails on whatever it asserted -- and on the watchdog having fired at all.
    /// @param context The invocation.
    /// @param bound How long the drain may take.
    /// @return How the session ended.
    [[nodiscard]] SessionEnding Run(VerbContext const& context, DrainBound bound = {})
    {
        auto fired = std::atomic<bool> { false };
        auto watchdog = std::jthread { [this, &fired](std::stop_token const& stop) {
            auto mutex = std::mutex {};
            auto wake = std::condition_variable_any {};
            auto lock = std::unique_lock { mutex };
            (void) wake.wait_for(lock, stop, SeatSessionBound, [] { return false; });
            if (stop.stop_requested())
                return;
            fired = true;
            CloseOnReactor(&reactor, &source);
        } };
        auto ending = RunLiveStatsSession(context, Seat(bound));
        watchdog.request_stop();
        watchdog.join();
        CHECK_FALSE(fired.load());
        return ending;
    }

    RenderOptions render;
    bool streamsInteractive;
    bool stoppedFromGather;
    IStatsDialer* dialer { nullptr };
    IStopSignalInstaller* stopsOverride { nullptr };
    ITerminalAcquisition* terminalsOverride { nullptr };
    SteadyClock clock;
    PlatformReactor reactor { clock };
    ThreadPoolExecutor samplePool { 1 };
    ThreadPoolExecutor stopWaiter { 1 };
    ThreadPoolExecutor terminalPool { 1 };
    StreamingSink sink;
    RecordingRemarks remarks;
    ScriptedInstaller stops { reactor, "" };
    StopOnDemandInstaller onDemand { reactor };
    ScriptedAcquisition terminals { reactor, std::unexpected(std::string { "stdin is not a terminal" }), false };
    StandardRungViews views { render, &FakeCellWidth };
    ThreadDrainWait drainWait;
    ScriptedGatherer gatherer { Reading() };
    GatedGatherer gated { &onDemand };
    GatedGatherer stuck { nullptr };
    ScriptedAcquisition acquiring { reactor, SixelTerminal, true };
    std::optional<LiveEventSource> source;
    std::jthread thread;
};

/// The invocation a session verb is handed, at the cache subject's floor interval.
/// @param operands The operands.
/// @param samples `--samples`; 0 for none given.
/// @param identity What the endpoint is.
/// @param stats What each sample asks.
/// @return The context.
[[nodiscard]] VerbContext SessionContext(std::span<std::string const> operands,
                                         std::size_t samples,
                                         IEndpointIdentity* identity,
                                         IStatsGatherer* stats)
{
    return VerbContext {
        .operands = operands,
        .options = VerbOptions { .interval = LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Cache)].minInterval,
                                 .samples = samples == 0 ? std::nullopt : std::optional<std::size_t> { samples } },
        .stats = stats,
        .identity = identity
    };
}

/// An admin surface that counts what it is asked and answers nothing.
class CountingAdmin final: public IAdminDocument
{
  public:
    [[nodiscard]] std::expected<std::string, AdminError> FetchAdmin(std::string_view /*path*/) override
    {
        ++_fetches;
        return std::unexpected(AdminError { .kind = AdminFailure::Unreachable, .detail = "not scripted" });
    }

    /// @return How many documents were asked for.
    [[nodiscard]] int Fetches() const noexcept
    {
        return _fetches;
    }

  private:
    int _fetches { 0 };
};

/// A cache daemon, as the identification describes one.
/// @return The identification.
[[nodiscard]] EndpointIdentity CacheDaemon()
{
    return EndpointIdentity { .kind = RemoteKind::FastcacheWireOnly,
                              .detail = "10.0.0.4:6674 speaks 0xFC and serves no node verbs, so it is not a compile node",
                              .unreadable = false };
}

} // namespace

TEST_CASE("a live-stats session refused at admission composes nothing", "[cli][live][session][seat]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { EndpointIdentity {
        .kind = std::nullopt, .detail = "no 0xFC connection was opened", .unreadable = false } };

    auto const ending = seat.Run(SessionContext({}, 1, &identity, &seat.gatherer));

    CHECK(ending.kind == SessionEndKind::Refused);
    CHECK(ending.answer.outcome == Outcome::Unreachable);
    CHECK(seat.stops.Calls() == 0);
    CHECK(seat.gatherer.Calls() == 0);
    CHECK_FALSE(seat.source.has_value());
}

TEST_CASE("a piped live-stats session runs to its budget, drains, and exits with what it read", "[cli][live][session][seat]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { CacheDaemon() };

    auto const ending = seat.Run(SessionContext({}, 1, &identity, &seat.gatherer));

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(ending.answer.outcome == Outcome::Affirmative);
    CHECK(seat.stops.Calls() == 1);
    CHECK(seat.terminals.Calls() == 0);
    // The header and the one row the budget allowed, in the format asked for.
    auto const stream = seat.Stream();
    CHECK(std::ranges::count(stream, '\n') == 2);
    // The cache panel's figures, named as the panel labels them.
    CHECK(stream.starts_with("source\thit_rate\t"));
    CHECK(stream.contains("\tbytes_limit\n"));
    CHECK((seat.source.has_value() && seat.source->IsDrained()));
}

TEST_CASE("live-stats --format=json at a terminal streams its records rather than drawing", "[cli][live][session][seat]")
{
    // §1.6: the format does not change because the streams are a terminal. A json run asks for
    // no terminal, installs the stop request a piped run ends on, and writes one document.
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Json }, true };
    auto identity = ScriptedIdentity { CacheDaemon() };

    auto const ending = seat.Run(SessionContext({}, 1, &identity, &seat.gatherer));

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(seat.terminals.Calls() == 0);
    CHECK(seat.stops.Calls() == 1);
    CHECK(seat.Stream().starts_with('{'));
}

TEST_CASE("a human live-stats session at a terminal asks for the terminal", "[cli][live][session][seat]")
{
    // The control for the case above: the same streams in the human format, and the terminal IS
    // asked -- here it cannot be acquired, which refuses the session with the outcome that puts
    // the remedy on this machine.
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Human }, true };
    auto identity = ScriptedIdentity { CacheDaemon() };

    auto const ending = seat.Run(SessionContext({}, 1, &identity, &seat.gatherer));

    CHECK(ending.kind == SessionEndKind::Refused);
    CHECK(ending.answer.outcome == Outcome::Local);
    CHECK(seat.terminals.Calls() == 1);
    CHECK(seat.stops.Calls() == 0);
    CHECK(seat.Stream().empty());
}

TEST_CASE("a live-stats session that ends with a sample stuck is abandoned with the outcome it earned",
          "[cli][live][session][seat]")
{
    // Ctrl-C lands while the first gather is out, and the gather does not return within the
    // bound. The session neither waits forever nor unwinds: it hands `main` the line naming the
    // endpoint and how long the sample had been out, and the outcome of a run that read nothing.
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false, true };
    auto identity = ScriptedIdentity { CacheDaemon() };
    auto const bound = DrainBound { .ceiling = std::chrono::milliseconds { 50 }, .poll = std::chrono::milliseconds { 5 } };

    auto const ending = seat.Run(SessionContext({}, 0, &identity, &seat.gated), bound);

    CHECK(ending.kind == SessionEndKind::Abandoned);
    CHECK(ending.answer.outcome == Outcome::Unreachable);
    CHECK(ending.line.contains("10.0.0.4:6674"));
    CHECK(ending.line.contains(" ms"));
    CHECK(seat.gated.Calls() == 1);
    CHECK((seat.source.has_value() && !seat.source->IsDrained()));
    // A piped session changed no terminal mode, so the ending carries nothing to put back.
    CHECK(ending.restore == nullptr);
}

TEST_CASE("an interactive live-stats session abandoned with a sample stuck hands main the terminal's restore handle",
          "[cli][live][session][seat]")
{
    // The terminal goes away while the first gather is out, and the gather never returns within
    // the bound. The events are not destroyed on this ending -- the process ends without unwinding
    // -- so the restore handle is the only thing that can leave raw mode and the alternate screen,
    // and the session hands it over uncalled: calling it is `main`'s, first, before any output.
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Human }, true };
    seat.terminalsOverride = &seat.acquiring;
    auto identity = ScriptedIdentity { CacheDaemon() };
    auto const bound = DrainBound { .ceiling = std::chrono::milliseconds { 50 }, .poll = std::chrono::milliseconds { 5 } };

    auto const ending = seat.Run(SessionContext({}, 0, &identity, &seat.stuck), bound);

    CHECK(ending.kind == SessionEndKind::Abandoned);
    CHECK(seat.acquiring.Calls() == 1);
    CHECK(seat.stuck.Calls() == 1);
    REQUIRE(ending.restore != nullptr);
    CHECK(ending.restore == seat.acquiring.RestoreHandle());
    CHECK(seat.acquiring.RestoreHandle()->Calls() == 0);
    seat.stuck.Open();
}

TEST_CASE("a live-stats session stopped with a sample out drains when the sample returns in time",
          "[cli][live][session][seat]")
{
    // The control for the case above: the same stop and the same gated gather, with the gate
    // already open. Nothing is abandoned.
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false, true };
    auto identity = ScriptedIdentity { CacheDaemon() };
    seat.gated.Open();

    auto const ending = seat.Run(SessionContext({}, 0, &identity, &seat.gated));

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(ending.line.empty());
    CHECK((seat.source.has_value() && seat.source->IsDrained()));
}

TEST_CASE("a live-stats session re-dials through the seat's dialer after a failed sample",
          "[cli][live][session][seat][redial]")
{
    // What `main` hands over reaches the source: the connection dies after one reading, the gap
    // spends a sample, and the third sample of a budget of three reads only over a re-dial. Two
    // intervals at the floor, so about two seconds. Without the re-dial the dead connection is
    // asked a third time, which presses Ctrl-C, and the last row is a gap rather than a reading.
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false, true };
    auto identity = ScriptedIdentity { CacheDaemon() };
    auto dying = BoundedDyingGatherer { &seat.onDemand };
    ScriptedDialer dialer { { true } };
    seat.dialer = &dialer;

    auto const ending = seat.Run(SessionContext({}, 3, &identity, &dying));

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(ending.answer.outcome == Outcome::Affirmative);
    CHECK(dialer.Dials() == 1);
    // The header, a reading, the gap, and the reading over the re-dial -- a READING, which a gap row
    // of absent cells is not.
    auto const stream = seat.Stream();
    CHECK(std::ranges::count(stream, '\n') == 4);
    auto const lastRow = std::string_view { stream }.substr(stream.rfind('\n', stream.size() - 2) + 1);
    CHECK(lastRow.starts_with("info\t"));
}

namespace
{

/// A process end that notes each step in a log shared with a restore handle.
class LoggingExit final: public IAbandonedExit
{
  public:
    /// @param log Where each step is noted.
    explicit LoggingExit(std::vector<std::string>* log) noexcept:
        _log { log }
    {
    }

    void Flush() override
    {
        _log->emplace_back("flush");
    }

    void Say(Answer const& /*answer*/, std::string_view line) override
    {
        _log->push_back(std::format("say {}", line));
    }

    void Exit(int code) override
    {
        _log->push_back(std::format("exit {}", code));
    }

  private:
    std::vector<std::string>* _log;
};

} // namespace

TEST_CASE("an abandoned session puts the terminal back before it flushes, says why, and exits", "[cli][live][session]")
{
    auto log = std::vector<std::string> {};
    auto restore = std::make_shared<LoggingRestore>(&log);
    auto exit = LoggingExit { &log };

    EndAbandonedSession(SessionEnding { .kind = SessionEndKind::Abandoned,
                                        .answer = Concluded(Outcome::Unreachable),
                                        .line = "gave up waiting",
                                        .restore = restore },
                        exit);

    CHECK(log
          == std::vector<std::string> {
              "restore", "flush", "say gave up waiting", std::format("exit {}", ExitCodeOf(Outcome::Unreachable)) });
}

TEST_CASE("an abandoned piped session has no terminal to put back and still flushes, says why, and exits",
          "[cli][live][session]")
{
    auto log = std::vector<std::string> {};
    auto exit = LoggingExit { &log };

    EndAbandonedSession(SessionEnding { .kind = SessionEndKind::Abandoned,
                                        .answer = Concluded(Outcome::Affirmative),
                                        .line = "gave up waiting",
                                        .restore = nullptr },
                        exit);

    CHECK(log
          == std::vector<std::string> {
              "flush", "say gave up waiting", std::format("exit {}", ExitCodeOf(Outcome::Affirmative)) });
}

TEST_CASE("a live-stats session reaches its endpoint only through the stats gatherer and the identity",
          "[cli][live][session][seat]")
{
    // §9.18, asserted as wiring. Every other door `main` hands a verb is in the context and
    // counts what it is asked: the RESP, memcached and 0xFC connections and the admin surface. A
    // session that took a second path to the endpoint -- a direct call on a connection it happened
    // to be given -- would still stream correct samples, so the counts are the assertion.
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { CacheDaemon() };
    auto resp = ScriptedExchange { {} };
    auto memcached = ScriptedMemcachedExchange { {} };
    auto node = ScriptedNodeExchange { {} };
    auto admin = CountingAdmin {};

    auto context = SessionContext({}, 2, &identity, &seat.gatherer);
    context.resp = &resp;
    context.memcached = &memcached;
    context.node = &node;
    context.admin = &admin;

    auto const ending = seat.Run(context);

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(seat.gatherer.Calls() == 2);
    CHECK(identity.Calls() == 1);
    CHECK(resp.Sent().empty());
    CHECK(memcached.Sent().empty());
    CHECK(node.Sent().empty());
    CHECK(admin.Fetches() == 0);
}

#if !defined(_WIN32)

namespace
{

/// A gatherer that notes, as it gathers, whether SIGINT is still ignored: DURING the session,
/// which is when an install over the ignore would show -- by the session's end it is restored.
class DispositionGatherer final: public IStatsGatherer
{
  public:
    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        struct sigaction current {};
        if (::sigaction(SIGINT, nullptr, &current) != 0 || current.sa_handler != SIG_IGN)
            _overridden = true;
        return Reading();
    }

    /// @return Whether any gather saw SIGINT caught rather than ignored.
    [[nodiscard]] bool Overridden() const noexcept
    {
        return _overridden.load();
    }

  private:
    std::atomic<bool> _overridden { false };
};

/// The process's own stop request, as `main` installs it.
class ProcessStopInstaller final: public IStopSignalInstaller
{
  public:
    [[nodiscard]] std::expected<std::unique_ptr<IStopSignal>, std::string> Install() override
    {
        return InstallStopSignal();
    }
};

} // namespace

TEST_CASE("a live-stats session that inherited SIGINT ignored runs to its budget and leaves it ignored",
          "[cli][live][session][seat]")
{
    // A background job started with SIGINT ignored: the session installs nothing over it, says
    // nothing about it, and still ends on its budget -- the stop signal it got can never fire,
    // which is not a failure to install one.
    struct sigaction original {};
    struct sigaction ignored {};
    ignored.sa_handler = SIG_IGN;
    static_cast<void>(::sigemptyset(&ignored.sa_mask));
    REQUIRE(::sigaction(SIGINT, &ignored, &original) == 0);

    {
        auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
        auto identity = ScriptedIdentity { CacheDaemon() };
        auto process = ProcessStopInstaller {};
        auto gatherer = DispositionGatherer {};
        seat.stopsOverride = &process;

        auto const ending = seat.Run(SessionContext({}, 2, &identity, &gatherer));

        CHECK(ending.kind == SessionEndKind::Ran);
        CHECK(ending.answer.outcome == Outcome::Affirmative);
        CHECK(ending.answer.advisories.empty());
        CHECK(std::ranges::count(seat.Stream(), '\n') == 3);
        CHECK_FALSE(gatherer.Overridden());
    }

    struct sigaction after {};
    static_cast<void>(::sigaction(SIGINT, nullptr, &after));
    CHECK(after.sa_handler == SIG_IGN);
    static_cast<void>(::sigaction(SIGINT, &original, nullptr));
}

#endif

namespace
{

/// A compile node, as the identification describes one.
/// @return The identification.
[[nodiscard]] EndpointIdentity NodeIdentity()
{
    return EndpointIdentity { .kind = RemoteKind::CompileNode,
                              .detail = "10.0.0.4:6674 is a fastcache-compile-node",
                              .unreadable = false };
}

} // namespace

TEST_CASE("a live-stats node session reads the node's status with every sample", "[cli][live][session][seat][status]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { NodeIdentity() };
    auto status = CountingNodeStatus {};
    auto context = SessionContext({}, 2, &identity, &seat.gatherer);
    context.nodeStatus = &status;

    auto const ending = seat.Run(context);

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(seat.gatherer.Calls() == 2);
    CHECK(status.Reads() == 2);
}

TEST_CASE("a live-stats cache session reads no node status even where one could be asked",
          "[cli][live][session][seat][status]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { CacheDaemon() };
    auto status = CountingNodeStatus {};
    auto context = SessionContext({}, 1, &identity, &seat.gatherer);
    context.nodeStatus = &status;

    auto const ending = seat.Run(context);

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(status.Reads() == 0);
}

TEST_CASE("a live-stats node session with no way to ask the node's status is refused before anything is composed",
          "[cli][live][session][seat][status]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { NodeIdentity() };

    auto const ending = seat.Run(SessionContext({}, 1, &identity, &seat.gatherer));

    CHECK(ending.kind == SessionEndKind::Refused);
    CHECK(ending.answer.outcome == Outcome::Unreachable);
    CHECK(AdvisoryText(ending.answer).contains(WireTable[static_cast<std::size_t>(Wire::Node)].unavailable));
    CHECK(seat.gatherer.Calls() == 0);
    CHECK_FALSE(seat.source.has_value());
}

TEST_CASE("a live-stats session with no stats source is refused before anything is composed", "[cli][live][session][seat]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { CacheDaemon() };

    auto const ending = seat.Run(SessionContext({}, 1, &identity, nullptr));

    CHECK(ending.kind == SessionEndKind::Refused);
    CHECK(ending.answer.outcome == Outcome::Unreachable);
    CHECK(seat.stops.Calls() == 0);
    CHECK_FALSE(seat.source.has_value());
}

TEST_CASE("a piped live-stats fleet session streams the leader's KPI strip, one row per whole document",
          "[cli][live][session][seat][fleet]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { EndpointIdentity {
        .kind = RemoteKind::CompileNode, .detail = "10.0.0.4:6674 is a fastcache-compile-node", .unreadable = false } };
    auto const operands = std::vector<std::string> { "fleet" };
    auto leader = Distributed::FleetSnapshot {};
    leader.role = Distributed::SchedulerRole::Leader;
    auto admin = ScriptedDocument { Distributed::RenderFleetText(leader, Distributed::FleetHistoryView {}, std::nullopt) };

    // At fleet's own floor, so two samples cost two seconds. No stats ladder: a fleet session never
    // reads one, so its absence must not be what refuses it.
    auto context = SessionContext(operands, 2, &identity, nullptr);
    context.options.interval = LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Fleet)].minInterval;
    context.admin = &admin;

    auto const ending = seat.Run(context);

    CHECK(ending.kind == SessionEndKind::Ran);
    CHECK(ending.answer.outcome == Outcome::Affirmative);
    // The whole document, once per sample: never a section at a time.
    CHECK(admin.Asked() == std::vector<std::string>(2, "/fleet.txt"));
    // The header and two rows, the header naming the strip's figures after the source.
    auto const stream = seat.Stream();
    CHECK(std::ranges::count(stream, '\n') == 3);
    CHECK(stream.starts_with(std::format("source\t{}\t", Distributed::FleetKpiKeys().front())));
    CHECK(stream.contains("\nfleet.txt\t"));
}

TEST_CASE("a live-stats fleet session with no admin surface is refused as usage before anything is composed",
          "[cli][live][session][seat]")
{
    auto seat = RunningSeat { RenderOptions { .format = OutputFormat::Tsv }, false };
    auto identity = ScriptedIdentity { EndpointIdentity {
        .kind = RemoteKind::CompileNode, .detail = "10.0.0.4:6674 is a fastcache-compile-node", .unreadable = false } };
    auto const operands = std::vector<std::string> { "fleet" };

    // A stats ladder is there and is not the door a fleet sample goes through.
    auto context = SessionContext(operands, 1, &identity, &seat.gatherer);
    context.options.interval = std::nullopt;

    auto const ending = seat.Run(context);

    CHECK(ending.kind == SessionEndKind::Refused);
    CHECK(ending.answer.outcome == Outcome::Usage);
    CHECK(AdvisoryText(ending.answer).contains(NoAdminSurface));
    CHECK(seat.gatherer.Calls() == 0);
    CHECK(seat.stops.Calls() == 0);
    CHECK_FALSE(seat.source.has_value());
}
