// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

namespace FastCache
{

/// The Windows code page identifier for UTF-8.
///
/// Spelled here rather than taken from `<windows.h>`'s `CP_UTF8`, so this header
/// stays self-contained and usable on every platform: the number is a fact about
/// Windows, not a fact only Windows can state.
inline constexpr std::uint32_t Utf8CodePage = 65001;

/// The code page every narrow (`char`) string crossing an OS boundary in this
/// process is transcoded through, or nothing where nothing is transcoded.
///
/// On Windows the OS holds command lines, environment blocks and paths as UTF-16
/// and converts them for a narrow caller, so `argv`, `getenv`, every `...A` API
/// and `std::filesystem::path`'s narrow conversions all pass through the process's
/// **active** code page. That is what this reports.
///
/// Everywhere else there is no such conversion: a byte the caller wrote reaches
/// this process unchanged, so there is no code page to name and this answers
/// `std::nullopt`. That is the honest answer rather than "UTF-8" -- a POSIX host
/// with a legacy locale hands over legacy bytes, and claiming otherwise would put
/// a lie in a diagnostic whose whole job is to explain an encoding.
///
/// @return The active code page on Windows; `std::nullopt` where narrow text is
///         passed through untouched.
[[nodiscard]] std::optional<std::uint32_t> ActiveCodePage() noexcept;

/// Whether narrow text survives this process's OS boundaries as UTF-8.
///
/// True when nothing is transcoded at all, and on Windows when the transcoding is
/// through UTF-8 -- which every executable in this tree asks for by embedding the
/// `activeCodePage` manifest `cmake/Utf8CodePage.cmake` attaches. It is false on a
/// Windows host too old to honour that manifest (before Windows 10 1903), and
/// there the fleet's UTF-8 rules cannot be met by a value an operator types.
///
/// @return True when a UTF-8 argument reaches this process as UTF-8.
[[nodiscard]] bool NarrowTextIsUtf8() noexcept;

} // namespace FastCache
