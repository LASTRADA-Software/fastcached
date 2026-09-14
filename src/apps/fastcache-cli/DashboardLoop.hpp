// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "CliValue.hpp"
#include "DashboardEvent.hpp"
#include "DashboardFrame.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace FastCache::Cli
{

struct FleetDocument;

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

/// The role a reading states when the endpoint that answered is its subject's leader: the fleet reader
/// writes it, and a title bar reads it back to say `leader <addr>`.
inline constexpr std::string_view LeaderRole = "leader";

/// When a reading was taken and where it came from: what decides whether it continues a run.
struct ReadingStamp
{
    TimePoint at {};       ///< When it was taken, on the steady clock.
    std::string source {}; ///< Which source produced it, by stable name.
    std::string route {};  ///< What was asked there, as the source line names it; see `SampleReading::route`.
    std::string where {};  ///< The `host:port` that answered; empty when the reader did not say.
    std::string role {};   ///< What the answering endpoint is to its subject, such as `leader`; usually empty.
};

/// One per-subject figure a sample read, kept in the history for a chart to draw.
///
/// **Numbers, never documents.** A fleet reading's history is what its chart draws from, so it keeps
/// the one figure per machine the chart needs and nothing else: the document itself lives only as
/// `DashboardModel::latestDocument`, for the newest sample.
struct SeriesPoint
{
    std::string_view series; ///< Which series: a `FleetChartMetrics` key, in static storage.
    std::string subject;     ///< Whose figure: a machine's key as the document names it.
    double value { 0.0 };    ///< The figure, already scaled.
};

/// Where a figure's number lives in a `StatsReading`: a catalogue counter, a field of the cache's statistics
/// (the whole cache, or one tier of it), or a field of the machine's capacity.
///
/// **A panel names the MODEL, never a series name** (#134, #1399). The live model is the `StatsReading` both
/// `/metrics` and the `0xFC` snapshot are encoded from, so a panel written against it reads the same figure
/// whichever encoding carried it, and a source switching encodings changes no panel. Typed member pointers
/// rather than names: a field that does not exist is a build failure, not a figure that silently reads absent.
///
/// Absent where the reading does not carry the block (no cache, no such tier, no host) or the counter (a sink
/// with no slot for the row): absence stays absence, never zero.
struct ReadingField
{
    /// The number, or nullopt where the reading does not carry it; null for a figure that names no field.
    /// @param reading The reading.
    /// @param tier The tier a tier column asks about, or nullopt for the whole cache.
    std::optional<double> (*read)(StatsReading const& reading, std::optional<StorageTier> tier) noexcept { nullptr };

    /// Whether this names a field at all.
    /// @return False for the default, which names nothing.
    [[nodiscard]] constexpr bool Names() const noexcept
    {
        return read != nullptr;
    }
};

/// The figure a catalogue counter reads.
/// @tparam Row The counter.
/// @return The field.
template <IMetricsSink::Counter Row>
[[nodiscard]] constexpr ReadingField CounterField() noexcept
{
    return ReadingField { .read = [](StatsReading const& reading, std::optional<StorageTier> /*tier*/) noexcept {
        auto const& value = reading.counters[static_cast<std::size_t>(Row)];
        return value.has_value() ? std::optional { static_cast<double>(*value) } : std::nullopt;
    } };
}

/// The figure one field of the cache's statistics reads: the whole cache's, or one tier's when a tier is asked.
/// @tparam Member The field.
/// @return The field.
template <auto Member>
    requires std::is_member_object_pointer_v<decltype(Member)>
[[nodiscard]] constexpr ReadingField StorageField() noexcept
{
    return ReadingField { .read = [](StatsReading const& reading, std::optional<StorageTier> tier) noexcept {
        auto const& stats =
            tier.has_value() ? reading.snapshot.storageTiers[static_cast<std::size_t>(*tier)] : reading.snapshot.storage;
        return stats.has_value() ? std::optional { static_cast<double>((*stats).*Member) } : std::nullopt;
    } };
}

/// The figure one field of the machine's capacity reads.
/// @tparam Member The field.
/// @return The field.
template <auto Member>
    requires std::is_member_object_pointer_v<decltype(Member)>
[[nodiscard]] constexpr ReadingField HostField() noexcept
{
    return ReadingField { .read = [](StatsReading const& reading, std::optional<StorageTier> /*tier*/) noexcept {
        auto const& host = reading.snapshot.host;
        return host.has_value() ? std::optional { static_cast<double>((*host).*Member) } : std::nullopt;
    } };
}

/// The figure one of the machine's cumulative CPU counters reads.
///
/// A counter in platform ticks, so it means nothing alone: a panel takes the RATE of busy over the rate of all ticks
/// (`FigureSource::RateQuotient`), the same share `CpuBusyPermille` takes over two readings.
/// @tparam Member `CpuTicks::busy` or `CpuTicks::total`.
/// @return The field.
template <auto Member>
    requires std::is_member_object_pointer_v<decltype(Member)>
[[nodiscard]] constexpr ReadingField CpuTicksField() noexcept
{
    return ReadingField { .read = [](StatsReading const& reading, std::optional<StorageTier> /*tier*/) noexcept {
        auto const& load = reading.snapshot.hostLoad;
        return load.has_value() && load->cpu.has_value() ? std::optional { static_cast<double>((*load->cpu).*Member) }
                                                         : std::nullopt;
    } };
}

/// The figure one field of what the machine is doing reads, each absent on its own where the platform did not say.
/// @tparam Member A `std::optional` member of `HostLoadReading` other than the CPU counters.
/// @return The field.
template <auto Member>
    requires std::is_member_object_pointer_v<decltype(Member)>
[[nodiscard]] constexpr ReadingField HostLoadField() noexcept
{
    return ReadingField { .read = [](StatsReading const& reading, std::optional<StorageTier> /*tier*/) noexcept {
        auto const& load = reading.snapshot.hostLoad;
        if (!load.has_value())
            return std::optional<double> {};
        auto const& value = (*load).*Member;
        return value.has_value() ? std::optional { static_cast<double>(*value) } : std::nullopt;
    } };
}

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

    /// The reading as the live model, for a `cache` or `node` sample; nullopt for a failure and for a subject
    /// whose reader produces none. What every panel figure is read from.
    std::optional<StatsReading> stats {};

    /// How long the interval ending at this entry lasted, or nullopt where none was measured.
    ///
    /// **The fold's decision, recorded rather than re-derived.** Engaged exactly when this reading
    /// continued the run of the entry before it -- the decision that moved `runLength` past one --
    /// so a first reading, a failure, a change of source and a stamp no later than the one before
    /// all leave it disengaged. A series that re-applied the run rule to stored stamps would be a
    /// second copy of that rule, free to disagree with the first.
    std::optional<Duration> elapsed {};

    /// The per-subject figures this sample read, for a chart; empty for a failure or a reader that
    /// reads none. A subject absent here is a gap in its series at this entry.
    std::vector<SeriesPoint> points {};
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

    /// The newest reading as the live model, for a `cache` or `node` session; nullopt before one arrives and for
    /// a subject whose reader produces none. What a title bar reads the version and the uptime from.
    std::optional<StatsReading> stats {};

    /// When `latest` was taken and which source produced it; engaged exactly when `latest` is.
    ///
    /// Kept because the NEXT reading is judged against it.
    std::optional<ReadingStamp> latestStamp {};

    /// The node's status as the NEWEST sample carried it, or nullopt when that sample carried none.
    ///
    /// Replaced by every sample taken, a failed one included, and never merely updated: a sample
    /// that brought no status leaves this empty rather than keeping the one before. A status block
    /// then draws the absent marker beside a gap, exactly as a figure does, instead of showing the
    /// last thing a node said before it stopped saying anything.
    std::optional<CompileCacheWire::NodeStatusFields> nodeStatus {};

    /// The fleet document the NEWEST sample's reading carried, or null when that sample carried none.
    ///
    /// Replaced by every sample taken, exactly as `nodeStatus` is: a failed sample, or a reading with
    /// no document, leaves it null rather than keeping the last fleet a leader described, so a fleet
    /// panel draws absent beside the gap instead of a table of figures from before it. Parsed once,
    /// by the reader, and shared rather than copied into the history: a panel draws the newest
    /// document only, and 256 copies of a whole fleet would be memory nobody reads.
    std::shared_ptr<FleetDocument const> latestDocument {};

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

    /// How many pixels a cell measures, as the last `Resize` reported it; nullopt until one says.
    ///
    /// Replaced by every `Resize`, never kept from an earlier one: a resize that carried no cell size
    /// leaves no size to draw an image from, rather than the size of a grid that is gone.
    std::optional<CellPixelSize> cellPixels {};

    /// How many readings have been accepted.
    std::size_t samples { 0 };

    /// How many samples were TAKEN, readings and failures alike: what `--samples` bounds.
    ///
    /// Apart from `samples` because the two answer different questions. `samples` is how much
    /// the dashboard knows; this is how many times it asked. A budget counting only readings
    /// never ends a run whose every sample fails -- measured, `live-stats --samples=3` against
    /// such an endpoint hung with no output -- and §9.17's never-answered exit is reachable only
    /// if a failed attempt spends the budget too.
    std::size_t attempts { 0 };

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

/// The number @p reading holds for @p field.
/// @param reading The reading, or nullopt where there was none.
/// @param field The field.
/// @param tier The tier a tier column asks about, or nullopt for the whole cache.
/// @return The number, or nullopt where there was no reading, the field names nothing, or the reading does not
///         carry it.
[[nodiscard]] std::optional<double> NumberIn(std::optional<StatsReading> const& reading,
                                             ReadingField field,
                                             std::optional<StorageTier> tier = std::nullopt) noexcept;

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
/// @param field The counter.
/// @param tier The tier a tier column asks about, or nullopt for the whole cache.
/// @return Events per second per entry; nullopt where no rate can be claimed.
[[nodiscard]] std::vector<std::optional<double>> CounterRateSeries(std::deque<HistoryEntry> const& history,
                                                                   ReadingField field,
                                                                   std::optional<StorageTier> tier = std::nullopt);

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

    /// The reading as the live model, for a reader of a `cache` or `node` subject; nullopt otherwise.
    std::optional<StatsReading> stats {};

    /// Which source produced it, by stable name. Meaningful iff `outcome` is `Affirmative`.
    ///
    /// Part of the reading rather than something the fold infers, because a change of source
    /// breaks the run: a change taken across `/metrics` and then `INFO` subtracts one
    /// vocabulary from another, and the result is not a rate of anything.
    std::string source {};

    /// Why there is no reading, in words for a person; empty when `outcome` is `Affirmative`.
    ///
    /// The decision's own account, carried out of the reader, so whoever reports a failed sample
    /// says why without running the decision a second time -- which would be a second place for
    /// the two accounts to disagree.
    std::string note {};

    /// The document this reading was parsed from, for a reader that parses one; null otherwise.
    ///
    /// **The reader's parse, handed over rather than repeated.** A `fleet` reader has to parse the
    /// leader's text to know whether it is a reading at all, and a panel drawing that parse again
    /// every frame would be the same work done twice with two chances to disagree. The fold keeps
    /// it as `DashboardModel::latestDocument`. Forward-declared here, so every unit including the
    /// loop does not compile the fleet's vocabulary.
    std::shared_ptr<FleetDocument const> document {};

    /// Per-subject figures for a chart, kept in the history entry this reading becomes; empty for a
    /// reader that reads none.
    std::vector<SeriesPoint> points {};

    /// What was asked, as a panel's source line names it: `/metrics`, `INFO`, `/fleet.txt`.
    ///
    /// **The reader's own spelling**, from the table that already names its source, so the source line
    /// cannot name a route the reader did not take. Meaningful iff `outcome` is `Affirmative`.
    std::string route {};

    /// The `host:port` that answered, or empty when the reader does not know it.
    std::string where {};

    /// What the answering endpoint is to the subject, when that is part of the reading: `leader` for a
    /// fleet document, which only the leader serves. Empty otherwise.
    std::string role {};
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

    /// Show one frame with the images placed over it.
    ///
    /// **The one thing a sink implements, and it has no default.** A default that handed on the text
    /// alone let a decorator that overrode only the text door drop every image before the terminal,
    /// while each row still drew. So forgetting the frame is a build failure rather than a blank chart.
    /// A sink that cannot draw an image -- a pipe, a test collecting text -- writes `frame.text`, which
    /// loses nothing a person reads, because a view places an image only over cells it left blank.
    /// @param frame The frame.
    virtual void PresentPlaced(DashboardFrame const& frame) = 0;

    /// Show one frame that places no image.
    ///
    /// **Deliberately not virtual.** It is `PresentPlaced` with no placements, so there is one door to
    /// implement and none to override by mistake: a decorator that wrote this instead of
    /// `PresentPlaced` would not compile. A different NAME, not an overload of `PresentPlaced`, so a
    /// sink's override hides nothing a caller reaches.
    /// @param text The bytes to present.
    void Present(std::string_view text)
    {
        PresentPlaced(DashboardFrame { .text = std::string { text }, .placements = {} });
    }
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

    /// Draw the model with any images placed over it.
    ///
    /// **What the loop calls, the one thing a view implements, and it has no default.** It is
    /// `IFrameSink::PresentPlaced`'s shape for `PresentPlaced`'s reason: with a text door beside it
    /// that the frame door defaulted to, a view that wrapped another and implemented only the text
    /// would draw every row and drop every image, and nothing would say so. A view that places no
    /// image returns its text with no placements.
    /// @param model What is known right now.
    /// @return The frame.
    [[nodiscard]] virtual DashboardFrame PlacedFrame(DashboardModel const& model) = 0;

    /// Draw the model's text alone.
    ///
    /// **Deliberately not virtual.** It is `PlacedFrame`'s text, so there is one door to implement and
    /// none to override by mistake: a view that wrote this instead of `PlacedFrame` would not compile.
    /// A different NAME, not an overload of `PlacedFrame`, so a view's override hides nothing a caller
    /// reaches.
    /// @param model What is known right now.
    /// @return The frame's bytes.
    [[nodiscard]] std::string Frame(DashboardModel const& model)
    {
        return PlacedFrame(model).text;
    }

    /// Act on a keystroke that is not a quit key.
    ///
    /// **What a view is showing is the view's state, not the model's**: the model is what was READ,
    /// and a key reads nothing. So a key the view acts on changes the view, and the loop draws the
    /// next frame at once rather than at the next `Tick` -- an interval can be seconds, and a key
    /// answered that late reads as ignored. The default acts on nothing, which is every view that
    /// has nothing to switch; there is no decorating view for one to be lost in.
    /// @param keys The bytes the keystroke delivered.
    /// @return Whether the next frame differs, so one is owed now.
    [[nodiscard]] virtual bool Key(std::string_view keys)
    {
        (void) keys;
        return false;
    }
};

/// What bounds the run.
struct DashboardLimits
{
    /// Stop after this many samples taken, failed ones included; 0 means no limit.
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
