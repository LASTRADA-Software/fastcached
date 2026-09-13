// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardFrame.hpp"
#include "DashboardGlyphs.hpp"
#include "DashboardLoop.hpp"
#include "DashboardRung.hpp"
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

/// A field's name in each stats source, empty where that source does not carry it.
///
/// Per source because the three vocabularies are different: `/metrics` and `NodeMetrics` share the
/// catalogue's names, while `INFO` spells its seven fields its own way. A figure a source does not
/// carry renders absent -- a 7-field reading is not a 102-field reading with 95 zeroes.
struct FieldNames
{
    std::string_view metrics {};     ///< `/metrics`.
    std::string_view nodeMetrics {}; ///< The node's `NodeMetrics` verb.
    std::string_view info {};        ///< RESP `INFO`.
};

/// @p names' spelling in @p origin.
/// @param names The names.
/// @param origin A source below `Last`.
/// @return The name, empty where the source does not carry the field.
[[nodiscard]] std::string_view NameIn(FieldNames const& names, StatsOrigin origin) noexcept;

/// How a figure is made from its fields.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class FigureSource : std::uint8_t
{
    Level,        ///< `field` as read. A reading of now.
    LevelRatio,   ///< `field / (field + other)` as read: a since-start proportion.
    Rate,         ///< How fast `field` rose, plus how fast `other` rose when `other` is named.
    RateRatio,    ///< `rate(field) / (rate(field) + rate(other))`: a proportion over each interval.
    RateQuotient, ///< `rate(field) / rate(other)`: a `_sum` over its `_count`, over each interval.
    Last,
};

/// One figure: which fields, combined how, written how.
///
/// The two byte-wide members are last, in one run, so the struct carries no padding between
/// 8-aligned members.
struct FigureSpec
{
    FieldNames field {};                         ///< The primary field.
    FieldNames other {};                         ///< The second operand; see `FigureSource`.
    double scale { 1.0 };                        ///< Applied to every value: 60 turns a rate per second into one per minute.
    std::string_view suffix {};                  ///< Written after a PRESENT value only, such as `/s`.
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
    std::string_view key;                   ///< Its machine name; see `PanelKeysAreWhole`.
    std::string_view before {};             ///< Words before the value.
    FigureSpec figure {};                   ///< The value.
    std::string_view after {};              ///< Words after the value.
    Priority priority { Priority::Normal }; ///< When it is dropped for width.
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
    std::string_view label;                    ///< What the figure is.
    std::string_view key;                      ///< Its machine name; see `PanelKeysAreWhole`.
    FigureSpec figure;                         ///< The figure.
    std::span<BesideFigure const> beside {};   ///< Figures written after it.
    std::string_view note {};                  ///< Words written after those.
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

/// How a panel draws the fleet document a `fleet` reading carries (#134 §5).
///
/// **No column list lives here, or anywhere in this client** (#1320): a section's columns are the
/// header line the leader sent, walked as it arrived. So what the layout knows about a table it
/// cannot name ahead of time is POSITIONAL, and stated here as data: the first column says which row
/// a line is and is `Essential`, and every later column shares `columnPriority` -- among equals
/// `FitPieces` drops the rightmost first, so columns go from the right.
///
/// **The chart is the Sixel rung's alone** (#134's decision on Sixel): on that rung, with a cell size the
/// terminal reported and an encoder to draw it, the first `FleetChartMetrics` row is drawn per machine
/// across the history as one image over `chartCellsHigh` rows of blank cells, placed in the frame and
/// dropped for height at `chartPriority` like any other item. On every other rung there is no chart
/// row at all -- not blank rows, not a glyph imitation.
///
/// The headline tiles are the `kpi` section's rows, in `Distributed::FleetKpiKeys()` order and under
/// those keys: the page's labels are prose beside its own table and are not restated here. Within a
/// tile the denominator goes before the tile does, and tiles that do not fit the width at all are
/// not drawn -- they are not essential, so they never turn a frame into the minimum-size line.
struct DocumentSpec
{
    std::size_t chartCellsHigh { 6 };               ///< Rows of cells the Sixel chart covers.
    std::size_t chartMinimumCells { 24 };           ///< The fewest cells across the chart is drawn in; narrower, it goes.
    Priority tilePriority { Priority::Normal };     ///< When a line of tiles goes, for height.
    Priority stripPriority { Priority::Low };       ///< When the section strip goes, for height.
    Priority stripTabPriority { Priority::Low };    ///< When a tab other than the active one goes, for width.
    Priority tablePriority { Priority::Essential }; ///< When the section's table shrinks to `+N more`, and goes.
    Priority columnPriority { Priority::Normal };   ///< When a column after the first goes, for width.
    Priority chartPriority { Priority::Low };       ///< When the Sixel chart goes, for height.
};

/// A whole panel.
///
/// **A table that does not fit vertically ends in `+N more`** rather than losing rows silently: it
/// shrinks, keeping its heading, before rows of its own priority are dropped.
struct PanelSpec
{
    std::string_view title;                      ///< The frame's title.
    std::span<RateRow const> rates;              ///< The upper block.
    std::span<LevelRow const> levels;            ///< The lower block.
    std::span<TierColumn const> tierColumns;     ///< The tier block; empty for a panel without one.
    std::span<std::string_view const> tierNote;  ///< Lines under the tier block, when it is drawn.
    Priority tierPriority { Priority::Normal };  ///< When the tier table shrinks, then goes, for height.
    Priority tierNotePriority { Priority::Low }; ///< When the tier note lines go.
    Priority sourcePriority { Priority::High };  ///< When the source line goes.
    std::optional<DocumentSpec> document {};     ///< The fleet document block; nullopt for a panel without one.
};

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

/// Call @p visit with every machine key @p panel's rate and level blocks name, in panel order.
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
    }
    for (auto const& row: panel.levels)
    {
        visit(row.key);
        if (row.limit.has_value())
            visit(row.limitKey);
    }
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
    std::string absent {};                                ///< What an absent figure reads as.
    std::string endpoint {};                              ///< Where the samples are asked; empty when not stated.
    std::optional<std::chrono::milliseconds> interval {}; ///< The sampling interval, when stated.
    CellWidth cellWidth { nullptr };                      ///< How many cells text occupies; must not be null.
    /// What draws an image on the Sixel rung, or null for a session with none; never owned.
    ISixelEncoder* sixel { nullptr };
    /// Which section of a fleet document the panel's table draws; a panel without a document block ignores it.
    Distributed::FleetSection section { Distributed::FleetSection::Machines };
    RenderRung rung { RenderRung::Ascii }; ///< What the frame is drawn with; last with `section`, so they pad nothing.
};

/// The source a model's newest reading came from.
/// @param model What is known.
/// @return The source, or nullopt before any reading or for a source this client does not name.
[[nodiscard]] std::optional<StatsOrigin> OriginOf(DashboardModel const& model) noexcept;

/// @p figure's value at every entry of @p history, oldest first, already scaled.
///
/// One element per entry, like `CounterRateSeries`, so every row's series lines up with every
/// other's. Absent wherever the figure cannot be claimed: a field the source does not carry, an
/// interval the fold did not measure, a counter that went down, or a ratio whose denominator is not
/// positive -- a cache that served no reads has no hit rate, rather than one of 0 %.
/// @param history The samples.
/// @param figure The figure.
/// @param origin The source whose names apply.
/// @param label A `tier` label value to select one tier's series, or empty for the unlabelled one.
/// @return The series.
[[nodiscard]] std::vector<std::optional<double>> FigureSeries(std::deque<HistoryEntry> const& history,
                                                              FigureSpec const& figure,
                                                              StatsOrigin origin,
                                                              std::string_view label);

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
/// claim the tier exists and reported nothing. Presence is asked of the reading's series for the
/// FIRST tier column, which the daemon omits entirely for a tier it lacks. One answer for the panel
/// and the piped record, so the two cannot disagree about which tiers exist.
/// @param spec The panel.
/// @param reading A reading.
/// @param origin The source it came from, whose names apply.
/// @return The tiers' names; empty for a panel without a tier block or a source that names none.
[[nodiscard]] std::vector<std::string_view> TiersIn(PanelSpec const& spec, Value const& reading, StatsOrigin origin);

/// The series name a labelled per-tier sample is exported under.
/// @param base The unlabelled series name.
/// @param tier The tier's `StorageTierTable` name.
/// @return `base{tier="<tier>"}`.
[[nodiscard]] std::string TierSeriesName(std::string_view base, std::string_view tier);

/// The fleet section a keystroke switches a document panel's table to, from @p active.
///
/// **The sections a key walks are the ones the strip names**: the tabular rows of
/// `Distributed::FleetSectionTable`, in its order. `Tab` and `Right` step to the next, `Left` to the
/// previous, both wrapping; a digit `1`-`9` names the strip's tab in that position. From a section
/// the strip does not name, the first step lands on its first or last tab. A key naming no section,
/// or a digit past the last tab, switches nothing.
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
    /// @return The frame, with the fleet chart placed over it on the Sixel rung.
    [[nodiscard]] DashboardFrame PlacedFrame(DashboardModel const& model) override;

    /// Switch a document panel's table to the section @p keys names (`SectionForKey`).
    ///
    /// A panel without a document block has no sections and acts on no key.
    /// @param keys The bytes the keystroke delivered.
    /// @return Whether the section changed.
    [[nodiscard]] bool Key(std::string_view keys) override;

  private:
    PanelSpec const* _spec;
    RungGlyphs const* _glyphs;
    PanelContext _context;

    /// Cells kept for the widest beside text any frame has written at the current width.
    ///
    /// Only grows while the width holds, so a figure gaining a digit narrows the trends once rather
    /// than making them jump from frame to frame; a new width starts it again, since a reserve
    /// measured at another width can be wider than this one allows.
    std::size_t _besideReserve { 0 };

    /// The content width `_besideReserve` was measured at.
    std::size_t _reserveColumns { 0 };
};

} // namespace FastCache::Cli
