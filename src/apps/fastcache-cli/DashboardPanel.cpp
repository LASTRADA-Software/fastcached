// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanel.hpp"

#include <FastCache/Cache/StorageTier.hpp>
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
        bool table { false };                   ///< Whether it shrinks to `+N more` before it goes.
        std::size_t hidden { 0 };               ///< How many of a table's rows are not shown.
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
    /// table goes whole. `Essential` items never go.
    /// @param items The frame's items, top to bottom.
    /// @param available The lines available, or nullopt for no limit.
    /// @return The lines; nullopt when the essential items alone do not fit.
    [[nodiscard]] std::optional<std::vector<std::string>> FitRows(std::vector<Item> items,
                                                                  std::optional<std::size_t> available)
    {
        auto total = std::size_t { 0 };
        for (auto const& item: items)
            total += HeightOf(item);

        if (available.has_value())
        {
            for (auto const level: std::views::iota(std::to_underlying(Priority::High), std::to_underlying(Priority::Last))
                                       | std::views::reverse)
            {
                auto const priority = static_cast<Priority>(level);
                while (total > *available)
                {
                    auto const table = LastWhere(items, [&](Item const& item) {
                        auto probe = item;
                        return item.table && item.priority == priority && Shrink(probe);
                    });
                    if (table != items.size())
                    {
                        total -= HeightOf(items[table]);
                        (void) Shrink(items[table]);
                        total += HeightOf(items[table]);
                        continue;
                    }
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
            if (total > *available)
                return std::nullopt;
        }

        auto lines = std::vector<std::string> {};
        for (auto& item: items)
        {
            if (!item.table)
            {
                std::ranges::move(item.lines, std::back_inserter(lines));
                continue;
            }
            auto const shown = (item.lines.size() - 1) - item.hidden;
            std::ranges::move(item.lines | std::views::take(1 + shown), std::back_inserter(lines));
            if (item.hidden > 0)
                lines.push_back(std::format("{}+{} more", Indent, item.hidden));
        }
        return lines;
    }

    /// A blank separator.
    /// @return The item.
    [[nodiscard]] Item Blank()
    {
        return Item { .lines = { std::string {} }, .priority = Priority::Spacing, .table = false, .hidden = 0 };
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
    rows += spec.sourcePriority == Priority::Essential ? 1 : 0;
    return PanelSize { .columns = content + FrameColumns, .rows = rows };
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
        return TooSmall(_spec->title,
                        PanelSize { .columns = std::max(minimum.columns, neededColumns), .rows = minimum.rows },
                        columns,
                        reportedRows,
                        in.cellWidth);
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
    for (auto const* block: { &rates, &levels, &tiers })
        if (!block->has_value())
            return tooSmall(block->error() + FrameColumns);

    auto items = std::vector<Item> { Blank() };
    std::ranges::move(*rates, std::back_inserter(items));
    for (auto* block: { &*levels, &*tiers })
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

    auto const lines = FitRows(std::move(items), available);
    if (!lines.has_value())
        return tooSmall(0);

    auto title = std::string { _spec->title };
    if (!_context.endpoint.empty())
        title += "  " + _context.endpoint;
    if (_context.interval.has_value())
        title += std::format("  every {}s", std::chrono::duration<double> { *_context.interval }.count());
    return Cli::Frame(title, *lines, columns, *_glyphs, in.cellWidth);
}

} // namespace FastCache::Cli
