// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanel.hpp"
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <numeric>
#include <ranges>
#include <span>
#include <system_error>
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
        std::optional<FrameTone> tone {};       ///< How its text is dressed, spaces around it excluded; none for plain.
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

    /// The columns of a table's heading that fit in @p budget cells: by rank, each one that still fits.
    ///
    /// **Not `FitPieces`, which drops a line's lowest pieces until the rest fit.** A line is read left to right,
    /// so a piece it loses goes whole; a table's columns are independent, so the room a wide column could not
    /// use is given to the next column that fits in it. At 80 cells that keeps `cores` beside the vital
    /// columns and leaves `memory` out, as §5 draws it, where dropping by rank alone left the room empty.
    /// Columns are taken most important first and, among equals, leftmost first; they come back in drawing order.
    /// @param pieces The heading's pieces, one per column, in drawing order.
    /// @param budget The cells available.
    /// @param cellWidth How wide text is.
    /// @return The kept columns; or, when the essential ones alone do not fit, the cells they needed.
    [[nodiscard]] std::expected<std::vector<Piece>, std::size_t> FitColumns(std::vector<Piece> pieces,
                                                                            std::size_t budget,
                                                                            CellWidth cellWidth)
    {
        auto order = std::vector<std::size_t>(pieces.size());
        std::ranges::iota(order, std::size_t { 0 });
        std::ranges::stable_sort(
            order, {}, [&pieces](std::size_t index) { return std::to_underlying(pieces[index].priority); });

        auto kept = std::vector<bool>(pieces.size(), false);
        auto used = std::size_t { 0 };
        for (auto const index: order)
        {
            auto const width = cellWidth(pieces[index].text);
            auto const essential = pieces[index].priority == Priority::Essential;
            if (!essential && used + width > budget)
                continue;
            kept[index] = true;
            used += width;
        }
        if (used > budget)
            return std::unexpected(used);

        auto fitted = std::vector<Piece> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, pieces.size()))
            if (kept[index])
                fitted.push_back(std::move(pieces[index]));
        return fitted;
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

    /// A run of one of an item's lines a presenter dresses.
    struct LineSpan
    {
        std::size_t line { 0 };                 ///< Which of the item's lines; a table's heading is 0.
        std::size_t byte { 0 };                 ///< Where the run starts in that line's text.
        std::size_t length { 0 };               ///< How many bytes it covers.
        FrameTone tone { FrameTone::Selected }; ///< What it is.
    };

    /// One vertical item of a frame: a line, a blank, or a table that can shrink.
    ///
    /// The byte-wide members close the struct in one run, so it pads nothing between 8-aligned ones.
    struct Item
    {
        std::vector<std::string> lines {};      ///< Its lines; a table's first is its heading.
        std::vector<LineSpan> spans {};         ///< The runs of its lines a presenter dresses.
        std::string_view noun {};               ///< What a table's rows are, for its overflow line; empty draws `+N more`.
        std::size_t hidden { 0 };               ///< How many of a table's rows are not shown, below those that are.
        std::size_t above { 0 };                ///< How many rows a scrolled table skipped above its first.
        std::size_t rowsKept { 0 };             ///< The rows a table keeps when it shrinks at `shrinkAt`.
        Priority priority { Priority::Normal }; ///< When it goes.
        std::optional<Priority> shrinkAt {};    ///< The level a table gives up rows at, when that is not `priority`.
        bool table { false };                   ///< Whether it shrinks to its overflow line before it goes.
        bool image { false };                   ///< Whether its lines are blank cells an image is placed over.
    };

    /// The lines a layout keeps, the runs it dresses, and where its image and its scrolled table landed.
    struct FittedRows
    {
        std::vector<std::string> lines {};        ///< Every kept line, top to bottom.
        std::vector<LineSpan> spans {};           ///< Every kept run; `line` indexes `lines`.
        std::optional<std::size_t> imageLine {};  ///< The index of the image item's first line, when kept.
        std::size_t imageRows { 0 };              ///< How many lines the image item kept.
        std::optional<std::size_t> tableShown {}; ///< How many rows a table with an overflow noun showed.
    };

    /// What a scrolled table's overflow line tells the operator to press, before and after scrolling.
    constexpr std::string_view ScrollDownHint = "PgDn scrolls, / filters";
    constexpr std::string_view ScrollBothHint = "PgUp/PgDn scroll, / filters";

    /// Whether @p item draws an overflow line below its rows.
    /// @param item A table.
    /// @return True when rows are hidden below or skipped above.
    [[nodiscard]] constexpr bool HasOverflowLine(Item const& item) noexcept
    {
        return item.hidden > 0 || item.above > 0;
    }

    /// How many lines @p item draws as it stands.
    /// @param item The item.
    /// @return Its height.
    [[nodiscard]] std::size_t HeightOf(Item const& item) noexcept
    {
        if (!item.table)
            return item.lines.size();
        auto const rows = item.lines.size() - 1;
        return 1 + (rows - item.hidden) + (HasOverflowLine(item) ? 1 : 0);
    }

    /// How many rows @p item hides after one more cut that leaves at least @p floor shown, if a cut can.
    ///
    /// A table with no overflow line yet hides two rows for the line it gains; every later cut hides one
    /// more. A table that has shown its last row keeps its heading and its overflow line until it goes
    /// whole.
    /// @param item The table.
    /// @param floor The fewest rows the cut may leave shown.
    /// @return The rows hidden after the cut, or nullopt when no cut makes it shorter.
    [[nodiscard]] std::optional<std::size_t> AfterCut(Item const& item, std::size_t floor) noexcept
    {
        auto const shown = (item.lines.size() - 1) - item.hidden;
        if (!HasOverflowLine(item))
            return shown >= floor + 2 ? std::optional<std::size_t> { 2 } : std::nullopt;
        return shown > floor ? std::optional<std::size_t> { item.hidden + 1 } : std::nullopt;
    }

    /// The overflow line under a table: what is not shown, and for a table with a noun, how to reach it.
    /// @param item The table.
    /// @return The line.
    [[nodiscard]] std::string OverflowLine(Item const& item)
    {
        if (item.noun.empty())
            return std::format("{}+{} more", Indent, item.hidden);
        auto what = item.hidden > 0 ? std::format("{} more {}", item.hidden, item.noun) : std::string {};
        if (item.above > 0)
            what += what.empty() ? std::format("{} {} above", item.above, item.noun) : std::format(", {} above", item.above);
        return std::format("{}... {}; {}", Indent, what, item.above > 0 ? ScrollBothHint : ScrollDownHint);
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

        // Hide one more row of the bottom-most table shrinking at @p priority that can still get shorter.
        // A table shrinks at its own `shrinkAt`, down to its `rowsKept`; the essential stage takes an
        // essential table below that, to its heading and its overflow line.
        auto const shrinkLast = [&items, &total](Priority priority) {
            auto const essentialStage = priority == Priority::Essential;
            auto const floorOf = [essentialStage](Item const& item) {
                return essentialStage || !item.shrinkAt.has_value() ? 0 : item.rowsKept;
            };
            auto const shrinksHere = [&](Item const& item) {
                auto const level = essentialStage ? item.priority : item.shrinkAt.value_or(item.priority);
                return item.table && level == priority && AfterCut(item, floorOf(item)).has_value();
            };
            auto const table = LastWhere(items, shrinksHere);
            if (table == items.size())
                return false;
            total -= HeightOf(items[table]);
            items[table].hidden = AfterCut(items[table], floorOf(items[table])).value_or(items[table].hidden);
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

        auto fitted = FittedRows {};
        for (auto& item: items)
        {
            auto const first = fitted.lines.size();
            auto const kept = item.table ? 1 + ((item.lines.size() - 1) - item.hidden) : item.lines.size();
            for (auto const& span: item.spans)
                if (span.line < kept)
                    fitted.spans.push_back(
                        LineSpan { .line = first + span.line, .byte = span.byte, .length = span.length, .tone = span.tone });
            if (item.image)
            {
                fitted.imageLine = first;
                fitted.imageRows = item.lines.size();
            }
            if (item.table && !item.noun.empty())
                fitted.tableShown = kept - 1;
            std::ranges::move(item.lines | std::views::take(kept), std::back_inserter(fitted.lines));
            if (item.table && HasOverflowLine(item))
            {
                auto line = OverflowLine(item);
                auto const words = line.find_first_not_of(' ');
                fitted.spans.push_back(LineSpan {
                    .line = fitted.lines.size(), .byte = words, .length = line.size() - words, .tone = FrameTone::Label });
                fitted.lines.push_back(std::move(line));
            }
        }
        return fitted;
    }

    /// A blank separator.
    /// @return The item.
    [[nodiscard]] Item Blank()
    {
        return Item { .lines = { std::string {} }, .priority = Priority::Spacing };
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

    /// One line's item from its kept pieces: the joined text, and a run for every piece with a tone.
    ///
    /// **The one way a panel dresses a piece.** A run covers the piece's text without the spaces around it,
    /// so a gap or an alignment pad is never dressed: inverse video over padding would draw a block wider
    /// than the word it marks.
    /// @param pieces The kept pieces.
    /// @param trend The trend's glyphs, drawn where the trend piece stands.
    /// @param priority When the item goes.
    /// @return The item.
    [[nodiscard]] Item LineOf(std::span<Piece const> pieces, std::string_view trend, Priority priority)
    {
        auto item = Item { .lines = { std::string {} }, .priority = priority };
        auto& line = item.lines.front();
        for (auto const& piece: pieces)
        {
            if (piece.trend)
            {
                line += std::string { TrendGap } + std::string { trend };
                continue;
            }
            auto const first = piece.text.find_first_not_of(' ');
            if (piece.tone.has_value() && first != std::string::npos)
            {
                auto const last = piece.text.find_last_not_of(' ');
                item.spans.push_back(
                    LineSpan { .line = 0, .byte = line.size() + first, .length = last + 1 - first, .tone = *piece.tone });
            }
            line += piece.text;
        }
        return item;
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

    /// The tier table: a heading and one row per tier the newest reading carries (`TiersIn`), or
    /// nothing. Columns are dropped for width from the heading, and every row keeps the same ones.
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

        auto const tiers = TiersIn(spec, *in.model->latest, *in.origin);
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

    /// What separates the two columns of headline tiles.
    constexpr std::string_view TileGap = "  ";

    /// Columns a tile's value is right-aligned into, at the least.
    constexpr auto TileValueColumns = std::size_t { 6 };

    /// What separates two columns of a document table, and two tabs of the section strip.
    constexpr std::string_view ColumnGap = "  ";

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

    /// The integer a document cell carries, or nullopt for an absent cell or one that is not an integer.
    /// @param cell The raw cell.
    /// @return The integer.
    [[nodiscard]] std::optional<std::uint64_t> CellInteger(Cell const& cell) noexcept
    {
        if (cell.kind == CellKind::Absent)
            return std::nullopt;
        auto number = std::uint64_t { 0 };
        auto const* const end = cell.lexical.data() + cell.lexical.size();
        auto const [at, error] = std::from_chars(cell.lexical.data(), end, number);
        return error == std::errc {} && at == end ? std::optional<std::uint64_t> { number } : std::nullopt;
    }

    /// A document cell written for a person, in the scale the leader's own tables give it.
    ///
    /// **No client-side knowledge of which column or unit is which**: the scale comes from
    /// `Distributed::FleetColumnFormat` or `Distributed::CellFormatFromName`, and the writing from
    /// `Distributed::HumanFleetFigure`, which is the leader's page's own. A cell whose scale is unknown,
    /// or that is not an integer, is shown as the leader sent it rather than guessed at.
    /// @param in The frame's inputs.
    /// @param cell The raw value.
    /// @param format The cell's scale, or nullopt where none is known.
    /// @return The absent marker, the written figure, or the cell as sent.
    [[nodiscard]] std::string HumanCellText(FrameInputs const& in,
                                            Cell const& cell,
                                            std::optional<Distributed::CellFormat> format)
    {
        if (cell.kind == CellKind::Absent)
            return std::string { in.absent };
        auto const number = CellInteger(cell);
        if (!format.has_value() || !number.has_value())
            return cell.lexical;
        return Distributed::HumanFleetFigure(*number, *format);
    }

    /// The scale of each column of a section's header, in the leader's own column tables.
    /// @param section The section the header belongs to.
    /// @param columns The header, as the leader sent it.
    /// @return One scale per column, index for index; nullopt for a name that section does not render.
    [[nodiscard]] std::vector<std::optional<Distributed::CellFormat>> ColumnFormats(FleetSection section,
                                                                                    std::span<std::string const> columns)
    {
        auto formats = std::vector<std::optional<Distributed::CellFormat>> {};
        formats.reserve(columns.size());
        for (auto const& name: columns)
            formats.push_back(Distributed::FleetColumnFormat(section, name));
        return formats;
    }

    /// A `kpi` cell written for a person, in the unit its row names.
    /// @param in The frame's inputs.
    /// @param cell The raw value.
    /// @param unit The row's unit.
    /// @return As `HumanCellText` writes it.
    [[nodiscard]] std::string KpiText(FrameInputs const& in, Cell const& cell, std::string_view unit)
    {
        return HumanCellText(in, cell, Distributed::CellFormatFromName(unit));
    }

    /// One headline tile's words.
    struct Tile
    {
        std::string_view key {};   ///< The figure's key, which its trend is read under.
        std::string_view label {}; ///< The page's label for it.
        std::string value {};      ///< Its value, written.
        std::string words {};      ///< `of 192 slots`, `not yet resolved`, or empty.
        bool trend { false };      ///< Whether a sparkline is drawn where its words would be.
        bool worded { false };     ///< Whether the figure's row carries words or a trend at all, read or not.
        bool alert { false };      ///< Whether its value is one the leader says is worth an operator's eye.
    };

    /// One tile per `FleetKpis()` row, in that order.
    ///
    /// Every figure has a tile whether or not the document carried its row, so the strip is one shape
    /// from the first frame on: a figure not yet read is the absent marker, never a missing tile. Its words
    /// are drawn only beside a row that was read, since *not yet resolved* beside a figure nobody reported
    /// would be a claim about it.
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
        auto const drawsTrend = in.glyphs->sparkLevels.size() >= 2;

        auto tiles = std::vector<Tile> {};
        for (auto const& kpi: Distributed::FleetKpis())
        {
            auto tile = Tile { .key = kpi.key,
                               .label = kpi.label,
                               .value = std::string { in.absent },
                               .trend = kpi.sparkline && drawsTrend,
                               .worded = kpi.sparkline || !kpi.ofNoun.empty() || !kpi.note.empty() };
            auto const* row =
                !readable ? nullptr : FindIfOrNull(table->rows, [&keyAt, &kpi](std::vector<Cell> const& cells) {
                    return cells[*keyAt].lexical == kpi.key;
                });
            if (row != nullptr && valueAt.has_value() && unitAt.has_value())
            {
                auto const& unit = (*row)[*unitAt].lexical;
                tile.value = KpiText(in, (*row)[*valueAt], unit);
                tile.alert = kpi.alertAboveZero && CellInteger((*row)[*valueAt]).value_or(0) > 0;
                if (ofAt.has_value() && (*row)[*ofAt].kind != CellKind::Absent)
                    tile.words = "of " + KpiText(in, (*row)[*ofAt], unit)
                                 + (kpi.ofNoun.empty() ? std::string {} : " " + std::string { kpi.ofNoun });
                else
                    tile.words = std::string { kpi.note };
            }
            tiles.push_back(std::move(tile));
        }
        return tiles;
    }

    /// A column of tiles, measured: every tile in it one width.
    struct TileColumn
    {
        std::vector<Tile const*> tiles {}; ///< Its tiles, top to bottom.
        std::size_t label { 0 };           ///< Cells its labels take.
        std::size_t value { 0 };           ///< Cells its values are right-aligned into.
        std::size_t words { 0 };           ///< Cells its words or trends take; zero for a column drawn without them.
    };

    /// @p tiles measured as one column, with or without their words.
    /// @param in The frame's inputs.
    /// @param tiles The tiles.
    /// @param withWords Whether words and trends are drawn.
    /// @return The column.
    [[nodiscard]] TileColumn MeasureTiles(FrameInputs const& in, std::vector<Tile const*> tiles, bool withWords)
    {
        auto column = TileColumn { .tiles = std::move(tiles), .label = 0, .value = TileValueColumns, .words = 0 };
        for (auto const* tile: column.tiles)
        {
            column.label = std::max(column.label, in.cellWidth(tile->label));
            column.value = std::max(column.value, in.cellWidth(tile->value));
            if (withWords)
                column.words = std::max(
                    { column.words, in.cellWidth(tile->words), tile->trend ? MinimumTrendCells : std::size_t { 0 } });
        }
        return column;
    }

    /// The cells a column of tiles takes; zero for an empty one.
    /// @param in The frame's inputs.
    /// @param column The column.
    /// @return The width.
    [[nodiscard]] std::size_t TileColumnWidth(FrameInputs const& in, TileColumn const& column) noexcept
    {
        if (column.tiles.empty())
            return 0;
        auto const gap = in.cellWidth(ColumnGap);
        return column.label + gap + column.value + (column.words > 0 ? gap + column.words : 0);
    }

    /// The @p index-th tile of @p column as pieces, or none past its last.
    ///
    /// The label and the words recede and the figure carries the weight -- or the alert, where the leader
    /// says its value is worth an operator's eye. The trend is drawn in the terminal's own colour.
    /// @param in The frame's inputs.
    /// @param column The column.
    /// @param index Which tile.
    /// @return The pieces, unpadded after the last cell.
    [[nodiscard]] std::vector<Piece> TilePieces(FrameInputs const& in, TileColumn const& column, std::size_t index)
    {
        if (index >= column.tiles.size())
            return {};
        auto const& tile = *column.tiles[index];
        auto pieces = std::vector<Piece> {
            Piece { .text = FitRight(tile.label, column.label, in.cellWidth), .tone = FrameTone::Label },
            Piece { .text = std::string { ColumnGap } + AlignRight(tile.value, column.value, in.cellWidth),
                    .tone = tile.alert ? FrameTone::Alert : FrameTone::Figure },
        };
        if (column.words == 0)
            return pieces;
        if (tile.trend)
            pieces.push_back(Piece {
                .text =
                    std::string { ColumnGap }
                    + Sparkline(Window(Levels(in.model->history, std::string { tile.key }), column.words), *in.glyphs) });
        else if (!tile.words.empty())
            pieces.push_back(Piece { .text = std::string { ColumnGap } + tile.words, .tone = FrameTone::Label });
        return pieces;
    }

    /// The headline tiles laid out for @p budget cells, as §5 draws them.
    ///
    /// **Two columns where they fit**: the tiles whose rows carry words or a trend fill the first and the
    /// rest fill the second, in `FleetKpis()` order within each -- which is what makes two columns fit at
    /// 80 wide at all. Decided by what a row CAN carry rather than by what this reading carried, so a tile
    /// does not jump columns the moment its denominator is first read. Narrower, one column with the
    /// words; narrower again, one without; and no tiles when not even a label and its value fit.
    /// @param in The frame's inputs.
    /// @param spec The document block.
    /// @param budget The content width.
    /// @return One item per line of tiles; none when not even one fits.
    [[nodiscard]] std::vector<Item> TileItems(FrameInputs const& in, DocumentSpec const& spec, std::size_t budget)
    {
        auto const tiles = Tiles(in);
        auto worded = std::vector<Tile const*> {};
        auto plain = std::vector<Tile const*> {};
        auto all = std::vector<Tile const*> {};
        for (auto const& tile: tiles)
        {
            (tile.worded ? worded : plain).push_back(&tile);
            all.push_back(&tile);
        }
        auto const indent = in.cellWidth(Indent);

        auto layout = std::vector<TileColumn> { MeasureTiles(in, worded, true), MeasureTiles(in, plain, true) };
        auto const twoFit =
            indent + TileColumnWidth(in, layout[0]) + in.cellWidth(TileGap) + TileColumnWidth(in, layout[1]) <= budget;
        if (!twoFit || layout[0].tiles.empty() || layout[1].tiles.empty())
        {
            layout = { MeasureTiles(in, all, true) };
            if (indent + TileColumnWidth(in, layout[0]) > budget)
                layout = { MeasureTiles(in, all, false) };
            if (indent + TileColumnWidth(in, layout[0]) > budget)
                return {};
        }

        auto lines = std::size_t { 0 };
        for (auto const& column: layout)
            lines = std::max(lines, column.tiles.size());
        auto items = std::vector<Item> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, lines))
        {
            auto pieces = std::vector<Piece> { Piece { .text = std::string { Indent } } };
            std::ranges::move(TilePieces(in, layout[0], index), std::back_inserter(pieces));
            if (layout.size() > 1 && index < layout[1].tiles.size())
            {
                // The first column padded to its width, so the second starts where every line's second does.
                auto const leftWidth = TileColumnWidth(in, layout[0]);
                auto const drawn = TextWidth(pieces, in.cellWidth) - in.cellWidth(Indent);
                pieces.push_back(
                    Piece { .text = std::string(leftWidth - std::min(leftWidth, drawn), ' ') + std::string { TileGap } });
                std::ranges::move(TilePieces(in, layout[1], index), std::back_inserter(pieces));
            }
            items.push_back(LineOf(pieces, {}, spec.tilePriority));
        }
        return items;
    }

    /// The key a section's tab answers to.
    struct SectionHotkey
    {
        FleetSection section; ///< The enumerator this row describes.
        std::string_view key; ///< The keystroke's bytes; empty for a section the strip does not name.
    };

    /// One row per `FleetSection`, in enumerator order: what the strip's `keys  m w l c t` hint lists.
    ///
    /// A letter per tab, from the section's key -- except `members`, whose `m` `machines` already has, so it
    /// answers to `c`, for cluster. None is a quit key or `/`, which the panel reads first.
    constexpr EnumTable<FleetSection, SectionHotkey> SectionHotkeyTable { {
        { .section = FleetSection::Kpi, .key = {} },
        { .section = FleetSection::Machines, .key = "m" },
        { .section = FleetSection::Workers, .key = "w" },
        { .section = FleetSection::Leases, .key = "l" },
        { .section = FleetSection::Members, .key = "c" },
        { .section = FleetSection::Tiers, .key = "t" },
    } };

    static_assert(RowsInEnumeratorOrder(SectionHotkeyTable, &SectionHotkey::section),
                  "SectionHotkeyTable must hold one row per FleetSection, in enumerator order");

    /// Whether every tab the strip names has a key of its own, and no other section has one.
    /// @return True when the keys are whole.
    [[nodiscard]] constexpr bool EveryTabHasItsOwnKey() noexcept
    {
        auto whole = true;
        for (auto const& row: Distributed::FleetSectionTable)
        {
            auto const key = SectionHotkeyTable[static_cast<std::size_t>(row.section)].key;
            whole = whole && row.tabular != key.empty();
            for (auto const& other: SectionHotkeyTable)
                whole = whole && (other.section == row.section || key.empty() || other.key != key);
        }
        return whole;
    }

    static_assert(EveryTabHasItsOwnKey(), "every tab the fleet strip names needs a key no other section has");

    /// The key @p section's tab answers to.
    /// @param section The section.
    /// @return Its key; empty for a section the strip does not name.
    [[nodiscard]] constexpr std::string_view SectionHotkeyOf(FleetSection section) noexcept
    {
        return SectionHotkeyTable[static_cast<std::size_t>(section)].key;
    }

    /// The section strip: every tabular section's key, the active one in brackets and dressed as selected,
    /// and the keys that name them right-aligned after.
    ///
    /// Not essential either: a terminal too narrow for the active tab draws no strip. The hint goes for
    /// width before any tab.
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
        // The hint's piece slot, so the spare cells go in front of it.
        constexpr auto HintSlot = std::size_t { 1 };
        auto pieces = std::vector<Piece> { Piece { .text = std::string { Indent }, .priority = Priority::Essential } };
        auto letters = std::string {};
        for (auto const& row: Distributed::FleetSectionTable)
        {
            if (!row.tabular)
                continue;
            auto const isActive = row.section == active;
            auto text = pieces.size() > 1 ? std::string { ColumnGap } : std::string {};
            text += isActive ? std::format("[{}]", row.key) : std::string { row.key };
            // The brackets stay where the terminal dresses the tab too: a plain frame is the same grid.
            pieces.push_back(Piece { .text = std::move(text),
                                     .priority = isActive ? Priority::Essential : spec.stripTabPriority,
                                     .tone = isActive ? std::optional<FrameTone> { FrameTone::Selected } : std::nullopt });
            auto const hotkey = SectionHotkeyOf(row.section);
            if (!hotkey.empty())
                letters += (letters.empty() ? "" : " ") + std::string { hotkey };
        }
        if (!letters.empty())
            pieces.push_back(Piece { .text = std::format("{}keys  {}", ColumnGap, letters),
                                     .priority = spec.keysHintPriority,
                                     .slot = HintSlot,
                                     .tone = FrameTone::Label });
        auto kept = FitPieces(std::move(pieces), budget, 0, in.cellWidth);
        if (!kept.has_value())
            return {};

        // The hint right-aligned: the spare cells go in front of it once the pieces that fit are known.
        auto const spare = budget - std::min(budget, TextWidth(*kept, in.cellWidth));
        for (auto& piece: *kept)
            if (piece.slot == HintSlot)
                piece.text.insert(0, spare, ' ');
        return { LineOf(*kept, {}, spec.stripPriority) };
    }

    /// What the operator has done to a document panel's table: how far it is scrolled and what filters it.
    struct TableState
    {
        std::string_view filter {}; ///< The text a row must contain; empty for none.
        std::size_t scroll { 0 };   ///< The matching rows skipped at the top.
        bool typing { false };      ///< Whether the filter is being typed.
    };

    /// A section's table as a frame draws it, with what the filter and the scroll made of it.
    struct SectionTable
    {
        std::vector<Item> items {}; ///< The table, or the absent marker where no table was read.
        std::size_t rows { 0 };     ///< The rows the leader sent.
        std::size_t matched { 0 };  ///< The rows the filter kept.
        std::size_t scroll { 0 };   ///< The scroll, clamped to the rows the filter kept.
    };

    /// How long a panel keeps a column of each `ColumnKeep` rank.
    struct KeepPriority
    {
        Distributed::ColumnKeep keep; ///< The enumerator this row describes.
        Priority priority;            ///< The panel's priority for it.
    };

    /// One row per `ColumnKeep`, in enumerator order: the leader's rank, in this layout's vocabulary.
    constexpr EnumTable<Distributed::ColumnKeep, KeepPriority> KeepPriorityTable { {
        { .keep = Distributed::ColumnKeep::Identity, .priority = Priority::Essential },
        { .keep = Distributed::ColumnKeep::Vital, .priority = Priority::High },
        { .keep = Distributed::ColumnKeep::Useful, .priority = Priority::Normal },
        { .keep = Distributed::ColumnKeep::Detail, .priority = Priority::Low },
    } };

    static_assert(RowsInEnumeratorOrder(KeepPriorityTable, &KeepPriority::keep),
                  "KeepPriorityTable must hold one row per ColumnKeep, in enumerator order");

    /// Whether @p text contains @p needle, ASCII letters compared without case.
    /// @param text The haystack.
    /// @param needle The needle; empty matches everything.
    /// @return True on a match.
    [[nodiscard]] bool ContainsIgnoringAsciiCase(std::string_view text, std::string_view needle) noexcept
    {
        auto const lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return !std::ranges::search(text, needle, [&lower](char a, char b) { return lower(a) == lower(b); }).empty()
               || needle.empty();
    }

    /// The frame's word for one of the leader's cell tones.
    struct CellToneDress
    {
        Distributed::CellTone tone;     ///< The enumerator this row describes.
        std::optional<FrameTone> frame; ///< How the frame dresses it; none for plain.
    };

    /// One row per `CellTone`, in enumerator order. A limit that withdrew slots is dressed as a stale age
    /// is: both say *look at this row*, and a terminal has fewer colours than the page's chips.
    constexpr EnumTable<Distributed::CellTone, CellToneDress> CellToneDressTable { {
        { .tone = Distributed::CellTone::Plain, .frame = std::nullopt },
        { .tone = Distributed::CellTone::Fresh, .frame = FrameTone::Fresh },
        { .tone = Distributed::CellTone::Stale, .frame = FrameTone::Stale },
        { .tone = Distributed::CellTone::Limited, .frame = FrameTone::Stale },
        { .tone = Distributed::CellTone::Alert, .frame = FrameTone::Alert },
    } };

    static_assert(RowsInEnumeratorOrder(CellToneDressTable, &CellToneDress::tone),
                  "CellToneDressTable must hold one row per CellTone, in enumerator order");

    /// The tone a cell is dressed with, where its column has one.
    ///
    /// Given whether or not the terminal shows colour: the presenter decides how a tone looks, and a
    /// plain one draws it as the text it already is. An absent cell has none, since a green age where
    /// nobody reported is a healthy reading nobody gave.
    /// @param section The section.
    /// @param column The column's name.
    /// @param cell The raw cell.
    /// @return The tone, or nullopt for none.
    [[nodiscard]] std::optional<FrameTone> CellFrameTone(FleetSection section, std::string_view column, Cell const& cell)
    {
        if (cell.kind == CellKind::Absent)
            return std::nullopt;
        auto const number = CellInteger(cell);
        auto const tone = number.has_value()
                              ? Distributed::FleetCellTone(section, column, *number)
                              : Distributed::FleetCellTone(section, column, std::string_view { cell.lexical });
        return tone < Distributed::CellTone::Last ? CellToneDressTable[static_cast<std::size_t>(tone)].frame : std::nullopt;
    }

    /// The active section's table, walked from the header line the leader sent.
    ///
    /// Every column's priority is its rank in the leader's tables (`FleetColumnKeep`), through
    /// `KeepPriorityTable`, and `FitColumns` fills the width by it; a name those tables do not have keeps the default. A
    /// header with no `Identity` column -- a section vocabulary this build does not know -- keeps its first column instead,
    /// so a line still says which row it is.
    /// @param in The frame's inputs.
    /// @param context The session facts: the section.
    /// @param spec The document block.
    /// @param state The filter and the scroll.
    /// @param budget The content width.
    /// @return The table -- or the absent marker where no document, or no such section, was read --
    ///         or the cells its essential columns needed when those do not fit.
    [[nodiscard]] std::expected<SectionTable, std::size_t> SectionItems(FrameInputs const& in,
                                                                        PanelContext const& context,
                                                                        DocumentSpec const& spec,
                                                                        TableState const& state,
                                                                        std::size_t budget)
    {
        auto const section = context.section;
        auto const* document = in.model->latestDocument.get();
        auto const* table = document == nullptr ? nullptr : document->Section(section);
        if (table == nullptr || table->columns.empty())
        {
            // Nothing read yet, or a reading without this section: the marker where the table goes,
            // never an empty table, which would claim the fleet has no such rows.
            auto line = std::string { Indent } + std::string { in.absent };
            if (in.cellWidth(line) > budget)
                return std::unexpected(in.cellWidth(line));
            return SectionTable { .items = { Item { .lines = { std::move(line) }, .priority = spec.tablePriority } } };
        }

        // Every cell written for a person FIRST, by the scale its column has in the leader's own tables,
        // so the widths below are the widths drawn, the drop for width sees the real figures, and the
        // filter matches what the operator reads.
        auto const formats = ColumnFormats(section, table->columns);
        auto matched = std::vector<std::size_t> {};
        auto cells = std::vector<std::vector<std::string>> {};
        for (auto const rowIndex: std::views::iota(std::size_t { 0 }, table->rows.size()))
        {
            auto const& row = table->rows[rowIndex];
            auto written = std::vector<std::string> {};
            written.reserve(row.size());
            for (auto const index: std::views::iota(std::size_t { 0 }, row.size()))
                written.push_back(HumanCellText(in, row[index], formats[index]));
            if (!std::ranges::any_of(
                    written, [&state](std::string const& cell) { return ContainsIgnoringAsciiCase(cell, state.filter); }))
                continue;
            matched.push_back(rowIndex);
            cells.push_back(std::move(written));
        }
        auto const scroll = matched.empty() ? 0 : std::min(state.scroll, matched.size() - 1);

        // Each column as wide as its widest matching cell, heading included, so no cell is ever cut.
        auto widths = std::vector<std::size_t> {};
        auto priorities = std::vector<Priority> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, table->columns.size()))
        {
            auto width = in.cellWidth(table->columns[index]);
            for (auto const& row: cells)
                width = std::max(width, in.cellWidth(row[index]));
            widths.push_back(width);
            auto const keep = Distributed::FleetColumnKeep(section, table->columns[index]);
            priorities.push_back(keep.has_value() ? KeepPriorityTable[static_cast<std::size_t>(*keep)].priority
                                                  : Priority::Normal);
        }
        if (std::ranges::none_of(priorities, [](Priority priority) { return priority == Priority::Essential; }))
            priorities.front() = Priority::Essential;

        auto const prefixOf = [](std::size_t index) {
            return index == 0 ? std::string { Indent } : std::string { ColumnGap };
        };
        auto const alignedOf = [&](std::size_t index, std::string_view text) {
            return index == 0 ? FitRight(text, widths[index], in.cellWidth) : AlignRight(text, widths[index], in.cellWidth);
        };
        auto heading = std::vector<Piece> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, table->columns.size()))
            heading.push_back(Piece { .text = prefixOf(index) + alignedOf(index, table->columns[index]),
                                      .priority = priorities[index],
                                      .slot = index });
        auto kept = FitColumns(std::move(heading), budget, in.cellWidth);
        if (!kept.has_value())
            return std::unexpected(kept.error());

        for (auto& piece: *kept)
            piece.tone = FrameTone::Label;
        auto headingLine = LineOf(*kept, {}, spec.tablePriority);
        auto item = Item { .lines = std::move(headingLine.lines),
                           .spans = std::move(headingLine.spans),
                           .noun = Distributed::FleetSectionTable[static_cast<std::size_t>(section)].key,
                           .above = scroll,
                           .rowsKept = spec.tableRowsKept,
                           .priority = spec.tablePriority,
                           .shrinkAt = spec.tableShrinkPriority,
                           .table = true };
        for (auto const shown: std::views::iota(scroll, cells.size()))
        {
            auto line = std::string {};
            for (auto const& piece: *kept)
            {
                line += prefixOf(piece.slot);
                auto const aligned = alignedOf(piece.slot, cells[shown][piece.slot]);
                auto const& raw = table->rows[matched[shown]][piece.slot];
                if (auto const tone = CellFrameTone(section, table->columns[piece.slot], raw); tone.has_value())
                {
                    auto const text = cells[shown][piece.slot];
                    auto const start = piece.slot == 0 ? 0 : aligned.size() - text.size();
                    item.spans.push_back(LineSpan {
                        .line = item.lines.size(), .byte = line.size() + start, .length = text.size(), .tone = *tone });
                }
                line += aligned;
            }
            item.lines.push_back(std::move(line));
        }
        return SectionTable {
            .items = { std::move(item) }, .rows = table->rows.size(), .matched = matched.size(), .scroll = scroll
        };
    }

    /// The line under a filtered table: what is typed, or what the filter kept.
    /// @param in The frame's inputs.
    /// @param spec The document block.
    /// @param state The filter.
    /// @param table What the filter made of the table.
    /// @param noun What the table's rows are.
    /// @param budget The content width.
    /// @return The line's item, or none when there is no filter and none is being typed.
    [[nodiscard]] std::vector<Item> FilterItems(FrameInputs const& in,
                                                DocumentSpec const& spec,
                                                TableState const& state,
                                                SectionTable const& table,
                                                std::string_view noun,
                                                std::size_t budget)
    {
        if (!state.typing && state.filter.empty())
            return {};
        // While typed it is essential -- an operator must see what they type -- and cut rather than
        // refused when it is wider than the terminal, since the text is theirs and not a figure.
        auto const text =
            state.typing
                ? std::format("{}filter  /{}_  Enter keeps, Esc clears", Indent, state.filter)
                : std::format("{}filter  /{}  {} of {} {}; / edits", Indent, state.filter, table.matched, table.rows, noun);
        return { Item { .lines = { FitRight(text, std::min(budget, in.cellWidth(text)), in.cellWidth) },
                        .priority = state.typing ? Priority::Essential : spec.filterPriority } };
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

    /// The fleet document block as one frame draws it, and where the table's scroll landed.
    struct DocumentBlock
    {
        std::vector<Item> items {}; ///< The block, top to bottom.
        std::size_t scroll { 0 };   ///< The table's scroll, clamped to its matching rows.
    };

    /// The fleet document block: the tiles, a blank, the chart, the strip, the active section's table and
    /// its filter line.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @param context The session facts: the section the table draws, the rung and the encoder.
    /// @param state The table's filter and scroll.
    /// @param budget The content width.
    /// @return The block, nothing for a panel without one, or the cells the table's essential columns needed.
    [[nodiscard]] std::expected<DocumentBlock, std::size_t> DocumentItems(FrameInputs const& in,
                                                                          PanelSpec const& spec,
                                                                          PanelContext const& context,
                                                                          TableState const& state,
                                                                          std::size_t budget)
    {
        auto block = DocumentBlock {};
        if (!spec.document.has_value())
            return block;
        auto table = SectionItems(in, context, *spec.document, state, budget);
        if (!table.has_value())
            return std::unexpected(table.error());
        auto& items = block.items;
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
        auto filter = FilterItems(in,
                                  *spec.document,
                                  state,
                                  *table,
                                  Distributed::FleetSectionTable[static_cast<std::size_t>(context.section)].key,
                                  budget);
        std::ranges::move(table->items, std::back_inserter(items));
        std::ranges::move(filter, std::back_inserter(items));
        block.scroll = table->scroll;
        return block;
    }

    /// Which source answered, as the source line names it.
    ///
    /// `metrics (/metrics at 127.0.0.1:9464)`: the source, and in brackets what was asked where. A reading
    /// from an endpoint with a role in its subject reads the other way round, because the route IS the
    /// source there: `/fleet.txt at build-01:9464 (leader)`. Each part the reader did not say is left out
    /// rather than guessed, and before any reading the whole of it is the absent marker.
    /// @param in The frame's inputs.
    /// @return The text.
    [[nodiscard]] std::string SourceText(FrameInputs const& in)
    {
        if (!in.model->latestStamp.has_value())
            return std::string { in.absent };
        auto const& stamp = *in.model->latestStamp;
        if (stamp.route.empty())
            return stamp.source;
        auto const asked = stamp.where.empty() ? stamp.route : std::format("{} at {}", stamp.route, stamp.where);
        if (!stamp.role.empty())
            return std::format("{} ({})", asked, stamp.role);
        return std::format("{} ({})", stamp.source, asked);
    }

    /// The last line: which source answered on the left, and on the right how many samples and how many
    /// of them were gaps.
    ///
    /// The counts go first for width -- they describe the run, the source says what the run is of -- and
    /// what is kept is laid out right-aligned, so the counts stay in one place as the source's text changes.
    /// @param in The frame's inputs.
    /// @param priority When the line goes; the counts go before it.
    /// @param budget The content width.
    /// @return The line; nullopt when even the source was dropped; or, when an essential source does not
    ///         fit, the cells it needed.
    [[nodiscard]] std::expected<std::optional<std::string>, std::size_t> SourceLine(FrameInputs const& in,
                                                                                    Priority priority,
                                                                                    std::size_t budget)
    {
        auto const& model = *in.model;
        auto const gaps =
            std::ranges::count_if(model.history, [](HistoryEntry const& entry) { return !entry.reading.has_value(); });
        auto const counts = std::format("{} samples, {} gap{}", model.samples, gaps, gaps == 1 ? "" : "s");
        auto kept =
            FitPieces({ Piece { .text = std::format("{}source  {}", Indent, SourceText(in)), .priority = priority },
                        Piece { .text = std::string { PieceGap } + counts, .priority = std::max(priority, Priority::Low) } },
                      budget,
                      0,
                      in.cellWidth);
        if (!kept.has_value())
            return std::unexpected(kept.error());
        if (kept->empty())
            return std::optional<std::string> {};
        auto line = kept->front().text;
        if (kept->size() == 2)
        {
            auto const used = in.cellWidth(line) + in.cellWidth(kept->back().text);
            line.append(budget - used, ' ');
            line += kept->back().text;
        }
        return line;
    }

    /// What renders one chrome fact in one frame.
    struct ChromeFactSpec
    {
        ChromeFact fact; ///< The enumerator this row describes.
        /// The fact's text in this frame, its absent marker by name where it has no reading.
        std::string (*render)(FrameInputs const& in, PanelContext const& context);
    };

    /// A text field of the newest reading, or nullptr when it carries none.
    /// @param model What is known.
    /// @param name The field.
    /// @return The field's lexical form, or nullptr.
    [[nodiscard]] std::string const* NewestText(DashboardModel const& model, std::string_view name) noexcept
    {
        auto const* field = model.latest.has_value() ? FindField(*model.latest, name) : nullptr;
        return field == nullptr || field->value.kind == CellKind::Absent ? nullptr : &field->value.lexical;
    }

    /// How long the endpoint has served, from whichever reading carries it.
    /// @param model What is known.
    /// @return Seconds, or nullopt when nothing read says.
    [[nodiscard]] std::optional<std::uint64_t> UptimeOf(DashboardModel const& model) noexcept
    {
        if (model.nodeStatus.has_value())
            return model.nodeStatus->uptimeSeconds;
        auto const* text = NewestText(model, CacheUptimeField);
        if (text == nullptr)
            return std::nullopt;
        auto seconds = std::uint64_t { 0 };
        auto const [end, error] = std::from_chars(text->data(), text->data() + text->size(), seconds);
        return error == std::errc {} && end == text->data() + text->size() ? std::optional { seconds } : std::nullopt;
    }

    /// One row per `ChromeFact`, in enumerator order.
    constexpr auto ChromeFactTable = EnumTable<ChromeFact, ChromeFactSpec> { {
        { .fact = ChromeFact::Version,
          .render = [](FrameInputs const& in, PanelContext const& /*context*/) -> std::string {
              // A node says it in its status; a cache's INFO says it as a field. Neither is invented.
              if (in.model->nodeStatus.has_value() && !in.model->nodeStatus->version.empty())
                  return in.model->nodeStatus->version;
              auto const* text = NewestText(*in.model, CacheVersionField);
              return text == nullptr ? std::string { in.absent } : *text;
          } },
        { .fact = ChromeFact::Endpoint,
          .render = [](FrameInputs const& in, PanelContext const& context) -> std::string {
              return context.endpoint.empty() ? std::string { in.absent } : context.endpoint;
          } },
        { .fact = ChromeFact::Leader,
          .render = [](FrameInputs const& in, PanelContext const& context) -> std::string {
              // Stated as the leader's only once a reading came from it: an endpoint that has not answered as
              // the leader has not been shown to be one, and is still where the session asks.
              auto const led = in.model->latestStamp.has_value() && in.model->latestStamp->role == LeaderRole;
              auto const where = context.endpoint.empty() ? in.absent : std::string_view { context.endpoint };
              return led ? std::format("{} {}", LeaderRole, where) : std::string { where };
          } },
        { .fact = ChromeFact::Uptime,
          .render = [](FrameInputs const& in, PanelContext const& /*context*/) -> std::string {
              auto const seconds = UptimeOf(*in.model);
              return std::format("up {}", seconds.has_value() ? UptimeText(*seconds) : std::string { in.absent });
          } },
        { .fact = ChromeFact::Machines,
          .render = [](FrameInputs const& in, PanelContext const& /*context*/) -> std::string {
              auto const* document = in.model->latestDocument.get();
              auto const* machines = document == nullptr ? nullptr : document->Section(FleetSection::Machines);
              return machines == nullptr ? std::format("{} machines", in.absent)
                                         : std::format("{} machines", machines->rows.size());
          } },
        { .fact = ChromeFact::Interval,
          .render = [](FrameInputs const& in, PanelContext const& context) -> std::string {
              return context.interval.has_value()
                         ? std::format("every {}s", std::chrono::duration<double> { *context.interval }.count())
                         : std::format("every {}", in.absent);
          } },
        { .fact = ChromeFact::Quit,
          .render = [](FrameInputs const& /*in*/, PanelContext const& /*context*/) -> std::string { return "q"; } },
        { .fact = ChromeFact::QuitWord,
          .render = [](FrameInputs const& /*in*/, PanelContext const& /*context*/) -> std::string { return "q quit"; } },
    } };

    static_assert(RowsInEnumeratorOrder(ChromeFactTable, &ChromeFactSpec::fact),
                  "ChromeFactTable must hold one row per ChromeFact, in enumerator order");

    /// A title bar's two halves.
    struct TitleText
    {
        std::string subject {}; ///< The left half: the subject and what is stated beside it.
        std::string facts {};   ///< The right half, right-aligned by the frame.
    };

    /// The title bar @p spec states, fitted to a frame @p columns wide.
    ///
    /// The subject is essential; every fact goes in `Priority` order, the rightmost among equals first, so
    /// what a narrow terminal keeps is decided by the same one function that decides every other line.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @param context The session's facts.
    /// @param columns The frame's width.
    /// @return The two halves.
    [[nodiscard]] TitleText TitleFor(FrameInputs const& in,
                                     PanelSpec const& spec,
                                     PanelContext const& context,
                                     std::size_t columns)
    {
        // `┌─ ` subject ` ` fill ` ` facts ` ─┐`: two corners, two edge glyphs, four spaces and the one fill
        // cell that keeps the halves apart are what the text cannot have.
        constexpr auto Chrome = std::size_t { 9 };
        constexpr auto SubjectSlot = std::size_t { 0 };
        constexpr auto FactsSlot = std::size_t { 1 };
        auto pieces = std::vector<Piece> { Piece { .text = std::string { spec.title }, .priority = Priority::Essential } };
        for (auto const& row: spec.titleFacts)
        {
            auto const beside = row.side == TitleSide::Subject;
            auto text = ChromeFactTable[static_cast<std::size_t>(row.fact)].render(in, context);
            pieces.push_back(Piece { .text = std::string { beside ? " " : "  " } + text,
                                     .priority = row.priority,
                                     .slot = beside ? SubjectSlot : FactsSlot });
        }
        auto kept = FitPieces(std::move(pieces), columns > Chrome ? columns - Chrome : 0, 0, in.cellWidth);
        auto title = TitleText {};
        if (!kept.has_value())
        {
            // Not even the subject fits; the frame cuts it.
            title.subject = std::string { spec.title };
            return title;
        }
        for (auto const& piece: *kept)
            (piece.slot == SubjectSlot ? title.subject : title.facts) += piece.text;
        // The first fact kept on the right carries the gap written for one before it.
        title.facts.erase(0, std::min(title.facts.find_first_not_of(' '), title.facts.size()));
        return title;
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

std::string UptimeText(std::uint64_t seconds)
{
    constexpr auto PerMinute = std::uint64_t { 60 };
    constexpr auto PerHour = PerMinute * 60;
    constexpr auto PerDay = PerHour * 24;
    return std::format("{}d{:02}:{:02}", seconds / PerDay, (seconds % PerDay) / PerHour, (seconds % PerHour) / PerMinute);
}

std::string_view NameIn(FieldNames const& names, StatsOrigin origin) noexcept
{
    return names.*(OriginFieldTable[static_cast<std::size_t>(origin)].name);
}

std::string TierFigureKey(std::string_view tier, std::string_view column)
{
    return std::format("{}{}{}", tier, TierKeySeparator, column);
}

std::vector<std::string_view> TiersIn(PanelSpec const& spec, Value const& reading, StatsOrigin origin)
{
    auto tiers = std::vector<std::string_view> {};
    if (spec.tierColumns.empty())
        return tiers;
    auto const name = NameIn(spec.tierColumns.front().figure.field, origin);
    if (name.empty())
        return tiers;
    for (auto const& tier: StorageTierTable)
        if (FindField(reading, TierSeriesName(name, tier.name)) != nullptr)
            tiers.push_back(tier.name);
    return tiers;
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

    /// Which way a scrolling key moves a document panel's table.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class TableScroll : std::uint8_t
    {
        PageDown, ///< By the rows the last frame showed.
        PageUp,   ///< Back by as many.
        Top,      ///< To the first row.
    };

    /// A keystroke that scrolls the table, as the bytes `KeyBytes` delivers it.
    struct TableScrollKey
    {
        std::string_view keys; ///< The keystroke's bytes.
        TableScroll scroll;    ///< What it does.
    };

    /// Every scrolling key.
    constexpr auto TableScrollKeys = std::to_array<TableScrollKey>({
        { .keys = "\x1b[6~", .scroll = TableScroll::PageDown },
        { .keys = "\x1b[5~", .scroll = TableScroll::PageUp },
        { .keys = "\x1b[H", .scroll = TableScroll::Top },
    });

    /// The key that starts typing a filter.
    constexpr std::string_view FilterStartKey = "/";

    /// What a key does to a filter being typed.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class FilterEdit : std::uint8_t
    {
        Keep,      ///< Stop typing and keep the filter.
        Clear,     ///< Stop typing and clear it.
        Backspace, ///< Take its last character.
    };

    /// A keystroke that edits a filter being typed, as the bytes `KeyBytes` delivers it.
    struct FilterEditKey
    {
        std::string_view keys; ///< The keystroke's bytes.
        FilterEdit edit;       ///< What it does.
    };

    /// Every editing key; any other printable text is typed into the filter.
    constexpr auto FilterEditKeys = std::to_array<FilterEditKey>({
        { .keys = "\r", .edit = FilterEdit::Keep },
        { .keys = "\x1b", .edit = FilterEdit::Clear },
        { .keys = "\x7f", .edit = FilterEdit::Backspace },
        { .keys = "\b", .edit = FilterEdit::Backspace },
    });

    /// Whether @p keys is text to type: no control byte and no escape sequence.
    /// @param keys The keystroke's bytes.
    /// @return True for printable text.
    [[nodiscard]] bool IsTypedText(std::string_view keys) noexcept
    {
        return !keys.empty() && std::ranges::none_of(keys, [](char c) {
            auto const byte = static_cast<unsigned char>(c);
            return byte < 0x20U || byte == 0x7FU;
        });
    }

    /// @p text without its last UTF-8 character.
    /// @param text Valid UTF-8.
    void PopCharacter(std::string& text) noexcept
    {
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U)
            text.pop_back();
        if (!text.empty())
            text.pop_back();
    }
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

    auto const* named = FindIfOrNull(tabs, [keys](FleetSection tab) { return SectionHotkeyOf(tab) == keys; });
    if (named != nullptr)
        return *named;

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

bool PanelView::Key(std::string_view keys)
{
    if (!_spec->document.has_value())
        return false;

    if (_typingFilter)
    {
        if (auto const* edit = FindIfOrNull(FilterEditKeys, [keys](FilterEditKey const& row) { return row.keys == keys; }))
        {
            switch (edit->edit)
            {
                case FilterEdit::Keep:
                    _typingFilter = false;
                    return true;
                case FilterEdit::Clear:
                    _typingFilter = false;
                    _filter.clear();
                    _scroll = 0;
                    return true;
                case FilterEdit::Backspace:
                    if (_filter.empty())
                        return false;
                    PopCharacter(_filter);
                    _scroll = 0;
                    return true;
            }
        }
        if (!IsTypedText(keys))
            return false;
        _filter += keys;
        _scroll = 0;
        return true;
    }

    if (keys == FilterStartKey)
    {
        _typingFilter = true;
        return true;
    }

    if (auto const* scroll = FindIfOrNull(TableScrollKeys, [keys](TableScrollKey const& row) { return row.keys == keys; }))
    {
        auto const before = _scroll;
        switch (scroll->scroll)
        {
            case TableScroll::PageDown:
                _scroll += _page;
                break;
            case TableScroll::PageUp:
                _scroll -= std::min(_scroll, _page);
                break;
            case TableScroll::Top:
                _scroll = 0;
                break;
        }
        return _scroll != before;
    }

    auto const section = SectionForKey(_context.section, keys);
    if (!section.has_value() || *section == _context.section)
        return false;
    // A section is its own table: it starts at its top, with nothing filtered out of it.
    _context.section = *section;
    _scroll = 0;
    _filter.clear();
    return true;
}

bool PanelView::CapturesText() const noexcept
{
    return _typingFilter;
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
    auto document = DocumentItems(
        in, *_spec, _context, TableState { .filter = _filter, .scroll = _scroll, .typing = _typingFilter }, budget);
    for (auto const* block: { &rates, &levels, &tiers })
        if (!block->has_value())
            return tooSmall(block->error() + FrameColumns);
    if (!document.has_value())
        return tooSmall(document.error() + FrameColumns);
    _scroll = document->scroll;

    auto items = std::vector<Item> {};
    for (auto* block: { &*rates, &*levels, &*tiers, &document->items })
    {
        if (block->empty())
            continue;
        items.push_back(Blank());
        std::ranges::move(*block, std::back_inserter(items));
    }
    items.push_back(Blank());
    auto source = SourceLine(in, _spec->sourcePriority, budget);
    if (!source.has_value())
        return tooSmall(source.error() + FrameColumns);
    auto const& sourceLine = *source;
    if (sourceLine.has_value())
        items.push_back(Item { .lines = { *sourceLine }, .priority = _spec->sourcePriority });

    // The chart grows into rows nothing else wanted, then the frame pads to the terminal's height: both
    // after a first fit, so neither ever takes a row a table or a tile could have had.
    auto fitted = FitRows(items, available);
    if (!fitted.has_value())
        return tooSmall(0);
    if (available.has_value() && _spec->document.has_value() && fitted->imageLine.has_value())
    {
        auto const spare = *available - std::min(*available, fitted->lines.size());
        auto const grown = std::min(_spec->document->chartCellsHighMost, fitted->imageRows + spare);
        if (grown > fitted->imageRows)
        {
            for (auto& item: items)
                if (item.image)
                    item.lines.resize(grown);
            if (auto regrown = FitRows(items, available); regrown.has_value())
                fitted = std::move(regrown);
        }
    }
    if (available.has_value() && _spec->fillsHeight && fitted->lines.size() < *available)
    {
        auto const pad = *available - fitted->lines.size();
        auto const at = sourceLine.has_value() && !fitted->lines.empty() && fitted->lines.back() == *sourceLine
                            ? fitted->lines.size() - 1
                            : fitted->lines.size();
        fitted->lines.insert(fitted->lines.begin() + static_cast<std::ptrdiff_t>(at), pad, std::string {});
    }
    if (fitted->tableShown.has_value())
        _page = std::max<std::size_t>(1, *fitted->tableShown);

    auto const title = TitleFor(in, *_spec, _context, columns);
    auto frame =
        DashboardFrame { .text = Cli::Frame(title.subject, title.facts, fitted->lines, columns, *_glyphs, in.cellWidth),
                         .placements = {},
                         .spans = {} };
    // A content line's frame row is its index plus two, after the top edge, and its text starts after the
    // left edge's bytes.
    for (auto const& span: fitted->spans)
        frame.spans.push_back(FrameSpan {
            .row = span.line + 2, .byte = _glyphs->vertical.size() + span.byte, .length = span.length, .tone = span.tone });

    // The chart kept its rows: draw the image over them. The frame's first row is its top edge and its
    // first column its left edge, so a content line's frame row is its index plus two and the chart's
    // first cell sits after the edge and the indent.
    if (fitted->imageLine.has_value() && _spec->document.has_value() && model.cellPixels.has_value()
        && _context.sixel != nullptr)
    {
        auto const cellsWide = budget - in.cellWidth(Indent);
        auto const cellsHigh = fitted->imageRows;
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
