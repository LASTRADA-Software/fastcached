// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanel.hpp"
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"
#include "HistoryChart.hpp"
#include "NodeSlots.hpp"
#include "NodeStatusText.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/NumericText.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

namespace
{
    using Series = std::vector<std::optional<double>>;

    /// A counted noun: a count of one is singular, so no frame says `1 machines`.
    struct CountNoun
    {
        std::string_view one {};  ///< For a count of one: `machine`.
        std::string_view many {}; ///< For every other count: `machines`.

        /// @param count How many.
        /// @return The noun @p count takes.
        [[nodiscard]] constexpr std::string_view For(std::size_t count) const noexcept
        {
            return count == 1 ? one : many;
        }

        /// @return Whether there is no noun.
        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return many.empty();
        }
    };

    /// Samples, as the source line and a chart's span count them.
    constexpr auto SampleNoun = CountNoun { .one = "sample", .many = "samples" };

    /// Gaps, as the source line counts them.
    constexpr auto GapNoun = CountNoun { .one = "gap", .many = "gaps" };

    /// @p count with the noun it takes: `1 machine`, `12 machines`.
    /// @param count How many.
    /// @param noun What is counted.
    /// @return The text.
    [[nodiscard]] std::string Counted(std::size_t count, CountNoun noun)
    {
        return std::format("{} {}", count, noun.For(count));
    }

    /// What a fleet section's rows are counted as, from the leader's one section table.
    /// @param section The section.
    /// @return The noun.
    [[nodiscard]] constexpr CountNoun SectionNoun(FleetSection section) noexcept
    {
        auto const& row = Distributed::FleetSectionTable[static_cast<std::size_t>(section)];
        return CountNoun { .one = row.one, .many = row.many };
    }

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

    /// The node's slots at history entry @p index, its CPU differenced against the entry before when the two are adjacent.
    ///
    /// Adjacent is the fold's decision (`HistoryEntry::elapsed`), never re-derived here: a gap, a failure or a change
    /// of source between the two leaves the CPU, and so the ceilings, unknown.
    /// @param history The samples, oldest first.
    /// @param index Which entry.
    /// @return The slots, or nullopt for an entry with no reading or a reading carrying no host.
    [[nodiscard]] std::optional<NodeSlots> SlotsAt(std::deque<HistoryEntry> const& history, std::size_t index)
    {
        auto const& entry = history[index];
        auto const& now = entry.stats;
        if (!now.has_value())
            return std::nullopt;
        auto const* previous = static_cast<StatsReading const*>(nullptr);
        if (entry.elapsed.has_value() && index > 0)
        {
            auto const& before = history[index - 1].stats;
            if (before.has_value())
                previous = &*before;
        }
        return NodeSlotsOf(previous, *now);
    }

    /// What the node may hold at each entry: `Distributed::SlotCeilingsFor`'s `available`, absent where it cannot be known.
    /// @param history The samples, oldest first.
    /// @return One value per entry.
    [[nodiscard]] Series AvailableSlots(std::deque<HistoryEntry> const& history)
    {
        auto series = Series(history.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, history.size()))
        {
            auto const slots = SlotsAt(history, index);
            if (slots.has_value() && slots->ceilings.has_value())
                series[index] = static_cast<double>(slots->ceilings->available);
        }
        return series;
    }

    /// Each entry's value of a field its DOCUMENT reading names, such as a fleet headline tile's.
    ///
    /// A fleet reading is the leader's rendered text, which no `StatsReading` models, so its figures are read by the
    /// name the document gives them rather than through a `ReadingField`.
    /// @param history The samples.
    /// @param name The field.
    /// @return One value per entry, absent where the entry holds no reading or the reading no such number.
    [[nodiscard]] Series DocumentLevels(std::deque<HistoryEntry> const& history, std::string_view name)
    {
        auto series = Series(history.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, history.size()))
        {
            auto const& reading = history[index].reading;
            auto const* field = reading.has_value() ? FindField(*reading, name) : nullptr;
            if (field == nullptr || (field->value.kind != CellKind::Number && field->value.kind != CellKind::Text))
                continue;
            auto parsed = 0.0;
            if (ParseFiniteDouble(field->value.lexical, parsed))
                series[index] = parsed;
        }
        return series;
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
        { .source = FigureSource::LevelQuotient,
          .compute =
              [](std::deque<HistoryEntry> const& history,
                 ReadingField field,
                 ReadingField other,
                 std::optional<StorageTier> tier) {
                  return Pairwise(Levels(history, field, tier), Levels(history, other, tier), &Quotient);
              } },
        { .source = FigureSource::SlotsAvailable,
          .compute = [](std::deque<HistoryEntry> const& history,
                        ReadingField /*field*/,
                        ReadingField /*other*/,
                        std::optional<StorageTier> /*tier*/) { return AvailableSlots(history); } },
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

    /// Columns a rate row's label takes after its indent, at the least; a panel whose widest label is wider takes that
    /// (`RateLabelColumnsFor`). §3 draws a figure ending at column 23 with a trend from 26, which is this and
    /// `FigureColumns` (#134 C7).
    constexpr auto LabelColumns = std::size_t { 12 };

    /// Columns a rate row's figure is right-aligned into, at the least. A wider figure widens the column for the
    /// whole block, and by a blank more than the figure, so a label filling its column and a figure filling its
    /// column never read as one word.
    ///
    /// Also what a LEVEL reading is budgeted when the minimum size is derived. A budget and not a pad: that reading
    /// is written straight after its label, left-aligned, as §3 draws `items       1 284 991`, and right-aligning it
    /// cost the `connected` row its reason at 80 columns (#134 C2, C7). One number for both blocks, so neither
    /// claims a minimum the other's layout contradicts; a wider reading names the cells it measured.
    constexpr auto FigureColumns = std::size_t { 9 };

    /// The fewest cells a trend narrows to before it is dropped rather than drawn narrower.
    constexpr auto MinimumTrendCells = std::size_t { 8 };

    /// Columns a level row's label takes after its indent, at the least (`LevelLabelColumnsFor`).
    constexpr auto LevelLabelColumns = std::size_t { 12 };

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

    /// The interval readings arrive at: what the server granted, or what was asked while no grant is known.
    ///
    /// The grant wins because the server keeps the cadence (#1399): a node whose floor is not this build's
    /// paces the stream at its own, and a title stating the asked interval would misstate every span drawn
    /// from the readings.
    /// @param in The frame's inputs.
    /// @param context The panel's context.
    /// @return The interval, when either states one.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ReadingInterval(FrameInputs const& in,
                                                                           PanelContext const& context) noexcept
    {
        return in.model->cadence.has_value() ? in.model->cadence : context.interval;
    }

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

    /// One figure written, its suffix part of its unit when it is present: `41` and `/s`.
    /// @param in The frame's inputs.
    /// @param figure The figure.
    /// @param value Its value.
    /// @return The number and its unit.
    [[nodiscard]] WrittenFigure FigureText(FrameInputs const& in, FigureSpec const& figure, std::optional<double> value)
    {
        auto written = FormatFigure(value, figure.format, in.absent);
        if (value.has_value() && std::isfinite(*value))
            written.unit += figure.suffix;
        return written;
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

    /// A run inside one piece's text, for a piece made of parts dressed differently: `evicted unfetched 41/s` is a
    /// label and a figure that go for width as one.
    struct PieceRun
    {
        std::size_t byte { 0 };               ///< Where it starts in the piece's text.
        std::size_t length { 0 };             ///< How many bytes it covers.
        FrameTone tone { FrameTone::Figure }; ///< What it is.
    };

    /// One droppable part of a line.
    struct Piece
    {
        std::string text {};                    ///< What it draws; empty for the trend, drawn once its width is known.
        Priority priority { Priority::Normal }; ///< When it goes.
        std::size_t slot { 0 };                 ///< Which column it is, for a table whose rows follow its heading.
        bool trend { false };                   ///< Whether this is the trend, which narrows before it goes.
        std::optional<FrameTone> tone {};       ///< How its text is dressed, spaces around it excluded; none for plain.
        std::vector<PieceRun> runs {};          ///< Runs within its text, for a piece whose parts differ; beside `tone`.
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
        CountNoun noun {};                      ///< What a table's rows are, for its overflow line; empty draws `+N more`.
        std::size_t hidden { 0 };               ///< How many of a table's rows are not shown, below those that are.
        std::size_t above { 0 };                ///< How many rows a scrolled table skipped above its first.
        std::size_t rowsKept { 0 };             ///< The rows a table keeps when it shrinks at `shrinkAt`.
        std::size_t imageFirst { 0 };           ///< For an image item, the first of its lines the image covers.
        std::size_t imageRows { 0 };            ///< For an image item, how many of its lines the image covers.
        Priority priority { Priority::Normal }; ///< When it goes.
        std::optional<Priority> shrinkAt {};    ///< The level a table gives up rows at, when that is not `priority`.
        bool table { false };                   ///< Whether it shrinks to its overflow line before it goes.
        bool image { false };                   ///< Whether an image is placed over blank cells of its lines.
    };

    /// The lines a layout keeps, the runs it dresses, and where its image and its scrolled table landed.
    struct FittedRows
    {
        std::vector<std::string> lines {};        ///< Every kept line, top to bottom.
        std::vector<LineSpan> spans {};           ///< Every kept run; `line` indexes `lines`.
        std::optional<std::size_t> imageLine {};  ///< The index of the image item's first line, when kept.
        std::size_t imageRows { 0 };              ///< How many of its lines the image covers.
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
        auto what = item.hidden > 0 ? std::format("{} more {}", item.hidden, item.noun.For(item.hidden)) : std::string {};
        if (item.above > 0)
            what += what.empty() ? std::format("{} above", Counted(item.above, item.noun))
                                 : std::format(", {} above", item.above);
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
                fitted.imageRows = item.imageRows;
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

    /// @p figure written onto the end of @p piece, dressed as G1 says: its number as @p tone, and its unit, which
    /// recedes, as a label. A figure with no tone -- the absent marker -- is written plain, unit and all.
    /// @param piece The piece.
    /// @param figure The figure.
    /// @param tone How its number is dressed.
    void AppendFigure(Piece& piece, WrittenFigure const& figure, std::optional<FrameTone> tone)
    {
        if (tone.has_value() && !figure.number.empty())
            piece.runs.push_back(PieceRun { .byte = piece.text.size(), .length = figure.number.size(), .tone = *tone });
        piece.text += figure.number;
        auto const unit = figure.unit.find_first_not_of(' ');
        if (tone.has_value() && unit != std::string::npos)
            piece.runs.push_back(PieceRun {
                .byte = piece.text.size() + unit, .length = figure.unit.size() - unit, .tone = FrameTone::Label });
        piece.text += figure.unit;
    }

    /// A figure written beside something else, with its words around it: `completed 12 884`.
    ///
    /// ONE piece, so the words and the value go for width together, dressed in runs as what each part is (G1):
    /// the words are labels, which recede as `since start` and `evicted unfetched` do in §3, and the value is a
    /// figure -- an alert past its threshold, and plain when it is the absent marker.
    /// @param in The frame's inputs.
    /// @param beside The figure.
    /// @return The piece.
    [[nodiscard]] Piece BesidePiece(FrameInputs const& in, BesideFigure const& beside)
    {
        auto piece = Piece { .priority = beside.priority };
        auto const part = [&piece](std::string_view text, std::optional<FrameTone> tone) {
            if (tone.has_value() && !text.empty())
                piece.runs.push_back(PieceRun { .byte = piece.text.size(), .length = text.size(), .tone = *tone });
            piece.text += text;
        };
        if (!beside.before.empty())
        {
            part(beside.before, FrameTone::Label);
            piece.text += ' ';
        }
        auto const value = Newest(SeriesFor(in, beside.figure, {}));
        AppendFigure(piece, FigureText(in, beside.figure, value), FigureTone(beside.figure, value, FrameTone::Figure));
        if (!beside.after.empty())
        {
            piece.text += ' ';
            part(beside.after, FrameTone::Label);
        }
        return piece;
    }

    /// @p piece with @p lead written before its text, its tone and its runs still covering only the text.
    /// @param lead What goes before: a gap or an indent.
    /// @param piece The piece.
    /// @return The piece.
    [[nodiscard]] Piece Led(std::string_view lead, Piece piece)
    {
        piece.text.insert(0, lead);
        for (auto& run: piece.runs)
            run.byte += lead.size();
        return piece;
    }

    /// A note as @p glyphs' rung spells it: every `DeltaMark` drawn as the rung's `delta`.
    /// @param note The note, as a panel's table writes it.
    /// @param glyphs The rung.
    /// @return The text to draw.
    [[nodiscard]] std::string NoteText(std::string_view note, RungGlyphs const& glyphs)
    {
        auto text = std::string {};
        text.reserve(note.size());
        for (auto at = note.find(DeltaMark); at != std::string_view::npos; at = note.find(DeltaMark))
        {
            text += note.substr(0, at);
            text += glyphs.delta;
            note.remove_prefix(at + DeltaMark.size());
        }
        text += note;
        return text;
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

    /// One line's item from its kept pieces: the joined text, a run for every piece with a tone, and each piece's own
    /// runs where they land.
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
            for (auto const& run: piece.runs)
                item.spans.push_back(
                    LineSpan { .line = 0, .byte = line.size() + run.byte, .length = run.length, .tone = run.tone });
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

    /// The columns a panel's rate labels take after the indent: its widest label, and never fewer than
    /// `LabelColumns`. One answer for the layout and for `MinimumPanelSize`, so the size a panel names is the size
    /// it draws.
    /// @param rows The panel's rate rows.
    /// @param cellWidth How wide text is.
    /// @return The columns.
    [[nodiscard]] std::size_t RateLabelColumnsFor(std::span<RateRow const> rows, CellWidth cellWidth)
    {
        auto columns = LabelColumns;
        for (auto const& row: rows)
            columns = std::max(columns, cellWidth(row.label));
        return columns;
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
        // One label column and one figure column for the whole block, so the trends line up; the figure column is
        // wide enough for its widest figure and a blank, so a figure is never cut and never touches its label.
        auto series = std::vector<Series> {};
        auto figures = std::vector<WrittenFigure> {};
        auto figureColumns = FigureColumns;
        for (auto const& row: rows)
        {
            series.push_back(SeriesFor(in, row.figure, {}));
            figures.push_back(FigureText(in, row.figure, Newest(series.back())));
            figureColumns = std::max(figureColumns, in.cellWidth(figures.back().Text()) + 1);
        }
        auto const labelColumns = RateLabelColumnsFor(rows, in.cellWidth);

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
                Piece { .text = std::string { Indent } + FitRight(row.label, labelColumns, in.cellWidth),
                        .priority = Priority::Essential,
                        .tone = FrameTone::Label },
                // Right-aligned into the figure column: the pad, then the number and its unit.
                Piece { .text = std::string(figureColumns - in.cellWidth(figures[index].Text()), ' '),
                        .priority = Priority::Essential },
            };
            AppendFigure(pieces.back(), figures[index], FigureTone(row.figure, Newest(series[index]), FrameTone::Figure));
            if (drawsTrend && row.trend == Trend::Drawn)
                pieces.push_back(Piece { .priority = row.trendPriority, .trend = true });
            for (auto const& beside: row.beside)
                pieces.push_back(Led(PieceGap, BesidePiece(in, beside)));
            // A row with no trend has nothing whose width the note competes with, so its note wraps under the
            // row instead of going; a row with one keeps its note a piece like any other.
            auto const wraps = row.trend == Trend::None && !row.note.empty();
            if (!row.note.empty() && !wraps)
                pieces.push_back(Piece { .text = std::string { PieceGap } + NoteText(row.note, *in.glyphs),
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
                    note.lines = Wrapped(NoteText(row.note, *in.glyphs), budget - note.start, in.cellWidth);
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

    /// The columns a panel's level labels take after the indent: its widest label and a blank, and never fewer than
    /// `LevelLabelColumns`. The blank is the label column's, since a level reading is left-aligned against it and
    /// would otherwise read as one word with its label. One answer for the layout and for `MinimumPanelSize`.
    /// @param rows The panel's level rows.
    /// @param cellWidth How wide text is.
    /// @return The columns.
    [[nodiscard]] std::size_t LevelLabelColumnsFor(std::span<LevelRow const> rows, CellWidth cellWidth)
    {
        auto columns = LevelLabelColumns;
        for (auto const& row: rows)
            columns = std::max(columns, cellWidth(row.label) + 1);
        return columns;
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
        // The readings start in one column, after the widest label and a blank.
        auto const labelColumns = LevelLabelColumnsFor(rows, in.cellWidth);

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
                Piece { .priority = Priority::Essential },
            };
            AppendFigure(pieces.back(), FigureText(in, row.value, value), FigureTone(value));
            if (row.limit.has_value())
            {
                auto const limit = Newest(SeriesFor(in, *row.limit, {}));
                pieces.push_back(Piece { .text = " / ", .priority = row.limitPriority });
                AppendFigure(pieces.back(), FigureText(in, *row.limit, limit), FigureTone(limit));
                // A limit of zero is `InMemoryLruStorage`'s spelling of UNBOUNDED, so there is no
                // proportion to draw -- and a gauge left empty would claim the store is idle.
                auto const fraction = (value.has_value() && limit.has_value()) ? Quotient(*value, *limit) : std::nullopt;
                if (fraction.has_value())
                    pieces.push_back(
                        Piece { .text = "  " + Gauge(*fraction, GaugeCells, *in.glyphs), .priority = row.gaugePriority });
                pieces.push_back(Piece { .text = "  ", .priority = row.limitPriority });
                AppendFigure(pieces.back(), FormatFigure(fraction, FigureFormat::Percent, in.absent), FigureTone(fraction));
            }
            if (!row.note.empty())
                pieces.push_back(Piece {
                    .text = "  " + NoteText(row.note, *in.glyphs), .priority = row.notePriority, .tone = FrameTone::Label });

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
        struct Cell
        {
            WrittenFigure figure;        ///< What it writes.
            std::optional<double> value; ///< What it reads, so an absent one is not dressed.
        };
        auto cells = std::vector<std::vector<Cell>>(tiers.size());
        auto widths = std::vector<std::size_t> {};
        for (auto const& column: spec.tierColumns)
        {
            auto width = std::max(TierFigureColumns, in.cellWidth(column.header) + TierColumnGap);
            for (auto const index: std::views::iota(std::size_t { 0 }, tiers.size()))
            {
                auto const value = Newest(SeriesFor(in, column.figure, tiers[index]));
                cells[index].push_back(Cell { .figure = FigureText(in, column.figure, value), .value = value });
                width = std::max(width, in.cellWidth(cells[index].back().figure.Text()) + TierColumnGap);
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

        // The heading is a line of pieces, and so is each row under it, cell for kept cell: the tier's name as a
        // label, each figure right-aligned into its column as a figure and its unit, an absent one plain.
        auto table = LineOf(*kept, {}, spec.tierPriority);
        table.table = true;
        for (auto const index: std::views::iota(std::size_t { 0 }, tiers.size()))
        {
            auto row = std::vector<Piece> { Piece { .text = std::string { Indent }
                                                            + FitRight(tiers[index], TierNameColumns, in.cellWidth),
                                                    .tone = FrameTone::Label } };
            for (auto const& piece: *kept | std::views::drop(1))
            {
                auto const& cell = cells[index][piece.slot - 1];
                row.push_back(Piece { .text = std::string(widths[piece.slot - 1] - in.cellWidth(cell.figure.Text()), ' ') });
                AppendFigure(row.back(), cell.figure, FigureTone(cell.value));
            }
            auto drawn = LineOf(row, {}, spec.tierPriority);
            for (auto const& span: drawn.spans)
                table.spans.push_back(
                    LineSpan { .line = table.lines.size(), .byte = span.byte, .length = span.length, .tone = span.tone });
            table.lines.push_back(std::move(drawn.lines.front()));
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

    /// The newest status a sample carried, kept through a gap: what decides whether a line applies, never what it says.
    /// @param in The frame's inputs.
    /// @return The status, or nullptr before any sample carried one.
    [[nodiscard]] CompileCacheWire::NodeStatusFields const* CarriedStatusOf(FrameInputs const& in) noexcept
    {
        return in.model->carriedNodeStatus.has_value() ? &*in.model->carriedNodeStatus : nullptr;
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

    /// How the frame dresses one of the leader's cell tones.
    /// @param tone The tone.
    /// @return The frame's tone, or nullopt for plain.
    [[nodiscard]] std::optional<FrameTone> FrameToneOf(Distributed::CellTone tone) noexcept
    {
        return tone < Distributed::CellTone::Last ? CellToneDressTable[static_cast<std::size_t>(tone)].frame : std::nullopt;
    }

    /// How the frame dresses a slot limit where it is named, judged where the fleet's cells and the page judge it.
    /// @param limit The limit.
    /// @return The frame's tone, or nullopt for plain.
    [[nodiscard]] std::optional<FrameTone> LimitFrameTone(Distributed::SlotLimit limit) noexcept
    {
        return FrameToneOf(Distributed::SlotLimitTone(limit));
    }

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

    /// The node's slots at its newest entry: the ones the slots fact's figures are the newest cells of.
    /// @param in The frame's inputs.
    /// @return The slots, or nullopt when the newest entry holds no reading carrying a host.
    [[nodiscard]] std::optional<NodeSlots> NewestSlots(FrameInputs const& in)
    {
        auto const& history = in.model->history;
        return history.empty() ? std::nullopt : SlotsAt(history, history.size() - 1);
    }

    /// @p text as a piece of a fact line, essential and plain.
    /// @param text The words.
    /// @return The piece.
    [[nodiscard]] Piece FactWords(std::string text)
    {
        return Piece { .text = std::move(text), .priority = Priority::Essential };
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
        line.push_back(Piece { .text = name, .priority = Priority::Essential, .tone = LimitFrameTone(ceilings->binding) });
        return line;
    }

    /// A fact cell's figures laid out as one line: each with its words, joined by its own lead, a gauge before a share.
    ///
    /// **The numbers a fact line states are its cell's figures**, the same series a piped stream writes under their
    /// keys, so a figure on the screen and the column a script reads cannot disagree. The first figure is essential;
    /// a gauge goes before the others do.
    /// @param in The frame's inputs.
    /// @param cell The cell.
    /// @return The line.
    [[nodiscard]] FactText FactFigures(FrameInputs const& in, FactCell const& cell)
    {
        auto fact = FactText {};
        for (auto const& figure: cell.figures)
        {
            // The first figure starts the line; a later one is joined by its own lead, or by the gap between figures.
            auto lead = figure.lead.empty() ? PieceGap : figure.lead;
            if (fact.pieces.empty())
                lead = {};
            if (figure.gauge)
            {
                if (auto const share = Newest(SeriesFor(in, figure.figure, {})); share.has_value() && std::isfinite(*share))
                {
                    fact.pieces.push_back(Piece { .text = std::string { lead } + Gauge(*share, FactGaugeCells, *in.glyphs),
                                                  .priority = Priority::Low });
                    lead = FactGap;
                }
            }
            auto piece = Led(lead, BesidePiece(in, figure));
            if (fact.pieces.empty())
                piece.priority = Priority::Essential;
            fact.pieces.push_back(std::move(piece));
        }
        return fact;
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
              // Whether the line applies is what the node last said it runs; the role is what it says now, the marker
              // beside a gap.
              if (!RunsConsensus(CarriedStatusOf(in)))
                  return std::nullopt;
              auto const* status = StatusOf(in);
              if (status == nullptr || !status->runtime.schedulerRole.has_value())
                  return Said(std::string { in.absent });
              auto const name = NameOfSchedulerRole(*status->runtime.schedulerRole);
              return Said(std::string { name }, ToneOfState(name));
          } },
        { .fact = StatusFact::Leader,
          .render = [](FrameInputs const& in, FactCell const& /*cell*/, std::size_t /*width*/) -> std::optional<FactText> {
              // Empty is a READING -- no leader is known -- so it is the marker, on the line the role is on. Except on
              // the leader itself, which names nobody because it is the one: it says so rather than drawing the
              // marker for the one fact it knows best.
              if (!RunsConsensus(CarriedStatusOf(in)))
                  return std::nullopt;
              auto const* status = StatusOf(in);
              if (status == nullptr)
                  return Said(std::string { in.absent });
              auto const& runtime = status->runtime;
              if (!runtime.leaderEndpoint.empty())
                  return Said(runtime.leaderEndpoint);
              if (runtime.schedulerRole == CompileCacheWire::WireSchedulerRole::Leader)
                  return Said(std::string { SelfLeader });
              return Said(std::string { in.absent });
          } },
        { .fact = StatusFact::Slots,
          .render = [](FrameInputs const& in, FactCell const& cell, std::size_t width) -> std::optional<FactText> {
              // A node whose status said it runs no worker has no slots to draw.
              auto const* carried = CarriedStatusOf(in);
              if (carried != nullptr && !carried->runtime.compileSlots.has_value())
                  return std::nullopt;
              auto fact = FactFigures(in, cell);

              // Under the numbers: the gauge and the limit, off the same newest entry the `available` figure is.
              auto const slots = NewestSlots(in);
              fact.more.push_back(SlotLimitLine(in, slots, width));
              // What to do about the limit, in `SlotLimitTable`'s own words, wrapped under the gauge.
              auto const ceilings = slots.has_value() ? slots->ceilings : std::nullopt;
              // A limit that withdrew nothing needs no remedy: `registered` is the machine offering all it has.
              if (ceilings.has_value() && ceilings->binding != Distributed::SlotLimit::Registered)
              {
                  for (auto& line: Wrapped(Distributed::TraitsFor(ceilings->binding).remedy, width, in.cellWidth))
                      fact.more.push_back(
                          { Piece { .text = std::move(line), .priority = Priority::Essential, .tone = FrameTone::Label } });
              }
              return fact;
          } },
        { .fact = StatusFact::CacheTier,
          .render = [](FrameInputs const& in, FactCell const& cell, std::size_t /*width*/) -> std::optional<FactText> {
              // Only on a node that said it runs a tier: a line of markers for a tier that does not exist would be a
              // claim. A gap is not news that it stopped, so the line stays, its figures the marker.
              auto const* carried = CarriedStatusOf(in);
              if (carried == nullptr || (carried->components & CompileCacheWire::NodeComponentBit::CacheTier) == 0)
                  return std::nullopt;
              return FactFigures(in, cell);
          } },
        { .fact = StatusFact::Host,
          .render = [](FrameInputs const& in, FactCell const& cell, std::size_t /*width*/) -> std::optional<FactText> {
              return FactFigures(in, cell);
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

    /// The palette ceiling a chart is encoded with: its ramp's eight colours and its track fit without merging.
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
                .text = std::string { ColumnGap }
                        + Sparkline(Window(DocumentLevels(in.model->history, tile.key), column.words), *in.glyphs) });
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
        return FrameToneOf(tone);
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
                           .noun = SectionNoun(section),
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
    /// @param noun What the table's rows are counted as.
    /// @param budget The content width.
    /// @return The line's item, or none when there is no filter and none is being typed.
    [[nodiscard]] std::vector<Item> FilterItems(FrameInputs const& in,
                                                DocumentSpec const& spec,
                                                TableState const& state,
                                                SectionTable const& table,
                                                CountNoun noun,
                                                std::size_t budget)
    {
        if (!state.typing && state.filter.empty())
            return {};
        // While typed it is essential -- an operator must see what they type -- and cut rather than
        // refused when it is wider than the terminal, since the text is theirs and not a figure.
        auto const text =
            state.typing
                ? std::format("{}filter  /{}_  Enter keeps, Esc clears", Indent, state.filter)
                : std::format(
                      "{}filter  /{}  {} of {}; / edits", Indent, state.filter, table.matched, Counted(table.rows, noun));
        return { Item { .lines = { FitRight(text, std::min(budget, in.cellWidth(text)), in.cellWidth) },
                        .priority = state.typing ? Priority::Essential : spec.filterPriority } };
    }

    /// Cells the legend's colour scale takes: one per step of the chart's ramp, so every colour is a whole cell.
    constexpr auto ChartScaleCells = std::size_t { 8 };

    /// How a history chart is laid out: how many bands, and how many rows of cells each.
    struct ChartShape
    {
        std::size_t bands { 0 };       ///< The bands drawn, the first of them first.
        std::size_t bandCells { 1 };   ///< Rows of cells per band.
        std::size_t spacerCells { 0 }; ///< Blank rows after every band but the last.
    };

    /// The chart as a frame draws it: its one item, and where its two images go in it.
    struct ChartBlock
    {
        Item item {};                      ///< Title, a row per band, axis and legend: one item, so it goes whole.
        std::vector<ChartTrack> tracks {}; ///< The bands drawn, top to bottom.
        std::size_t machines { 0 };        ///< Every machine the span holds a reading for.
        std::size_t window { 0 };          ///< The samples the image's width stands for.
        std::size_t imageColumn { 0 };     ///< Content cells before the chart image.
        std::size_t imageCells { 0 };      ///< Cells across the chart image.
        std::size_t scaleColumn { 0 };     ///< Content cells before the legend's scale.
        std::size_t scaleLine { 0 };       ///< The item's line the scale is drawn on.
        ChartShape shape {};               ///< The shape it was laid out in.
        bool pixels { false };             ///< Whether its bands are an image over blank cells, rather than text.
        bool ramped { false };             ///< Whether a band drawn is in the ramp, so the legend shows its scale.
    };

    /// Whether a chart on @p context's rung is an image: the Sixel rung, with an encoder and a cell size to lay it out in.
    /// @param in The frame's inputs.
    /// @param context The session facts.
    /// @return True for pixels; false for the rung's chart marks, or for none.
    [[nodiscard]] bool ChartIsPixels(FrameInputs const& in, PanelContext const& context) noexcept
    {
        return context.rung == RenderRung::Sixel && context.sixel != nullptr && in.model->cellPixels.has_value();
    }

    /// The machines the fleet chart could band: those with a reading of its figure in the span it counts them over --
    /// the image's fixed step, or as much history as text could draw.
    /// @param in The frame's inputs.
    /// @param context The session facts.
    /// @return How many machines.
    [[nodiscard]] std::size_t FleetChartMachines(FrameInputs const& in, PanelContext const& context)
    {
        auto const window = ChartIsPixels(in, context) ? ChartWindowFor(in.model->history.size()) : HistoryCapacity;
        return FleetChartBands(in.model->history, FleetChartMetrics.front(), window).size();
    }

    /// A span of @p samples readings, written for the title and the axis.
    /// @param samples How many samples.
    /// @param interval The sampling interval, when stated.
    /// @return `40 s`, `4 min 16 s` -- or `30 samples` where the interval is not known, rather than a guess.
    [[nodiscard]] std::string WindowText(std::size_t samples, std::optional<std::chrono::milliseconds> interval)
    {
        constexpr auto SecondsAsMinutesFrom = std::size_t { 120 };
        if (!interval.has_value() || interval->count() <= 0)
            return Counted(samples, SampleNoun);
        auto const seconds = ((static_cast<std::size_t>(interval->count()) * samples) + 500) / 1000;
        if (seconds < SecondsAsMinutesFrom)
            return std::format("{} s", seconds);
        return seconds % 60 == 0 ? std::format("{} min", seconds / 60)
                                 : std::format("{} min {} s", seconds / 60, seconds % 60);
    }

    /// A chart figure written for a person in its column's own scale: `18.2 %` for a `cpu-busy` of 0.182.
    /// @param in The frame's inputs.
    /// @param metric The figure.
    /// @param scaled The scaled value, or nullopt for none.
    /// @return The written figure, or the absent marker.
    [[nodiscard]] std::string ChartFigureText(FrameInputs const& in,
                                              FleetChartMetric const& metric,
                                              std::optional<double> scaled)
    {
        if (!scaled.has_value())
            return std::string { in.absent };
        auto const format = Distributed::FleetColumnFormat(metric.section, metric.valueColumn);
        auto const scale = format.has_value() ? Distributed::CellFormatTable[static_cast<std::size_t>(*format)].scale : 0.0;
        if (!format.has_value() || scale <= 0.0)
            return std::format("{}", *scaled);
        return Distributed::HumanFleetFigure(static_cast<std::uint64_t>(std::llround(std::max(0.0, *scaled) / scale)),
                                             *format);
    }

    /// Append one line built from @p pieces to @p item, with the runs its pieces carry.
    /// @param item The item.
    /// @param pieces The line's pieces.
    void AppendLine(Item& item, std::span<Piece const> pieces)
    {
        auto line = LineOf(pieces, {}, item.priority);
        for (auto span: line.spans)
        {
            span.line = item.lines.size();
            item.spans.push_back(span);
        }
        item.lines.push_back(std::move(line.lines.front()));
    }

    /// What a history chart draws, as the one layout (`LayoutChart`) takes it from a panel.
    struct ChartSource
    {
        std::string what {};      ///< The one figure a chart of it draws, dressed as a figure; empty for several.
        std::string whatAfter {}; ///< Words after it and before the span: ` per machine`, or `history`.
        /// Words after the span that go first for width, given the bands drawn and the span they were taken over.
        std::function<std::string(std::size_t drawn, std::size_t window)> qualifier {};
        std::string low {};          ///< On a pixel chart, what the scale's coldest colour stands for.
        std::string high {};         ///< On a pixel chart, what its hottest stands for.
        std::string drawn {};        ///< What a bar is, for the legend: `bar: cpu-busy`.
        std::string rest {};         ///< The legend's last words, which go first for width.
        std::size_t available { 0 }; ///< Every band the chart could draw; the shape draws the first of them.
        /// The first @p count bands over the newest @p window samples, their latest figures and tops written.
        std::function<std::vector<ChartTrack>(std::size_t count, std::size_t window)> tracks {};
    };

    /// @p source laid out in @p shape: a title, a row per band, a time axis and a legend, as one item that goes whole.
    ///
    /// **The one layout every history chart has** (#134 F14), so a panel supplies what it draws and nothing of how:
    ///   - the title names the figure, or the chart, and the span its width covers;
    ///   - a band's row names it, writes its newest figure, and -- for a band scaled to its own peak -- what its top
    ///     stands for;
    ///   - on a PIXEL chart the bands are one image over blank cells, with the scale's colours in the legend; on a
    ///     TEXT chart they are the rung's chart marks, drawn in the band's own rows;
    ///   - the axis under the bands runs from the span ago to `now` at the right edge.
    /// @param in The frame's inputs.
    /// @param source What it draws.
    /// @param context The session facts: the interval.
    /// @param budget The content width.
    /// @param shape How many bands, and rows of cells each.
    /// @param priority When the chart goes for height.
    /// @param minimumCells The fewest cells across its bands are drawn in; narrower, it is not drawn.
    /// @param pixels Whether the bands are an image rather than text.
    /// @return The chart, or nullopt where it cannot be drawn at @p budget.
    [[nodiscard]] std::optional<ChartBlock> LayoutChart(FrameInputs const& in,
                                                        ChartSource const& source,
                                                        PanelContext const& context,
                                                        std::size_t budget,
                                                        ChartShape shape,
                                                        Priority priority,
                                                        std::size_t minimumCells,
                                                        bool pixels)
    {
        auto block = ChartBlock {};
        block.machines = source.available;
        block.pixels = pixels;
        block.shape = shape;
        block.shape.bands = std::min(shape.bands, source.available);
        if (block.shape.bands == 0 || !source.tracks)
            return std::nullopt;
        auto& item = block.item;
        item.priority = priority;
        item.image = pixels;

        // A pixel chart's span is a fixed step holding the history; a text chart's is the cells it has, a sample a
        // cell. Its columns are measured once over the history the tracks could show, then the tracks are taken
        // over the span the image cells hold -- a column never narrower than either needs.
        auto const measure =
            [&](std::vector<ChartTrack> const& tracks, std::size_t& name, std::size_t& figure, std::size_t& top) {
                for (auto const& track: tracks)
                {
                    name = std::max(name, in.cellWidth(track.label));
                    figure = std::max(figure, in.cellWidth(track.latest));
                    top = std::max(top, in.cellWidth(track.top));
                }
            };
        auto nameCells = std::size_t { 0 };
        auto figureCells = std::size_t { 0 };
        auto topCells = std::size_t { 0 };
        auto const firstWindow = pixels ? ChartWindowFor(in.model->history.size()) : HistoryCapacity;
        measure(source.tracks(block.shape.bands, firstWindow), nameCells, figureCells, topCells);
        auto const indent = in.cellWidth(Indent);
        auto const gap = in.cellWidth(ColumnGap);
        auto const columnsOf = [&] {
            return indent + std::min(nameCells, budget / 3) + gap + figureCells + (topCells > 0 ? gap + topCells : 0) + gap;
        };
        if (budget < columnsOf() + minimumCells)
            return std::nullopt;
        block.window = pixels ? firstWindow : std::min(budget - columnsOf(), HistoryCapacity);
        block.tracks = source.tracks(block.shape.bands, block.window);
        // A band the span it was measured over held and this span does not is no band: the rows follow the tracks.
        block.shape.bands = std::min(block.shape.bands, block.tracks.size());
        if (block.shape.bands == 0)
            return std::nullopt;
        measure(block.tracks, nameCells, figureCells, topCells);
        nameCells = std::min(nameCells, budget / 3);
        block.imageColumn = columnsOf();
        if (budget < block.imageColumn + minimumCells)
            return std::nullopt;
        block.imageCells = budget - block.imageColumn;
        if (!pixels)
            block.window = std::min(block.window, block.imageCells);
        auto const span = WindowText(block.window, ReadingInterval(in, context));

        // The title: what the chart draws, over how long, and what else it says where there is room.
        auto title = std::vector<Piece> { Piece { .text = std::string { Indent }, .priority = Priority::Essential } };
        if (!source.what.empty())
            title.push_back(Piece { .text = source.what, .priority = Priority::Essential, .tone = FrameTone::Figure });
        title.push_back(Piece { .text = std::format("{}, last {}", source.whatAfter, span),
                                .priority = Priority::Essential,
                                .tone = FrameTone::Label });
        if (auto qualifier = source.qualifier ? source.qualifier(block.shape.bands, block.window) : std::string {};
            !qualifier.empty())
            title.push_back(Piece { .text = std::move(qualifier), .priority = Priority::Low, .tone = FrameTone::Label });
        auto const keptTitle = FitPieces(std::move(title), budget, 0, in.cellWidth);
        if (!keptTitle.has_value())
            return std::nullopt;
        AppendLine(item, *keptTitle);

        item.imageFirst = item.lines.size();
        item.imageRows = (block.shape.bands * block.shape.bandCells) + ((block.shape.bands - 1) * block.shape.spacerCells);
        auto const& levels = in.glyphs->chartLevels;
        for (auto const& track: block.tracks)
        {
            // Every band after the first starts after its spacer rows.
            if (item.lines.size() > item.imageFirst)
                item.lines.resize(item.lines.size() + block.shape.spacerCells);
            auto row = std::vector<Piece> {
                Piece { .text = std::string { Indent } + FitRight(track.label, nameCells, in.cellWidth) },
                Piece { .text = std::string { ColumnGap } + AlignRight(track.latest, figureCells, in.cellWidth),
                        .tone = FrameTone::Figure },
            };
            if (topCells > 0)
                row.push_back(Piece { .text = std::string { ColumnGap } + FitRight(track.top, topCells, in.cellWidth),
                                      .tone = FrameTone::Label });
            auto const first = item.lines.size();
            AppendLine(item, row);
            item.lines.resize(item.lines.size() + (block.shape.bandCells - 1));
            if (pixels)
                continue;
            // A text band draws in its own rows, from the image column.
            auto const bars = ChartTextRows(track.shares, block.shape.bandCells, block.imageCells, levels);
            for (auto const index: std::views::iota(std::size_t { 0 }, bars.size()))
            {
                auto& line = item.lines[first + index];
                line.append(block.imageColumn - std::min(block.imageColumn, in.cellWidth(line)), ' ');
                line += bars[index];
            }
        }

        // The axis under the bands: how long ago their left edge is, and `now` at their right.
        constexpr auto Now = std::string_view { "now" };
        auto const ago = std::format("-{}", span);
        auto const between = block.imageCells - std::min(block.imageCells, in.cellWidth(ago) + in.cellWidth(Now));
        auto const axis = std::to_array<Piece>({
            Piece { .text = std::string(block.imageColumn, ' ') },
            Piece { .text = ago + std::string(between, ' ') + std::string { Now }, .tone = FrameTone::Label },
        });
        AppendLine(item, axis);

        // The legend. A pixel chart with a band in the ramp: the scale's colours between the two values they run
        // between, then what the marks mean; with none, no scale, since no colour stands for a value. A text chart:
        // its zero mark and its full cell, and that a blank is a sample not read.
        block.ramped =
            pixels
            && std::ranges::any_of(block.tracks, [](ChartTrack const& track) { return track.paint == ChartPaint::Ramp; });
        auto legend = std::vector<Piece> { Piece { .text = std::string { Indent }, .priority = Priority::Essential } };
        if (block.ramped)
        {
            legend.push_back(Piece { .text = source.low, .priority = Priority::Essential, .tone = FrameTone::Label });
            legend.push_back(
                Piece { .text = " " + std::string(ChartScaleCells, ' ') + " ", .priority = Priority::Essential });
            legend.push_back(Piece { .text = source.high, .priority = Priority::Essential, .tone = FrameTone::Label });
            block.scaleColumn = indent + in.cellWidth(source.low) + 1;
        }
        else if (!pixels && levels.size() >= 2)
        {
            legend.push_back(Piece {
                .text = std::format("{} zero", levels.front()), .priority = Priority::Essential, .tone = FrameTone::Label });
            legend.push_back(Piece { .text = std::format("{}{} the top", ColumnGap, levels.back()),
                                     .priority = Priority::Essential,
                                     .tone = FrameTone::Label });
        }
        legend.push_back(Piece {
            .text = std::string { ColumnGap } + source.drawn, .priority = Priority::Normal, .tone = FrameTone::Label });
        legend.push_back(Piece { .text = source.rest, .priority = Priority::Low, .tone = FrameTone::Label });
        auto const keptLegend = FitPieces(std::move(legend), budget, 0, in.cellWidth);
        if (!keptLegend.has_value())
            return std::nullopt;
        block.scaleLine = item.lines.size();
        AppendLine(item, *keptLegend);
        return block;
    }

    /// Rows a history chart spends besides its bands: its blank, its title, its axis and its legend.
    constexpr auto ChartOverheadRows = std::size_t { 4 };

    /// A panel's history chart laid out in @p shape: one band per `ChartRow`, the first of them first.
    ///
    /// Pixels on the Sixel rung with a cell size and an encoder; the rung's chart marks on a rung that has them;
    /// nothing otherwise. A band scaled to its own peak says what its top stands for, since a bar's height means
    /// nothing without it.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @param context The session facts: the rung, the encoder and the interval.
    /// @param budget The content width.
    /// @param shape How many bands, and rows of cells each.
    /// @return The chart, or nullopt where the panel has none or it cannot be drawn.
    [[nodiscard]] std::optional<ChartBlock> PanelChartOf(
        FrameInputs const& in, PanelSpec const& spec, PanelContext const& context, std::size_t budget, ChartShape shape)
    {
        if (spec.charts.empty())
            return std::nullopt;
        auto const pixels = ChartIsPixels(in, context);
        if (!pixels && in.glyphs->chartLevels.size() < 2)
            return std::nullopt;
        // A chart with no reading in any band draws nothing but blanks, and laid out before the first readings it takes
        // rows the status facts claim a moment later -- a chart that flashes up and vanishes. It waits for a reading.
        auto const readAnything = std::ranges::any_of(spec.charts, [&in](ChartRow const& row) {
            return std::ranges::any_of(SeriesFor(in, row.figure, {}),
                                       [](std::optional<double> const& value) { return value.has_value(); });
        });
        if (!readAnything)
            return std::nullopt;
        auto source = ChartSource {
            .what = {},
            .whatAfter = "history",
            .qualifier = [](std::size_t, std::size_t) { return std::string { "; each band against its top" }; },
            .low = "0",
            .high = "top",
            .drawn = "bar: share of its band's top",
            .rest = pixels ? ", grey: to the top, blank: no reading" : ", blank: no reading",
            .available = spec.charts.size(),
            .tracks =
                [&in, &spec](std::size_t count, std::size_t window) {
                    auto tracks = std::vector<ChartTrack> {};
                    for (auto const& row: spec.charts | std::views::take(count))
                    {
                        auto const series = SeriesFor(in, row.figure, {});
                        auto const shown = std::min(series.size(), window);
                        auto const drawn = std::span { series }.last(shown);
                        auto const top = row.top.has_value() ? row.top : PeakOf(drawn);
                        tracks.push_back(ChartTrack {
                            .label = std::string { row.label },
                            .latest = FigureText(in, row.figure, Newest(series)).Text(),
                            .top = "to " + FigureText(in, row.figure, top).Text(),
                            .shares = SharesOf(drawn, top.value_or(0.0)),
                            .paint = row.paint,
                        });
                    }
                    return tracks;
                },
        };
        return LayoutChart(in, source, context, budget, shape, Priority::Low, spec.chartGrowth.minimumCells, pixels);
    }

    /// The fewest rows of marks a text band keeps with a spacer row under it; a band any shorter is packed.
    constexpr auto SpacedBandCellsLeast = std::size_t { 2 };

    /// The shape a history chart takes in @p spare rows: every band a row first, then taller bands.
    ///
    /// **Text bands are one blank row apart** (#134 F-b): stacked marks from two bands run together into one shape,
    /// and nothing then says where one band's top is. The spacer rows come out of the rows the bands grow into, never
    /// on top of them. A chart is spaced only while every band keeps `SpacedBandCellsLeast` rows of marks: a crowded
    /// chart keeps its bands rather than trading one for a blank row. A pixel chart is never spaced, since its image
    /// draws its own gap inside each band.
    /// @param growth How the chart grows.
    /// @param bands How many bands it could draw.
    /// @param spare Rows the fitted frame left over.
    /// @param spaced Whether the bands are text marks, which are spaced where the rows allow.
    /// @return The shape; no bands where the rows cannot hold a chart.
    [[nodiscard]] ChartShape PanelChartShape(ChartGrowth const& growth,
                                             std::size_t bands,
                                             std::size_t spare,
                                             bool spaced) noexcept
    {
        if (spare <= ChartOverheadRows || bands == 0)
            return ChartShape { .bands = 0, .bandCells = 1, .spacerCells = 0 };
        auto const rows = std::min(growth.cellsHighMost, spare - ChartOverheadRows);
        auto const drawn = std::min(bands, rows);
        auto const between = drawn - 1;
        auto const spacer = spaced && between > 0 && (rows - between) / drawn >= SpacedBandCellsLeast ? std::size_t { 1 }
                                                                                                      : std::size_t { 0 };
        return ChartShape { .bands = drawn,
                            .bandCells = std::clamp((rows - (spacer * between)) / drawn,
                                                    std::size_t { 1 },
                                                    std::max<std::size_t>(1, growth.bandCellsMost)),
                            .spacerCells = spacer };
    }

    /// The fleet chart laid out in @p shape: the first `FleetChartMetrics` row, one band per machine by name.
    ///
    /// Pixels where the rung can draw them, the rung's chart marks where it has those, nothing otherwise. Before any
    /// machine reported the figure it is one line saying so, never an empty chart, which would read as a fleet with
    /// nothing on it.
    /// @param in The frame's inputs.
    /// @param spec The document block.
    /// @param context The session facts: the rung, the encoder and the interval.
    /// @param budget The content width.
    /// @param shape How many bands, and rows of cells each.
    /// @return The chart, or nullopt where it is not drawn: a rung with no chart marks, or too narrow.
    [[nodiscard]] std::optional<ChartBlock> FleetChartOf(
        FrameInputs const& in, DocumentSpec const& spec, PanelContext const& context, std::size_t budget, ChartShape shape)
    {
        auto const pixels = ChartIsPixels(in, context);
        if (!pixels && in.glyphs->chartLevels.size() < 2)
            return std::nullopt;
        auto const& metric = FleetChartMetrics.front();
        auto const machines = FleetChartMachines(in, context);
        if (machines == 0)
        {
            auto block = ChartBlock {};
            block.item.priority = Priority::Low;
            block.pixels = pixels;
            auto const nothing = std::to_array<Piece>({
                Piece { .text = std::string { Indent } },
                Piece { .text = std::string { metric.key }, .tone = FrameTone::Figure },
                Piece { .text = " per machine: no machine has reported it yet", .tone = FrameTone::Label },
            });
            AppendLine(block.item, nothing);
            return block;
        }
        auto const high = ChartFigureText(in, metric, metric.full);
        auto source = ChartSource {
            .what = std::string { metric.key },
            .whatAfter = " per machine",
            // Counted over the span the bands were taken over, which on text is the cells the chart has.
            .qualifier =
                [&in, chartMetric = &metric](std::size_t drawn, std::size_t window) {
                    auto const of = FleetChartBands(in.model->history, *chartMetric, window).size();
                    return drawn < of ? std::format("; the first {} of {} by name", drawn, of) : std::string {};
                },
            .low = ChartFigureText(in, metric, 0.0),
            .high = high,
            // Every band has one scale, the metric's whole, so the legend states it once rather than a band each.
            .drawn = pixels ? std::format("bar: {}", metric.key) : std::format("bar: {} of {}", metric.key, high),
            // The grey is the band's whole scale, behind every reading: a bar of zero leaves it bare.
            .rest = pixels ? std::format(", grey: to {}, blank: no reading", high) : std::string { ", blank: no reading" },
            .available = machines,
            .tracks =
                [&in, chartMetric = &metric](std::size_t count, std::size_t window) {
                    auto bands = FleetChartBands(in.model->history, *chartMetric, window);
                    bands.resize(std::min(bands.size(), count));
                    auto tracks = FleetChartTracks(in.model->history, *chartMetric, bands, window);
                    for (auto const index: std::views::iota(std::size_t { 0 }, tracks.size()))
                        tracks[index].latest = ChartFigureText(in, *chartMetric, bands[index].latest);
                    return tracks;
                },
        };
        return LayoutChart(in, source, context, budget, shape, Priority::Low, spec.chartGrowth.minimumCells, pixels);
    }

    /// A history chart laid out in the rows @p fitted left over, and only those: a panel's, or the fleet's.
    ///
    /// **The one grow step.** It is laid out for exactly those rows -- every band a row first, then taller bands, as
    /// @p growth bounds them -- so the chart item and its blank take no more rows than the first fit left over, and a
    /// chart never takes a row a table, a tile or a fact could have had (`PanelChartShape` counts every row of it).
    /// **A layout that draws fewer bands than it was offered is laid out again for the bands it drew**: a text span
    /// can hold fewer machines than the history does, and the rows laid out for the rest would go to nobody.
    /// @param available The rows the content may take.
    /// @param growth How the chart grows.
    /// @param bands How many bands it could draw.
    /// @param spaced Whether the bands are text marks, spaced where the rows allow (`PanelChartShape`).
    /// @param layout The chart laid out in a shape.
    /// @param compose The frame's items with a chart item placed, or with none.
    /// @param items The frame's items; they carry the chart when it is kept.
    /// @param fitted The first fit; replaced when the chart is kept.
    /// @return The chart kept, or nullopt.
    [[nodiscard]] std::optional<ChartBlock> GrowChart(
        std::size_t available,
        ChartGrowth const& growth,
        std::size_t bands,
        bool spaced,
        std::function<std::optional<ChartBlock>(ChartShape)> const& layout,
        std::function<std::vector<Item>(std::optional<Item> const&)> const& compose,
        std::vector<Item>& items,
        FittedRows& fitted)
    {
        auto const spare = available - std::min(available, fitted.lines.size());
        auto const shape = PanelChartShape(growth, bands, spare, spaced);
        if (shape.bands == 0)
            return std::nullopt;
        auto chart = layout(shape);
        if (chart.has_value() && chart->shape.bands < shape.bands)
            chart = layout(PanelChartShape(growth, chart->shape.bands, spare, spaced));
        if (!chart.has_value())
            return std::nullopt;
        auto withChart = compose(chart->item);
        auto grown = FitRows(withChart, available);
        if (!grown.has_value())
            return std::nullopt;
        items = std::move(withChart);
        fitted = std::move(*grown);
        return chart;
    }

    /// The fleet document block as one frame draws it, and where the table's scroll landed.
    ///
    /// Two groups, because the history chart goes between them when the rows allow one.
    struct DocumentBlock
    {
        std::vector<Item> tiles {}; ///< The headline tiles.
        std::vector<Item> body {};  ///< The strip, the active section's table and its filter line.
        std::size_t scroll { 0 };   ///< The table's scroll, clamped to its matching rows.
    };

    /// The fleet document block: the tiles, then the strip, the active section's table and its filter line.
    /// @param in The frame's inputs.
    /// @param spec The panel.
    /// @param context The session facts: the section the table draws.
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
        block.tiles = TileItems(in, *spec.document, budget);
        std::ranges::move(StripItems(in, *spec.document, context.section, budget), std::back_inserter(block.body));
        auto filter = FilterItems(in, *spec.document, state, *table, SectionNoun(context.section), budget);
        std::ranges::move(table->items, std::back_inserter(block.body));
        std::ranges::move(filter, std::back_inserter(block.body));
        block.scroll = table->scroll;
        return block;
    }

    /// How much of where a source line says the source answered.
    enum class SourceWhere : std::uint8_t
    {
        Named,   ///< `SUBSCRIBE at <where>`, when the reading said.
        Omitted, ///< `SUBSCRIBE` alone: the title bar names the address, so it is the part a narrow line spares.
    };

    /// Which source answered, as the source line names it.
    ///
    /// `subscription (SUBSCRIBE at 127.0.0.1:6674)`: the source, and in brackets what was asked where. A
    /// reading from an endpoint with a role in its subject adds the role in the same brackets, so every
    /// panel reads one way: `subscription (SUBSCRIBE at build-01:7071, leader)`. Each part the reader did not
    /// say is left out rather than guessed, and before any reading the whole of it is the absent marker.
    /// @param in The frame's inputs.
    /// @param where Whether the address is said.
    /// @return The text.
    [[nodiscard]] std::string SourceText(FrameInputs const& in, SourceWhere where)
    {
        if (!in.model->latestStamp.has_value())
            return std::string { in.absent };
        auto const& stamp = *in.model->latestStamp;
        if (stamp.route.empty())
            return stamp.source;
        auto const asked = stamp.where.empty() || where == SourceWhere::Omitted
                               ? stamp.route
                               : std::format("{} at {}", stamp.route, stamp.where);
        if (!stamp.role.empty())
            return std::format("{} ({}, {})", stamp.source, asked, stamp.role);
        return std::format("{} ({})", stamp.source, asked);
    }

    /// The last line: which source answered on the left, and on the right how many samples and how many
    /// of them were gaps.
    ///
    /// The counts go first for width -- they describe the run, the source says what the run is of -- and
    /// what is kept is laid out right-aligned, so the counts stay in one place as the source's text changes.
    ///
    /// **But the address goes before the counts do** (#1399): it is in the title bar already, and a fleet
    /// line naming `SUBSCRIBE at <where>, leader` is 82 cells with its counts at 80 columns. So the line is
    /// fitted with the address, then without it, and only a line that keeps no counts either way drops them
    /// -- saying the address, which is then the most the room holds.
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
        auto const counts =
            std::format("{}, {}", Counted(model.samples, SampleNoun), Counted(static_cast<std::size_t>(gaps), GapNoun));
        auto const fitted = [&](SourceWhere where) {
            return FitPieces(
                { Piece { .text = std::format("{}source  {}", Indent, SourceText(in, where)), .priority = priority },
                  Piece { .text = std::string { PieceGap } + counts, .priority = std::max(priority, Priority::Low) } },
                budget,
                0,
                in.cellWidth);
        };
        auto kept = fitted(SourceWhere::Named);
        if (kept.has_value() && kept->size() < 2)
        {
            auto spared = fitted(SourceWhere::Omitted);
            if (spared.has_value() && spared->size() == 2)
                kept = std::move(spared);
        }
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

    /// Whether the newest sample was read: false before any sample and beside a gap.
    /// @param model What is known.
    /// @return True when the newest history entry holds a reading.
    [[nodiscard]] bool NewestSampleRead(DashboardModel const& model) noexcept
    {
        return !model.history.empty() && model.history.back().reading.has_value();
    }

    /// How long the endpoint has served, from whichever part of the NEWEST sample carries it.
    ///
    /// The newest sample's, never the last reading's: an uptime is a figure about now, and beside a gap it is not
    /// known -- the last one read went on standing in the title, frozen, while the node it described was gone.
    /// @param model What is known.
    /// @return Seconds, or nullopt when the newest sample does not say.
    [[nodiscard]] std::optional<std::uint64_t> UptimeOf(DashboardModel const& model) noexcept
    {
        if (model.nodeStatus.has_value())
            return model.nodeStatus->uptimeSeconds;
        if (model.history.empty() || !model.history.back().stats.has_value())
            return std::nullopt;
        return static_cast<std::uint64_t>(model.history.back().stats->snapshot.uptime.value.count());
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
              // Stated as the leader's only while the newest sample was read from it: an endpoint that has not
              // answered as the leader -- yet, or since a gap -- has not been shown to be one, and is still where the
              // session asks.
              auto const led = NewestSampleRead(*in.model) && in.model->latestStamp.has_value()
                               && in.model->latestStamp->role == LeaderRole;
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
              auto const noun = SectionNoun(FleetSection::Machines);
              return machines == nullptr ? std::format("{} {}", in.absent, noun.many) : Counted(machines->rows.size(), noun);
          } },
        { .fact = ChromeFact::Interval,
          .render = [](FrameInputs const& in, PanelContext const& context) -> std::string {
              auto const interval = ReadingInterval(in, context);
              return interval.has_value() ? std::format("every {}s", std::chrono::duration<double> { *interval }.count())
                                          : std::format("every {}", in.absent);
          } },
        { .fact = ChromeFact::Quit,
          .render = [](FrameInputs const& /*in*/, PanelContext const& /*context*/) -> std::string { return "q"; } },
        { .fact = ChromeFact::QuitWord,
          .render = [](FrameInputs const& /*in*/, PanelContext const& /*context*/) -> std::string { return "q quit"; } },
    } };

    static_assert(RowsInEnumeratorOrder(ChromeFactTable, &ChromeFactSpec::fact),
                  "ChromeFactTable must hold one row per ChromeFact, in enumerator order");

    /// The subject a title bar names: what answered for a panel titled by its server, when a session said.
    /// @param spec The panel.
    /// @param context The session's facts.
    /// @return The title.
    [[nodiscard]] std::string_view TitleOf(PanelSpec const& spec, PanelContext const& context) noexcept
    {
        return spec.titleNamesServer && !context.server.empty() ? std::string_view { context.server } : spec.title;
    }

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
        auto pieces =
            std::vector<Piece> { Piece { .text = std::string { TitleOf(spec, context) }, .priority = Priority::Essential } };
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
            title.subject = std::string { TitleOf(spec, context) };
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
        content = std::max(content, cellWidth(Indent) + RateLabelColumnsFor(spec.rates, cellWidth) + FigureColumns);
        rows += row.priority == Priority::Essential ? 1 : 0;
    }
    for (auto const& row: spec.levels)
    {
        content = std::max(content, cellWidth(Indent) + LevelLabelColumnsFor(spec.levels, cellWidth) + FigureColumns);
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
    auto const in =
        FrameInputs { .model = &model, .glyphs = _glyphs, .absent = _context.absent, .cellWidth = _context.cellWidth };
    auto const columns = model.columns > 0 ? static_cast<std::size_t>(model.columns) : DefaultColumns;
    auto const reportedRows = model.rows > 0 ? static_cast<std::size_t>(model.rows) : std::size_t { 0 };
    auto const available =
        reportedRows > 0 ? std::optional<std::size_t> { reportedRows - std::min(reportedRows, FrameRows) } : std::nullopt;
    auto const minimum = MinimumPanelSize(*_spec, in.cellWidth);
    auto const tooSmall = [&](std::size_t neededColumns) {
        return DashboardFrame { .text = TooSmall(
                                    TitleOf(*_spec, _context),
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
    auto document = DocumentItems(
        in, *_spec, _context, TableState { .filter = _filter, .scroll = _scroll, .typing = _typingFilter }, budget);
    auto below = FactGroups(in, *_spec, FactPlace::BelowRates, budget);
    for (auto const* block: { &rates, &levels, &tiers })
        if (!block->has_value())
            return tooSmall(block->error() + FrameColumns);
    for (auto const* facts: { &above, &below })
        if (!facts->has_value())
            return tooSmall(facts->error() + FrameColumns);
    if (!document.has_value())
        return tooSmall(document.error() + FrameColumns);
    _scroll = document->scroll;

    // Top to bottom, a blank before every block that has something to draw. The history chart, when the rows allow
    // one, is its own block: under the rates, or on a fleet panel between the tiles and the strip.
    auto groups = std::move(*above);
    auto const underRates = groups.size() + 1;
    for (auto* block: { &*rates, &*levels, &*tiers, &document->tiles })
        groups.push_back(std::move(*block));
    auto const chartAt = _spec->document.has_value() ? groups.size() : underRates;
    groups.push_back(std::move(document->body));
    std::ranges::move(*below, std::back_inserter(groups));
    auto source = SourceLine(in, _spec->sourcePriority, budget);
    if (!source.has_value())
        return tooSmall(source.error() + FrameColumns);
    auto const& sourceLine = *source;
    auto const compose = [&](std::optional<Item> const& chartItem) {
        auto composed = std::vector<Item> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, groups.size()))
        {
            if (index == chartAt && chartItem.has_value())
            {
                composed.push_back(Blank());
                composed.push_back(*chartItem);
            }
            if (groups[index].empty())
                continue;
            composed.push_back(Blank());
            std::ranges::copy(groups[index], std::back_inserter(composed));
        }
        composed.push_back(Blank());
        if (sourceLine.has_value())
            composed.push_back(Item { .lines = { *sourceLine }, .priority = _spec->sourcePriority });
        return composed;
    };
    auto items = compose(std::nullopt);

    // The chart grows into rows nothing else wanted, then the frame pads to the terminal's height: both
    // after a first fit, so neither ever takes a row a table or a tile could have had.
    auto fitted = FitRows(items, available);
    if (!fitted.has_value())
        return tooSmall(0);
    auto chart = std::optional<ChartBlock> {};
    if (available.has_value() && _spec->document.has_value())
    {
        // Before any machine reported the figure, one band's worth of rows holds the line saying so.
        chart = GrowChart(
            *available,
            _spec->document->chartGrowth,
            std::max<std::size_t>(1, FleetChartMachines(in, _context)),
            !ChartIsPixels(in, _context),
            [&](ChartShape shape) { return FleetChartOf(in, *_spec->document, _context, budget, shape); },
            compose,
            items,
            *fitted);
    }
    else if (available.has_value() && !_spec->charts.empty())
        chart = GrowChart(
            *available,
            _spec->chartGrowth,
            _spec->charts.size(),
            !ChartIsPixels(in, _context),
            [&](ChartShape shape) { return PanelChartOf(in, *_spec, _context, budget, shape); },
            compose,
            items,
            *fitted);
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

    // The chart kept its rows: draw its two images over them, laid out exactly as its item was. The frame's
    // first row is its top edge and its first column its left edge, so a content line's frame row is its
    // index plus two and a content cell's column is its index plus two.
    // An image needs the cell's pixel size to be laid out at all; without one the chart keeps its text.
    if (chart.has_value() && chart->pixels && !chart->tracks.empty() && fitted->imageLine.has_value()
        && model.cellPixels.has_value() && _context.sixel != nullptr)
    {
        auto const& cell = *model.cellPixels;
        auto const itemRow = *fitted->imageLine + 2;
        auto const place = [&](ChartRaster const& raster, FramePlacement placement) {
            auto encoded = _context.sixel->Encode(
                RgbaImage { .pixels = raster.rgba, .width = raster.width, .height = raster.height }, ChartColours);
            if (!encoded.has_value())
                return;
            placement.sixel = std::move(*encoded);
            frame.placements.push_back(std::move(placement));
        };
        place(ChartBandsRaster(
                  chart->tracks, chart->window, chart->imageCells * cell.width, chart->item.imageRows * cell.height),
              FramePlacement { .row = itemRow + chart->item.imageFirst,
                               .column = 2 + chart->imageColumn,
                               .cellsWide = chart->imageCells,
                               .cellsHigh = chart->item.imageRows,
                               .sixel = {} });
        // The scale only beside a band whose colours stand for values: a chart of plain bands states none.
        if (chart->ramped)
            place(ChartScaleRaster(ChartScaleCells * cell.width, cell.height),
                  FramePlacement { .row = itemRow + chart->scaleLine,
                                   .column = 2 + chart->scaleColumn,
                                   .cellsWide = ChartScaleCells,
                                   .cellsHigh = 1,
                                   .sixel = {} });
    }
    return frame;
}

} // namespace FastCache::Cli
