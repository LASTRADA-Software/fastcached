// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ranges.hpp>
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

/// How a tab-separated document spells it.
///
/// U+FFFD itself, where the other two spell it in an escape syntax: plain text has
/// no such syntax, so the only way to write the replacement character is to write
/// it. That is also why this constant is three bytes of UTF-8 rather than ASCII --
/// the output of this format is text, not a document another parser will re-read.
inline constexpr std::string_view DelimitedReplacement = "\xEF\xBF\xBD";

/// One byte an output format must not carry literally, and what it writes instead.
///
/// Outside `Detail` because a SECOND binary spells the same rows: `fastcache-cli`'s
/// `--format=tsv` writes the same four as `/fleet.txt`, and an operator pipes both
/// into one script. It was a second struct there, with the replacement field named
/// `replacement` rather than `spelling` -- and two names for one concept is the
/// worse half of a duplicate, because a reader who knows one file does not
/// recognise the other (#1334).
struct TextEscape
{
    char byte;                 ///< The byte.
    std::string_view spelling; ///< What the format writes in its place.
};

/// The bytes a tab-separated document cannot carry literally.
///
/// The two delimiters -- and the escape CHARACTER itself, which is the row that
/// is easy to leave out and the one that makes the mapping injective: without it
/// a value holding a literal backslash-t comes back out of any reader that
/// unescapes as a TAB, forging a column boundary it never contained. Arm order
/// is not what carries that, since this is one pass over each byte; the row
/// being PRESENT is.
///
/// Shared with `fastcache-cli`, and the TABLE is the shared part rather than the
/// walk -- `EscapeDelimited` below escapes more than the client's TSV does, on
/// purpose. That divergence is argued at `EscapeTsvField` in
/// `apps/fastcache-cli/CliFormat.cpp`, which is where somebody is tempted to call
/// the node's function instead, and restating it here would be this change's own
/// defect one level up.
inline constexpr std::array DelimitedEscapes {
    TextEscape { .byte = '\\', .spelling = R"(\\)" },
    TextEscape { .byte = '\t', .spelling = R"(\t)" },
    TextEscape { .byte = '\n', .spelling = R"(\n)" },
    TextEscape { .byte = '\r', .spelling = R"(\r)" },
};

/// What one format writes for `byte`, or nothing when it may carry it as it is.
///
/// `FindOrNull` rather than `std::ranges::find_if`, for the reason `Core/Ranges.hpp`
/// exists: over a `std::array` libstdc++ and libc++ yield a raw pointer, so
/// clang-tidy's `readability-qualified-auto` asks for `auto const* const`, which
/// MSVC's class-type iterator cannot deduce. `TraitsOf` in `Platform/ServiceControl.cpp`
/// is the same two lines.
///
/// This site used to hand-roll the scan, and the rule in
/// `.agent/rules/build-and-toolchain.md` permits that -- a site wanting a VALUE out
/// of a range may scan and name no iterator. It takes the helper anyway because the
/// function became cross-binary API in this change, and a shared helper that
/// re-derives what a shared helper already solved is how a tree ends up with the
/// explanation written out seventeen times instead of the answer used once. Where
/// the helper is genuinely unreachable the scan is still right:
/// `CompileCacheWire::FindOp` hand-rolls it because that header must stay
/// dependency-free for the launcher.
/// @param escapes The format's table.
/// @param byte The byte to spell.
/// @return Its spelling, or an empty view when the byte needs none.
[[nodiscard]] constexpr std::string_view EscapeFor(std::span<TextEscape const> escapes, char byte) noexcept
{
    auto const* const row = FindOrNull(escapes, byte, &TextEscape::byte);
    return row != nullptr ? row->spelling : std::string_view {};
}

/// How each format spells what it cannot carry.
///
/// Namespaced because nothing outside this header reaches these two tables BY
/// NAME -- which is a statement about spelling and **not** a claim that no other
/// file implements the same conventions. It is not: `CliFormat.cpp` hand-rolls
/// the JSON convention as a `switch` and has drifted from `JsonEscapes` in both
/// directions (#1356), and `fastcache-cc/Stats.cpp` and
/// `Platform/ServiceControl.cpp` each hand-roll the markup one. Said the other
/// way round, this comment previously read as *nobody else does this*, which is
/// the sentence a future reader uses to decide not to look.
///
/// `TextEscape`, `DelimitedEscapes` and `EscapeFor` sit OUTSIDE it because a
/// second binary shares them; the replacement constants stay outside for the
/// older reason, so a test can assert against the one spelling rather than
/// against a second copy of it.
namespace Detail
{

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

    /// DEL, which is a control character above the printable range rather than below
    /// it, so `FirstPrintableByte` does not cover it.
    inline constexpr unsigned char DeleteByte = 0x7F;

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
        if (auto const escape = EscapeFor(Detail::MarkupEscapes, ch); !escape.empty())
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
        if (auto const escape = EscapeFor(Detail::JsonEscapes, ch); !escape.empty())
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

/// Escape one field of a tab-separated document.
///
/// The third format the fleet renders into, and the only one whose consumer is a
/// TERMINAL rather than a parser. That changes what has to be escaped and why.
///
/// The delimiters first, for the obvious reason: a fingerprint, a version, a display
/// name and a cluster member id are all text a PEER chose, and the only gate they
/// pass is `IsValidUtf8` -- which says nothing about control characters, because a
/// tab is valid UTF-8 and is legal XML `Char` besides. Nothing upstream stops a
/// worker registering with a tab in its display name, and unescaped that shifts every
/// later column of the row while a newline invents a row outright: one peer
/// corrupting the document for every reader, which is the shape of the rule that one
/// bad byte must not make `/fleet.json` unparseable for the whole fleet. Answered
/// here rather than by narrowing the registration gate, because a tab in a display
/// name is legal text and refusing it would be a rendering concern reaching back
/// into what a machine may call itself.
///
/// Then every OTHER control byte, which markup and JSON escape for legality and this
/// escapes for its reader: an ESC a peer chose is not a nuisance in a terminal, it is
/// a sequence the terminal obeys. `\xNN` rather than `\uNNNN`, because these are
/// bytes and the reader unescaping them has `printf` rather than a JSON parser.
///
/// What that does NOT cover, deliberately: the C1 controls U+0080-U+009F are two
/// bytes each and decode as ordinary text here, so they pass through. This tests
/// BYTES where `EscapeMarkup` tests code points, and it can afford to -- in UTF-8 a
/// control sequence is introduced by ESC, which is escaped above. The single-byte C1
/// introducer is an 8-bit-mode spelling, which is not how a UTF-8 document is read.
/// Said out loud because a guard that does not name its edge gets either trusted past
/// it or rewritten into a code-point walk it does not need.
///
/// And invalid UTF-8 is REPLACED, as in both siblings, so this function is total:
/// what it returns is text whatever it was given.
/// @param text Untrusted text.
/// @return The same text, carrying no delimiter and no control byte, and valid UTF-8
///         whatever it was given.
[[nodiscard]] inline std::string EscapeDelimited(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    while (!text.empty())
    {
        auto const ch = text.front();
        auto const byte = static_cast<unsigned char>(ch);
        if (auto const escape = EscapeFor(DelimitedEscapes, ch); !escape.empty())
        {
            out += escape;
            text.remove_prefix(1);
        }
        else if (byte < Detail::FirstPrintableByte || byte == Detail::DeleteByte)
        {
            out += std::format("\\x{:02x}", static_cast<unsigned>(byte));
            text.remove_prefix(1);
        }
        else if (auto const decoded = DecodeUtf8(text); decoded.has_value())
        {
            out += text.substr(0, decoded->length);
            text.remove_prefix(decoded->length);
        }
        else
        {
            // One replacement per BYTE, as in the JSON branch and unlike markup's:
            // nothing decoded, so there is no sequence to consume.
            out += DelimitedReplacement;
            text.remove_prefix(1);
        }
    }
    return out;
}

} // namespace FastCache::Distributed
