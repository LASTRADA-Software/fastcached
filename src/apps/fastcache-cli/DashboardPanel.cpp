// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanel.hpp"
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/NumericText.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <ranges>
#include <span>
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

    /// Lines a frame spends on its top and bottom edges.
    constexpr auto FrameRows = std::size_t { 2 };

    /// Cells a frame spends across: its two edges and the blank column before the right one, which
    /// is what `ContentColumns` takes away.
    constexpr auto FrameColumns = std::size_t { 3 };

    /// Columns a rate row's label takes, after its indent.
    constexpr auto LabelColumns = std::size_t { 16 };

    /// Columns a rate row's figure is right-aligned into, at the least.
    constexpr auto FigureColumns = std::size_t { 9 };

    /// The fewest cells a trend narrows to before it is dropped rather than drawn narrower.
    constexpr auto MinimumTrendCells = std::size_t { 8 };

    /// Columns a level row's label takes, after its indent.
    constexpr auto LevelLabelColumns = std::size_t { 12 };

    /// Columns a level row's reading is right-aligned into, at the least.
    constexpr auto LevelFigureColumns = std::size_t { 12 };

    /// Cells in a level row's gauge.
    constexpr auto GaugeCells = std::size_t { 20 };

    /// Columns a tier row's name takes, after its indent.
    constexpr auto TierNameColumns = std::size_t { 8 };

    /// Columns each tier figure is right-aligned into, at the least.
    constexpr auto TierFigureColumns = std::size_t { 12 };

    /// The indent every content line starts with.
    constexpr std::string_view Indent = "  ";

    /// What separates a figure from the trend after it.
    constexpr std::string_view TrendGap = "  ";

    /// What separates one beside piece from the one before it.
    constexpr std::string_view PieceGap = "   ";

    /// What one frame is drawn from, looked up once.
    struct FrameInputs
    {
        DashboardModel const* model;       ///< What is known.
        std::optional<StatsOrigin> origin; ///< Whose names apply; nullopt before any reading.
        RungGlyphs const* glyphs;          ///< What to draw with.
        std::string_view absent;           ///< The absent marker.
        CellWidth cellWidth;               ///< How wide text is.
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

    /// Whether @p piece is kept longer than @p other: a lower enumerator is kept longer.
    /// @param piece One priority.
    /// @param other The other.
    /// @return True when @p piece outlasts @p other.
    [[nodiscard]] constexpr bool OutlastsOrTies(Priority piece, Priority other) noexcept
    {
        return std::to_underlying(piece) <= std::to_underlying(other);
    }

    /// One droppable part of a line.
    struct Piece
    {
        std::string text {};                    ///< What it draws; empty for the trend, drawn once its width is known.
        Priority priority { Priority::Normal }; ///< When it goes.
        std::size_t slot { 0 };                 ///< Which column it is, for a table whose rows follow its heading.
        bool trend { false };                   ///< Whether this is the trend, which narrows before it goes.
    };

    /// The pieces of a line that fit in @p budget cells.
    ///
    /// **The one place a piece is dropped for width**, so the drop order is `Priority` and nothing
    /// else: the lowest-kept non-essential piece goes first, the rightmost among equals, until the
    /// rest fit. The trend is measured at its minimum here -- it has narrowed before anything goes.
    /// @param pieces The line's pieces, in drawing order.
    /// @param budget The cells available.
    /// @param trendMinimum The trend's width at its narrowest, gap included.
    /// @param cellWidth How wide text is.
    /// @return The kept pieces, possibly none; or, when the essential pieces alone do not fit, the
    ///         cells they needed -- measured, so the minimum-size line names a real number.
    [[nodiscard]] std::expected<std::vector<Piece>, std::size_t> FitPieces(std::vector<Piece> pieces,
                                                                           std::size_t budget,
                                                                           std::size_t trendMinimum,
                                                                           CellWidth cellWidth)
    {
        auto const widthOf = [&](Piece const& piece) {
            return piece.trend ? trendMinimum : cellWidth(piece.text);
        };
        auto total = std::size_t { 0 };
        for (auto const& piece: pieces)
            total += widthOf(piece);

        while (total > budget)
        {
            auto victim = pieces.size();
            for (auto const index: std::views::iota(std::size_t { 0 }, pieces.size()))
            {
                auto const& piece = pieces[index];
                if (piece.priority == Priority::Essential)
                    continue;
                if (victim == pieces.size() || OutlastsOrTies(pieces[victim].priority, piece.priority))
                    victim = index;
            }
            if (victim == pieces.size())
                return std::unexpected(total);
            total -= widthOf(pieces[victim]);
            pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(victim));
        }
        return pieces;
    }

    /// The cells @p pieces take, the trend excluded.
    /// @param pieces The pieces.
    /// @param cellWidth How wide text is.
    /// @return The width.
    [[nodiscard]] std::size_t TextWidth(std::span<Piece const> pieces, CellWidth cellWidth)
    {
        auto total = std::size_t { 0 };
        for (auto const& piece: pieces)
            if (!piece.trend)
                total += cellWidth(piece.text);
        return total;
    }

    /// One vertical item of a frame: a line, a blank, or a table that can shrink.
    struct Item
    {
        std::vector<std::string> lines {};      ///< Its lines; a table's first is its heading.
        Priority priority { Priority::Normal }; ///< When it goes.
        std::size_t hidden { 0 };               ///< How many of a table's rows are not shown.
        bool table { false };                   ///< Whether it shrinks to `+N more` before it goes.
        bool image { false };                   ///< Whether its lines are blank cells an image is placed over.
    };

    /// The lines a layout keeps, and where its image item landed if it kept one.
    struct FittedRows
    {
        std::vector<std::string> lines {};       ///< Every kept line, top to bottom.
        std::optional<std::size_t> imageLine {}; ///< The index of the image item's first line, when kept.
    };

    /// How many lines @p item draws as it stands.
    /// @param item The item.
    /// @return Its height.
    [[nodiscard]] std::size_t HeightOf(Item const& item) noexcept
    {
        if (!item.table)
            return item.lines.size();
        auto const rows = item.lines.size() - 1;
        return 1 + (rows - item.hidden) + (item.hidden > 0 ? 1 : 0);
    }

    /// Hide one more of a table's rows, if that makes it shorter.
    ///
    /// The first cut hides two rows for one `+N more` line; each later cut hides one more. A table
    /// that has shown its last row keeps its heading and its `+N more` until it goes whole.
    /// @param item The table.
    /// @return True when the table got shorter.
    [[nodiscard]] bool Shrink(Item& item) noexcept
    {
        auto const shown = (item.lines.size() - 1) - item.hidden;
        if (item.hidden == 0)
        {
            if (shown < 2)
                return false;
            item.hidden = 2;
            return true;
        }
        if (shown == 0)
            return false;
        ++item.hidden;
        return true;
    }

    /// The last index in @p items matching @p match, or `items.size()`.
    /// @param items The items.
    /// @param match The test.
    /// @return The index.
    [[nodiscard]] std::size_t LastWhere(std::vector<Item> const& items, auto match)
    {
        auto found = items.size();
        for (auto const index: std::views::iota(std::size_t { 0 }, items.size()))
            if (match(items[index]))
                found = index;
        return found;
    }

    /// The items that fit in @p available lines, as lines.
    ///
    /// **The one place a line is dropped for height.** Lowest priority first: at each priority the
    /// bottom-most table shrinks to `+N more`, then the bottom-most other item goes, then the bottom-most
    /// table goes whole. `Essential` items never go -- an essential table only shrinks, and only once
    /// everything else has gone.
    /// @param items The frame's items, top to bottom.
    /// @param available The lines available, or nullopt for no limit.
    /// @return The lines, and where the image item landed; nullopt when the essential items alone do not fit.
    [[nodiscard]] std::optional<FittedRows> FitRows(std::vector<Item> items, std::optional<std::size_t> available)
    {
        auto total = std::size_t { 0 };
        for (auto const& item: items)
            total += HeightOf(item);

        // Hide one more row of the bottom-most table of @p priority that can still get shorter.
        auto const shrinkLast = [&items, &total](Priority priority) {
            auto const table = LastWhere(items, [priority](Item const& item) {
                auto probe = item;
                return item.table && item.priority == priority && Shrink(probe);
            });
            if (table == items.size())
                return false;
            total -= HeightOf(items[table]);
            (void) Shrink(items[table]);
            total += HeightOf(items[table]);
            return true;
        };

        if (available.has_value())
        {
            for (auto const level: std::views::iota(std::to_underlying(Priority::High), std::to_underlying(Priority::Last))
                                       | std::views::reverse)
            {
                auto const priority = static_cast<Priority>(level);
                while (total > *available)
                {
                    if (shrinkLast(priority))
                        continue;
                    auto const line =
                        LastWhere(items, [&](Item const& item) { return !item.table && item.priority == priority; });
                    auto const whole = line != items.size()
                                           ? line
                                           : LastWhere(items, [&](Item const& item) { return item.priority == priority; });
                    if (whole == items.size())
                        break;
                    total -= HeightOf(items[whole]);
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(whole));
                }
            }
            // Last of all, an ESSENTIAL table keeps its heading and says how many rows it hides: it
            // never goes, but a frame with room for its heading and a count is not the minimum-size line.
            while (total > *available)
                if (!shrinkLast(Priority::Essential))
                    break;
            if (total > *available)
                return std::nullopt;
        }

        auto lines = std::vector<std::string> {};
        auto imageLine = std::optional<std::size_t> {};
        for (auto& item: items)
        {
            if (!item.table)
            {
                if (item.image)
                    imageLine = lines.size();
                std::ranges::move(item.lines, std::back_inserter(lines));
                continue;
            }
            auto const shown = (item.lines.size() - 1) - item.hidden;
            std::ranges::move(item.lines | std::views::take(1 + shown), std::back_inserter(lines));
            if (item.hidden > 0)
                lines.push_back(std::format("{}+{} more", Indent, item.hidden));
        }
        return FittedRows { .lines = std::move(lines), .imageLine = imageLine };
    }

    /// A blank separator.
    /// @return The item.
    [[nodiscard]] Item Blank()
    {
        return Item { .lines = { std::string {} }, .priority = Priority::Spacing, .hidden = 0, .table = false };
    }

    /// The pieces of a line joined, the trend drawn where the trend piece stands.
    /// @param pieces The kept pieces.
    /// @param trend The trend's glyphs, drawn where the trend piece stands.
    /// @return The line.
    [[nodiscard]] std::string Joined(std::span<Piece const> pieces, std::string_view trend)
    {
        auto line = std::string {};
        for (auto const& piece: pieces)
            line += piece.trend ? std::string { TrendGap } + std::string { trend } : piece.text;
        return line;
    }

    /// The rate block, laid out for @p budget cells.
    /// @param in The frame's inputs.
    /// @param rows The panel's rate rows.
    /// @param budget The content width.
    /// @param reserve The beside reserve kept across frames at this width; grown here.
    /// @return One item per row; or the cells a row's essentials needed when they do not fit.
    [[nodiscard]] std::expected<std::vector<Item>, std::size_t> RateItems(FrameInputs const& in,
                                                                          std::span<RateRow const> rows,
                                                                          std::size_t budget,
                                                                          std::size_t& reserve)
    {
        // One figure column for the whole block, wide enough for its widest figure: the trends line
        // up, and a figure is never cut to keep them lined up.
        auto series = std::vector<Series> {};
        auto figures = std::vector<std::string> {};
        auto figureColumns = FigureColumns;
        for (auto const& row: rows)
        {
            series.push_back(SeriesFor(in, row.figure, {}));
            figures.push_back(FigureText(in, row.figure, Newest(series.back())));
            figureColumns = std::max(figureColumns, in.cellWidth(figures.back()));
        }

        auto const drawsTrend = in.glyphs->sparkLevels.size() >= 2;
        auto const trendMinimum = in.cellWidth(TrendGap) + MinimumTrendCells;
        auto fitted = std::vector<std::vector<Piece>> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
        {
            auto const& row = rows[index];
            auto pieces =
                std::vector<Piece> { Piece { .text = std::string { Indent } + FitRight(row.label, LabelColumns, in.cellWidth)
                                                     + AlignRight(figures[index], figureColumns, in.cellWidth),
                                             .priority = Priority::Essential } };
            if (drawsTrend && row.trend == Trend::Drawn)
                pieces.push_back(Piece { .priority = row.trendPriority, .trend = true });
            for (auto const& beside: row.beside)
            {
                auto text = std::string { PieceGap };
                if (!beside.before.empty())
                    text += std::string { beside.before } + " ";
                text += FigureText(in, beside.figure, Newest(SeriesFor(in, beside.figure, {})));
                if (!beside.after.empty())
                    text += " " + std::string { beside.after };
                pieces.push_back(Piece { .text = std::move(text), .priority = beside.priority });
            }
            if (!row.note.empty())
                pieces.push_back(
                    Piece { .text = std::string { PieceGap } + std::string { row.note }, .priority = row.notePriority });

            auto kept = FitPieces(std::move(pieces), budget, trendMinimum, in.cellWidth);
            if (!kept.has_value())
                return std::unexpected(kept.error());
            fitted.push_back(std::move(*kept));
        }

        // Every trend is one width: what the block's widest beside text leaves. Each row's kept text
        // fitted beside a MINIMUM trend, so the reserve never pushes a trend below its minimum.
        auto essential = std::size_t { 0 };
        for (auto const& pieces: fitted)
        {
            essential = std::max(essential, in.cellWidth(pieces.front().text));
            if (std::ranges::any_of(pieces, &Piece::trend))
                reserve = std::max(reserve, TextWidth(std::span { pieces }.subspan(1), in.cellWidth));
        }
        auto const used = essential + in.cellWidth(TrendGap) + reserve;
        auto const trendCells = budget > used ? std::max(MinimumTrendCells, budget - used) : MinimumTrendCells;

        auto items = std::vector<Item> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
        {
            auto const trend = Sparkline(Window(series[index], trendCells), *in.glyphs);
            items.push_back(Item { .lines = { Joined(fitted[index], trend) }, .priority = rows[index].priority });
        }
        return items;
    }

    /// The level block, laid out for @p budget cells.
    /// @param in The frame's inputs.
    /// @param rows The panel's level rows.
    /// @param budget The content width.
    /// @return One item per row; or the cells a row's essentials needed when they do not fit.
    [[nodiscard]] std::expected<std::vector<Item>, std::size_t> LevelItems(FrameInputs const& in,
                                                                           std::span<LevelRow const> rows,
                                                                           std::size_t budget)
    {
        auto items = std::vector<Item> {};
        for (auto const& row: rows)
        {
            auto const value = Newest(SeriesFor(in, row.value, {}));
            auto pieces = std::vector<Piece> { Piece {
                .text = std::string { Indent } + FitRight(row.label, LevelLabelColumns, in.cellWidth)
                        + AlignRight(FigureText(in, row.value, value), LevelFigureColumns, in.cellWidth),
                .priority = Priority::Essential } };
            if (row.limit.has_value())
            {
                auto const limit = Newest(SeriesFor(in, *row.limit, {}));
                pieces.push_back(Piece { .text = " / " + FigureText(in, *row.limit, limit), .priority = row.limitPriority });
                // A limit of zero is `InMemoryLruStorage`'s spelling of UNBOUNDED, so there is no
                // proportion to draw -- and a gauge left empty would claim the store is idle.
                auto const fraction = (value.has_value() && limit.has_value()) ? Quotient(*value, *limit) : std::nullopt;
                if (fraction.has_value())
                    pieces.push_back(
                        Piece { .text = "  " + Gauge(*fraction, GaugeCells, *in.glyphs), .priority = row.gaugePriority });
                pieces.push_back(Piece { .text = "  " + FormatFigure(fraction, FigureFormat::Percent, in.absent),
                                         .priority = row.limitPriority });
            }
            if (!row.note.empty())
                pieces.push_back(Piece { .text = "  " + std::string { row.note }, .priority = row.notePriority });

            auto kept = FitPieces(std::move(pieces), budget, 0, in.cellWidth);
            if (!kept.has_value())
                return std::unexpected(kept.error());
            items.push_back(Item { .lines = { Joined(*kept, {}) }, .priority = row.priority });
        }
        return items;
    }

    /// The tier table: a heading and one row per tier the newest reading carries, or nothing.
    ///
    /// **A tier the cache does not run contributes no row** (§9.5) -- not a row of absent markers,
    /// which would claim the tier exists and reported nothing. Presence is asked of the NEWEST
    /// reading's first column, the same series the daemon omits entirely for a tier it lacks.
    /// Columns are dropped for width from the heading, and every row keeps the same ones.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @param budget The content width.
    /// @return The table and its notes, nothing when no tier is present, or the cells the essential
    ///         columns needed when they do not fit.
    [[nodiscard]] std::expected<std::vector<Item>, std::size_t> TierItems(FrameInputs const& in,
                                                                          PanelSpec const& spec,
                                                                          std::size_t budget)
    {
        auto items = std::vector<Item> {};
        if (spec.tierColumns.empty() || !in.origin.has_value() || !in.model->latest.has_value())
            return items;

        auto const& presence = spec.tierColumns.front().figure;
        auto tiers = std::vector<std::string_view> {};
        for (auto const& tier: StorageTierTable)
        {
            auto const name = NameIn(presence.field, *in.origin);
            if (!name.empty() && FindField(*in.model->latest, TierSeriesName(name, tier.name)) != nullptr)
                tiers.push_back(tier.name);
        }
        if (tiers.empty())
            return items;

        // Each column is as wide as its widest cell, so a figure is never cut to fit the column.
        auto cells = std::vector<std::vector<std::string>>(tiers.size());
        auto widths = std::vector<std::size_t> {};
        for (auto const& column: spec.tierColumns)
        {
            auto width = std::max(TierFigureColumns, in.cellWidth(column.header) + 1);
            for (auto const index: std::views::iota(std::size_t { 0 }, tiers.size()))
            {
                cells[index].push_back(FigureText(in, column.figure, Newest(SeriesFor(in, column.figure, tiers[index]))));
                width = std::max(width, in.cellWidth(cells[index].back()) + 1);
            }
            widths.push_back(width);
        }

        auto heading =
            std::vector<Piece> { Piece { .text = std::string { Indent } + FitRight("tier", TierNameColumns, in.cellWidth),
                                         .priority = Priority::Essential } };
        for (auto const index: std::views::iota(std::size_t { 0 }, spec.tierColumns.size()))
            heading.push_back(Piece { .text = AlignRight(spec.tierColumns[index].header, widths[index], in.cellWidth),
                                      .priority = spec.tierColumns[index].priority,
                                      .slot = index + 1 });
        auto kept = FitPieces(std::move(heading), budget, 0, in.cellWidth);
        if (!kept.has_value())
            return std::unexpected(kept.error());

        auto table = Item { .lines = { Joined(*kept, {}) }, .priority = spec.tierPriority, .table = true };
        for (auto const index: std::views::iota(std::size_t { 0 }, tiers.size()))
        {
            auto row = std::string { Indent } + FitRight(tiers[index], TierNameColumns, in.cellWidth);
            for (auto const& piece: *kept | std::views::drop(1))
                row += AlignRight(cells[index][piece.slot - 1], widths[piece.slot - 1], in.cellWidth);
            table.lines.push_back(std::move(row));
        }
        items.push_back(std::move(table));
        for (auto const note: spec.tierNote)
        {
            // Prose, not a figure: a note that does not fit is dropped whole rather than cut.
            auto fitted = FitPieces(
                { Piece { .text = std::string { Indent } + std::string { note }, .priority = spec.tierNotePriority } },
                budget,
                0,
                in.cellWidth);
            if (!fitted.has_value())
                return std::unexpected(fitted.error());
            if (!fitted->empty())
                items.push_back(Item { .lines = { Joined(*fitted, {}) }, .priority = spec.tierNotePriority });
        }
        return items;
    }

    /// The palette ceiling a chart is encoded with: its ramp's eight colours fit without merging.
    constexpr auto ChartColours = std::size_t { 16 };

    /// What separates two headline tiles on one line.
    constexpr std::string_view TileGap = "   ";

    /// Columns a tile's value is right-aligned into, at the least.
    constexpr auto TileValueColumns = std::size_t { 10 };

    /// What separates two columns of a document table, and two tabs of the section strip.
    constexpr std::string_view ColumnGap = "  ";

    /// The text a document cell reads as.
    /// @param in The frame's inputs.
    /// @param cell The cell.
    /// @return The absent marker for an absent cell, its text otherwise.
    [[nodiscard]] std::string CellText(FrameInputs const& in, Cell const& cell)
    {
        return cell.kind == CellKind::Absent ? std::string { in.absent } : cell.lexical;
    }

    /// Where the column named @p name is in @p table.
    /// @param table The table.
    /// @param name The column's name.
    /// @return Its index, or nullopt when the table has no such column.
    [[nodiscard]] std::optional<std::size_t> ColumnNamed(Value const& table, std::string_view name)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, table.columns.size()))
            if (table.columns[index] == name)
                return index;
        return std::nullopt;
    }

    /// A `kpi` cell written for a person, in the unit its row names.
    /// @param in The frame's inputs.
    /// @param cell The raw value.
    /// @param unit The row's unit.
    /// @return Formatted where `KpiUnitTable` says how; the cell as the leader sent it otherwise.
    [[nodiscard]] std::string KpiText(FrameInputs const& in, Cell const& cell, std::string_view unit)
    {
        if (cell.kind == CellKind::Absent)
            return std::string { in.absent };
        auto const* row = FindIfOrNull(KpiUnitTable, [unit](KpiUnit const& candidate) { return candidate.name == unit; });
        auto number = 0.0;
        if (row == nullptr || !row->format.has_value() || !ParseFiniteDouble(cell.lexical, number))
            return cell.lexical;
        return FormatFigure(number * row->scale, *row->format, in.absent);
    }

    /// One headline tile's words.
    struct Tile
    {
        std::string key;   ///< The figure's key.
        std::string value; ///< Its value, written.
        std::string of;    ///< `of <n>`, or empty for a figure with no denominator.
    };

    /// One tile per `FleetKpiKeys()` key, in that order.
    ///
    /// Every key has a tile whether or not the document carried its row, so the strip is one shape from
    /// the first frame on: a figure not yet read is the absent marker, never a missing tile.
    /// @param in The frame's inputs.
    /// @return The tiles.
    [[nodiscard]] std::vector<Tile> Tiles(FrameInputs const& in)
    {
        // The `kpi` section's own column names, from the one door that lists them: the first names
        // the figure, and the three after it are its value, its unit and its denominator.
        auto const names = Distributed::FleetColumnNames(FleetSection::Kpi, Distributed::FleetSnapshot {});
        auto const* document = in.model->latestDocument.get();
        auto const* table = document == nullptr ? nullptr : document->Section(FleetSection::Kpi);
        auto const column = [&names, table](std::size_t position) {
            return table == nullptr || position >= names.size() ? std::nullopt : ColumnNamed(*table, names[position]);
        };
        auto const keyAt = column(0);
        auto const valueAt = column(1);
        auto const unitAt = column(2);
        auto const ofAt = column(3);
        auto const readable = keyAt.has_value() && valueAt.has_value() && unitAt.has_value();

        auto tiles = std::vector<Tile> {};
        for (auto const key: Distributed::FleetKpiKeys())
        {
            auto tile = Tile { .key = std::string { key }, .value = std::string { in.absent }, .of = {} };
            auto const* row = !readable ? nullptr : FindIfOrNull(table->rows, [&keyAt, key](std::vector<Cell> const& cells) {
                return cells[*keyAt].lexical == key;
            });
            if (row != nullptr && valueAt.has_value() && unitAt.has_value())
            {
                auto const& unit = (*row)[*unitAt].lexical;
                tile.value = KpiText(in, (*row)[*valueAt], unit);
                if (ofAt.has_value() && (*row)[*ofAt].kind != CellKind::Absent)
                    tile.of = "of " + KpiText(in, (*row)[*ofAt], unit);
            }
            tiles.push_back(std::move(tile));
        }
        return tiles;
    }

    /// The headline tiles laid out for @p budget cells: as many to a line as fit, every tile one width.
    /// @param in The frame's inputs.
    /// @param spec The document block.
    /// @param budget The content width.
    /// @return One item per line of tiles; none when not even one tile's key and value fit.
    [[nodiscard]] std::vector<Item> TileItems(FrameInputs const& in, DocumentSpec const& spec, std::size_t budget)
    {
        auto const tiles = Tiles(in);
        auto keyColumns = std::size_t { 0 };
        auto valueColumns = TileValueColumns;
        auto ofColumns = std::size_t { 0 };
        for (auto const& tile: tiles)
        {
            keyColumns = std::max(keyColumns, in.cellWidth(tile.key));
            valueColumns = std::max(valueColumns, in.cellWidth(tile.value));
            ofColumns = std::max(ofColumns, in.cellWidth(tile.of));
        }
        auto const indent = in.cellWidth(Indent);
        auto const gap = in.cellWidth(ColumnGap);
        auto const essential = keyColumns + gap + valueColumns;
        if (indent + essential > budget)
            return {};
        // The denominators go before a line holds fewer than one tile.
        auto const withOf = ofColumns > 0 && indent + essential + gap + ofColumns <= budget;
        auto const tileWidth = essential + (withOf ? gap + ofColumns : 0);
        auto const perLine = 1 + ((budget - indent - tileWidth) / (in.cellWidth(TileGap) + tileWidth));

        auto items = std::vector<Item> {};
        auto line = std::string {};
        auto onLine = std::size_t { 0 };
        for (auto const& tile: tiles)
        {
            line += onLine == 0 ? std::string { Indent } : std::string { TileGap };
            line += FitRight(tile.key, keyColumns, in.cellWidth) + std::string { ColumnGap }
                    + AlignRight(tile.value, valueColumns, in.cellWidth);
            if (withOf)
                line += std::string { ColumnGap } + FitRight(tile.of, ofColumns, in.cellWidth);
            if (++onLine == perLine)
            {
                items.push_back(Item { .lines = { std::exchange(line, {}) }, .priority = spec.tilePriority });
                onLine = 0;
            }
        }
        if (onLine > 0)
            items.push_back(Item { .lines = { std::move(line) }, .priority = spec.tilePriority });
        return items;
    }

    /// The section strip: every tabular section's key, the active one in brackets.
    ///
    /// Not essential either: a terminal too narrow for the active tab draws no strip.
    /// @param in The frame's inputs.
    /// @param spec The document block.
    /// @param active The section the table draws.
    /// @param budget The content width.
    /// @return The strip's one item, or none.
    [[nodiscard]] std::vector<Item> StripItems(FrameInputs const& in,
                                               DocumentSpec const& spec,
                                               FleetSection active,
                                               std::size_t budget)
    {
        auto pieces = std::vector<Piece> { Piece { .text = std::string { Indent }, .priority = Priority::Essential } };
        for (auto const& row: Distributed::FleetSectionTable)
        {
            if (!row.tabular)
                continue;
            auto const isActive = row.section == active;
            auto text = pieces.size() > 1 ? std::string { ColumnGap } : std::string {};
            text += isActive ? std::format("[{}]", row.key) : std::format(" {} ", row.key);
            pieces.push_back(
                Piece { .text = std::move(text), .priority = isActive ? Priority::Essential : spec.stripTabPriority });
        }
        auto kept = FitPieces(std::move(pieces), budget, 0, in.cellWidth);
        if (!kept.has_value())
            return {};
        return { Item { .lines = { Joined(*kept, {}) }, .priority = spec.stripPriority } };
    }

    /// The active section's table, walked from the header line the leader sent.
    /// @param in The frame's inputs.
    /// @param spec The document block.
    /// @param section The section to draw.
    /// @param budget The content width.
    /// @return The table -- or the absent marker where no document, or no such section, was read --
    ///         or the cells its first column needed when that does not fit.
    [[nodiscard]] std::expected<std::vector<Item>, std::size_t> SectionItems(FrameInputs const& in,
                                                                             DocumentSpec const& spec,
                                                                             FleetSection section,
                                                                             std::size_t budget)
    {
        auto const* document = in.model->latestDocument.get();
        auto const* table = document == nullptr ? nullptr : document->Section(section);
        if (table == nullptr || table->columns.empty())
        {
            // Nothing read yet, or a reading without this section: the marker where the table goes,
            // never an empty table, which would claim the fleet has no such rows.
            auto line = std::string { Indent } + std::string { in.absent };
            if (in.cellWidth(line) > budget)
                return std::unexpected(in.cellWidth(line));
            return std::vector<Item> { Item { .lines = { std::move(line) }, .priority = spec.tablePriority } };
        }

        // Each column as wide as its widest cell, heading included, so no cell is ever cut.
        auto widths = std::vector<std::size_t> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, table->columns.size()))
        {
            auto width = in.cellWidth(table->columns[index]);
            for (auto const& row: table->rows)
                width = std::max(width, in.cellWidth(CellText(in, row[index])));
            widths.push_back(width);
        }

        // The positional rule, as pieces: the first column is essential, every later one shares one
        // priority, and `FitPieces` takes the rightmost of equals first.
        auto heading = std::vector<Piece> { Piece { .text = std::string { Indent }
                                                            + FitRight(table->columns.front(), widths.front(), in.cellWidth),
                                                    .priority = Priority::Essential,
                                                    .slot = 0 } };
        for (auto const index: std::views::iota(std::size_t { 1 }, table->columns.size()))
            heading.push_back(
                Piece { .text = std::string { ColumnGap } + AlignRight(table->columns[index], widths[index], in.cellWidth),
                        .priority = spec.columnPriority,
                        .slot = index });
        auto kept = FitPieces(std::move(heading), budget, 0, in.cellWidth);
        if (!kept.has_value())
            return std::unexpected(kept.error());

        auto item = Item { .lines = { Joined(*kept, {}) }, .priority = spec.tablePriority, .table = true };
        for (auto const& row: table->rows)
        {
            auto line = std::string { Indent } + FitRight(CellText(in, row.front()), widths.front(), in.cellWidth);
            for (auto const& piece: *kept | std::views::drop(1))
                line +=
                    std::string { ColumnGap } + AlignRight(CellText(in, row[piece.slot]), widths[piece.slot], in.cellWidth);
            item.lines.push_back(std::move(line));
        }
        return std::vector<Item> { std::move(item) };
    }

    /// The Sixel chart's blank cells, on the rung and terminal that can draw it; nothing otherwise.
    /// @param in The frame's inputs.
    /// @param spec The document block.
    /// @param context The session facts: the rung and the encoder.
    /// @param budget The content width.
    /// @return The chart item, or none.
    [[nodiscard]] std::vector<Item> ChartItems(FrameInputs const& in,
                                               DocumentSpec const& spec,
                                               PanelContext const& context,
                                               std::size_t budget)
    {
        auto const indent = in.cellWidth(Indent);
        if (context.rung != RenderRung::Sixel || context.sixel == nullptr || !in.model->cellPixels.has_value()
            || spec.chartCellsHigh == 0 || budget < indent + spec.chartMinimumCells)
            return {};
        return { Item { .lines = std::vector<std::string>(spec.chartCellsHigh, std::string {}),
                        .priority = spec.chartPriority,
                        .image = true } };
    }

    /// The fleet document block: the tiles, a blank, the strip and the active section's table.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @param context The session facts: the section the table draws, the rung and the encoder.
    /// @param budget The content width.
    /// @return The block, nothing for a panel without one, or the cells the table's first column needed.
    [[nodiscard]] std::expected<std::vector<Item>, std::size_t> DocumentItems(FrameInputs const& in,
                                                                              PanelSpec const& spec,
                                                                              PanelContext const& context,
                                                                              std::size_t budget)
    {
        auto items = std::vector<Item> {};
        if (!spec.document.has_value())
            return items;
        auto table = SectionItems(in, *spec.document, context.section, budget);
        if (!table.has_value())
            return std::unexpected(table.error());
        std::ranges::move(TileItems(in, *spec.document, budget), std::back_inserter(items));
        if (!items.empty())
            items.push_back(Blank());
        auto chart = ChartItems(in, *spec.document, context, budget);
        if (!chart.empty())
        {
            std::ranges::move(chart, std::back_inserter(items));
            items.push_back(Blank());
        }
        std::ranges::move(StripItems(in, *spec.document, context.section, budget), std::back_inserter(items));
        std::ranges::move(*table, std::back_inserter(items));
        return items;
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

    /// The one line drawn instead of a panel that does not fit.
    ///
    /// The two sizes come first, so a terminal too narrow for the whole line loses the title rather
    /// than a figure; one too narrow for the sizes as well has them cut, since nothing else is drawable.
    /// @param title The panel's title.
    /// @param needed The size it needs.
    /// @param columns The terminal's width.
    /// @param rows The terminal's height, or zero when not reported.
    /// @param cellWidth How wide text is.
    /// @return The line, no wider than @p columns.
    [[nodiscard]] std::string TooSmall(
        std::string_view title, PanelSize needed, std::size_t columns, std::size_t rows, CellWidth cellWidth)
    {
        auto const sizes = std::format("needs {}x{}, have {}x{}", needed.columns, needed.rows, columns, rows);
        auto const text = std::format("{} -- {}", sizes, title);
        auto const shown = cellWidth(text) <= columns ? text : sizes;
        return FitRight(shown, std::min(columns, cellWidth(shown)), cellWidth);
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

PanelSize MinimumPanelSize(PanelSpec const& spec, CellWidth cellWidth) noexcept
{
    // The content each block's essential pieces need at their column widths; the frame adds its
    // edges and the blank column before the right one.
    auto content = std::size_t { 0 };
    auto rows = FrameRows;
    for (auto const& row: spec.rates)
    {
        content = std::max(content, cellWidth(Indent) + LabelColumns + FigureColumns);
        rows += row.priority == Priority::Essential ? 1 : 0;
    }
    for (auto const& row: spec.levels)
    {
        content = std::max(content, cellWidth(Indent) + LevelLabelColumns + LevelFigureColumns);
        rows += row.priority == Priority::Essential ? 1 : 0;
    }
    if (!spec.tierColumns.empty())
    {
        auto tier = cellWidth(Indent) + TierNameColumns;
        for (auto const& column: spec.tierColumns)
            tier += column.priority == Priority::Essential ? TierFigureColumns : 0;
        content = std::max(content, tier);
        rows += spec.tierPriority == Priority::Essential ? 2 : 0;
    }
    if (spec.document.has_value())
    {
        // A table's first column is as wide as what the leader sent, so no width is knowable here
        // beyond the indent; the layout names the cells it measured when that column does not fit.
        content = std::max(content, cellWidth(Indent));
        rows += spec.document->tablePriority == Priority::Essential ? 2 : 0;
    }
    rows += spec.sourcePriority == Priority::Essential ? 1 : 0;
    return PanelSize { .columns = content + FrameColumns, .rows = rows };
}

namespace
{
    /// Which way a stepping key walks the strip.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class SectionStep : std::uint8_t
    {
        Next,     ///< The tab after the active one, wrapping to the first.
        Previous, ///< The tab before it, wrapping to the last.
    };

    /// A keystroke that steps through the fleet sections, as the bytes `KeyBytes` delivers it.
    struct SectionStepKey
    {
        std::string_view keys; ///< The keystroke's bytes.
        SectionStep step;      ///< Which way it walks.
    };

    /// Every stepping key. `Shift+Tab` is not here: it arrives as the bytes of `Tab`.
    constexpr auto SectionStepKeys = std::to_array<SectionStepKey>({
        { .keys = "\t", .step = SectionStep::Next },
        { .keys = "\x1b[C", .step = SectionStep::Next },
        { .keys = "\x1b[D", .step = SectionStep::Previous },
    });
} // namespace

std::optional<FleetSection> SectionForKey(FleetSection active, std::string_view keys)
{
    auto tabs = std::vector<FleetSection> {};
    for (auto const& row: Distributed::FleetSectionTable)
        if (row.tabular)
            tabs.push_back(row.section);
    if (tabs.empty())
        return std::nullopt;

    if (keys.size() == 1 && keys.front() >= '1' && keys.front() <= '9')
    {
        auto const position = static_cast<std::size_t>(keys.front() - '1');
        return position < tabs.size() ? std::optional<FleetSection> { tabs[position] } : std::nullopt;
    }

    auto const* stepping = FindIfOrNull(SectionStepKeys, [keys](SectionStepKey const& row) { return row.keys == keys; });
    if (stepping == nullptr)
        return std::nullopt;
    auto const found = std::ranges::find(tabs, active);
    auto const forward = stepping->step == SectionStep::Next;
    if (found == tabs.end())
        return forward ? tabs.front() : tabs.back();
    auto const index = static_cast<std::size_t>(found - tabs.begin());
    return tabs[forward ? (index + 1) % tabs.size() : (index + tabs.size() - 1) % tabs.size()];
}

PanelView::PanelView(PanelSpec const& spec, PanelContext context):
    _spec { &spec },
    _glyphs { &GlyphsFor(context.rung) },
    _context { std::move(context) }
{
    assert(_context.cellWidth != nullptr && "a panel is laid out through the one width function it is handed");
}

std::string PanelView::Frame(DashboardModel const& model)
{
    return PlacedFrame(model).text;
}

bool PanelView::Key(std::string_view keys)
{
    if (!_spec->document.has_value())
        return false;
    auto const section = SectionForKey(_context.section, keys);
    if (!section.has_value() || *section == _context.section)
        return false;
    _context.section = *section;
    return true;
}

DashboardFrame PanelView::PlacedFrame(DashboardModel const& model)
{
    auto const in = FrameInputs { .model = &model,
                                  .origin = OriginOf(model),
                                  .glyphs = _glyphs,
                                  .absent = _context.absent,
                                  .cellWidth = _context.cellWidth };
    auto const columns = model.columns > 0 ? static_cast<std::size_t>(model.columns) : DefaultColumns;
    auto const reportedRows = model.rows > 0 ? static_cast<std::size_t>(model.rows) : std::size_t { 0 };
    auto const available =
        reportedRows > 0 ? std::optional<std::size_t> { reportedRows - std::min(reportedRows, FrameRows) } : std::nullopt;
    auto const minimum = MinimumPanelSize(*_spec, in.cellWidth);
    auto const tooSmall = [&](std::size_t neededColumns) {
        return DashboardFrame { .text = TooSmall(
                                    _spec->title,
                                    PanelSize { .columns = std::max(minimum.columns, neededColumns), .rows = minimum.rows },
                                    columns,
                                    reportedRows,
                                    in.cellWidth),
                                .placements = {} };
    };
    // No early refusal against `minimum`: the layout below is the one judge of what fits, and the
    // minimum only NAMES the size. That keeps `MinimumPanelSize` a claim the layout can contradict --
    // a minimum stated too high would otherwise refuse terminals the panel fits, and nothing would say.

    auto const budget = ContentColumns(columns);
    if (budget != _reserveColumns)
    {
        _besideReserve = 0;
        _reserveColumns = budget;
    }

    auto rates = RateItems(in, _spec->rates, budget, _besideReserve);
    auto levels = LevelItems(in, _spec->levels, budget);
    auto tiers = TierItems(in, *_spec, budget);
    auto document = DocumentItems(in, *_spec, _context, budget);
    for (auto const* block: { &rates, &levels, &tiers, &document })
        if (!block->has_value())
            return tooSmall(block->error() + FrameColumns);

    auto items = std::vector<Item> {};
    for (auto* block: { &*rates, &*levels, &*tiers, &*document })
    {
        if (block->empty())
            continue;
        items.push_back(Blank());
        std::ranges::move(*block, std::back_inserter(items));
    }
    items.push_back(Blank());
    auto source =
        FitPieces({ Piece { .text = SourceLine(in), .priority = _spec->sourcePriority } }, budget, 0, in.cellWidth);
    if (!source.has_value())
        return tooSmall(source.error() + FrameColumns);
    if (!source->empty())
        items.push_back(Item { .lines = { Joined(*source, {}) }, .priority = _spec->sourcePriority });

    auto const fitted = FitRows(std::move(items), available);
    if (!fitted.has_value())
        return tooSmall(0);

    auto title = std::string { _spec->title };
    if (!_context.endpoint.empty())
        title += "  " + _context.endpoint;
    if (_context.interval.has_value())
        title += std::format("  every {}s", std::chrono::duration<double> { *_context.interval }.count());
    auto frame =
        DashboardFrame { .text = Cli::Frame(title, fitted->lines, columns, *_glyphs, in.cellWidth), .placements = {} };

    // The chart kept its rows: draw the image over them. The frame's first row is its top edge and its
    // first column its left edge, so a content line's frame row is its index plus two and the chart's
    // first cell sits after the edge and the indent.
    if (fitted->imageLine.has_value() && _spec->document.has_value() && model.cellPixels.has_value()
        && _context.sixel != nullptr)
    {
        auto const cellsWide = budget - in.cellWidth(Indent);
        auto const cellsHigh = _spec->document->chartCellsHigh;
        auto const raster = FleetChartRaster(model.history,
                                             FleetChartMetrics.front(),
                                             cellsWide * model.cellPixels->width,
                                             cellsHigh * model.cellPixels->height);
        auto encoded = _context.sixel->Encode(
            RgbaImage { .pixels = raster.rgba, .width = raster.width, .height = raster.height }, ChartColours);
        if (encoded.has_value())
            frame.placements.push_back(FramePlacement { .row = *fitted->imageLine + 2,
                                                        .column = 2 + in.cellWidth(Indent),
                                                        .cellsWide = cellsWide,
                                                        .cellsHigh = cellsHigh,
                                                        .sixel = std::move(*encoded) });
    }
    return frame;
}

} // namespace FastCache::Cli
