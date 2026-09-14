// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanel.hpp"
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"
#include "NodeSlots.hpp"
#include "NodeStatusText.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <ranges>
#include <span>
#include <system_error>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    using Series = std::vector<std::optional<double>>;

    /// The tier a `StorageTierTable` name names, or nullopt for empty (the whole cache) and for a name no row has.
    /// @param name The name.
    /// @return The tier.
    [[nodiscard]] std::optional<StorageTier> TierNamed(std::string_view name) noexcept
    {
        auto const* row =
            name.empty() ? nullptr : FindIfOrNull(StorageTierTable, [name](auto const& tier) { return tier.name == name; });
        return row == nullptr ? std::nullopt : std::optional { row->tier };
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
    /// @param field The field; one naming nothing yields an absent series.
    /// @param tier The tier asked about, or nullopt for the whole cache.
    /// @return The series.
    [[nodiscard]] Series Levels(std::deque<HistoryEntry> const& history, ReadingField field, std::optional<StorageTier> tier)
    {
        auto series = Series(history.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, history.size()))
            series[index] = NumberIn(history[index].stats, field, tier);
        return series;
    }

    /// Each entry's rate of one counter.
    /// @param history The samples.
    /// @param field The counter; one naming nothing yields an absent series.
    /// @param tier The tier asked about, or nullopt for the whole cache.
    /// @return The series.
    [[nodiscard]] Series Rates(std::deque<HistoryEntry> const& history, ReadingField field, std::optional<StorageTier> tier)
    {
        return CounterRateSeries(history, field, tier);
    }

    /// How one `FigureSource` is computed.
    struct FigureSourceSpec
    {
        FigureSource source; ///< The enumerator this row describes.
        /// The computation, over the primary and second fields, for the whole cache or one tier.
        Series (*compute)(std::deque<HistoryEntry> const& history,
                          ReadingField field,
                          ReadingField other,
                          std::optional<StorageTier> tier);
        /// Whether a figure's `addends` add their rates to what `compute` made.
        bool takesAddends { false };
    };

    /// One row per `FigureSource`, in enumerator order.
    constexpr EnumTable<FigureSource, FigureSourceSpec> FigureSourceTable { {
        { .source = FigureSource::Level,
          .compute = [](std::deque<HistoryEntry> const& history,
                        ReadingField field,
                        ReadingField /*other*/,
                        std::optional<StorageTier> tier) { return Levels(history, field, tier); } },
        { .source = FigureSource::LevelRatio,
          .compute =
              [](std::deque<HistoryEntry> const& history,
                 ReadingField field,
                 ReadingField other,
                 std::optional<StorageTier> tier) {
                  return Pairwise(Levels(history, field, tier), Levels(history, other, tier), &Proportion);
              } },
        { .source = FigureSource::Rate,
          .compute =
              [](std::deque<HistoryEntry> const& history,
                 ReadingField field,
                 ReadingField other,
                 std::optional<StorageTier> tier) {
                  // A second counter is an ADDEND only when named: `ops/sec` is gets plus sets. An
                  // unnamed one must not turn the rate absent, so it is not paired at all.
                  return !other.Names() ? Rates(history, field, tier)
                                        : Pairwise(Rates(history, field, tier), Rates(history, other, tier), &Sum);
              },
          .takesAddends = true },
        { .source = FigureSource::RateRatio,
          .compute =
              [](std::deque<HistoryEntry> const& history,
                 ReadingField field,
                 ReadingField other,
                 std::optional<StorageTier> tier) {
                  return Pairwise(Rates(history, field, tier), Rates(history, other, tier), &Proportion);
              } },
        { .source = FigureSource::RateQuotient,
          .compute =
              [](std::deque<HistoryEntry> const& history,
                 ReadingField field,
                 ReadingField other,
                 std::optional<StorageTier> tier) {
                  return Pairwise(Rates(history, field, tier), Rates(history, other, tier), &Quotient);
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

    /// Columns a level row's reading is budgeted when the minimum size is derived.
    ///
    /// A budget and not a pad: the reading is written straight after its label, left-aligned, as §3 draws
    /// `items       1 284 991` and `connected   -  no level is exported`. Right-aligning it into this many
    /// cells cost the `connected` row its reason at 80 columns (#134 C2, C7).
    constexpr auto LevelFigureColumns = std::size_t { 12 };

    /// Cells in a level row's gauge.
    constexpr auto GaugeCells = std::size_t { 20 };

    /// Columns a tier row's name takes, after its indent.
    constexpr auto TierNameColumns = std::size_t { 8 };

    /// Columns each tier figure is right-aligned into, at the least.
    constexpr auto TierFigureColumns = std::size_t { 12 };

    /// Blank cells a tier column keeps before its widest cell, as §3's table separates every pair.
    constexpr auto TierColumnGap = std::size_t { 3 };

    /// The indent every content line starts with.
    constexpr std::string_view Indent = "  ";

    /// What separates a figure from the trend after it.
    constexpr std::string_view TrendGap = "  ";

    /// What separates one beside piece from the one before it.
    constexpr std::string_view PieceGap = "   ";

    /// What one frame is drawn from, looked up once.
    struct FrameInputs
    {
        DashboardModel const* model; ///< What is known.
        RungGlyphs const* glyphs;    ///< What to draw with.
        std::string_view absent;     ///< The absent marker.
        CellWidth cellWidth;         ///< How wide text is.
    };

    /// @p figure's series for this frame.
    /// @param in The frame's inputs.
    /// @param figure The figure.
    /// @param tier A tier's name, or empty for the whole cache.
    /// @return The series; never empty while the history is not.
    [[nodiscard]] Series SeriesFor(FrameInputs const& in, FigureSpec const& figure, std::string_view tier)
    {
        return FigureSeries(in.model->history, figure, tier);
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

    /// The newest @p cells of @p series, from the trend's first cell: a session younger than the trend
    /// fills it from the left, and the cells after its newest reading are drawn as no reading, which is
    /// what they are -- nothing has been sampled for them yet.
    ///
    /// **From the left, so a trend starts where its figure ends** (#134 §3, §4). Right-aligned, a young
    /// session drew a blank run between the figure and its first readings that read as a narrow trend
    /// pushed to the far edge; once the history is as long as the trend the two are the same picture,
    /// newest on the right, and it scrolls.
    /// @param series The series.
    /// @param cells How many cells the sparkline has.
    /// @return The window.
    [[nodiscard]] Series Window(Series const& series, std::size_t cells)
    {
        auto window = Series(cells);
        auto const shown = std::min(cells, series.size());
        std::ranges::copy(series | std::views::drop(series.size() - shown), window.begin());
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
    struct Item
    {
        std::vector<std::string> lines {};      ///< Its lines; a table's first is its heading.
        Priority priority { Priority::Normal }; ///< When it goes.
        std::size_t hidden { 0 };               ///< How many of a table's rows are not shown.
        bool table { false };                   ///< Whether it shrinks to `+N more` before it goes.
        bool image { false };                   ///< Whether its lines are blank cells an image is placed over.
        std::vector<LineSpan> spans {};         ///< The runs of its lines a presenter dresses.
    };

    /// The lines a layout keeps, the runs it dresses, and where its image item landed if it kept one.
    struct FittedRows
    {
        std::vector<std::string> lines {};       ///< Every kept line, top to bottom.
        std::vector<LineSpan> spans {};          ///< Every kept run; `line` indexes `lines`.
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

        auto fitted = FittedRows {};
        for (auto& item: items)
        {
            // A table keeps its heading and the rows it shows, and the runs on them; a run on a hidden row
            // goes with the row.
            auto const first = fitted.lines.size();
            auto const kept = item.table ? 1 + ((item.lines.size() - 1) - item.hidden) : item.lines.size();
            for (auto const& span: item.spans)
                if (span.line < kept)
                    fitted.spans.push_back(
                        LineSpan { .line = first + span.line, .byte = span.byte, .length = span.length, .tone = span.tone });
            if (item.image)
                fitted.imageLine = first;
            std::ranges::move(item.lines | std::views::take(kept), std::back_inserter(fitted.lines));
            if (item.table && item.hidden > 0)
                fitted.lines.push_back(std::format("{}+{} more", Indent, item.hidden));
        }
        return fitted;
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

    /// Whether @p value is past what @p figure calls worth acting on.
    /// @param figure The figure.
    /// @param value Its newest value.
    /// @return True for a present, finite value above the figure's alert threshold.
    [[nodiscard]] bool Alarming(FigureSpec const& figure, std::optional<double> value) noexcept
    {
        return figure.alertAbove.has_value() && value.has_value() && std::isfinite(*value) && *value > *figure.alertAbove;
    }

    /// How a figure is dressed: an alert when it is past its threshold, @p otherwise when it is a reading that is
    /// not, and plain for the absent marker.
    /// @param figure The figure.
    /// @param value Its newest value.
    /// @param otherwise The tone of an unalarming reading; nullopt for none.
    /// @return The tone.
    [[nodiscard]] std::optional<FrameTone> FigureTone(FigureSpec const& figure,
                                                      std::optional<double> value,
                                                      std::optional<FrameTone> otherwise) noexcept
    {
        if (Alarming(figure, value))
            return FrameTone::Alert;
        // The absent marker is no figure, so it is never given a figure's weight.
        return value.has_value() && std::isfinite(*value) ? otherwise : std::nullopt;
    }

    /// A figure written beside something else, with its words around it: `completed 12 884`.
    /// @param in The frame's inputs.
    /// @param beside The figure.
    /// @return The text, dressed as an alert when the figure is past its threshold and not at all otherwise:
    ///         one piece carries one tone, and the words and the value go for width together.
    [[nodiscard]] Piece BesidePiece(FrameInputs const& in, BesideFigure const& beside)
    {
        auto const value = Newest(SeriesFor(in, beside.figure, {}));
        auto text = std::string {};
        if (!beside.before.empty())
            text += std::string { beside.before } + " ";
        text += FigureText(in, beside.figure, value);
        if (!beside.after.empty())
            text += " " + std::string { beside.after };
        return Piece { .text = std::move(text),
                       .priority = beside.priority,
                       .tone = FigureTone(beside.figure, value, std::nullopt) };
    }

    /// @p piece with @p lead written before its text, its tone still covering only the text.
    /// @param lead What goes before: a gap or an indent.
    /// @param piece The piece.
    /// @return The piece.
    [[nodiscard]] Piece Led(std::string_view lead, Piece piece)
    {
        piece.text.insert(0, lead);
        return piece;
    }

    /// @p text wrapped at its spaces into lines of at most @p width cells; a word wider than that is a line of its own.
    /// @param text Prose.
    /// @param width The cells a line may take.
    /// @param cellWidth How wide text is.
    /// @return The lines, none for text with no words.
    [[nodiscard]] std::vector<std::string> Wrapped(std::string_view text, std::size_t width, CellWidth cellWidth)
    {
        auto lines = std::vector<std::string> {};
        auto line = std::string {};
        for (auto const word: std::views::split(text, ' '))
        {
            auto const piece = std::string_view { word.begin(), word.end() };
            if (piece.empty())
                continue;
            auto candidate = line.empty() ? std::string { piece } : std::format("{} {}", line, piece);
            if (!line.empty() && cellWidth(candidate) > width)
            {
                lines.push_back(std::exchange(line, std::string { piece }));
                continue;
            }
            line = std::move(candidate);
        }
        if (!line.empty())
            lines.push_back(std::move(line));
        return lines;
    }

    /// The fewest cells a wrapped note is given before it is dropped rather than wrapped narrower.
    constexpr auto MinimumNoteCells = std::size_t { 16 };

    /// What a breakdown line under a rate row starts with: deeper than the rows, so it reads as theirs.
    constexpr std::string_view SplitIndent = "      ";

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

    /// How a figure's text is dressed: as a figure when there is a reading, plain when the text is the absent
    /// marker, which is no figure to give weight to.
    /// @param value The reading.
    /// @return The tone, or none.
    [[nodiscard]] std::optional<FrameTone> FigureTone(std::optional<double> value) noexcept
    {
        return value.has_value() && std::isfinite(*value) ? std::optional { FrameTone::Figure } : std::nullopt;
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
        // Where a wrapped note starts on its row, and its lines.
        struct WrappedNote
        {
            std::size_t start { 0 };           ///< The cell its first line starts at, and its later lines hang at.
            std::vector<std::string> lines {}; ///< Its lines, without the space before them.
        };
        auto notes = std::vector<WrappedNote> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
        {
            auto const& row = rows[index];
            // The label recedes and the figure carries the weight, so they are two pieces, both essential.
            auto pieces = std::vector<Piece> {
                Piece { .text = std::string { Indent } + FitRight(row.label, LabelColumns, in.cellWidth),
                        .priority = Priority::Essential,
                        .tone = FrameTone::Label },
                Piece { .text = AlignRight(figures[index], figureColumns, in.cellWidth),
                        .priority = Priority::Essential,
                        .tone = FigureTone(row.figure, Newest(series[index]), FrameTone::Figure) },
            };
            if (drawsTrend && row.trend == Trend::Drawn)
                pieces.push_back(Piece { .priority = row.trendPriority, .trend = true });
            for (auto const& beside: row.beside)
                pieces.push_back(Led(PieceGap, BesidePiece(in, beside)));
            // A row with no trend has nothing whose width the note competes with, so its note wraps under the
            // row instead of going; a row with one keeps its note a piece like any other.
            auto const wraps = row.trend == Trend::None && !row.note.empty();
            if (!row.note.empty() && !wraps)
                pieces.push_back(Piece { .text = std::string { PieceGap } + std::string { row.note },
                                         .priority = row.notePriority,
                                         .tone = FrameTone::Label });

            auto kept = FitPieces(std::move(pieces), budget, trendMinimum, in.cellWidth);
            if (!kept.has_value())
                return std::unexpected(kept.error());
            auto note = WrappedNote {};
            if (wraps)
            {
                note.start = TextWidth(*kept, in.cellWidth) + in.cellWidth(PieceGap);
                if (budget >= note.start + MinimumNoteCells)
                    note.lines = Wrapped(row.note, budget - note.start, in.cellWidth);
            }
            notes.push_back(std::move(note));
            fitted.push_back(std::move(*kept));
        }

        // Every trend is one width: what the block's widest beside text leaves. Each row's kept text
        // fitted beside a MINIMUM trend, so the reserve never pushes a trend below its minimum.
        auto essential = std::size_t { 0 };
        for (auto const& pieces: fitted)
        {
            // The label and the figure are the row's first two pieces, and both essential.
            auto const leading = std::span { pieces }.first(std::min<std::size_t>(2, pieces.size()));
            essential = std::max(essential, TextWidth(leading, in.cellWidth));
            if (std::ranges::any_of(pieces, &Piece::trend))
                reserve = std::max(reserve, TextWidth(std::span { pieces }.subspan(leading.size()), in.cellWidth));
        }
        auto const used = essential + in.cellWidth(TrendGap) + reserve;
        auto const trendCells = budget > used ? std::max(MinimumTrendCells, budget - used) : MinimumTrendCells;

        auto items = std::vector<Item> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
        {
            auto const& row = rows[index];
            auto const trend = Sparkline(Window(series[index], trendCells), *in.glyphs);
            auto item = LineOf(fitted[index], trend, row.priority);
            // The note's first line continues the row; the rest hang under where it began. Each is a Label run.
            auto const& note = notes[index];
            for (auto const at: std::views::iota(std::size_t { 0 }, note.lines.size()))
            {
                if (at > 0)
                    item.lines.emplace_back();
                auto& line = item.lines.back();
                line.append(at == 0 ? PieceGap.size() : note.start, ' ');
                item.spans.push_back(LineSpan { .line = item.lines.size() - 1,
                                                .byte = line.size(),
                                                .length = note.lines[at].size(),
                                                .tone = FrameTone::Label });
                line += note.lines[at];
            }
            items.push_back(std::move(item));

            if (row.split.empty())
                continue;
            // The breakdown goes with the row, part by part as the width asks, and is its own line so a
            // narrow terminal keeps the total's trend.
            auto parts = std::vector<Piece> {};
            for (auto const& part: row.split)
                parts.push_back(Led(parts.empty() ? SplitIndent : PieceGap, BesidePiece(in, part)));
            auto kept = FitPieces(std::move(parts), budget, 0, in.cellWidth);
            if (kept.has_value() && !kept->empty())
                items.push_back(LineOf(*kept, {}, row.priority));
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
        // The readings start in one column, and it is never narrower than the widest label and a blank: a
        // left-aligned reading written against its label would read as one word with it.
        auto labelColumns = LevelLabelColumns;
        for (auto const& row: rows)
            labelColumns = std::max(labelColumns, in.cellWidth(row.label) + 1);

        auto items = std::vector<Item> {};
        for (auto const& row: rows)
        {
            auto const value = Newest(SeriesFor(in, row.value, {}));
            // The label and the reading are two ESSENTIAL pieces, so each is dressed as what it is (G1): a
            // label recedes, a reading carries weight, and an absent reading is left plain.
            auto pieces = std::vector<Piece> {
                Piece { .text = std::string { Indent } + FitRight(row.label, labelColumns, in.cellWidth),
                        .priority = Priority::Essential,
                        .tone = FrameTone::Label },
                Piece {
                    .text = FigureText(in, row.value, value), .priority = Priority::Essential, .tone = FigureTone(value) },
            };
            if (row.limit.has_value())
            {
                auto const limit = Newest(SeriesFor(in, *row.limit, {}));
                pieces.push_back(Piece { .text = " / " + FigureText(in, *row.limit, limit),
                                         .priority = row.limitPriority,
                                         .tone = FigureTone(limit) });
                // A limit of zero is `InMemoryLruStorage`'s spelling of UNBOUNDED, so there is no
                // proportion to draw -- and a gauge left empty would claim the store is idle.
                auto const fraction = (value.has_value() && limit.has_value()) ? Quotient(*value, *limit) : std::nullopt;
                if (fraction.has_value())
                    pieces.push_back(
                        Piece { .text = "  " + Gauge(*fraction, GaugeCells, *in.glyphs), .priority = row.gaugePriority });
                pieces.push_back(Piece { .text = "  " + FormatFigure(fraction, FigureFormat::Percent, in.absent),
                                         .priority = row.limitPriority,
                                         .tone = FigureTone(fraction) });
            }
            if (!row.note.empty())
                pieces.push_back(Piece {
                    .text = "  " + std::string { row.note }, .priority = row.notePriority, .tone = FrameTone::Label });

            auto kept = FitPieces(std::move(pieces), budget, 0, in.cellWidth);
            if (!kept.has_value())
                return std::unexpected(kept.error());
            items.push_back(LineOf(*kept, {}, row.priority));
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
        if (spec.tierColumns.empty() || !in.model->stats.has_value())
            return items;

        auto const tiers = TiersIn(spec, *in.model->stats);
        if (tiers.empty())
            return items;

        // Each column is as wide as its widest cell and the gap before it, so a figure is never cut to fit
        // the column and no two columns read as one: `evict/s index (RAM)` is three headings or two.
        auto cells = std::vector<std::vector<std::string>>(tiers.size());
        auto widths = std::vector<std::size_t> {};
        for (auto const& column: spec.tierColumns)
        {
            auto width = std::max(TierFigureColumns, in.cellWidth(column.header) + TierColumnGap);
            for (auto const index: std::views::iota(std::size_t { 0 }, tiers.size()))
            {
                cells[index].push_back(FigureText(in, column.figure, Newest(SeriesFor(in, column.figure, tiers[index]))));
                width = std::max(width, in.cellWidth(cells[index].back()) + TierColumnGap);
            }
            widths.push_back(width);
        }

        auto heading =
            std::vector<Piece> { Piece { .text = std::string { Indent } + FitRight("tier", TierNameColumns, in.cellWidth),
                                         .priority = Priority::Essential,
                                         .tone = FrameTone::Label } };
        for (auto const index: std::views::iota(std::size_t { 0 }, spec.tierColumns.size()))
            heading.push_back(Piece { .text = AlignRight(spec.tierColumns[index].header, widths[index], in.cellWidth),
                                      .priority = spec.tierColumns[index].priority,
                                      .slot = index + 1,
                                      .tone = FrameTone::Label });
        auto kept = FitPieces(std::move(heading), budget, 0, in.cellWidth);
        if (!kept.has_value())
            return std::unexpected(kept.error());

        // The heading is a line of pieces; a row is aligned under it cell by cell, so its runs are placed where
        // each cell's text lands: the tier's name as a label, each figure as a figure, an absent one plain.
        auto table = LineOf(*kept, {}, spec.tierPriority);
        table.table = true;
        for (auto const index: std::views::iota(std::size_t { 0 }, tiers.size()))
        {
            auto const line = table.lines.size();
            auto row = std::string { Indent };
            table.spans.push_back(
                LineSpan { .line = line, .byte = row.size(), .length = tiers[index].size(), .tone = FrameTone::Label });
            row += FitRight(tiers[index], TierNameColumns, in.cellWidth);
            for (auto const& piece: *kept | std::views::drop(1))
            {
                auto const& text = cells[index][piece.slot - 1];
                auto const cell = AlignRight(text, widths[piece.slot - 1], in.cellWidth);
                if (text != in.absent)
                    table.spans.push_back(LineSpan { .line = line,
                                                     .byte = row.size() + (cell.size() - text.size()),
                                                     .length = text.size(),
                                                     .tone = FrameTone::Figure });
                row += cell;
            }
            table.lines.push_back(std::move(row));
        }
        items.push_back(std::move(table));
        for (auto const note: spec.tierNote)
        {
            // Prose, not a figure: a note that does not fit is dropped whole rather than cut.
            auto fitted = FitPieces({ Piece { .text = std::string { Indent } + std::string { note },
                                              .priority = spec.tierNotePriority,
                                              .tone = FrameTone::Label } },
                                    budget,
                                    0,
                                    in.cellWidth);
            if (!fitted.has_value())
                return std::unexpected(fitted.error());
            if (!fitted->empty())
                items.push_back(LineOf(*fitted, {}, spec.tierNotePriority));
        }
        return items;
    }

    /// What separates a fact's label from its value, and one cell of a fact line from the next.
    constexpr std::string_view FactGap = "  ";

    /// What one status fact says in a frame.
    struct FactText
    {
        std::vector<Piece> pieces {};            ///< Its line, in pieces a narrow terminal drops by priority.
        std::vector<std::vector<Piece>> more {}; ///< Lines under it, laid out for the width it was given, in pieces.
    };

    /// @p text as a fact of one piece.
    /// @param text The text.
    /// @param tone How it is dressed; none for a reading that needs nobody.
    /// @return The fact.
    [[nodiscard]] FactText Said(std::string text, std::optional<FrameTone> tone = std::nullopt)
    {
        return FactText { .pieces = { Piece { .text = std::move(text), .priority = Priority::Essential, .tone = tone } },
                          .more = {} };
    }

    /// How a state a node reports is dressed: a word for what it means to an operator, keyed on the wire's name.
    struct StateTone
    {
        std::string_view name; ///< The state, as `NodeStatusText` spells it.
        FrameTone tone;        ///< What it means.
    };

    /// The states worth a glance: a survey not done yet, an election under way, and a worker with nothing to
    /// serve. A state not here -- `serving`, `follower`, `leader` -- is the ordinary one and is not dressed.
    constexpr auto StateTones = std::to_array<StateTone>({
        { .name = "surveying", .tone = FrameTone::Stale },
        { .name = "undecided", .tone = FrameTone::Stale },
        { .name = "nothing-to-serve", .tone = FrameTone::Alert },
    });

    /// The tone of the state named @p name.
    /// @param name The state's name.
    /// @return Its tone, or nullopt for an ordinary state.
    [[nodiscard]] std::optional<FrameTone> ToneOfState(std::string_view name) noexcept
    {
        auto const* row = FindIfOrNull(StateTones, [name](StateTone const& one) { return one.name == name; });
        return row == nullptr ? std::nullopt : std::optional<FrameTone> { row->tone };
    }

    /// The newest node status, or nullptr before any was read.
    /// @param in The frame's inputs.
    /// @return The status.
    [[nodiscard]] CompileCacheWire::NodeStatusFields const* StatusOf(FrameInputs const& in) noexcept
    {
        return in.model->nodeStatus.has_value() ? &*in.model->nodeStatus : nullptr;
    }

    /// Whether @p status is of a node running consensus: its component, or a role only a scheduler reports.
    /// @param status The status, or nullptr.
    /// @return False before a status was read.
    [[nodiscard]] bool RunsConsensus(CompileCacheWire::NodeStatusFields const* status) noexcept
    {
        return status != nullptr
               && ((status->components & CompileCacheWire::NodeComponentBit::Consensus) != 0
                   || status->runtime.schedulerRole.has_value());
    }

    /// @p value written, or the absent marker for a node that did not say.
    /// @param in The frame's inputs.
    /// @param value The number.
    /// @return The text.
    [[nodiscard]] std::string CountText(FrameInputs const& in, std::optional<std::uint32_t> value)
    {
        return value.has_value() ? std::to_string(*value) : std::string { in.absent };
    }

    /// Fill cells of a gauge on a fact line: §4's slots and cache tier bars.
    constexpr auto FactGaugeCells = std::size_t { 16 };

    /// What stands between a slot gauge and the limit that shapes it, and between a figure and the next on a fact line.
    constexpr std::string_view FactFigureGap = "   ";

    /// How a slot limit is dressed where it is named.
    struct SlotLimitDress
    {
        Distributed::SlotLimit limit;  ///< The limit this row describes.
        std::optional<FrameTone> tone; ///< How its name is dressed; none for the one that is no problem.
        bool remedy;                   ///< Whether the panel writes `SlotLimitTable`'s remedy under it.
    };

    /// One row per `Distributed::SlotLimit`, in enumerator order: nothing withdrawn is not dressed and says no
    /// remedy; somebody else using the machine is worth a look; memory or scratch running out is an alert, since
    /// the machine will refuse work until an operator frees it.
    constexpr EnumTable<Distributed::SlotLimit, SlotLimitDress> SlotLimitDressTable { {
        { .limit = Distributed::SlotLimit::Registered, .tone = std::nullopt, .remedy = false },
        { .limit = Distributed::SlotLimit::ExternalCpu, .tone = FrameTone::Stale, .remedy = true },
        { .limit = Distributed::SlotLimit::Memory, .tone = FrameTone::Alert, .remedy = true },
        { .limit = Distributed::SlotLimit::Scratch, .tone = FrameTone::Alert, .remedy = true },
    } };

    static_assert(RowsInEnumeratorOrder(SlotLimitDressTable, &SlotLimitDress::limit),
                  "SlotLimitDressTable must hold one row per SlotLimit, in enumerator order");

    /// How the running part of a slot gauge is dressed: the band of what is available that runs.
    /// @param inFlight Compiles running.
    /// @param available What the node may hold now.
    /// @return The band; the top one for work running where nothing is available.
    [[nodiscard]] FrameTone RunningTone(std::uint32_t inFlight, std::uint32_t available) noexcept
    {
        if (available == 0)
            return inFlight == 0 ? FrameTone::LevelLow : FrameTone::LevelHigh;
        auto const share = static_cast<double>(inFlight) / static_cast<double>(available);
        if (share < 1.0 / 3.0)
            return FrameTone::LevelLow;
        return share < 2.0 / 3.0 ? FrameTone::LevelMid : FrameTone::LevelHigh;
    }

    /// The node's slots at its newest reading, its CPU differenced against the reading before when the two are adjacent.
    ///
    /// Adjacent is the fold's decision (`HistoryEntry::elapsed`), never re-derived here: a gap, a failure or a
    /// change of source between the two leaves the CPU, and so the ceilings, unknown.
    /// @param in The frame's inputs.
    /// @return The slots, or nullopt before a reading carrying a host.
    [[nodiscard]] std::optional<NodeSlots> NewestSlots(FrameInputs const& in)
    {
        auto const& history = in.model->history;
        if (history.empty() || !history.back().stats.has_value())
            return in.model->stats.has_value() ? NodeSlotsOf(nullptr, *in.model->stats) : std::nullopt;
        auto const& newest = history.back();
        auto const& latest = newest.stats;
        auto const* previous = static_cast<StatsReading const*>(nullptr);
        if (newest.elapsed.has_value() && history.size() >= 2)
        {
            auto const& before = history[history.size() - 2].stats;
            if (before.has_value())
                previous = &*before;
        }
        return latest.has_value() ? NodeSlotsOf(previous, *latest) : std::nullopt;
    }

    /// @p value as a figure piece of a fact line: essential, and plain like every figure written beside a fact.
    /// @param in The frame's inputs.
    /// @param value The number, or nullopt for the marker.
    /// @param format How it is written.
    /// @return The piece.
    [[nodiscard]] Piece FactFigure(FrameInputs const& in, std::optional<double> value, FigureFormat format)
    {
        return Piece { .text = FormatFigure(value, format, in.absent), .priority = Priority::Essential };
    }

    /// @p text as a piece of a fact line, essential and plain.
    /// @param text The words.
    /// @return The piece.
    [[nodiscard]] Piece FactWords(std::string text)
    {
        return Piece { .text = std::move(text), .priority = Priority::Essential };
    }

    /// The three numbers a slots fact states.
    struct SlotNumbers
    {
        std::optional<double> inFlight {};   ///< Compiles running.
        std::optional<double> available {};  ///< What the node may hold now; absent until the ceilings are known.
        std::optional<double> registered {}; ///< What it registered with.
    };

    /// The numbers a slots fact states: the reading's when there is one, since the ceilings are worked out from that
    /// same reading; a status alone gives the two it states, and `available` stays the marker until a reading says more.
    /// @param slots The newest reading's slots, or nullopt.
    /// @param status The newest status, or nullptr.
    /// @return The numbers.
    [[nodiscard]] SlotNumbers SlotNumbersOf(std::optional<NodeSlots> const& slots,
                                            CompileCacheWire::NodeStatusFields const* status) noexcept
    {
        auto const count = [](std::uint32_t value) {
            return std::optional { static_cast<double>(value) };
        };
        if (slots.has_value())
            return SlotNumbers { .inFlight = count(slots->inFlight),
                                 .available = slots->ceilings.has_value() ? count(slots->ceilings->available) : std::nullopt,
                                 .registered = count(slots->registered) };
        auto numbers = SlotNumbers {};
        if (status == nullptr)
            return numbers;
        if (status->runtime.compilesInFlight.has_value())
            numbers.inFlight = count(*status->runtime.compilesInFlight);
        if (status->runtime.compileSlots.has_value())
            numbers.registered = count(*status->runtime.compileSlots);
        return numbers;
    }

    /// The line under a slots fact: the gauge, when it fits, and the limit that binds, or the marker while none is known.
    /// @param in The frame's inputs.
    /// @param slots The slots, or nullopt when only a status spoke.
    /// @param width The cells the line may take.
    /// @return The line's pieces.
    [[nodiscard]] std::vector<Piece> SlotLimitLine(FrameInputs const& in,
                                                   std::optional<NodeSlots> const& slots,
                                                   std::size_t width)
    {
        auto const label = std::string { "limited-by" } + std::string { FactGap };
        auto const ceilings = slots.has_value() ? slots->ceilings : std::nullopt;
        if (!ceilings.has_value())
            return { Piece { .text = label, .priority = Priority::Essential, .tone = FrameTone::Label },
                     FactWords(std::string { in.absent }) };

        auto const& dress = SlotLimitDressTable[static_cast<std::size_t>(ceilings->binding)];
        auto const name = std::string { Distributed::TraitsFor(ceilings->binding).name };
        auto line = std::vector<Piece> {};
        // The gauge goes whole or not at all: part of one would be a different share, so it is laid out here rather
        // than left for the width to take piece by piece.
        auto const gauge = SlotGaugeOf(slots->inFlight, ceilings->available, slots->registered, FactGaugeCells, *in.glyphs);
        auto const gaugeCells = in.cellWidth(gauge.open + gauge.running + gauge.held + gauge.withdrawn + gauge.close);
        if (gaugeCells + in.cellWidth(FactFigureGap) + in.cellWidth(label) + in.cellWidth(name) <= width)
        {
            line.push_back(FactWords(gauge.open));
            line.push_back(Piece { .text = gauge.running,
                                   .priority = Priority::Essential,
                                   .tone = RunningTone(slots->inFlight, ceilings->available) });
            line.push_back(FactWords(gauge.held));
            line.push_back(FactWords(gauge.withdrawn + gauge.close));
            line.push_back(Piece {
                .text = std::string { FactFigureGap } + label, .priority = Priority::Essential, .tone = FrameTone::Label });
        }
        else
        {
            line.push_back(Piece { .text = label, .priority = Priority::Essential, .tone = FrameTone::Label });
        }
        line.push_back(Piece { .text = name, .priority = Priority::Essential, .tone = dress.tone });
        return line;
    }

    /// What a leading node's `leader` reads: it names no endpoint because it is the leader.
    constexpr std::string_view SelfLeader = "this node";

    /// What renders one status fact.
    struct StatusFactSpec
    {
        StatusFact fact; ///< The enumerator this row describes.
        /// The fact laid out for @p width cells after its label, or nullopt where it does not apply to this node.
        std::optional<FactText> (*render)(FrameInputs const& in, FactCell const& cell, std::size_t width);
    };

    /// One row per `StatusFact`, in enumerator order.
    constexpr auto StatusFactTable = EnumTable<StatusFact, StatusFactSpec> { {
        { .fact = StatusFact::NodeId,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t /*width*/) -> std::optional<FactText> {
              // Absent on a node running no consensus: it has no minted identity, and an empty one is not a name.
              auto const* status = StatusOf(in);
              return Said(status == nullptr || status->nodeId.empty() ? std::string { in.absent } : status->nodeId);
          } },
        { .fact = StatusFact::Components,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t /*width*/) -> std::optional<FactText> {
              auto const* status = StatusOf(in);
              return Said(status == nullptr ? std::string { in.absent } : DescribeComponents(status->components));
          } },
        { .fact = StatusFact::Toolchains,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t /*width*/) -> std::optional<FactText> {
              // The counts stand or fall with the state: `0 of 0` under no state is the collapse it exists to end.
              auto const* status = StatusOf(in);
              if (status == nullptr || !status->runtime.toolchains.has_value())
                  return Said(std::string { in.absent });
              auto const& runtime = status->runtime;
              auto const state = NameOfToolchainState(*runtime.toolchains);
              return Said(std::format("{} {} of {}", state, runtime.toolchainsServed, runtime.toolchainsDiscovered),
                          ToneOfState(state));
          } },
        { .fact = StatusFact::Registrars,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t /*width*/) -> std::optional<FactText> {
              // A node holding no registration at all -- no scheduler named -- has no count to state.
              auto const* status = StatusOf(in);
              if (status == nullptr || !status->runtime.registrarsTotal.has_value())
                  return Said(std::string { in.absent });
              auto const& runtime = status->runtime;
              // Absent means NEVER, which is not a long time ago: a node no scheduler has accepted says so, and it
              // is the state worth acting on; some of its registrations held and not all of them is worth a look.
              auto const never = !runtime.lastRegistrationSecondsAgo.has_value();
              auto tone = std::optional<FrameTone> {};
              if (never)
                  tone = FrameTone::Alert;
              else if (runtime.registrarsRegistered.value_or(0) < *runtime.registrarsTotal)
                  tone = FrameTone::Stale;
              return Said(std::format("{} of {}, {}",
                                      CountText(in, runtime.registrarsRegistered),
                                      *runtime.registrarsTotal,
                                      never ? std::string { "never accepted" }
                                            : std::format("last {}s ago", *runtime.lastRegistrationSecondsAgo)),
                          tone);
          } },
        { .fact = StatusFact::Consensus,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t /*width*/) -> std::optional<FactText> {
              auto const* status = StatusOf(in);
              if (!RunsConsensus(status))
                  return std::nullopt;
              auto const& role = status->runtime.schedulerRole;
              if (!role.has_value())
                  return Said(std::string { in.absent });
              auto const name = NameOfSchedulerRole(*role);
              return Said(std::string { name }, ToneOfState(name));
          } },
        { .fact = StatusFact::Leader,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t /*width*/) -> std::optional<FactText> {
              // Empty is a READING -- no leader is known -- so it is the marker, on the line the role is on. Except on
              // the leader itself, which names nobody because it is the one: it says so rather than drawing the
              // marker for the one fact it knows best.
              auto const* status = StatusOf(in);
              if (!RunsConsensus(status))
                  return std::nullopt;
              auto const& runtime = status->runtime;
              if (!runtime.leaderEndpoint.empty())
                  return Said(runtime.leaderEndpoint);
              if (runtime.schedulerRole == CompileCacheWire::WireSchedulerRole::Leader)
                  return Said(std::string { SelfLeader });
              return Said(std::string { in.absent });
          } },
        { .fact = StatusFact::Slots,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t width) -> std::optional<FactText> {
              // A node whose status says it runs no worker has no slots to draw.
              auto const* status = StatusOf(in);
              if (status != nullptr && !status->runtime.compileSlots.has_value())
                  return std::nullopt;
              auto const slots = NewestSlots(in);
              if (status == nullptr && !slots.has_value())
                  return Said(std::string { in.absent });

              auto const numbers = SlotNumbersOf(slots, status);
              auto fact = FactText {};
              fact.pieces = { FactFigure(in, numbers.inFlight, FigureFormat::Count),   FactWords(" in flight / "),
                              FactFigure(in, numbers.available, FigureFormat::Count),  FactWords(" available / "),
                              FactFigure(in, numbers.registered, FigureFormat::Count), FactWords(" registered") };
              fact.more.push_back(SlotLimitLine(in, slots, width));

              // What to do about the limit, in `SlotLimitTable`'s own words, wrapped under the gauge.
              auto const ceilings = slots.has_value() ? slots->ceilings : std::nullopt;
              if (ceilings.has_value() && SlotLimitDressTable[static_cast<std::size_t>(ceilings->binding)].remedy)
              {
                  for (auto& line: Wrapped(Distributed::TraitsFor(ceilings->binding).remedy, width, in.cellWidth))
                      fact.more.push_back(
                          { Piece { .text = std::move(line), .priority = Priority::Essential, .tone = FrameTone::Label } });
              }
              return fact;
          } },
        { .fact = StatusFact::CacheTier,
          .render = [](FrameInputs const& in, FactCell const& cell, std::size_t /*width*/) -> std::optional<FactText> {
              auto const* status = StatusOf(in);
              if (status == nullptr || (status->components & CompileCacheWire::NodeComponentBit::CacheTier) == 0)
                  return std::nullopt;
              auto fact = FactText {};
              for (auto const& figure: cell.figures)
              {
                  auto piece = Led(fact.pieces.empty() ? std::string_view {} : PieceGap, BesidePiece(in, figure));
                  if (fact.pieces.empty())
                      piece.priority = Priority::Essential;
                  fact.pieces.push_back(std::move(piece));
              }
              // How full the tier is, off the newest reading's cache: used / limit, the gauge and its share. A limit of
              // zero is `InMemoryLruStorage`'s spelling of UNBOUNDED, so there is no share to draw, and no gauge.
              auto const& storage = in.model->stats.has_value() ? in.model->stats->snapshot.storage : std::nullopt;
              auto const used = storage.transform([](StorageStats const& s) { return static_cast<double>(s.bytesUsed); });
              auto const limit = storage.transform([](StorageStats const& s) { return static_cast<double>(s.bytesLimit); });
              auto const share = used.has_value() && limit.has_value() ? Quotient(*used, *limit) : std::nullopt;
              fact.pieces.push_back(Piece { .text = std::string { PieceGap }
                                                    + FormatFigure(used, FigureFormat::Bytes, in.absent) + " / "
                                                    + FormatFigure(limit, FigureFormat::Bytes, in.absent),
                                            .priority = Priority::Normal });
              if (share.has_value())
                  fact.pieces.push_back(Piece { .text = std::string { FactGap } + Gauge(*share, FactGaugeCells, *in.glyphs),
                                                .priority = Priority::Low });
              fact.pieces.push_back(
                  Piece { .text = std::string { FactGap } + FormatFigure(share, FigureFormat::Percent, in.absent),
                          .priority = Priority::Normal });
              return fact;
          } },
        { .fact = StatusFact::Host,
          .render = [](FrameInputs const& in, FactCell const& cell, std::size_t /*width*/) -> std::optional<FactText> {
              // CPU busy is the share between the two newest adjacent readings, and memory free is the newest reading's
              // own: the figures the slot ceilings above are worked out from, so the two lines cannot disagree.
              auto const slots = NewestSlots(in);
              auto const busy = slots.has_value() ? slots->cpuBusyPermille : std::nullopt;
              auto const memory = slots.has_value() ? slots->availableMemoryBytes : std::nullopt;
              auto fact = FactText {};
              fact.pieces.push_back(
                  Piece { .text = "cpu-busy "
                                  + FormatFigure(busy.transform([](std::uint32_t permille) { return permille / 1000.0; }),
                                                 FigureFormat::Percent,
                                                 in.absent),
                          .priority = Priority::Essential });
              fact.pieces.push_back(Piece {
                  .text = std::string { PieceGap } + "mem free "
                          + FormatFigure(memory.transform([](std::uint64_t bytes) { return static_cast<double>(bytes); }),
                                         FigureFormat::Bytes,
                                         in.absent),
                  .priority = Priority::Normal });
              for (auto const& figure: cell.figures)
                  fact.pieces.push_back(Led(PieceGap, BesidePiece(in, figure)));
              return fact;
          } },
    } };

    static_assert(RowsInEnumeratorOrder(StatusFactTable, &StatusFactSpec::fact),
                  "StatusFactTable must hold one row per StatusFact, in enumerator order");

    /// The fact blocks @p spec draws at @p place, one group of items per block that has a line to draw.
    ///
    /// Every first cell shares one label column across the whole panel, so `slots` and `host` start where
    /// `toolchains` does; a second cell's label is aligned across the lines of its own block.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @param place Which blocks.
    /// @param budget The content width.
    /// @return The groups; or the cells an essential line needed when it does not fit.
    [[nodiscard]] std::expected<std::vector<std::vector<Item>>, std::size_t> FactGroups(FrameInputs const& in,
                                                                                        PanelSpec const& spec,
                                                                                        FactPlace place,
                                                                                        std::size_t budget)
    {
        auto labelColumns = std::size_t { 0 };
        for (auto const& block: spec.facts)
            for (auto const& line: block.lines)
                if (!line.cells.empty())
                    labelColumns = std::max(labelColumns, in.cellWidth(line.cells.front().label) + in.cellWidth(FactGap));
        auto const lead = in.cellWidth(Indent) + labelColumns;
        auto const valueBudget = budget > lead ? budget - lead : 0;

        auto groups = std::vector<std::vector<Item>> {};
        for (auto const& block: spec.facts)
        {
            if (block.place != place)
                continue;

            struct Rendered
            {
                FactLine const* line;           ///< The line.
                FactText first;                 ///< Its first cell, which applies, or the line is not drawn.
                std::optional<FactText> second; ///< Its second cell, when it has one that applies.
            };
            auto rendered = std::vector<Rendered> {};
            auto firstColumns = std::size_t { 0 };
            auto secondLabelColumns = std::size_t { 0 };
            for (auto const& line: block.lines)
            {
                if (line.cells.empty())
                    continue;
                auto const render = [&in, valueBudget](FactCell const& cell) {
                    return StatusFactTable[static_cast<std::size_t>(cell.fact)].render(in, cell, valueBudget);
                };
                auto first = render(line.cells.front());
                if (!first.has_value())
                    continue;
                auto entry = Rendered { .line = &line, .first = std::move(*first), .second = std::nullopt };
                if (line.cells.size() > 1)
                {
                    entry.second = render(line.cells[1]);
                    firstColumns = std::max(firstColumns, TextWidth(entry.first.pieces, in.cellWidth));
                    secondLabelColumns =
                        std::max(secondLabelColumns, in.cellWidth(line.cells[1].label) + in.cellWidth(FactGap));
                }
                rendered.push_back(std::move(entry));
            }

            auto group = std::vector<Item> {};
            for (auto& entry: rendered)
            {
                auto kept = FitPieces(std::move(entry.first.pieces), valueBudget, 0, in.cellWidth);
                if (!kept.has_value())
                {
                    if (entry.line->priority == Priority::Essential)
                        return std::unexpected(lead + kept.error());
                    continue;
                }
                auto pieces = std::vector<Piece> { Piece {
                    .text = std::string { Indent } + FitRight(entry.line->cells.front().label, labelColumns, in.cellWidth),
                    .priority = Priority::Essential,
                    .tone = FrameTone::Label } };
                std::ranges::move(*kept, std::back_inserter(pieces));
                if (entry.second.has_value())
                {
                    auto const used = TextWidth(pieces, in.cellWidth);
                    auto const aligned = std::max(used, lead + firstColumns);
                    auto const secondCells =
                        in.cellWidth(FactGap) + secondLabelColumns + TextWidth(entry.second->pieces, in.cellWidth);
                    // The second cell goes whole when it does not fit beside the first.
                    if (aligned + secondCells <= budget)
                    {
                        pieces.push_back(
                            Piece { .text = std::string(aligned - used, ' ') + std::string { FactGap }
                                            + FitRight(entry.line->cells[1].label, secondLabelColumns, in.cellWidth),
                                    .priority = Priority::Essential,
                                    .tone = FrameTone::Label });
                        std::ranges::move(entry.second->pieces, std::back_inserter(pieces));
                    }
                }
                auto item = LineOf(pieces, {}, entry.line->priority);
                // A line under the fact hangs at the value column, fitted to the value's budget like the fact itself;
                // its runs move with it.
                for (auto& more: entry.first.more)
                {
                    auto fitted = FitPieces(std::move(more), valueBudget, 0, in.cellWidth);
                    if (!fitted.has_value() || fitted->empty())
                        continue;
                    auto under = LineOf(*fitted, {}, entry.line->priority);
                    for (auto span: under.spans)
                    {
                        span.line = item.lines.size();
                        span.byte += lead;
                        item.spans.push_back(span);
                    }
                    item.lines.push_back(std::string(lead, ' ') + under.lines.front());
                }
                group.push_back(std::move(item));
            }
            if (!group.empty())
                groups.push_back(std::move(group));
        }
        return groups;
    }

    /// The palette ceiling a chart is encoded with: its ramp's eight colours fit without merging.
    constexpr auto ChartColours = std::size_t { 16 };

    /// What separates two headline tiles on one line.
    constexpr std::string_view TileGap = "   ";

    /// Columns a tile's value is right-aligned into, at the least.
    constexpr auto TileValueColumns = std::size_t { 10 };

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
        auto number = std::uint64_t { 0 };
        auto const* const end = cell.lexical.data() + cell.lexical.size();
        auto const [at, error] = std::from_chars(cell.lexical.data(), end, number);
        if (!format.has_value() || error != std::errc {} || at != end)
            return cell.lexical;
        return Distributed::HumanFleetFigure(number, *format);
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
            // The brackets stay where the terminal dresses the tab too: a plain frame is the same grid.
            pieces.push_back(Piece { .text = std::move(text),
                                     .priority = isActive ? Priority::Essential : spec.stripTabPriority,
                                     .tone = isActive ? std::optional<FrameTone> { FrameTone::Selected } : std::nullopt });
        }
        auto kept = FitPieces(std::move(pieces), budget, 0, in.cellWidth);
        if (!kept.has_value())
            return {};
        return { LineOf(*kept, {}, spec.stripPriority) };
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

        // Every cell written for a person FIRST, by the scale its column has in the leader's own tables,
        // so the widths below are the widths drawn and the drop for width sees the real figures.
        auto const formats = ColumnFormats(section, table->columns);
        auto cells = std::vector<std::vector<std::string>> {};
        cells.reserve(table->rows.size());
        for (auto const& row: table->rows)
        {
            auto& written = cells.emplace_back();
            written.reserve(row.size());
            for (auto const index: std::views::iota(std::size_t { 0 }, row.size()))
                written.push_back(HumanCellText(in, row[index], formats[index]));
        }

        // Each column as wide as its widest cell, heading included, so no cell is ever cut.
        auto widths = std::vector<std::size_t> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, table->columns.size()))
        {
            auto width = in.cellWidth(table->columns[index]);
            for (auto const& row: cells)
                width = std::max(width, in.cellWidth(row[index]));
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
        for (auto const& row: cells)
        {
            auto line = std::string { Indent } + FitRight(row.front(), widths.front(), in.cellWidth);
            for (auto const& piece: *kept | std::views::drop(1))
                line += std::string { ColumnGap } + AlignRight(row[piece.slot], widths[piece.slot], in.cellWidth);
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

    /// How long the endpoint has served, from whichever reading carries it.
    /// @param model What is known.
    /// @return Seconds, or nullopt when nothing read says.
    [[nodiscard]] std::optional<std::uint64_t> UptimeOf(DashboardModel const& model) noexcept
    {
        if (model.nodeStatus.has_value())
            return model.nodeStatus->uptimeSeconds;
        if (!model.stats.has_value())
            return std::nullopt;
        return static_cast<std::uint64_t>(model.stats->snapshot.uptime.value.count());
    }

    /// One row per `ChromeFact`, in enumerator order.
    constexpr auto ChromeFactTable = EnumTable<ChromeFact, ChromeFactSpec> { {
        { .fact = ChromeFact::Version,
          .render = [](FrameInputs const& in, PanelContext const& /*context*/) -> std::string {
              // A node says it in its status; a cache's reading carries the build that captured it. Neither is
              // invented, and an empty one is no version.
              if (in.model->nodeStatus.has_value() && !in.model->nodeStatus->version.empty())
                  return in.model->nodeStatus->version;
              auto const& stats = in.model->stats;
              return stats.has_value() && !stats->version.empty() ? stats->version : std::string { in.absent };
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

std::string TierFigureKey(std::string_view tier, std::string_view column)
{
    return std::format("{}{}{}", tier, TierKeySeparator, column);
}

std::vector<std::string_view> TiersIn(PanelSpec const& spec, StatsReading const& reading)
{
    auto tiers = std::vector<std::string_view> {};
    if (spec.tierColumns.empty())
        return tiers;
    for (auto const& tier: StorageTierTable)
        if (reading.snapshot.storageTiers[static_cast<std::size_t>(tier.tier)].has_value())
            tiers.push_back(tier.name);
    return tiers;
}

std::vector<std::optional<double>> FigureSeries(std::deque<HistoryEntry> const& history,
                                                FigureSpec const& figure,
                                                std::string_view tier)
{
    // A tier name no row has reads nothing, rather than silently reading the whole cache.
    auto const tierAsked = TierNamed(tier);
    if (!tier.empty() && !tierAsked.has_value())
        return Series(history.size());
    auto const& row = FigureSourceTable[static_cast<std::size_t>(figure.source)];
    auto series = row.compute(history, figure.field, figure.other, tierAsked);
    if (row.takesAddends)
        for (auto const& addend: figure.addends)
            series = Pairwise(series, Rates(history, addend, tierAsked), &Sum);
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
    auto const in =
        FrameInputs { .model = &model, .glyphs = _glyphs, .absent = _context.absent, .cellWidth = _context.cellWidth };
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

    auto above = FactGroups(in, *_spec, FactPlace::AboveRates, budget);
    auto rates = RateItems(in, _spec->rates, budget, _besideReserve);
    auto levels = LevelItems(in, _spec->levels, budget);
    auto tiers = TierItems(in, *_spec, budget);
    auto document = DocumentItems(in, *_spec, _context, budget);
    auto below = FactGroups(in, *_spec, FactPlace::BelowRates, budget);
    for (auto const* block: { &rates, &levels, &tiers, &document })
        if (!block->has_value())
            return tooSmall(block->error() + FrameColumns);
    for (auto const* facts: { &above, &below })
        if (!facts->has_value())
            return tooSmall(facts->error() + FrameColumns);

    // Top to bottom, a blank before every block that has something to draw.
    auto groups = std::move(*above);
    for (auto* block: { &*rates, &*levels, &*tiers, &*document })
        groups.push_back(std::move(*block));
    std::ranges::move(*below, std::back_inserter(groups));
    auto items = std::vector<Item> {};
    for (auto& group: groups)
    {
        if (group.empty())
            continue;
        items.push_back(Blank());
        std::ranges::move(group, std::back_inserter(items));
    }
    items.push_back(Blank());
    auto source = SourceLine(in, _spec->sourcePriority, budget);
    if (!source.has_value())
        return tooSmall(source.error() + FrameColumns);
    if (source->has_value())
        items.push_back(Item { .lines = { std::move(**source) }, .priority = _spec->sourcePriority });

    auto const fitted = FitRows(std::move(items), available);
    if (!fitted.has_value())
        return tooSmall(0);

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
