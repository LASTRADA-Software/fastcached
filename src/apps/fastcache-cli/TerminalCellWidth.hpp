// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string_view>

namespace FastCache::Cli
{

/// @file TerminalCellWidth.hpp
/// How many cells a terminal gives a piece of text: the one production `CellWidth`.
///
/// **Every panel frame a terminal shows is laid out through this function and no other.** It is
/// bound where a session is composed, as `PanelContext::cellWidth`; the layout code measures nothing
/// itself, and tests bind `Testing::FakeCellWidth` instead. Implemented over core-cpp's
/// `core::tui::stringWidth` in `TerminalCellWidth.cpp`, the only file here that includes
/// `<tui/Unicode.hpp>`: it walks grapheme clusters, so a CJK character is two cells, a combining
/// sequence is its base character's width, and an emoji ZWJ sequence is two.
///
/// **Measures only where core-cpp's terminal library is built** (`FASTCACHED_BUILD_TUI`). Without
/// it there is no terminal to lay a frame out for -- `MakeTerminalEvents` refuses first -- and the
/// twin in `TerminalCellWidthUnavailable.cpp` ends the process naming the build rather than answering
/// through a width function that knows no Unicode.

/// The cells @p text occupies on a terminal.
/// @param text Valid UTF-8.
/// @return The width in cells.
[[nodiscard]] std::size_t TerminalCellWidth(std::string_view text) noexcept;

} // namespace FastCache::Cli
