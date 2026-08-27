// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache
{

/// The Windows code page identifier for UTF-8.
///
/// Spelled here rather than taken from `<windows.h>`'s `CP_UTF8`, so this header
/// stays self-contained and usable on every platform: the number is a fact about
/// Windows, not a fact only Windows can state. NarrowText.cpp static_asserts that
/// the two agree, where both are in scope.
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

/// How this host reads narrow text that something ELSE wrote.
///
/// Two facts rather than one, because they answer different questions and a host
/// can hold either without the other:
///
/// - `pathsAreUtf8` is about **this** process. It says a `std::filesystem::path`
///   built from narrow bytes decodes them as UTF-8 -- and therefore *refuses*
///   bytes that are not, by throwing. False on POSIX, where bytes are bytes and a
///   legacy filename is perfectly usable, and false on a pre-1903 Windows, where
///   paths are still read through the host's legacy page.
/// - `toolCodePage` is about a **child** process. Its active code page is its own
///   manifest's business rather than ours, and `cl.exe` does not use it for output
///   anyway: it writes the paths in `/showIncludes` in the CONSOLE OUTPUT code
///   page, measured on this tree's own toolchain at `C3 BC` for U+00FC under CP
///   65001 and `81` under CP 850. A build with no console -- MSBuild, an IDE, a
///   detached agent -- has none of that, and there the answer is the SYSTEM
///   default ANSI page, which is what a child carrying no `activeCodePage`
///   manifest gets as its own.
///
/// Passed to whoever needs it rather than read where it is needed, so the code
/// acting on it stays pure and both hosts' behaviour is testable on either.
struct NarrowTextPolicy
{
    /// Whether narrow bytes must be UTF-8 to be usable as a path here.
    bool pathsAreUtf8 { false };

    /// The code page a tool this process ran writes its text in, when that text is
    /// not UTF-8 already; nothing when there is none to read.
    std::optional<std::uint32_t> toolCodePage {};
};

/// This host's policy, read from the platform.
///
/// The one ambient probe this header offers for the pair; everything that decides
/// anything takes the result as a parameter.
/// @return The policy. Default-constructed on POSIX -- bytes are bytes there.
[[nodiscard]] NarrowTextPolicy HostNarrowTextPolicy() noexcept;

/// The UTF-8 form of narrow text this process did not write.
///
/// Two candidate encodings, in this order and no other:
///
///   1. **UTF-8 already.** Self-validating, and what every modern producer emits,
///      so it is asked first and answered without touching the platform at all.
///      `Core/Utf8.hpp` decides it, strictly -- an overlong form or a lone
///      surrogate is not UTF-8 and must fall through rather than be passed on as
///      text (#141).
///   2. **`codePage`.** What the producer used when it was not UTF-8.
///
/// Nothing else is tried, because a third candidate is a guess: CP-850 and CP-1252
/// both decode nearly every byte, so an ordered ladder of legacy pages does not
/// find the right answer, it finds the first one -- and a path decoded wrongly
/// names a file that does not exist, which is worse than a path this process
/// admits it cannot read.
///
/// @param text     The bytes.
/// @param codePage How to read them when they are not UTF-8; `std::nullopt` means
///                 there is no second candidate, so the bytes are UTF-8 or they
///                 are nothing.
/// @return The UTF-8 text, or `std::nullopt` when neither candidate applies.
[[nodiscard]] std::optional<std::string> Utf8FromNarrowText(std::string_view text, std::optional<std::uint32_t> codePage);

/// A `std::filesystem::path` from narrow bytes, without throwing.
///
/// The one place this project turns the platform's refusal into an answer, and it
/// exists because that refusal is easy to miss: on a host reading narrow bytes as
/// UTF-8, `std::filesystem::path`'s narrow constructor **throws** for bytes that
/// are not -- `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` rejects
/// them -- and it throws before any `std::error_code` overload downstream is
/// reached, so a call site that carefully takes an `error_code` for every
/// operation is still not protected.
///
/// It is a guard rather than a decision. Where a path's readability *decides*
/// something -- a cache key, a dependency set -- the caller asks
/// `Utf8FromNarrowText` and says what it does about the answer. Callers here are
/// the ones that already degrade when a path names nothing: a toolchain root that
/// cannot be walked is one this fingerprint does not cover, exactly as an absent
/// one is.
///
/// @param text The bytes.
/// @return The path, or `std::nullopt` when this process cannot read those bytes.
[[nodiscard]] std::optional<std::filesystem::path> PathFromNarrowText(std::string_view text) noexcept;

} // namespace FastCache
