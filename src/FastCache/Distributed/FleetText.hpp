// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Utf8.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Distributed
{

/// How markup spells a code point the bytes did not encode.
///
/// A numeric character reference rather than the three bytes of U+FFFD, so the
/// repair is visible in a `view-source:` and costs the document nothing: the
/// output stays ASCII exactly where the input stopped being text.
inline constexpr std::string_view MarkupReplacement = "&#xFFFD;";

/// How JSON spells the same thing.
inline constexpr std::string_view JsonReplacement = "\\ufffd";

/// How each format spells what it cannot carry, and the walk that applies it.
///
/// Namespaced because the two functions below are the whole of what a caller
/// wants; these are how they agree with each other. The replacement constants
/// stay outside it deliberately, so a test can assert against the one spelling
/// rather than against a second copy of it.
namespace Detail
{

    /// One byte an output format must not carry literally, and what it writes instead.
    struct TextEscape
    {
        char byte;                 ///< The byte.
        std::string_view spelling; ///< What the format writes in its place.
    };

    /// The bytes markup cannot carry literally.
    inline constexpr std::array MarkupEscapes {
        TextEscape { .byte = '&', .spelling = "&amp;" },  TextEscape { .byte = '<', .spelling = "&lt;" },
        TextEscape { .byte = '>', .spelling = "&gt;" },   TextEscape { .byte = '"', .spelling = "&quot;" },
        TextEscape { .byte = '\'', .spelling = "&#39;" },
    };

    /// The bytes JSON spells with a short escape rather than with `\uXXXX`.
    ///
    /// Not the whole of what JSON forbids -- every byte below 0x20 is also illegal
    /// and is written as `\u00xx` below, because listing thirty-two of them here
    /// would be a table nobody reads standing in for one comparison.
    inline constexpr std::array JsonEscapes {
        TextEscape { .byte = '"', .spelling = "\\\"" }, TextEscape { .byte = '\\', .spelling = "\\\\" },
        TextEscape { .byte = '\n', .spelling = "\\n" }, TextEscape { .byte = '\r', .spelling = "\\r" },
        TextEscape { .byte = '\t', .spelling = "\\t" },
    };

    /// The lowest byte a text format may carry literally; everything below is a
    /// control character.
    inline constexpr unsigned char FirstPrintableByte = 0x20;

    /// One inclusive range of code points a format may carry.
    struct CodePointRange
    {
        char32_t first; ///< First code point in the range.
        char32_t last;  ///< Last code point in the range.
    };

    /// XML 1.0 section 2.2's `Char` production, verbatim.
    ///
    /// Markup may carry exactly these and nothing else, and what the production
    /// EXCLUDES is the part that earns it. Below 0x20 it admits only tab, LF and
    /// CR, and forbids the rest OUTRIGHT rather than merely unescaped -- `&#xB;`
    /// is as unparseable as the raw byte, so there is nothing to escape a NUL
    /// *to* and replacing is the only move available. At the other end it stops at
    /// U+FFFD, so U+FFFE and U+FFFF are excluded even though both are perfectly
    /// good UTF-8. That second exclusion is why this is a table of CODE POINTS
    /// rather than of bytes, and why `DecodeUtf8` answers with the value: a check
    /// that could only see bytes would call them valid and emit them.
    ///
    /// The surrogate hole the production also carves is absent here because it
    /// cannot arise -- `DecodeUtf8` refuses a surrogate as invalid UTF-8 before
    /// this is ever asked.
    ///
    /// The production rather than HTML's laxer rules because this escape feeds
    /// BOTH surfaces, and one escaper must meet the stricter of its consumers. The
    /// page is `text/html`, where a browser repairs most of this quietly; a chart
    /// is `image/svg+xml`, which is XML, where a parser refuses the whole document
    /// rather than drawing it with a gap. Only chart labels this build writes
    /// itself reach the SVG path today -- which is exactly the kind of thing a
    /// later column labelled by toolchain would change, silently, with no reason
    /// for anyone to revisit this function.
    ///
    /// JSON shares none of it -- every code point has a legal spelling there --
    /// which is why `AppendJsonText` escapes where this replaces.
    inline constexpr std::array MarkupCarriable {
        CodePointRange { .first = 0x0009, .last = 0x0009 }, CodePointRange { .first = 0x000A, .last = 0x000A },
        CodePointRange { .first = 0x000D, .last = 0x000D }, CodePointRange { .first = 0x0020, .last = 0xD7FF },
        CodePointRange { .first = 0xE000, .last = 0xFFFD }, CodePointRange { .first = 0x10000, .last = 0x10FFFF },
    };

    /// Whether markup may carry `value` as itself.
    /// @param value A decoded code point.
    /// @return True when the `Char` production admits it.
    [[nodiscard]] constexpr bool MarkupMayCarry(char32_t value) noexcept
    {
        // `any_of` rather than a scan, unlike `EscapeFor` below: this one answers a
        // boolean and so has a spelling clang-tidy's `readability-use-anyofallof`
        // accepts, while that one returns a value out of the range and does not.
        return std::ranges::any_of(
            MarkupCarriable, [value](CodePointRange const& range) { return value >= range.first && value <= range.last; });
    }

    /// What one format writes for `byte`, or nothing when it may carry it as it is.
    ///
    /// A range-based scan rather than `std::ranges::find_if`, and the reason is
    /// portability rather than taste: over a `std::array`, libc++ and libstdc++ yield
    /// a raw pointer -- so clang-tidy's `readability-qualified-auto` requires
    /// `auto const* const` -- while MSVC yields a class-type iterator that such a
    /// declaration cannot deduce. `CompileCacheWire::FindOp` and `SchedulerService`'s
    /// refusal table already scan this way.
    /// @param escapes The format's table.
    /// @param byte The byte to spell.
    /// @return Its spelling, or an empty view when the byte needs none.
    [[nodiscard]] constexpr std::string_view EscapeFor(std::span<TextEscape const> escapes, char byte) noexcept
    {
        for (auto const& row: escapes)
            if (row.byte == byte)
                return row.spelling;
        return {};
    }

} // namespace Detail

/// Escape text for HTML or SVG.
///
/// Every value the fleet surfaces came off a wire: a toolchain fingerprint and an
/// endpoint are whatever a peer sent, and the page and the charts both interpolate
/// them. Shared between the two renderers rather than copied, because two copies
/// are two places for one of them to be forgotten.
///
/// `apps/fastcache-cc/Stats.cpp` keeps its own sibling of this deliberately: that
/// binary does not link this library at all, which is a documented constraint
/// rather than an oversight.
/// @param text Untrusted text.
/// @return The same text, safe to interpolate into markup, and valid UTF-8
///         whatever it was given.
[[nodiscard]] inline std::string EscapeMarkup(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    while (!text.empty())
    {
        auto const ch = text.front();
        if (auto const escape = Detail::EscapeFor(Detail::MarkupEscapes, ch); !escape.empty())
        {
            out += escape;
            text.remove_prefix(1);
        }
        else if (auto const decoded = DecodeUtf8(text); decoded.has_value() && Detail::MarkupMayCarry(decoded->value))
        {
            out += text.substr(0, decoded->length);
            text.remove_prefix(decoded->length);
        }
        else
        {
            // Replaced rather than escaped, which is the whole difference from the
            // JSON branch: nothing here has a spelling markup accepts, so the choice
            // is a replacement or a document nothing will parse.
            //
            // A code point the production excludes is consumed WHOLE -- one
            // replacement for the sequence, not one per byte. Only bytes that
            // decoded to nothing advance singly, because there is no sequence to
            // consume.
            out += MarkupReplacement;
            text.remove_prefix(decoded.has_value() ? decoded->length : 1);
        }
    }
    return out;
}

/// Append one JSON string literal, quotes included.
/// @param out Where to append.
/// @param text The string's contents; need not be valid UTF-8, and what is
///         appended always is.
inline void AppendJsonText(std::string& out, std::string_view text)
{
    out += '"';
    while (!text.empty())
    {
        auto const ch = text.front();
        if (auto const escape = Detail::EscapeFor(Detail::JsonEscapes, ch); !escape.empty())
        {
            out += escape;
            text.remove_prefix(1);
        }
        else if (static_cast<unsigned char>(ch) < Detail::FirstPrintableByte)
        {
            out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
            text.remove_prefix(1);
        }
        else if (auto const decoded = DecodeUtf8(text); decoded.has_value())
        {
            out += text.substr(0, decoded->length);
            text.remove_prefix(decoded->length);
        }
        else
        {
            // The other half of what JSON requires of a byte, and the half this
            // function used not to do: RFC 8259 section 8.1 requires UTF-8 of JSON
            // exchanged between systems, so a byte belonging to no valid sequence is
            // as illegal in a string literal as an unescaped quote is.
            //
            // One replacement per BYTE here, unlike markup's: there is no sequence
            // to consume, because nothing decoded.
            out += JsonReplacement;
            text.remove_prefix(1);
        }
    }
    out += '"';
}

} // namespace FastCache::Distributed
