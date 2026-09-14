// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardRung.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/FigureText.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// @file DashboardGlyphs.hpp
/// What each rung of the `live-stats` ladder draws WITH, and the few drawing rules every panel
/// shares.
///
/// **One table, so a panel never branches on its rung.** A panel is handed a rung as a value
/// (`DashboardRung.hpp`), looks its row up once, and draws through the row: the sparkline, the
/// frame and the gauge are all spelled here. Two panels asking `if (rung == Ascii)` would be two
/// places for the rungs to come to disagree about what a gap looks like.
///
/// **What is NOT in the table is as deliberate as what is.** The absent marker has no column:
/// it is the operator's `--absent`, resolved once by whoever composes the session, and it is the
/// same text on every rung (#134 §9.6). A column for it would be the one way to let two rungs
/// render one absent cell differently.

/// The eight block elements a Unicode sparkline draws, lowest first.
///
/// U+2581 to U+2588. `▁` is the lowest VALUE, which is why a cell with no reading is never drawn
/// with it (§6.1): a space is *nothing was read*, `▁` is *zero was read*.
inline constexpr std::array<std::string_view, 8> BlockLevels { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };

/// Everything one rung draws with.
struct RungGlyphs
{
    RenderRung rung; ///< The enumerator this row describes.

    /// The sparkline's levels, lowest first; EMPTY on a rung that draws no sparkline.
    ///
    /// Empty rather than one `#`, and that is §10's rule rather than a gap in the table: one level
    /// per cell carries no information and LOOKS like data, which is worse than no column.
    std::span<std::string_view const> sparkLevels;

    /// A sparkline cell with no reading behind it.
    std::string_view noReading;

    std::string_view horizontal;  ///< The frame's top and bottom edge.
    std::string_view vertical;    ///< The frame's left and right edge.
    std::string_view topLeft;     ///< The frame's corners, clockwise from the top left.
    std::string_view topRight;    ///< See `topLeft`.
    std::string_view bottomRight; ///< See `topLeft`.
    std::string_view bottomLeft;  ///< See `topLeft`.

    std::string_view gaugeOpen;   ///< What opens a gauge; empty where the fill alone is legible.
    std::string_view gaugeFilled; ///< One filled gauge cell.
    /// One cell of a slot gauge's middle part: capacity that is available and not in use, between the
    /// filled cells of what runs and the empty cells of what a limit has withdrawn.
    std::string_view gaugeHeld;
    std::string_view gaugeEmpty; ///< One unfilled gauge cell.
    std::string_view gaugeClose; ///< What closes a gauge; see `gaugeOpen`.
};

/// The rungs' glyphs, one row per `RenderRung`, in enumerator order.
///
/// `Sixel` draws its TEXT exactly as `Unicode` does -- a chart is an image beside the figures, not
/// a different spelling of them. **So the cache and node panels are byte-identical on the two rungs,
/// and that is correct rather than a missing image** (#134's decision on Sixel): their figures fit in
/// cells as sparklines, and the one image a panel draws is the fleet chart, 40 machines across a time
/// window, which does not. `Piped` has a row only because the table covers the enum: a piped
/// session draws no frame, and no panel is constructed for it.
inline constexpr EnumTable<RenderRung, RungGlyphs> RungGlyphTable { {
    { .rung = RenderRung::Sixel,
      .sparkLevels = BlockLevels,
      .noReading = " ",
      .horizontal = "─",
      .vertical = "│",
      .topLeft = "┌",
      .topRight = "┐",
      .bottomRight = "┘",
      .bottomLeft = "└",
      .gaugeOpen = "",
      .gaugeFilled = "█",
      .gaugeHeld = "▒",
      .gaugeEmpty = "░",
      .gaugeClose = "" },
    { .rung = RenderRung::Unicode,
      .sparkLevels = BlockLevels,
      .noReading = " ",
      .horizontal = "─",
      .vertical = "│",
      .topLeft = "┌",
      .topRight = "┐",
      .bottomRight = "┘",
      .bottomLeft = "└",
      .gaugeOpen = "",
      .gaugeFilled = "█",
      .gaugeHeld = "▒",
      .gaugeEmpty = "░",
      .gaugeClose = "" },
    { .rung = RenderRung::Ascii,
      .sparkLevels = {},
      .noReading = " ",
      .horizontal = "-",
      .vertical = "|",
      .topLeft = "+",
      .topRight = "+",
      .bottomRight = "+",
      .bottomLeft = "+",
      .gaugeOpen = "[",
      .gaugeFilled = "#",
      .gaugeHeld = "=",
      .gaugeEmpty = ".",
      .gaugeClose = "]" },
    { .rung = RenderRung::Piped,
      .sparkLevels = {},
      .noReading = " ",
      .horizontal = "-",
      .vertical = "|",
      .topLeft = "+",
      .topRight = "+",
      .bottomRight = "+",
      .bottomLeft = "+",
      .gaugeOpen = "[",
      .gaugeFilled = "#",
      .gaugeHeld = "=",
      .gaugeEmpty = ".",
      .gaugeClose = "]" },
} };

static_assert(RowsInEnumeratorOrder(RungGlyphTable, &RungGlyphs::rung),
              "RungGlyphTable must hold one row per RenderRung, in enumerator order");

/// The row for @p rung.
/// @param rung A rung below `Last`.
/// @return Its glyphs.
[[nodiscard]] RungGlyphs const& GlyphsFor(RenderRung rung) noexcept;

/// How many terminal CELLS a piece of UTF-8 text occupies.
///
/// **A value the panels are handed, never a function they own.** A cell is not a byte and not a
/// code point: a CJK hostname is two cells a character, a combining mark is none, and an emoji
/// sequence is two however many code points it spells. The only function in this binary that knows
/// that is the vendored terminal library's, and it is reached through the adapter layer rather than
/// named here, so this is the seam: one pointer, passed into every drawing function below and into
/// `PanelContext`, and the SAME pointer everywhere a frame is laid out -- two width functions would
/// be two opinions about where the right edge is.
///
/// A plain function pointer for the reason `SampleReader` is one: a width has nothing to carry.
using CellWidth = std::size_t (*)(std::string_view text) noexcept;

/// @p text cut or padded on the right to exactly @p width cells.
///
/// Cut only at a code-point boundary, as the longest prefix @p cellWidth measures within @p width,
/// so a character is never split; a wide character that would straddle the edge is left out and
/// the cell it would have half-filled is padded.
/// @param text Valid UTF-8.
/// @param width The cells to fill.
/// @param cellWidth How wide text is.
/// @return The fitted text.
[[nodiscard]] std::string FitRight(std::string_view text, std::size_t width, CellWidth cellWidth);

/// @p text padded on the LEFT to @p width cells, for a figure that aligns on its last digit.
/// Text wider than @p width is returned whole: a figure is never cut, since a truncated number is
/// a different number.
/// @param text Valid UTF-8.
/// @param width The cells to fill.
/// @param cellWidth How wide text is.
/// @return The aligned text.
[[nodiscard]] std::string AlignRight(std::string_view text, std::size_t width, CellWidth cellWidth);

/// One sparkline, one glyph per cell, oldest on the left.
///
/// **Scaled from ZERO to the largest present value, never from the smallest**: a sparkline scaled
/// from its minimum draws a flat series at 1000/s and one at 0/s identically, which is the trend a
/// reader came to see erased. So a present zero is the lowest level, the largest value is the
/// highest, and any other present value is at least one level above zero -- `▁` claims zero and
/// must not be borrowed for "small". A cell with no reading is `noReading`, and so is a value that
/// is not finite, because that is not a reading either.
/// @param cells The values, oldest first; nullopt where there was no reading.
/// @param glyphs The rung's glyphs.
/// @return The glyphs, or empty on a rung that draws no sparkline.
[[nodiscard]] std::string Sparkline(std::span<std::optional<double> const> cells, RungGlyphs const& glyphs);

/// A gauge @p width cells wide, filled in proportion to @p fraction.
/// @param fraction Clamped into [0, 1].
/// @param width How many fill cells, excluding the rung's open and close.
/// @param glyphs The rung's glyphs.
/// @return The gauge.
[[nodiscard]] std::string Gauge(double fraction, std::size_t width, RungGlyphs const& glyphs);

/// A slot gauge, part by part, so each part can be dressed on its own.
struct SlotGauge
{
    std::string open {};      ///< What opens it on this rung.
    std::string running {};   ///< One filled cell per share of the registered slots running.
    std::string held {};      ///< One held cell per share available and not running.
    std::string withdrawn {}; ///< One empty cell per share a limit has taken away.
    std::string close {};     ///< What closes it on this rung.
};

/// A gauge @p width cells wide over @p registered slots: what runs, what is available beside it, and what is withdrawn.
///
/// Three parts because a node offering fewer slots than it registered has two different kinds of not-running
/// capacity, and one of them is a problem: `██████▒▒▒▒▒▒░░░░` is 6 running, 6 more it may take, and 4 a limit took.
/// A compile count above what is available draws no held cells, never a negative run.
/// @param inFlight Compiles running.
/// @param available What the node may hold right now, running ones included.
/// @param registered What it registered with; the whole width.
/// @param width Fill cells, excluding the rung's open and close.
/// @param glyphs The rung's glyphs.
/// @return The parts; every cell withdrawn for a node registered with none.
[[nodiscard]] SlotGauge SlotGaugeOf(
    std::uint32_t inFlight, std::uint32_t available, std::uint32_t registered, std::size_t width, RungGlyphs const& glyphs);

/// How a figure is written: the library's, so a panel and the browser page write one figure alike.
using FastCache::FigureFormat;

/// A written figure's number and unit: the library's, so every surface splits a figure at one place.
using FastCache::WrittenFigure;

/// @p value written as @p format says.
///
/// **Takes no rung, and that is the point** (§9.6): every rung writes a figure through this one
/// function, so no rung can come to write one differently. The writing itself is
/// `FastCache::WriteFigure`, which the leader's page uses too; this adds the absent marker.
/// @param value The value, or nullopt where nothing was reported.
/// @param format How to write it.
/// @param absent The resolved absent marker.
/// @return The number and its unit; @p absent standing as the number, with no unit, when @p value is nullopt or
///         not finite.
[[nodiscard]] WrittenFigure FormatFigure(std::optional<double> value, FigureFormat format, std::string_view absent);

/// How many columns of content a frame @p width columns wide holds: the two edges and the blank
/// column before the right one taken away.
/// @param width The frame's total width; `Frame` treats anything below four as four.
/// @return The content columns.
[[nodiscard]] constexpr std::size_t ContentColumns(std::size_t width) noexcept
{
    return width > 4 ? width - 3 : 1;
}

/// @p lines inside the rung's frame, @p width columns wide in total.
///
/// Each line is fitted to `ContentColumns(width)` and followed by one blank column before the
/// right edge, so a panel lays out its rows and the frame decides nothing but the edge. The title is set into the top edge.
/// Lines are separated by `\n` with no cursor movement: where the picture goes on the screen is the terminal sink's
/// decision.
/// @param title The top edge's title; cut to fit.
/// @param lines The content lines.
/// @param width The frame's total width in cells, edges included; at least four.
/// @param glyphs The rung's glyphs.
/// @param cellWidth How wide text is.
/// @return The frame.
[[nodiscard]] std::string Frame(std::string_view title,
                                std::span<std::string const> lines,
                                std::size_t width,
                                RungGlyphs const& glyphs,
                                CellWidth cellWidth);

/// @p lines inside the rung's frame, with @p title on the left of the top edge and @p trailing right-aligned
/// on it: `┌─ title ───── trailing ─┐`.
///
/// What goes into either half is the caller's to decide; the frame only draws it. When both do not fit
/// with a fill cell between them the trailing half is left off rather than overlapping the title.
/// @param title The top edge's left half; cut to fit.
/// @param trailing The top edge's right half; empty for none.
/// @param lines The content lines.
/// @param width The frame's total width in cells, edges included; at least four.
/// @param glyphs The rung's glyphs.
/// @param cellWidth How wide text is.
/// @return The frame.
[[nodiscard]] std::string Frame(std::string_view title,
                                std::string_view trailing,
                                std::span<std::string const> lines,
                                std::size_t width,
                                RungGlyphs const& glyphs,
                                CellWidth cellWidth);

} // namespace FastCache::Cli
