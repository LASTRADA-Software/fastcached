// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Markup.hpp>
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

/// How JSON spells a code point the bytes did not encode.
inline constexpr std::string_view JsonReplacement = "\\ufffd";

/// How a tab-separated document spells it.
///
/// U+FFFD itself, where the other two spell it in an escape syntax: plain text has
/// no such syntax, so the only way to write the replacement character is to write
/// it. That is also why this constant is three bytes of UTF-8 rather than ASCII --
/// the output of this format is text, not a document another parser will re-read.
inline constexpr std::string_view DelimitedReplacement = "\xEF\xBF\xBD";

/// How the fleet dashboard and the node's small pages both open.
///
/// **Two documents, and there is no reading of either on which these bytes may
/// differ.** `RenderFleetHtml` and the node's own one-paragraph pages -- its 401 and
/// its bad-range refusal -- each spelled this prologue out, and the two copies had
/// already drifted apart by a newline before anybody consolidated them
/// ([#1344](https://github.com/LASTRADA-Software/fastcached/issues/1344)). Neither
/// half is cosmetic: a document with no viewport meta renders at desktop zoom on a
/// phone, and the 401 is the most-seen page of a rollout, while a charset one document
/// declares and the other does not is how a peer's display name comes back mojibake on
/// one page and reads correctly on the next.
///
/// **It stops where the pages legitimately diverge, which is why this is a constant
/// and not a helper.** What the dashboard writes next is a conditional
/// `<meta http-equiv="refresh">` that the 401 must not carry, and then each page's own
/// title and stylesheet. A helper spanning that would take a refresh interval, a title
/// and a stylesheet -- three parameters to place one shared line -- so what is shared
/// is exactly the bytes that must never differ and nothing below them.
///
/// **The two `<title>` literals are identical text today and are deliberately not in
/// here**, and the reason is not that a title names its page -- in this tree it does
/// not: `MinimalHtmlPage` hands the same `fastcache fleet` title to the 401 and to the
/// bad-range refusal, so the title already spans three documents on one URL. The reason
/// is that the prologue is a CORRECTNESS property of any document -- how it renders,
/// which encoding it is read in, whether a screen reader knows its language -- while a
/// title is CONTENT. Two documents disagreeing about a title is cosmetic; two
/// disagreeing about a charset is mojibake.
///
/// The newline is the dashboard's, kept so that page's bytes are unchanged: its
/// `<style>` block is multi-line, so its source is readable in a `view-source:`.
/// Nothing pinned it -- the doctype assertions on both surfaces are
/// `starts_with("<!doctype html>")`, which both spellings satisfy -- and the node's
/// small pages gaining one costs them nothing.
///
/// `apps/fastcache-cc/Stats.cpp` opens its report with a third spelling and keeps it
/// for now, and **not because that binary cannot reach this header**: the launcher's
/// own rule is header-only and std-only -- stated at the top of `Stats.cpp` against
/// its own `Core/Ranges.hpp` include -- and this header plus both `Core/` headers it
/// pulls in, `Ranges.hpp` and `Utf8.hpp`, have no `.cpp` between them. One of the two
/// is the very header `Stats.cpp` already compiles in. It is excluded because that page is a
/// DIFFERENT document: it declares no language and no viewport at all, so giving it
/// this prologue changes what it renders. That is a fix to that report, not a fourth
/// site to fold in here.
inline constexpr std::string_view HtmlDocumentPrologue =
    "<!doctype html>\n"
    R"(<html lang="en"><head><meta charset="utf-8">)"
    R"(<meta name="viewport" content="width=device-width, initial-scale=1">)";

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
///
/// `TextEscape` and `EscapeFor` are `Core/Markup.hpp`'s, which is where they went
/// when `Platform/ServiceControl.cpp` became the third consumer of the markup half
/// of this header.
inline constexpr std::array DelimitedEscapes {
    TextEscape { .byte = '\\', .spelling = R"(\\)" },
    TextEscape { .byte = '\t', .spelling = R"(\t)" },
    TextEscape { .byte = '\n', .spelling = R"(\n)" },
    TextEscape { .byte = '\r', .spelling = R"(\r)" },
};

/// How the formats this header owns spell what they cannot carry.
///
/// Namespaced because nothing outside this header reaches this table BY NAME --
/// which is a statement about spelling and **not** a claim that no other file
/// implements the same convention. It is not: `CliFormat.cpp` hand-rolls the JSON
/// convention as a `switch` and has drifted from `JsonEscapes` in both directions
/// (#1356), and `fastcache-cc/Stats.cpp` hand-rolls the markup one. Said the other
/// way round, this comment previously read as *nobody else does this*, which is
/// the sentence a future reader uses to decide not to look.
///
/// `DelimitedEscapes` sits OUTSIDE it because a second binary shares it; the
/// replacement constants stay outside for the older reason, so a test can assert
/// against the one spelling rather than against a second copy of it.
namespace Detail
{

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

} // namespace Detail

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
