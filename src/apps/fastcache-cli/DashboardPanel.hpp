// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardFrame.hpp"
#include "DashboardGlyphs.hpp"
#include "DashboardLoop.hpp"
#include "DashboardRung.hpp"
#include "HistoryChart.hpp"
#include "SixelEncoder.hpp"
#include "StatsSource.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/FleetView.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

/// @file DashboardPanel.hpp
/// A `live-stats` panel as DATA, and the one view that draws any of them.
///
/// **A panel is a table of rows, not a renderer.** `cache` and `node` differ in which figures they
/// show and what those figures are made of; they do not differ in how a rate is taken, how a gap
/// is drawn or where a sparkline goes. So each panel is a `PanelSpec` -- rows naming fields and a
/// way of combining them -- and `PanelView` is the only code that turns a model into a frame. A
/// fourth panel is a spec, and a fix to how a gap renders is one edit that reaches all of them.
///
/// **Every figure is the newest cell of its own series.** A row's number and its sparkline are one
/// computation over `DashboardModel::history`, so they cannot disagree; and a sample that failed
/// is the newest entry, so the figure beside a gap is the absent marker rather than the last value
/// read before the source went away (§9.17 draws a failure as a gap, not as a frozen number).

/// How a figure is made from its fields.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class FigureSource : std::uint8_t
{
    Level,          ///< `field` as read. A reading of now.
    LevelRatio,     ///< `field / (field + other)` as read: a since-start proportion.
    Rate,           ///< How fast `field` rose, plus how fast `other` rose when `other` is named.
    RateRatio,      ///< `rate(field) / (rate(field) + rate(other))`: a proportion over each interval.
    RateQuotient,   ///< `rate(field) / rate(other)`: a `_sum` over its `_count`, over each interval.
    LevelQuotient,  ///< `field / other` as read: a fill against its limit, absent for a limit of zero (unbounded).
    SlotsAvailable, ///< What a node may hold now: the scheduler's slot ceilings over each reading and the one before.
    Last,
};

/// One figure: which fields, combined how, written how.
///
/// The two byte-wide members are last, in one run, so the struct carries no padding between
/// 8-aligned members.
struct FigureSpec
{
    ReadingField field {}; ///< The primary field.
    ReadingField other {}; ///< The second operand; see `FigureSource`.
    double scale { 1.0 };  ///< Applied to every value: 60 turns a rate per second into one per minute.
    /// A present value above this is a state worth acting on, and is drawn as an alert where the terminal can
    /// dress it (#134 G1: a refusal rate above zero); nullopt for a figure that states no alarm.
    std::optional<double> alertAbove {};
    std::string_view suffix {}; ///< Written after a PRESENT value only, such as `/s`.
    /// Further counters whose rates a `Rate` adds beyond `other`: a total over more than two, such as
    /// every refusal. Read only by a source whose `FigureSourceTable` row takes addends.
    std::span<ReadingField const> addends {};
    FigureSource source { FigureSource::Level }; ///< How it is made.
    FigureFormat format { FigureFormat::Count }; ///< How it is written.
};

/// How much a piece of a panel matters when the terminal cannot fit all of it.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted at their place in the order.
///
/// **Declaration order IS the keep order, and it is the panel's whole drop policy** (#134 decision
/// 5). `Essential` is never dropped: a terminal that cannot fit every essential piece gets the
/// minimum-size line instead of a clipped panel. Everything else goes lowest first -- `Spacing`, then
/// `Low`, then `Normal`, then `High` -- and among pieces of one priority the rightmost or bottom-most
/// goes first. A trend narrows to its minimum before anything is dropped for it.
///
/// A column on every row, beside figure and tier column rather than an order written into the
/// layout code, so that a panel's priorities are read in its table and a new row states its own.
enum class Priority : std::uint8_t
{
    Essential, ///< Never dropped; not fitting it is the minimum-size line.
    High,      ///< Dropped last.
    Normal,    ///< The default.
    Low,       ///< Dropped first among content.
    Spacing,   ///< A blank separator line; dropped before any content.
    Last,
};

/// A figure written beside a row's own, with words around it.
struct BesideFigure
{
    std::string_view key;       ///< Its machine name; see `PanelKeysAreWhole`.
    std::string_view before {}; ///< Words before the value.
    FigureSpec figure {};       ///< The value.
    std::string_view after {};  ///< Words after the value.
    /// What stands before it on a fact line when it is not the line's first figure; empty for the gap between
    /// figures. A single space joins it to the figure before, as `/ 8.00 GiB` continues `2.00 GiB`.
    std::string_view lead {};
    Priority priority { Priority::Normal }; ///< When it is dropped for width.
    /// Whether a fact line draws a gauge of the value before it: the value is a share of something whole.
    bool gauge { false };
};

/// Whether a row draws its series as a sparkline.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class Trend : std::uint8_t
{
    Drawn, ///< On every rung that draws one.
    None,  ///< The figure has no trend worth a column, such as a mean.
};

/// One row of the upper block: a figure of each interval, its trend, and what sits beside it.
///
/// The label and the figure are ESSENTIAL and are not a column: a row that has lost its figure is
/// not a row. The byte-wide members close the struct in one run, so it pads nothing between
/// 8-aligned members.
struct RateRow
{
    std::string_view label;                  ///< What the figure is.
    std::string_view key;                    ///< Its machine name; see `PanelKeysAreWhole`.
    FigureSpec figure;                       ///< The figure.
    std::span<BesideFigure const> beside {}; ///< Figures written after it.
    /// Figures written on a line of their own under the row: the breakdown a total is never drawn without
    /// (#134 §4 -- each refusal has a different fix, so a total alone is the collapse this tree paid for).
    std::span<BesideFigure const> split {};
    /// Words written after those. On a row that draws no trend, a note too long for the line is wrapped
    /// onto lines under it, aligned where it began, rather than dropped.
    std::string_view note {};
    Trend trend { Trend::Drawn };              ///< Whether a sparkline is drawn.
    Priority priority { Priority::Normal };    ///< When the whole row is dropped for height.
    Priority trendPriority { Priority::High }; ///< When the trend is dropped for width, after narrowing.
    Priority notePriority { Priority::Low };   ///< When the note is dropped for width.
};

/// One row of the lower block: a reading of now, optionally against its limit.
///
/// The label and the reading are ESSENTIAL; the limit, its gauge and the percentage are columns.
struct LevelRow
{
    std::string_view label;                      ///< What the figure is.
    std::string_view key;                        ///< Its machine name; see `PanelKeysAreWhole`.
    FigureSpec value;                            ///< The reading.
    std::optional<FigureSpec> limit {};          ///< What bounds it, drawn with a gauge and a percentage.
    std::string_view limitKey {};                ///< The limit's machine name; named exactly when `limit` is.
    std::string_view note {};                    ///< Words written after it.
    Priority priority { Priority::Normal };      ///< When the whole row is dropped for height.
    Priority limitPriority { Priority::Normal }; ///< When the limit and the percentage are dropped for width.
    Priority gaugePriority { Priority::Low };    ///< When the gauge is dropped for width.
    Priority notePriority { Priority::Low };     ///< When the note is dropped for width.
};

/// One column of the tier block.
///
/// Its fields name the UNLABELLED series; the view adds each tier's label, walking
/// `StorageTierTable`, so no tier is spelled here.
struct TierColumn
{
    std::string_view header;                ///< The column's heading.
    std::string_view key;                   ///< Its machine name, per tier `TierFigureKey`; see `PanelKeysAreWhole`.
    FigureSpec figure;                      ///< The figure, per tier.
    Priority priority { Priority::Normal }; ///< When the column is dropped for width.
};

/// How a panel's history chart grows into rows nothing else wants: the node's and the fleet's alike.
///
/// The bounds are high on purpose: a tall terminal's rows are the chart's to use, and a band twelve rows high still
/// reads as one band. They exist so a very tall terminal does not draw a band as a wall.
struct ChartGrowth
{
    std::size_t cellsHighMost { 48 }; ///< The most rows its bands take together.
    std::size_t bandCellsMost { 12 }; ///< The most rows one band grows to.
    std::size_t minimumCells { 24 };  ///< The fewest cells across its bands are drawn in; narrower, it is not drawn.
};

/// How a panel draws the fleet document a `fleet` reading carries (#134 §5).
///
/// **No column list lives here, or anywhere in this client** (#1320): a section's columns are the
/// header line the leader sent, walked as it arrived. **Which of them survive a narrow terminal is the
/// leader's decision too**, asked by name (`Distributed::FleetColumnKeep`) exactly as a column's scale
/// is: `Identity` is essential, `Vital` goes last, `Detail` first, and a column the leader's tables do
/// not name keeps the default. **The width is filled by rank**: the most important columns first, then
/// every next column that still fits, among equals the leftmost first -- so a narrow column is drawn in
/// room a wider, more important one could not use, as §5 keeps `cores` at 80 and leaves `memory` out. The
/// positional rule this replaced dropped `heartbeat-age` -- the column §5 calls the row this panel exists
/// for -- first.
///
/// **The chart is the panels' one history chart** (`LayoutChart`): the first `FleetChartMetrics` row per machine,
/// as an image on the Sixel rung with a cell size and an encoder, and as the rung's chart marks on a rung that has
/// them. A rung with neither draws no chart row at all -- not blank rows, not a glyph imitation.
///
/// **The chart explains itself** (#134 F14; the owner could not tell what a band of colour meant). It is
/// one item, so it goes whole:
///   - a title naming the figure and the span it covers;
///   - a row per machine, its name and newest figure left of the image, the band beside them -- on a text rung
///     one blank row apart where every band keeps two rows of marks;
///   - the time axis under the image, from how long ago its left edge is to `now`;
///   - a legend: on the Sixel rung the colour scale as a second image between its two values, and what a bar,
///     the grey track and a blank mean; on a text rung the zero mark, the full cell, and the top every band has.
///
/// **It yields rows to the table** by construction: it is laid out after the frame is fitted, in the rows nothing
/// else wanted and only those, between the tiles and the strip -- more machines first, then taller bands, as
/// `chartGrowth` bounds them. A frame with no rows to spare has no chart.
///
/// The headline tiles are the `kpi` section's rows, in `Distributed::FleetKpis()` order, under the
/// page's own labels and with its nouns (`of 192 slots`, `not yet resolved`); a figure the page draws a
/// trend for draws one here. Tiles that carry words fill the first column and the rest the second, so
/// the strip is two columns at 80 wide, as §5 draws it; narrower they stack, then lose their words, and
/// tiles that do not fit at all are not drawn -- they are not essential, so they never turn a frame
/// into the minimum-size line.
///
/// **A table taller than its room gives up rows before the tiles go**: it shrinks at `tableShrinkPriority`
/// down to `tableRowsKept` rows, and only an essential shrink takes it below that. Its last line says what
/// is not shown and how to reach it (`... 8 more machines; PgDn scrolls, / filters`).
struct DocumentSpec
{
    ChartGrowth chartGrowth {};                        ///< How the chart grows: the one policy every history chart has.
    std::size_t tableRowsKept { 3 };                   ///< The rows a table keeps before the tiles go.
    Priority tilePriority { Priority::Normal };        ///< When a line of tiles goes, for height.
    Priority stripPriority { Priority::High };         ///< When the section strip goes, for height.
    Priority stripTabPriority { Priority::Low };       ///< When a tab other than the active one goes, for width.
    Priority keysHintPriority { Priority::Low };       ///< When the strip's `keys` hint goes, for width; before any tab.
    Priority tablePriority { Priority::Essential };    ///< When the section's table goes; an essential one only shrinks.
    Priority tableShrinkPriority { Priority::Normal }; ///< When the table gives up rows down to `tableRowsKept`.
    Priority filterPriority { Priority::High };        ///< When an applied filter's line goes; while typed it stays.
};

/// A fact a panel's title bar states about its session rather than its content (#134 §3-§5).
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
///
/// **One vocabulary for every panel's chrome**, each rendered by one row of `ChromeFactTable` from
/// what the frame already knows. A panel does not write its title bar: it lists the facts it states,
/// in reading order and with a drop priority, and the frame lays them out. A fact whose reading is
/// not known renders its absent marker BY NAME (`up -`), so a title bar never loses a slot silently.
enum class ChromeFact : std::uint8_t
{
    Version,  ///< The endpoint's version, beside the subject.
    Endpoint, ///< The address the session asks.
    Leader,   ///< `leader <addr>`: the address, stated as the leader's once a reading came from it.
    Uptime,   ///< `up <d>d<hh>:<mm>`: how long the endpoint has served.
    Machines, ///< `<N> machines`: the rows of the fleet document's machines section.
    Interval, ///< `every <N>s`: the sampling interval.
    Quit,     ///< `q`.
    QuitWord, ///< `q quit`.
    Last,
};

/// Which side of a title bar a fact is drawn on.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class TitleSide : std::uint8_t
{
    Subject, ///< After the subject, on the left: a version.
    Right,   ///< Right-aligned, in reading order.
};

/// One fact a panel's title bar states.
struct TitleFactRow
{
    ChromeFact fact { ChromeFact::Endpoint }; ///< What it says.
    TitleSide side { TitleSide::Right };      ///< Where it is drawn.
    Priority priority { Priority::Normal };   ///< When it is dropped for width; the subject itself never is.
};

/// A fact a panel states in words about the endpoint it asks, read from that endpoint's own status rather
/// than from a series (#134 §4).
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
///
/// **What a node is DOING, as opposed to how fast it does it.** Each is rendered by one row of
/// `StatusFactTable` from the newest `NodeStatusFields`. A fact the status does not carry renders the
/// absent marker by name; one that does not apply to this node -- a consensus role on a node running no
/// consensus, a cache tier on one with none -- draws no line at all, rather than a line of markers claiming
/// the thing exists. **Whether it applies is asked of the newest status a sample CARRIED**
/// (`DashboardModel::carriedNodeStatus`), never of the newest sample's: a gap carries none, and asked there
/// *unknown* read as *does not apply*, so every outage took lines away and gave them back.
enum class StatusFact : std::uint8_t
{
    NodeId,     ///< Its minted identity.
    Components, ///< What it runs.
    Toolchains, ///< `serving 3 of 3`: the survey's state and its counts.
    Registrars, ///< `1 of 1, last 4s ago`: its registrations, and when one was last accepted.
    Consensus,  ///< Its scheduler role; only on a node running consensus.
    Leader,     ///< The leader it knows about; beside `Consensus`.
    Slots,      ///< In flight, available and registered, and what limits them; only on a node running a worker.
    CacheTier,  ///< Its cache tier's hit rate and fill; only on a node running one.
    Host,       ///< The machine: CPU busy, memory free, scratch free.
    Last,
};

/// One label and the fact written after it.
struct FactCell
{
    std::string_view label;                   ///< What the fact is.
    StatusFact fact { StatusFact::NodeId };   ///< The fact.
    std::span<BesideFigure const> figures {}; ///< Series the fact writes beside what the status says, in order.
};

/// One line of facts: one or two cells, a second one aligned across the lines of its block.
struct FactLine
{
    std::span<FactCell const> cells;        ///< The cells, left to right; the line is drawn when the first applies.
    Priority priority { Priority::Normal }; ///< When the line goes, for height.
};

/// Where a block of facts is drawn in a panel.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class FactPlace : std::uint8_t
{
    AboveRates, ///< Under the title, before the rates.
    BelowRates, ///< After every figure block.
};

/// Lines of facts drawn together, their columns aligned, with a blank before them.
struct FactBlock
{
    std::span<FactLine const> lines;           ///< The lines.
    FactPlace place { FactPlace::AboveRates }; ///< Where.
};

/// A whole panel.
///
/// **A table that does not fit vertically ends in `+N more`** rather than losing rows silently: it
/// shrinks, keeping its heading, before rows of its own priority are dropped.
/// One figure a panel draws as a band of its history chart, once the terminal has rows to spare.
///
/// **A row, so which figures grow and in what order is data** (#134): the chart bands the first of them first, and a
/// taller terminal gives it the next, then taller bands.
struct ChartRow
{
    std::string_view label; ///< What the band is.
    FigureSpec figure;      ///< Its figure: the series the band draws, its newest cell the figure beside it.
    /// What the band's top stands for, when the figure has a whole of its own -- `1.0` for a share; nullopt to scale
    /// the band to its own peak over the span it draws.
    std::optional<double> top {};
    /// How its bars are coloured on a pixel chart: the ramp only where a high share against a real whole is a
    /// warning, such as a machine's CPU or a cache's fill; plain otherwise, a hit rate included, where high is good.
    ChartPaint paint { ChartPaint::Plain };
};

struct PanelSpec
{
    /// The frame's title; for a panel whose title names its server, only the name before a session states one.
    std::string_view title;
    std::span<RateRow const> rates;              ///< The upper block.
    std::span<LevelRow const> levels;            ///< The lower block.
    std::span<TierColumn const> tierColumns;     ///< The tier block; empty for a panel without one.
    std::span<std::string_view const> tierNote;  ///< Lines under the tier block, when it is drawn.
    Priority tierPriority { Priority::Normal };  ///< When the tier table shrinks, then goes, for height.
    Priority tierNotePriority { Priority::Low }; ///< When the tier note lines go.
    Priority sourcePriority { Priority::High };  ///< When the source line goes.
    /// Whether the frame is as tall as the terminal, its source line on the last row.
    ///
    /// A frame that ends halfway down the screen reads as one that stopped drawing, and §5 draws the fleet
    /// panel's box the terminal's full height. The rows are padded above the source line, after the chart
    /// has grown into what it can use.
    bool fillsHeight { false };
    /// Whether the title names WHAT ANSWERED -- `PanelContext::server` -- rather than `title`.
    ///
    /// True for the cache panel, which a cache daemon and a compile node both serve (#1399): a node's cache
    /// titled `fastcached` names a process that is not there. A panel only one kind serves, or one whose
    /// subject is not a process (`fleet`), keeps its own.
    bool titleNamesServer { false };
    std::optional<DocumentSpec> document {};     ///< The fleet document block; nullopt for a panel without one.
    std::span<TitleFactRow const> titleFacts {}; ///< What the title bar states beside `title`, in reading order.
    std::span<FactBlock const> facts {};         ///< Blocks of status facts; empty for a panel that reads no status.
    /// The figures a history chart bands under the rates, in the order it grows; empty for a panel with none.
    ///
    /// The chart takes only rows the layout left over, after every other block has fitted: at §3's and §4's 80x24
    /// it is not there at all, and a taller terminal draws history rather than a frame half empty.
    std::span<ChartRow const> charts {};
    ChartGrowth chartGrowth {}; ///< How that chart grows.
};

/// How long an endpoint has served, as a title bar states it: `2d11:48`.
///
/// Days and then hours and minutes, always all three, so the width holds still from one frame to the
/// next and a restart reads as the number going back to `0d00:00` rather than as a change of format.
/// @param seconds The uptime.
/// @return The text, without the `up` a title bar puts in front of it.
[[nodiscard]] std::string UptimeText(std::uint64_t seconds);

/// A terminal size, in cells.
struct PanelSize
{
    std::size_t columns { 0 }; ///< Cells across.
    std::size_t rows { 0 };    ///< Lines down.
};

/// The smallest terminal @p spec is drawn in, rather than the minimum-size line.
///
/// Derived from the spec rather than written beside it: the width every essential piece needs at its
/// column widths plus the frame, and one line per essential row plus the frame -- a table counting as
/// its heading and its `+N more`. A figure wider than its column can still need more, and the view
/// then names what it needed.
/// @param spec The panel.
/// @param cellWidth How wide text is.
/// @return The minimum size.
[[nodiscard]] PanelSize MinimumPanelSize(PanelSpec const& spec, CellWidth cellWidth) noexcept;

/// Call @p visit with every machine key @p panel's rate, level and fact blocks name, in panel order.
/// @param panel The panel.
/// @param visit Called with each key.
template <typename Visit>
constexpr void ForEachFigureKey(PanelSpec const& panel, Visit const& visit)
{
    for (auto const& row: panel.rates)
    {
        visit(row.key);
        for (auto const& beside: row.beside)
            visit(beside.key);
        for (auto const& part: row.split)
            visit(part.key);
    }
    for (auto const& row: panel.levels)
    {
        visit(row.key);
        if (row.limit.has_value())
            visit(row.limitKey);
    }
    for (auto const& block: panel.facts)
        for (auto const& line: block.lines)
            for (auto const& cell: line.cells)
                for (auto const& figure: cell.figures)
                    visit(figure.key);
}

/// What joins a tier's name to a tier column's key in the machine name of that tier's figure.
inline constexpr std::string_view TierKeySeparator = "_";

/// Whether @p key is the machine name `TierFigureKey` gives @p column's figure in @p tier.
/// @param key A machine name.
/// @param tier A `StorageTierTable` name.
/// @param column A tier column's key.
/// @return True when @p key is `<tier>_<column>`.
[[nodiscard]] constexpr bool IsTierFigureKey(std::string_view key, std::string_view tier, std::string_view column) noexcept
{
    return key.size() == tier.size() + TierKeySeparator.size() + column.size() && key.starts_with(tier)
           && key.substr(tier.size(), TierKeySeparator.size()) == TierKeySeparator && key.ends_with(column);
}

/// Whether @p panel names every figure it draws for a program, once each.
///
/// **A figure's machine name is its own column, never derived from what a person reads**: a label
/// reworded for the screen must not rename what a script reads, and the two change for different
/// reasons. So every rate row, beside figure, level row, limit and tier column states a `key`, and
/// this is the check that none is missing and none repeats: the rate and level keys, beside
/// figures and limits included, are one namespace; the tier columns' keys are unique among
/// themselves, and no figure key spells a tier column's figure in any `StorageTierTable` tier
/// (`<tier>_<key>`), which is how a program meets them beside the others. A level row's
/// `limitKey` is named exactly when it has a limit. `static_assert`ed beside every panel, so a row
/// added without one fails the build on every compiler.
/// @param panel The panel.
/// @return True when the keys are whole.
[[nodiscard]] constexpr bool PanelKeysAreWhole(PanelSpec const& panel) noexcept
{
    auto whole = true;
    auto position = std::size_t { 0 };
    ForEachFigureKey(panel, [&panel, &whole, &position](std::string_view key) {
        whole = whole && !key.empty();
        auto earlier = std::size_t { 0 };
        ForEachFigureKey(panel, [&whole, &earlier, position, key](std::string_view other) {
            whole = whole && !(earlier < position && other == key);
            ++earlier;
        });
        ++position;
    });
    for (auto const& row: panel.levels)
        whole = whole && row.limit.has_value() == !row.limitKey.empty();
    for (auto const index: std::views::iota(std::size_t { 0 }, panel.tierColumns.size()))
    {
        whole = whole && !panel.tierColumns[index].key.empty();
        for (auto const earlier: std::views::iota(std::size_t { 0 }, index))
            whole = whole && panel.tierColumns[earlier].key != panel.tierColumns[index].key;
        for (auto const& tier: StorageTierTable)
            ForEachFigureKey(panel, [&whole, &panel, index, &tier](std::string_view key) {
                whole = whole && !IsTierFigureKey(key, tier.name, panel.tierColumns[index].key);
            });
    }
    return whole;
}

/// Everything a panel view is told rather than reads.
///
/// **The rung is a value here, decided once by whoever composed the session** (§9.12), and so is
/// the absent marker: the operator's `--absent`, resolved, and the same text on every rung (§9.6).
///
/// And so is how wide text is, for the same reason: the one width function the adapter layer
/// provides, never a count this view makes of its own. Required; a view is never constructed
/// without one.
struct PanelContext
{
    std::string absent {};   ///< What an absent figure reads as.
    std::string endpoint {}; ///< Where the samples are asked; empty when not stated.
    /// What answered, as a title names it (`RemoteKindSpec::product`); empty when not stated.
    std::string server {};
    std::optional<std::chrono::milliseconds> interval {}; ///< The sampling interval, when stated.
    CellWidth cellWidth { nullptr };                      ///< How many cells text occupies; must not be null.
    /// What draws an image on the Sixel rung, or null for a session with none; never owned.
    ISixelEncoder* sixel { nullptr };
    /// Which section of a fleet document the panel's table draws; a panel without a document block ignores it.
    Distributed::FleetSection section { Distributed::FleetSection::Machines };
    RenderRung rung { RenderRung::Ascii }; ///< What the frame is drawn with; last with `section`, so they pad nothing.
};

/// @p figure's value at every entry of @p history, oldest first, already scaled.
///
/// One element per entry, like `CounterRateSeries`, so every row's series lines up with every
/// other's. Absent wherever the figure cannot be claimed: a field the source does not carry, an
/// interval the fold did not measure, a counter that went down, or a ratio whose denominator is not
/// positive -- a cache that served no reads has no hit rate, rather than one of 0 %.
/// @param history The samples.
/// @param figure The figure.
/// @param tier A `StorageTierTable` name to read one tier's figure, or empty for the whole cache's.
/// @return The series.
[[nodiscard]] std::vector<std::optional<double>> FigureSeries(std::deque<HistoryEntry> const& history,
                                                              FigureSpec const& figure,
                                                              std::string_view tier = {});

/// The machine name of @p column's figure in @p tier: `<tier>_<column>`.
///
/// A program reads a tier's figures beside the panel's others, so they need names that cannot meet
/// one of those; `PanelKeysAreWhole` refuses a panel where one would.
/// @param tier A `StorageTierTable` name.
/// @param column A tier column's key.
/// @return The name.
[[nodiscard]] std::string TierFigureKey(std::string_view tier, std::string_view column);

/// The tiers @p spec draws a tier row for from @p reading, in `StorageTierTable` order.
///
/// **A tier the cache does not run has no row** (§9.5) -- not a row of absent markers, which would
/// claim the tier exists and reported nothing. Presence is the reading's own: a tier's statistics are
/// in `MetricsSnapshot::storageTiers` exactly when the cache runs it. One answer for the panel and the
/// piped record, so the two cannot disagree about which tiers exist.
/// @param spec The panel.
/// @param reading A reading.
/// @return The tiers' names; empty for a panel without a tier block.
[[nodiscard]] std::vector<std::string_view> TiersIn(PanelSpec const& spec, StatsReading const& reading);

/// The fleet section a keystroke switches a document panel's table to, from @p active.
///
/// **The sections a key walks are the ones the strip names**: the tabular rows of
/// `Distributed::FleetSectionTable`, in its order. `Tab` and `Right` step to the next, `Left` to the
/// previous, both wrapping; a digit `1`-`9` names the strip's tab in that position, and each tab's
/// letter -- the strip's `keys  m w l c t` -- names that tab. From a section the strip does not name,
/// the first step lands on its first or last tab. A key naming no section, or a digit past the last
/// tab, switches nothing.
/// @param active The section drawn now.
/// @param keys The bytes the keystroke delivered.
/// @return The section to draw, or nullopt when the key names none.
[[nodiscard]] std::optional<Distributed::FleetSection> SectionForKey(Distributed::FleetSection active,
                                                                     std::string_view keys);

/// Draws any `PanelSpec` on the rung it was given, laid out for the model's current size.
///
/// **Responsive to `columns x rows`** (#134 decision 5), which arrive as `Resize` events: no line is
/// wider than `columns` cells and no frame taller than `rows`. What does not fit goes in `Priority`
/// order; a figure is never cut; below `MinimumPanelSize` the frame is one line naming the minimum
/// and the current size. The mockups in #134 are one size, not the layout.
class PanelView final: public IDashboardView
{
  public:
    /// @param spec The panel; must outlive the view.
    /// @param context The rung, the absent marker and the session facts.
    PanelView(PanelSpec const& spec, PanelContext context);

    /// @param model What is known.
    /// @return The frame, with a history chart's images placed over it on the Sixel rung.
    [[nodiscard]] DashboardFrame PlacedFrame(DashboardModel const& model) override;

    /// Act on a key for a document panel's table.
    ///
    /// A section key (`SectionForKey`) switches the table, and the new section starts at its top with no
    /// filter. `PgDn` and `PgUp` scroll it by the rows the last frame showed, `Home` goes back to the top.
    /// `/` starts typing a filter: while typed, printable text is appended, `Backspace` takes the last
    /// character, `Enter` keeps the filter and `Esc` clears it. A row is drawn when any of its written
    /// cells contains the filter, ignoring ASCII case. A panel without a document block acts on no key.
    /// @param keys The bytes the keystroke delivered.
    /// @return Whether the next frame differs.
    [[nodiscard]] bool Key(std::string_view keys) override;

    /// @return True while a filter is being typed.
    [[nodiscard]] bool CapturesText() const noexcept override;

  private:
    PanelSpec const* _spec;
    RungGlyphs const* _glyphs;
    PanelContext _context;

    /// The table's rows skipped at the top; clamped by the frame that draws it.
    std::size_t _scroll { 0 };

    /// How many of the table's rows the last frame showed: the step `PgDn` and `PgUp` take.
    std::size_t _page { 1 };

    /// The filter the table's rows are matched against; empty for none.
    std::string _filter {};

    /// Cells kept for the widest beside text any frame has written at the current width.
    ///
    /// Only grows while the width holds, so a figure gaining a digit narrows the trends once rather
    /// than making them jump from frame to frame; a new width starts it again, since a reserve
    /// measured at another width can be wider than this one allows.
    std::size_t _besideReserve { 0 };

    /// The content width `_besideReserve` was measured at.
    std::size_t _reserveColumns { 0 };

    /// Whether the filter is being typed; last, so it pads nothing between 8-aligned members.
    bool _typingFilter { false };
};

} // namespace FastCache::Cli
