// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanel.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <ranges>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    using Series = std::vector<std::optional<double>>;

    /// Which member of `FieldNames` one source's name is.
    struct OriginField
    {
        StatsOrigin origin;                  ///< The enumerator this row describes.
        std::string_view FieldNames::* name; ///< Its member.
    };

    /// One row per `StatsOrigin`, in enumerator order, so a fourth source is a member and a row
    /// rather than a new arm somewhere a name is looked up.
    constexpr EnumTable<StatsOrigin, OriginField> OriginFieldTable { {
        { .origin = StatsOrigin::Metrics, .name = &FieldNames::metrics },
        { .origin = StatsOrigin::NodeMetrics, .name = &FieldNames::nodeMetrics },
        { .origin = StatsOrigin::Info, .name = &FieldNames::info },
    } };

    static_assert(RowsInEnumeratorOrder(OriginFieldTable, &OriginField::origin),
                  "OriginFieldTable must hold one row per StatsOrigin, in enumerator order");

    /// The series name @p names gives in @p origin, labelled for one tier when @p label is set.
    /// @param names The names.
    /// @param origin The source.
    /// @param label A tier's name, or empty.
    /// @return The name, or empty where the source does not carry the field.
    [[nodiscard]] std::string ResolvedName(FieldNames const& names, StatsOrigin origin, std::string_view label)
    {
        auto const base = NameIn(names, origin);
        if (base.empty())
            return {};
        return label.empty() ? std::string { base } : TierSeriesName(base, label);
    }

    /// Two series combined cell by cell; a cell is absent wherever either operand is.
    /// @param left The first operand.
    /// @param right The second operand; the same length.
    /// @param combine What to make of two present values; nullopt when they make nothing.
    /// @return The combined series.
    [[nodiscard]] Series Pairwise(Series const& left, Series const& right, std::optional<double> (*combine)(double, double))
    {
        auto combined = Series(left.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, std::min(left.size(), right.size())))
        {
            auto const& one = left[index];
            auto const& two = right[index];
            if (one.has_value() && two.has_value())
                combined[index] = combine(*one, *two);
        }
        return combined;
    }

    /// `left / (left + right)`, or nothing where that sum is not positive.
    /// @param left The part.
    /// @param right The rest.
    /// @return The proportion.
    [[nodiscard]] std::optional<double> Proportion(double left, double right) noexcept
    {
        auto const whole = left + right;
        return whole > 0.0 ? std::optional<double> { left / whole } : std::nullopt;
    }

    /// `left / right`, or nothing where @p right is not positive.
    /// @param left The dividend.
    /// @param right The divisor.
    /// @return The quotient.
    [[nodiscard]] std::optional<double> Quotient(double left, double right) noexcept
    {
        return right > 0.0 ? std::optional<double> { left / right } : std::nullopt;
    }

    /// `left + right`.
    /// @param left One addend.
    /// @param right The other.
    /// @return The sum.
    [[nodiscard]] std::optional<double> Sum(double left, double right) noexcept
    {
        return left + right;
    }

    /// Each entry's reading of one field.
    /// @param history The samples.
    /// @param name The field; empty yields an absent series.
    /// @return The series.
    [[nodiscard]] Series Levels(std::deque<HistoryEntry> const& history, std::string const& name)
    {
        auto series = Series(history.size());
        if (name.empty())
            return series;
        for (auto const index: std::views::iota(std::size_t { 0 }, history.size()))
            series[index] = NumberIn(history[index].reading, name);
        return series;
    }

    /// Each entry's rate of one counter.
    /// @param history The samples.
    /// @param name The counter; empty yields an absent series.
    /// @return The series.
    [[nodiscard]] Series Rates(std::deque<HistoryEntry> const& history, std::string const& name)
    {
        return name.empty() ? Series(history.size()) : CounterRateSeries(history, name);
    }

    /// How one `FigureSource` is computed.
    struct FigureSourceSpec
    {
        FigureSource source; ///< The enumerator this row describes.
        /// The computation, over the resolved primary and second field names.
        Series (*compute)(std::deque<HistoryEntry> const& history, std::string const& field, std::string const& other);
    };

    /// One row per `FigureSource`, in enumerator order.
    constexpr EnumTable<FigureSource, FigureSourceSpec> FigureSourceTable { {
        { .source = FigureSource::Level,
          .compute = [](std::deque<HistoryEntry> const& history,
                        std::string const& field,
                        std::string const& /*other*/) { return Levels(history, field); } },
        { .source = FigureSource::LevelRatio,
          .compute =
              [](std::deque<HistoryEntry> const& history, std::string const& field, std::string const& other) {
                  return Pairwise(Levels(history, field), Levels(history, other), &Proportion);
              } },
        { .source = FigureSource::Rate,
          .compute =
              [](std::deque<HistoryEntry> const& history, std::string const& field, std::string const& other) {
                  // A second counter is an ADDEND only when named: `ops/sec` is gets plus sets. An
                  // unnamed one must not turn the rate absent, so it is not paired at all.
                  return other.empty() ? Rates(history, field)
                                       : Pairwise(Rates(history, field), Rates(history, other), &Sum);
              } },
        { .source = FigureSource::RateRatio,
          .compute =
              [](std::deque<HistoryEntry> const& history, std::string const& field, std::string const& other) {
                  return Pairwise(Rates(history, field), Rates(history, other), &Proportion);
              } },
        { .source = FigureSource::RateQuotient,
          .compute =
              [](std::deque<HistoryEntry> const& history, std::string const& field, std::string const& other) {
                  return Pairwise(Rates(history, field), Rates(history, other), &Quotient);
              } },
    } };

    static_assert(RowsInEnumeratorOrder(FigureSourceTable, &FigureSourceSpec::source),
                  "FigureSourceTable must hold one row per FigureSource, in enumerator order");

    /// The frame width used before the terminal has reported one: §3's layout floor.
    constexpr auto DefaultColumns = std::size_t { 80 };

    /// Columns a rate row's label takes, after its indent.
    constexpr auto LabelColumns = std::size_t { 16 };

    /// Columns a rate row's figure is right-aligned into.
    constexpr auto FigureColumns = std::size_t { 9 };

    /// Columns a level row's label takes, after its indent.
    constexpr auto LevelLabelColumns = std::size_t { 12 };

    /// Cells in a level row's gauge.
    constexpr auto GaugeCells = std::size_t { 20 };

    /// Columns a tier row's name takes, after its indent.
    constexpr auto TierNameColumns = std::size_t { 8 };

    /// Columns each tier figure is right-aligned into.
    constexpr auto TierFigureColumns = std::size_t { 12 };

    /// The indent every content line starts with.
    constexpr std::string_view Indent = "  ";

    /// What one frame is drawn from, looked up once.
    struct FrameInputs
    {
        DashboardModel const* model;       ///< What is known.
        std::optional<StatsOrigin> origin; ///< Whose names apply; nullopt before any reading.
        RungGlyphs const* glyphs;          ///< What to draw with.
        std::string_view absent;           ///< The absent marker.
    };

    /// @p figure's series for this frame, or an absent series when no source applies.
    /// @param in The frame's inputs.
    /// @param figure The figure.
    /// @param label A tier label, or empty.
    /// @return The series; never empty while the history is not.
    [[nodiscard]] Series SeriesFor(FrameInputs const& in, FigureSpec const& figure, std::string_view label)
    {
        if (!in.origin.has_value())
            return Series(in.model->history.size());
        return FigureSeries(in.model->history, figure, *in.origin, label);
    }

    /// The newest cell of @p series.
    /// @param series The series.
    /// @return Its last value, or nullopt for an empty series.
    [[nodiscard]] std::optional<double> Newest(Series const& series) noexcept
    {
        return series.empty() ? std::nullopt : series.back();
    }

    /// One figure's text, with its suffix when it is present.
    /// @param in The frame's inputs.
    /// @param figure The figure.
    /// @param value Its value.
    /// @return The text.
    [[nodiscard]] std::string FigureText(FrameInputs const& in, FigureSpec const& figure, std::optional<double> value)
    {
        auto text = FormatFigure(value, figure.format, in.absent);
        if (value.has_value() && std::isfinite(*value))
            text += figure.suffix;
        return text;
    }

    /// The newest @p cells of @p series, right-aligned, with the cells before the session began
    /// drawn as no reading -- which is what they were.
    /// @param series The series.
    /// @param cells How many cells the sparkline has.
    /// @return The window.
    [[nodiscard]] Series Window(Series const& series, std::size_t cells)
    {
        auto window = Series(cells);
        auto const shown = std::min(cells, series.size());
        std::ranges::copy(series | std::views::drop(series.size() - shown),
                          window.begin() + static_cast<std::ptrdiff_t>(cells - shown));
        return window;
    }

    /// What a rate row writes after its figure and trend: its beside figures, then its note.
    /// @param in The frame's inputs.
    /// @param row The row.
    /// @return The text, each piece led by three spaces; empty when the row has neither.
    [[nodiscard]] std::string BesideText(FrameInputs const& in, RateRow const& row)
    {
        auto text = std::string {};
        for (auto const& beside: row.beside)
        {
            auto const value = Newest(SeriesFor(in, beside.figure, {}));
            text += "   ";
            if (!beside.before.empty())
                text += std::string { beside.before } + " ";
            text += FigureText(in, beside.figure, value);
            if (!beside.after.empty())
                text += " " + std::string { beside.after };
        }
        if (!row.note.empty())
            text += "   " + std::string { row.note };
        return text;
    }

    /// One upper-block row.
    /// @param in The frame's inputs.
    /// @param row The row.
    /// @param sparkCells How many cells a sparkline gets on this frame.
    /// @param beside What `BesideText` wrote for the row.
    /// @return The line.
    [[nodiscard]] std::string RateLine(FrameInputs const& in,
                                       RateRow const& row,
                                       std::size_t sparkCells,
                                       std::string_view beside)
    {
        auto const series = SeriesFor(in, row.figure, {});
        auto line = std::string { Indent } + FitRight(row.label, LabelColumns)
                    + AlignRight(FigureText(in, row.figure, Newest(series)), FigureColumns);

        // The trend is the only column a rung may drop, and it drops the column rather than leaving
        // a hole where the chart would have been (§10): `Sparkline` answers empty on such a rung.
        if (row.trend == Trend::Drawn && sparkCells > 0)
        {
            auto const spark = Sparkline(Window(series, sparkCells), *in.glyphs);
            if (!spark.empty())
                line += "  " + spark;
        }
        return line + std::string { beside };
    }

    /// One lower-block row.
    /// @param in The frame's inputs.
    /// @param row The row.
    /// @return The line.
    [[nodiscard]] std::string LevelLine(FrameInputs const& in, LevelRow const& row)
    {
        auto const value = Newest(SeriesFor(in, row.value, {}));
        auto line = std::string { Indent } + FitRight(row.label, LevelLabelColumns) + FigureText(in, row.value, value);

        if (row.limit.has_value())
        {
            auto const limit = Newest(SeriesFor(in, *row.limit, {}));
            line += " / " + FigureText(in, *row.limit, limit);
            // A limit of zero is `InMemoryLruStorage`'s spelling of UNBOUNDED, so there is no
            // proportion to draw -- and a gauge left empty would claim the store is idle.
            auto const fraction = (value.has_value() && limit.has_value()) ? Quotient(*value, *limit) : std::nullopt;
            if (fraction.has_value())
                line += "  " + Gauge(*fraction, GaugeCells, *in.glyphs);
            line += "  " + FormatFigure(fraction, FigureFormat::Percent, in.absent);
        }
        if (!row.note.empty())
            line += "  " + std::string { row.note };
        return line;
    }

    /// The tier block: a heading and one row per tier the newest reading carries, or nothing.
    ///
    /// **A tier the cache does not run contributes no row** (§9.5) -- not a row of absent markers,
    /// which would claim the tier exists and reported nothing. Presence is asked of the NEWEST
    /// reading's first column, the same series the daemon omits entirely for a tier it lacks.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @return The lines.
    [[nodiscard]] std::vector<std::string> TierLines(FrameInputs const& in, PanelSpec const& spec)
    {
        auto lines = std::vector<std::string> {};
        if (spec.tierColumns.empty() || !in.origin.has_value() || !in.model->latest.has_value())
            return lines;

        auto const& presence = spec.tierColumns.front().figure;
        auto heading = std::string { Indent } + FitRight("tier", TierNameColumns);
        for (auto const& column: spec.tierColumns)
            heading += AlignRight(column.header, TierFigureColumns);

        for (auto const& tier: StorageTierTable)
        {
            auto const name = ResolvedName(presence.field, *in.origin, tier.name);
            if (name.empty() || FindField(*in.model->latest, name) == nullptr)
                continue;
            if (lines.empty())
                lines.push_back(heading);
            auto row = std::string { Indent } + FitRight(tier.name, TierNameColumns);
            for (auto const& column: spec.tierColumns)
            {
                auto const value = Newest(SeriesFor(in, column.figure, tier.name));
                row += AlignRight(FigureText(in, column.figure, value), TierFigureColumns);
            }
            lines.push_back(std::move(row));
        }

        if (!lines.empty())
            for (auto const note: spec.tierNote)
                lines.push_back(std::string { Indent } + std::string { note });
        return lines;
    }

    /// The last line: which source answered, how many samples, how many of them were gaps.
    /// @param in The frame's inputs.
    /// @return The line.
    [[nodiscard]] std::string SourceLine(FrameInputs const& in)
    {
        auto const& model = *in.model;
        auto const gaps =
            std::ranges::count_if(model.history, [](HistoryEntry const& entry) { return !entry.reading.has_value(); });
        auto const source = model.latestStamp.has_value() ? std::string_view { model.latestStamp->source } : in.absent;
        return std::format("{}source  {}   {} samples, {} gap{}", Indent, source, model.samples, gaps, gaps == 1 ? "" : "s");
    }

} // namespace

std::string_view NameIn(FieldNames const& names, StatsOrigin origin) noexcept
{
    return names.*(OriginFieldTable[static_cast<std::size_t>(origin)].name);
}

std::string TierSeriesName(std::string_view base, std::string_view tier)
{
    return std::format("{}{{tier=\"{}\"}}", base, tier);
}

std::optional<StatsOrigin> OriginOf(DashboardModel const& model) noexcept
{
    if (!model.latestStamp.has_value())
        return std::nullopt;
    auto const* row = FindIfOrNull(StatsOriginTable,
                                   [&model](StatsOriginSpec const& spec) { return spec.name == model.latestStamp->source; });
    return row == nullptr ? std::nullopt : std::optional<StatsOrigin> { row->origin };
}

std::vector<std::optional<double>> FigureSeries(std::deque<HistoryEntry> const& history,
                                                FigureSpec const& figure,
                                                StatsOrigin origin,
                                                std::string_view label)
{
    auto series = FigureSourceTable[static_cast<std::size_t>(figure.source)].compute(
        history, ResolvedName(figure.field, origin, label), ResolvedName(figure.other, origin, label));
    for (auto& cell: series)
        if (cell.has_value())
            *cell *= figure.scale;
    return series;
}

PanelView::PanelView(PanelSpec const& spec, PanelContext context):
    _spec { &spec },
    _glyphs { &GlyphsFor(context.rung) },
    _context { std::move(context) }
{
}

std::string PanelView::Frame(DashboardModel const& model)
{
    auto const in = FrameInputs { .model = &model, .origin = OriginOf(model), .glyphs = _glyphs, .absent = _context.absent };
    auto const width = model.columns > 0 ? static_cast<std::size_t>(model.columns) : DefaultColumns;
    auto const inside = ContentColumns(width);

    // The trend gets whatever the widest beside text leaves, so a figure is never cut to make room
    // for a chart -- and a wider terminal gives the difference to the trend, which is §3's rule.
    // Only rows that draw a trend are measured: a row without one writes its text where the chart
    // would have been. The reserve only GROWS, so a figure gaining a digit narrows the trend once
    // rather than making it jump back and forth from one frame to the next.
    auto besides = std::vector<std::string> {};
    for (auto const& row: _spec->rates)
    {
        besides.push_back(BesideText(in, row));
        if (row.trend == Trend::Drawn)
            _besideReserve = std::max(_besideReserve, DisplayWidth(besides.back()));
    }
    auto const fixed = Indent.size() + LabelColumns + FigureColumns + 2 + _besideReserve;
    auto const sparkCells = inside > fixed ? inside - fixed : 0;

    auto lines = std::vector<std::string> { std::string {} };
    for (auto const index: std::views::iota(std::size_t { 0 }, _spec->rates.size()))
        lines.push_back(RateLine(in, _spec->rates[index], sparkCells, besides[index]));
    if (!_spec->levels.empty())
    {
        lines.emplace_back();
        for (auto const& row: _spec->levels)
            lines.push_back(LevelLine(in, row));
    }
    if (auto tiers = TierLines(in, *_spec); !tiers.empty())
    {
        lines.emplace_back();
        std::ranges::move(tiers, std::back_inserter(lines));
    }
    lines.emplace_back();
    lines.push_back(SourceLine(in));

    // A frame taller than the terminal scrolls, and every later frame is then drawn one screen
    // lower than the one before. The lines past the bottom are dropped instead.
    if (model.rows > 2 && lines.size() > static_cast<std::size_t>(model.rows) - 2)
        lines.resize(static_cast<std::size_t>(model.rows) - 2);

    auto title = std::string { _spec->title };
    if (!_context.endpoint.empty())
        title += "  " + _context.endpoint;
    if (_context.interval.has_value())
        title += std::format("  every {}s", std::chrono::duration<double> { *_context.interval }.count());
    return Cli::Frame(title, lines, width, *_glyphs);
}

} // namespace FastCache::Cli
