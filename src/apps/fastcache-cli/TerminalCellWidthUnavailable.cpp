// SPDX-License-Identifier: Apache-2.0
#include "TerminalCellWidth.hpp"

#include <cstdio>
#include <cstdlib>

namespace FastCache::Cli
{

/// @file TerminalCellWidthUnavailable.cpp
/// The cell width for a build without core-cpp's terminal UI (`FASTCACHED_BUILD_TUI=OFF`).
///
/// **Unreachable, and it ends the process if that stops being true** rather than answering: a panel
/// is laid out only for a terminal a session acquired, and `MakeTerminalEvents` refuses to start one
/// in this build. Any number returned here would be a second opinion about a terminal's right edge
/// from a function that knows no Unicode, and a wrong layout that looks right is worse than an abort
/// that names why.

std::size_t TerminalCellWidth(std::string_view /*text*/) noexcept
{
    std::fputs("fastcache-cli: a panel was laid out in a build without the terminal UI (FASTCACHED_BUILD_TUI=OFF)\n",
               stderr);
    std::abort();
}

} // namespace FastCache::Cli
