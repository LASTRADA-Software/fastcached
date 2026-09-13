// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

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

/// Determine whether standard input AND standard output are both an interactive
/// terminal -- the condition for opening a view that reads keystrokes and redraws
/// in place.
///
/// Both, because either one redirected breaks the view differently: a piped stdin
/// has no keystrokes to read, and a redirected stdout would receive cursor motion
/// meant for a screen. And unlike `StdoutSupportsColor` it ignores `NO_COLOR`, which
/// asks for no colour and says nothing about whether there is a terminal.
///
/// Implementation per platform:
///   - Windows: GetFileType(FILE_TYPE_CHAR) and GetConsoleMode on both handles
///   - POSIX:   isatty on STDIN_FILENO and STDOUT_FILENO
///
/// @return true when both standard streams are an interactive terminal.
[[nodiscard]] bool StandardStreamsAreInteractive() noexcept;

/// What a terminal's environment says about the text encoding it draws.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
///
/// Three values, because *nothing to read* is not *not UTF-8*. A dashboard draws ASCII for both,
/// and only one of them is a finding about the terminal; folding them would have it report a
/// confident "no Unicode" for a terminal nobody asked about.
enum class TerminalTextEncoding : std::uint8_t
{
    Utf8,    ///< The environment names UTF-8.
    Other,   ///< The environment names another encoding, or the `C` / `POSIX` locale.
    Unknown, ///< There was nothing to read.
};

/// The POSIX row: the encoding the locale variables name, by libc's precedence.
///
/// The FIRST non-empty of `LC_ALL`, `LC_CTYPE`, `LANG` decides, and the others are not consulted,
/// so `LC_ALL=C LANG=en_US.UTF-8` is `Other`: that process runs in the C locale whatever `LANG`
/// says, and a scan of all three for any mention of UTF-8 would get exactly that case wrong.
/// A locale name is `language[_territory][.codeset][@modifier]`; its codeset is compared with
/// case and hyphens ignored, so `UTF-8`, `utf8` and `C.UTF-8` all name UTF-8.
/// @param lcAll `LC_ALL`, or no value when unset.
/// @param lcCtype `LC_CTYPE`, or no value when unset.
/// @param lang `LANG`, or no value when unset.
/// @return The encoding, `Unknown` when every variable is unset or empty.
[[nodiscard]] TerminalTextEncoding EncodingFromLocaleVariables(std::optional<std::string_view> lcAll,
                                                               std::optional<std::string_view> lcCtype,
                                                               std::optional<std::string_view> lang) noexcept;

/// The Windows row: the encoding the console's OUTPUT code page names.
///
/// Not `LANG`, which is normally unset on Windows, so a POSIX port would draw every Windows
/// Terminal user in ASCII. And not the process code page this tree declares UTF-8 for every
/// executable: that governs how the process reads its own narrow strings, while the console
/// output page is what the console does with the bytes written to it.
/// @param codePage The console output code page, or no value when there is no console to ask.
/// @return `Utf8` for 65001, `Other` for any other page, `Unknown` without a console.
[[nodiscard]] TerminalTextEncoding EncodingFromConsoleOutputCodePage(std::optional<std::uint32_t> codePage) noexcept;

/// This host's row, read from the environment or the console once.
///
/// The ambient half of the two functions above, and the only one that reads anything: it asks
/// `ReadEnvironmentVariable` on POSIX and the console output code page on Windows, so a caller
/// that must not read ambient state is handed the result as a value.
/// @return The encoding.
[[nodiscard]] TerminalTextEncoding DetectTerminalTextEncoding();

} // namespace FastCache
