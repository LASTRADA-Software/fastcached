// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
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
    /// introducer or the string terminator. The presenter frames it, through endo, as it spells
    /// every other sequence it writes.
    std::string sixel {};
};

/// One frame: the text a sink always drew, and the images placed over it.
///
/// A sink that cannot draw an image -- a pipe, a test collecting text -- presents `text` alone, and
/// that is not a fallback that loses anything: a view places an image only over cells it left
/// blank, on a rung whose terminal said it can draw one.
struct DashboardFrame
{
    std::string text {};                       ///< The frame's rows, `\n`-separated.
    std::vector<FramePlacement> placements {}; ///< The images over it, possibly none.
};

} // namespace FastCache::Cli
