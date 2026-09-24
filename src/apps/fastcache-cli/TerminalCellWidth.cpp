// SPDX-License-Identifier: Apache-2.0
#include "TerminalCellWidth.hpp"

#include <algorithm>

#include <core/tui/Unicode.hpp>

namespace FastCache::Cli
{

std::size_t TerminalCellWidth(std::string_view text) noexcept
{
    // Core-cpp's answer is an `int` it never makes negative; the clamp keeps a future one that
    // did from wrapping into a width wider than any terminal.
    return static_cast<std::size_t>(std::max(0, core::tui::stringWidth(text)));
}

} // namespace FastCache::Cli
