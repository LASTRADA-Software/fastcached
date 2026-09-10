// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <cstddef>
#include <locale>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>

namespace FastCache
{

/// @file NumericText.hpp
/// Parsing numbers out of text, where the obvious spelling is not portable.
///
/// **This exists because `std::from_chars` has no floating-point overload on some
/// libc++ versions** -- macOS before 26.0, which is what `macos-14` runners have --
/// so `std::from_chars(first, last, someDouble)` compiles on libstdc++ and MSVC and
/// fails to compile on macOS. Every integral use of `from_chars` in this tree is
/// fine; only the floating-point one is not.
///
/// The implementation below was written for `RedisResp.cpp`'s `INCRBYFLOAT` and lived
/// in that file's anonymous namespace, with its reasoning in a comment there. That is
/// exactly where a rule reaches nobody: a new file's author has no reason to open the
/// RESP handler, and the natural thing to write is the `from_chars` call that does not
/// build on one platform. It cost `fastcache-cli` a red `macOS-clang-release` on its
/// first push. Moved here rather than copied -- a second implementation citing the
/// first in a comment vouches for its bugs without inheriting its fixes.

/// Parse a finite double from @p text in a locale-independent way, rejecting trailing
/// garbage and non-finite inputs.
///
/// Two steps, and both are load-bearing:
///
/// 1. **Validate the bytes against the numeric grammar** first: an optional sign, then
///    digits carrying at most one `.` anywhere among them, then an optional exponent
///    whose own digits are required. `3.` and `.5` are accepted, as `strtod` accepts
///    them and as the callers this was written for expect. This step is what catches an
///    embedded NUL, whitespace, and a locale-specific decimal separator (`,`) that no
///    wire here speaks -- and it runs BEFORE the stream, so a refusal never depends on
///    what the host's locale happens to be.
/// 2. **Parse with `std::istringstream` pinned to the classic C locale.** Without the
///    `imbue`, a host whose `LC_NUMERIC` does not use `.` -- `de_DE.UTF-8`, say --
///    would reject the very format this project's own `std::format` writes. Portable
///    across POSIX and Windows, unlike `uselocale`.
///
/// Non-finite is refused rather than returned: a caller that wants RESP3's `inf`,
/// `-inf` and `nan` matches those names itself, because they are spellings rather than
/// numbers and nothing should be guessing which of the two it has. That refusal happens
/// in step 1, not in the finiteness guard -- those three words carry letters, so the
/// grammar has already rejected them.
///
/// One `istringstream` construction per call. On no hot path -- `INCRBYFLOAT`, and a
/// reply type the client does not request.
///
/// @param text Text to parse.
/// @param out Receives the parsed value on success; untouched on failure.
/// @return True iff the whole of @p text is a finite double.
[[nodiscard]] inline bool ParseFiniteDouble(std::string_view text, double& out) noexcept
{
    if (text.empty())
        return false;

    // Locale-neutral pre-validation: every byte must come from the numeric grammar.
    bool sawDot = false;
    bool sawExp = false;
    bool sawDigit = false;
    for (auto const index: std::views::iota(std::size_t { 0 }, text.size()))
    {
        auto const ch = text[index];
        if (ch >= '0' && ch <= '9')
            sawDigit = true;
        else if (ch == '.')
        {
            if (sawDot || sawExp)
                return false;
            sawDot = true;
        }
        else if (ch == 'e' || ch == 'E')
        {
            if (sawExp || !sawDigit)
                return false;
            sawExp = true;
            sawDigit = false; // the exponent must have digits of its own
        }
        else if (ch == '+' || ch == '-')
        {
            if (index != 0 && !(sawExp && (text[index - 1] == 'e' || text[index - 1] == 'E')))
                return false;
        }
        else
            return false;
    }
    if (!sawDigit)
        return false;

    try
    {
        std::istringstream stream { std::string { text } };
        stream.imbue(std::locale::classic());
        double value = 0;
        stream >> value;
        // Three clauses, and the third is DEAD TODAY -- said here because a reader who
        // notices that on their own reads it as a defect. `inf`, `-inf` and `nan` carry
        // letters, so the byte grammar above refuses them before any stream runs, and
        // an overflowing literal such as `1e400` sets `failbit`. Measured: deleting
        // `!std::isfinite(value)` leaves all 111 assertions in this header's tests and
        // all 81 in `RedisResp_test.cpp`'s `[incr]` set green. It stays because its
        // reachability is a property of whichever standard library is compiling this,
        // not of this tree -- an implementation that reports an out-of-range double as
        // an infinity WITHOUT setting `failbit` is the case it covers, and the cost of
        // being wrong about that is a wrong number rather than a refusal.
        if (stream.fail() || !stream.eof() || !std::isfinite(value))
            return false;
        out = value;
        return true;
    }
    catch (...)
    {
        // An allocation failure inside the stream is the only way here. Reporting it
        // as "not a number" is the honest answer for a parser whose contract is a
        // `bool`, and it keeps this `noexcept`.
        return false;
    }
}

} // namespace FastCache
