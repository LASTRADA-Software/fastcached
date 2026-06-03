// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace FastCache
{

/// Determine whether the process's standard output is an interactive terminal
/// that can render ANSI color, and, on Windows, enable virtual-terminal
/// processing so SGR escape sequences are interpreted rather than printed
/// literally.
///
/// Implementation per platform:
///   - Windows: GetStdHandle + GetFileType (rejects pipes/files) and
///              GetConsoleMode/SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
///   - POSIX:   isatty(STDOUT_FILENO)
///
/// The conventional `NO_COLOR` environment variable is honored: when it is set
/// to a non-empty value, color is suppressed regardless of TTY state.
///
/// @return true if colored output should be emitted to stdout.
[[nodiscard]] bool StdoutSupportsColor() noexcept;

} // namespace FastCache
