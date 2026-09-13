// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardGlyphs.hpp"
#include "DashboardLoop.hpp"
#include "DashboardRung.hpp"
#include "StatsSource.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// A figure written beside a row's own, with words around it.
struct BesideFigure
{
    std::string_view before {}; ///< Words before the value.
    FigureSpec figure {};       ///< The value.
    std::string_view after {};  ///< Words after the value.
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
struct RateRow
{
    std::string_view label;                  ///< What the figure is.
    FigureSpec figure;                       ///< The figure.
    std::span<BesideFigure const> beside {}; ///< Figures written after it.
    std::string_view note {};                ///< Words written after those.
    Trend trend { Trend::Drawn };            ///< Whether a sparkline is drawn; last, so it pads nothing.
};

/// One row of the lower block: a reading of now, optionally against its limit.
struct LevelRow
{
    std::string_view label;             ///< What the figure is.
    FigureSpec value;                   ///< The reading.
    std::optional<FigureSpec> limit {}; ///< What bounds it, drawn with a gauge and a percentage.
    std::string_view note {};           ///< Words written after it.
};

/// One column of the tier block.
///
/// Its fields name the UNLABELLED series; the view adds each tier's label, walking
/// `StorageTierTable`, so no tier is spelled here.
struct TierColumn
{
    std::string_view header; ///< The column's heading.
    FigureSpec figure;       ///< The figure, per tier.
};

/// A whole panel.
struct PanelSpec
{
    std::string_view title;                     ///< The frame's title.
    std::span<RateRow const> rates;             ///< The upper block.
    std::span<LevelRow const> levels;           ///< The lower block.
    std::span<TierColumn const> tierColumns;    ///< The tier block; empty for a panel without one.
    std::span<std::string_view const> tierNote; ///< Lines under the tier block, when it is drawn.
};

/// Everything a panel view is told rather than reads.
///
/// **The rung is a value here, decided once by whoever composed the session** (§9.12), and so is
/// the absent marker: the operator's `--absent`, resolved, and the same text on every rung (§9.6).
struct PanelContext
{
    std::string absent {};                                ///< What an absent figure reads as.
    std::string endpoint {};                              ///< Where the samples are asked; empty when not stated.
    std::optional<std::chrono::milliseconds> interval {}; ///< The sampling interval, when stated.
    RenderRung rung { RenderRung::Ascii };                ///< What the frame is drawn with; last, so it pads nothing.
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

/// The series name a labelled per-tier sample is exported under.
/// @param base The unlabelled series name.
/// @param tier The tier's `StorageTierTable` name.
/// @return `base{tier="<tier>"}`.
[[nodiscard]] std::string TierSeriesName(std::string_view base, std::string_view tier);

/// Draws any `PanelSpec` on the rung it was given.
class PanelView final: public IDashboardView
{
  public:
    /// @param spec The panel; must outlive the view.
    /// @param context The rung, the absent marker and the session facts.
    PanelView(PanelSpec const& spec, PanelContext context);

    /// @param model What is known.
    /// @return The frame.
    [[nodiscard]] std::string Frame(DashboardModel const& model) override;

  private:
    PanelSpec const* _spec;
    RungGlyphs const* _glyphs;
    PanelContext _context;

    /// Columns kept for the widest beside text any frame has written; only ever grows.
    std::size_t _besideReserve { 0 };
};

} // namespace FastCache::Cli
