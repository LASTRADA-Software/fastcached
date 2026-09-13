// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "CliValue.hpp"
#include "DashboardEvent.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file DashboardLoop.hpp
/// The `live-stats` render loop: a fold over one ordered event stream.
///
/// **The loop initiates nothing.** It does not ask the time, does not ask for a sample
/// and does not ask the terminal anything -- ticks, readings and geometry all arrive as
/// events. That is not an aesthetic preference: it is what makes *the same event list
/// renders byte-identical frames, every run* true by construction rather than by
/// discipline, because there is no statement in the fold that could read a clock. The
/// behavioural half of §9.11's `getenv`/`isatty`/`ioctl`/`GetConsoleMode` scan, and the
/// two catch different things -- a scan cannot see a hidden ambient read reached through
/// a helper, and determinism cannot see one whose answer happens to be stable.

/// When a reading was taken and where it came from: what decides whether it continues a run.
struct ReadingStamp
{
    TimePoint at {};       ///< When it was taken, on the steady clock.
    std::string source {}; ///< Which source produced it, by stable name.
};

/// One point of the dashboard's sample history: a reading, or the fact that there was none.
///
/// **A failed sample is an entry, not a missing one.** The gap is the information: a history
/// that dropped the failure would hold the readings either side of it as neighbours, and every
/// trend drawn from it would join them as if the source had never gone away. §9.2's gap is only
/// expressible because an element exists at its position and says *nothing was read*.
struct HistoryEntry
{
    /// What was read, or nullopt where the sample produced no reading.
    std::optional<Value> reading {};

    /// How long the interval ending at this entry lasted, or nullopt where none was measured.
    ///
    /// **The fold's decision, recorded rather than re-derived.** Engaged exactly when this reading
    /// continued the run of the entry before it -- the decision that moved `runLength` past one --
    /// so a first reading, a failure, a change of source and a stamp no later than the one before
    /// all leave it disengaged. A series that re-applied the run rule to stored stamps would be a
    /// second copy of that rule, free to disagree with the first.
    std::optional<Duration> elapsed {};
};

/// How many history entries the model keeps, oldest dropped first.
///
/// Bounded so a dashboard left open for days holds the same memory as one opened a minute ago.
/// One entry is one sparkline cell, and §3's 80-column layout draws about thirty, so this is
/// several screens' worth; a wider terminal draws the most recent entries that fit.
inline constexpr std::size_t HistoryCapacity = 256;

/// What the dashboard knows right now.
///
/// Everything a frame is drawn from, and nothing else. A renderer that needed a fact not
/// in here would have to acquire it, which is the ambient read the design exists to
/// exclude -- so this struct growing a field is the honest way to add one.
struct DashboardModel
{
    /// The most recent reading, or nullopt before the first one arrives.
    ///
    /// **Absent rather than a default-constructed `Value`**, because *no reading yet* and
    /// *a reading of nothing* render differently and an empty `Value` cannot tell them
    /// apart. §9.1's first-frame-every-rate-absent clause is exactly this distinction.
    std::optional<Value> latest {};

    /// The reading before `latest`, or nullopt when there has not been one.
    ///
    /// A rate needs two readings and a gap needs to know one is missing, so the previous
    /// reading is model state rather than something a renderer recomputes.
    std::optional<Value> previous {};

    /// When `latest` was taken and which source produced it; engaged exactly when `latest` is.
    ///
    /// Kept because the NEXT reading is judged against it.
    std::optional<ReadingStamp> latestStamp {};

    /// How many readings in a row belong to one measurable run.
    ///
    /// **A counter rather than a flag, because the rules turned out to be one.** A rate
    /// needs two readings (so the first reading of a session has none), and it must not
    /// span anything that makes the interval unmeasurable: a failure, a change of source,
    /// or a stamp no later than the one before. Written as flags those are conditions that
    /// have to agree, and the first version of this file got one wrong in the direction
    /// nothing notices: it cleared the flag on the reading straight after a failure,
    /// drawing a rate over an interval where the source was down. One counter, one rule.
    std::size_t runLength { 0 };

    /// The recent samples, oldest first, a failure included as an entry with no reading.
    ///
    /// `latest` and `previous` answer *what is the value now*; this answers *what has it been
    /// doing*, which is what a sparkline draws. The fold writes it in the same two helpers that
    /// write `runLength`, so a reading and a failure reach both or neither, and a test holds the
    /// newest entry's interval to `BrokenRun()` at every frame.
    std::deque<HistoryEntry> history {};

    /// The terminal geometry, as last reported. Zero until a `Resize` says.
    int columns { 0 };
    int rows { 0 };

    /// How many readings have been accepted.
    std::size_t samples { 0 };

    /// How many frames have been drawn.
    std::size_t frames { 0 };

    /// Whether the interval between `previous` and `latest` can be measured.
    ///
    /// Derived rather than stored beside `runLength`, so the two cannot disagree. A
    /// renderer cannot work this out from the readings alone: a source that failed and
    /// a source that reported the same numbers twice look identical in the values.
    /// @return True when no rate may be drawn.
    [[nodiscard]] bool BrokenRun() const noexcept
    {
        return runLength < 2;
    }
};

/// How fast one counter rose over the interval ending at each entry of @p history, per second.
///
/// **One element per ENTRY, oldest first**, so a panel draws one cell per sample and a cell sits
/// at the same position in every row. A rate is present only when all three hold:
///
/// - **the fold measured the interval** (`HistoryEntry::elapsed`), which is `BrokenRun()`'s rule
///   applied to every entry rather than only the newest -- so a failure costs TWO cells, the
///   interval into it and the interval out of it, since neither was measured;
/// - **both ends are in the window and read the field as a finite number**, so the oldest entry
///   kept has no rate even where its run continued past the bound;
/// - **the counter did not go DOWN.** A decrease is a restart and is a gap -- never a negative
///   rate, and never clamped to zero, which would claim the server did nothing (§9.3).
///
/// Divided by the MEASURED elapsed time rather than the nominal interval, so a sampler that fell
/// behind draws the rate that happened rather than one inflated by how far behind it was.
///
/// @param history The samples, oldest first.
/// @param field The counter's field name, as the reading spells it.
/// @return Events per second per entry; nullopt where no rate can be claimed.
[[nodiscard]] std::vector<std::optional<double>> CounterRateSeries(std::deque<HistoryEntry> const& history,
                                                                   std::string_view field);

/// Why the loop stopped.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class DashboardStop : std::uint8_t
{
    Quit,           ///< The operator asked to leave.
    SampleBudget,   ///< `--samples=N` was satisfied.
    SourceDetached, ///< The source went away and will send nothing more.
    Last,
};

/// How the run ended.
struct DashboardExit
{
    DashboardStop stop { DashboardStop::Quit };

    /// The outcome the exit code is derived from.
    ///
    /// **`Unreachable` only when NOTHING ever answered.** One successful sample followed
    /// by failures all the way to the end is `Affirmative` with the failures drawn as
    /// gaps -- §9.17 -- because a dashboard that showed real data and then lost its
    /// source did its job. A run that only ever tested the healthy path cannot tell those
    /// two apart, which is why the distinction lives here rather than in the renderer.
    Outcome outcome { Outcome::Unreachable };

    DashboardModel model {};
};

/// What one `Sample` reads as, once its session's reader has looked at it.
struct SampleReading
{
    /// `Affirmative` when there is a reading; otherwise why there is none.
    Outcome outcome { Outcome::Unreachable };

    /// The reading. Meaningful iff `outcome` is `Affirmative`.
    Value value {};

    /// Which source produced it, by stable name. Meaningful iff `outcome` is `Affirmative`.
    ///
    /// Part of the reading rather than something the fold infers, because a change of source
    /// breaks the run: a change taken across `/metrics` and then `INFO` subtracts one
    /// vocabulary from another, and the result is not a rate of anything.
    std::string source {};
};

/// Turns one session's raw `Sample` payload into a reading.
///
/// **Injected, so the fold never branches on a subject.** A `cache` or `node` session reads
/// `attempts` through the stats ladder and a `fleet` session reads `document`; the loop calls
/// whichever reader it was given and cannot tell them apart. That keeps the choice in the
/// deterministic half, where a loop fixture reaches it.
///
/// **A plain function pointer rather than `std::function`, deliberately.** A reader has no
/// state to carry, and a capture is exactly how a gatherer, a socket or a clock would reach
/// the one half of this design that must touch none of them. The type forbids that rather
/// than a comment asking.
using SampleReader = SampleReading (*)(DashboardEvent const& event);

/// The reader for a `cache` or `node` session: the stats ladder's own decision.
///
/// `ChooseStats` over the event's attempts, which is what the fold ran inline before a reader
/// was injected -- moved, not rewritten, so a `cache` session sees the decision it always saw.
/// @param event The `Sample`.
/// @return The chosen record and its source, or why nothing could be chosen.
[[nodiscard]] SampleReading ReadStatsSample(DashboardEvent const& event);

/// Where a rendered frame goes.
///
/// Separate from the renderer so the loop can be driven with no terminal at all: a test
/// collects frames in a vector and compares them to each other, which is what the
/// determinism property needs and what a golden-frame comparison would destroy.
class IFrameSink
{
  public:
    IFrameSink() = default;
    IFrameSink(IFrameSink const&) = delete;
    IFrameSink(IFrameSink&&) = delete;
    IFrameSink& operator=(IFrameSink const&) = delete;
    IFrameSink& operator=(IFrameSink&&) = delete;
    virtual ~IFrameSink() = default;

    /// Show one frame.
    /// @param frame The bytes to present.
    virtual void Present(std::string_view frame) = 0;
};

/// Turns the model into one frame.
///
/// **The capability ladder lives behind this seam, not in the loop.** Sixel, Unicode
/// bars and ASCII are three implementations of this interface, chosen ONCE when the view
/// is constructed -- which is what §9.12 asserts by counting capability-seam calls across
/// *n* frames and requiring 1 rather than *n*. The loop never learns which rung it is on,
/// so no panel can branch on it.
class IDashboardView
{
  public:
    IDashboardView() = default;
    IDashboardView(IDashboardView const&) = delete;
    IDashboardView(IDashboardView&&) = delete;
    IDashboardView& operator=(IDashboardView const&) = delete;
    IDashboardView& operator=(IDashboardView&&) = delete;
    virtual ~IDashboardView() = default;

    /// Draw the model.
    /// @param model What is known right now.
    /// @return The frame's bytes.
    [[nodiscard]] virtual std::string Frame(DashboardModel const& model) = 0;
};

/// What bounds the run.
struct DashboardLimits
{
    /// Stop after this many accepted readings; 0 means no limit.
    ///
    /// Zero as *no limit* rather than a disengaged optional, matching how
    /// `--cache-memory 0` and `InMemoryLruStorage` already spell unbounded in this tree.
    std::size_t samples { 0 };
};

/// Run the dashboard until it stops.
///
/// Pointers rather than references, and not as a style preference: a coroutine
/// frame outlives the call that created it, so a reference parameter dangles the
/// moment the referent dies before the coroutine resumes. Every production
/// coroutine in this tree takes pointers for that reason, and clang-tidy refuses
/// the alternative. None may be null.
///
/// @param events Where every input comes from, in order.
/// @param reader What turns a `Sample` into a reading; the session's subject is chosen by
///        which one is passed.
/// @param view What draws a frame.
/// @param sink Where a frame goes.
/// @param limits What bounds the run.
/// @return How it ended, and what it knew when it did.
[[nodiscard]] Task<DashboardExit> RunDashboard(
    IDashboardEventSource* events, SampleReader reader, IDashboardView* view, IFrameSink* sink, DashboardLimits limits);

/// Whether @p keys asks the dashboard to quit.
///
/// Its own function so the loop states no key literal and a test can assert the set
/// rather than a behaviour that happens to follow from it.
/// @param keys The bytes a keystroke delivered.
/// @return True when the operator asked to leave.
[[nodiscard]] bool IsQuitKey(std::string_view keys) noexcept;

} // namespace FastCache::Cli
