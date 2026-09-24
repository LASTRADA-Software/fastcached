// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace FastCache::Cli
{

/// @file DashboardFrame.hpp
/// A frame as a view hands it to a sink: its text, and the images placed over it.
///
/// **Its own header, dependency-free,** because it is the contract between three halves that build
/// apart: the panels, which decide WHERE an image goes and what it shows; the loop, which carries a
/// frame from one to the other; and the terminal presenter, which is the only code that spells an
/// escape sequence. None of them may see another's machinery through this.

/// How many pixels one terminal cell measures, as the terminal reported it.
///
/// **Reported, never guessed** (#134 decision on Sixel): a cell size nobody measured is the one
/// input that makes an image the wrong size while every figure beside it is right. So it travels as
/// an `optional` everywhere it travels, and the Sixel rung is not chosen without one.
struct CellPixelSize
{
    std::size_t width { 0 };  ///< Pixels across one cell.
    std::size_t height { 0 }; ///< Pixels down one cell.
};

/// One image a frame places over its text.
///
/// **A rectangle of CELLS, and the image's own bytes.** The view lays the frame out in cells and
/// leaves this rectangle blank in its text; the presenter writes the text rows as it always does and
/// then, for each placement, positions the cursor at `row`, `column` and writes the image -- inside
/// the same synchronized-output bracket as the text, so the terminal shows the two together.
struct FramePlacement
{
    std::size_t row { 0 };       ///< The rectangle's top row, 1-based, counted from the frame's first row.
    std::size_t column { 0 };    ///< The rectangle's left column, 1-based.
    std::size_t cellsWide { 0 }; ///< Cells across; the image is `cellsWide` times the cell width in pixels.
    std::size_t cellsHigh { 0 }; ///< Cells down; the image is `cellsHigh` times the cell height in pixels.

    /// The image, as `ISixelEncoder::Encode` returns it: the Sixel BODY, without the `DCS q`
    /// introducer or the string terminator. The presenter frames it, through core-cpp, as it spells
    /// every other sequence it writes.
    std::string sixel {};
};

/// How a run of a frame's text is dressed where the terminal can dress it.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
///
/// **A word for what the run IS, never an escape sequence**: the view decides that a tab is the
/// selected one or that an age is stale, and the presenter -- the only code that spells a sequence --
/// looks up how that looks in `TonePalette`, the one palette every panel is drawn with. A terminal
/// whose capability record allows no colour gets none of it: the grid is the same text either way.
enum class FrameTone : std::uint8_t
{
    Selected,  ///< The active one of several choices, such as the section a table draws.
    Figure,    ///< A figure an operator reads first.
    Label,     ///< What names or qualifies a figure: labels, units, notes.
    Fresh,     ///< A value inside its freshness threshold.
    Stale,     ///< A value past it: the rest of its row is as old as it is.
    Alert,     ///< A state worth acting on, such as a refusal rate above zero.
    LevelLow,  ///< A level in the lowest band of its scale, such as a bar a third full.
    LevelMid,  ///< A level in the middle band.
    LevelHigh, ///< A level in the top band.
    Last,      ///< Not a tone.
};

/// A run of one row's text the presenter dresses.
///
/// **Bytes of the row as `DashboardFrame::text` carries it**, not cells: the view composed those bytes
/// and knows where a cell's text begins, while counting cells back into bytes would need the width
/// function a second time, in a second place. A span never changes a byte of the text, so a sink that
/// dresses nothing -- a pipe, a test collecting text -- loses nothing but the dress.
struct FrameSpan
{
    std::size_t row { 0 };                  ///< The row, 1-based, counted from the frame's first row.
    std::size_t byte { 0 };                 ///< Where the run starts in that row's text.
    std::size_t length { 0 };               ///< How many bytes it covers.
    FrameTone tone { FrameTone::Selected }; ///< What it is.
};

/// One frame: the text a sink always drew, the images placed over it, and the runs it dresses.
///
/// A sink that cannot draw an image -- a pipe, a test collecting text -- presents `text` alone, and
/// that is not a fallback that loses anything: a view places an image only over cells it left
/// blank, on a rung whose terminal said it can draw one.
struct DashboardFrame
{
    std::string text {};                       ///< The frame's rows, `\n`-separated.
    std::vector<FramePlacement> placements {}; ///< The images over it, possibly none.
    std::vector<FrameSpan> spans {};           ///< The runs of its text a presenter dresses, possibly none.
};

} // namespace FastCache::Cli
