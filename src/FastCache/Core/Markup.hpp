// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Core/Utf8.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// @file Markup.hpp
/// The one answer to *what may an XML-family document carry, and what does it write
/// instead* -- shared by everything in this tree that interpolates untrusted text
/// into markup.
///
/// **Why this is in `Core/` rather than beside its first consumer.** It began in
/// `Distributed/FleetText.hpp`, which was right while the fleet page and the fleet
/// charts were the only consumers. The third consumer is
/// `Platform/ServiceControl.cpp`, which interpolates a path, a launch argument, a
/// label and an account name into a launchd plist -- and `Platform` must not depend
/// on `Distributed`, which sits above it. The content is a pure function of
/// `Core/Utf8.hpp` with no fleet in it, so `Core` is where it belongs and where the
/// comment on `TextEscape` below already anticipated it going.
///
/// **Where its tests are, which is not here.** `Distributed/FleetText_test.cpp`.
/// That is deliberate rather than an oversight left behind by the move: those cases
/// assert markup, JSON and the tab-separated format *as a family* -- "Whatever these
/// are given, what comes out is UTF-8" is one case over all three -- and the family
/// property is what a reader needs to see, so splitting the cases to follow the
/// declarations would cost more than the convention of a `_test.cpp` per header buys.

/// How markup spells a code point the bytes did not encode.
///
/// A numeric character reference rather than the three bytes of U+FFFD, so the
/// repair is visible in a `view-source:` and costs the document nothing: the
/// output stays ASCII exactly where the input stopped being text.
inline constexpr std::string_view MarkupReplacement = "&#xFFFD;";

/// One byte an output format must not carry literally, and what it writes instead.
///
/// Here rather than in a `Detail` namespace because SEVERAL translation units spell
/// the same rows: `fastcache-cli`'s `--format=tsv` writes the same four as
/// `/fleet.txt`, and an operator pipes both into one script. It was a second struct
/// there, with the replacement field named `replacement` rather than `spelling` --
/// and two names for one concept is the worse half of a duplicate, because a reader
/// who knows one file does not recognise the other (#1334).
struct TextEscape
{
    char byte;                 ///< The byte.
    std::string_view spelling; ///< What the format writes in its place.
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
/// function is cross-binary API, and a shared helper that re-derives what a shared
/// helper already solved is how a tree ends up with the explanation written out
/// seventeen times instead of the answer used once. Where the helper is genuinely
/// unreachable the scan is still right: `CompileCacheWire::FindOp` hand-rolls it
/// because that header must stay dependency-free for the launcher.
/// @param escapes The format's table.
/// @param byte The byte to spell.
/// @return Its spelling, or an empty view when the byte needs none.
[[nodiscard]] constexpr std::string_view EscapeFor(std::span<TextEscape const> escapes, char byte) noexcept
{
    auto const* const row = FindOrNull(escapes, byte, &TextEscape::byte);
    return row != nullptr ? row->spelling : std::string_view {};
}

namespace Detail
{

    /// The bytes markup cannot carry literally.
    ///
    /// **The apostrophe row retires `&apos;`, and that is a decision rather than a
    /// merge artefact.** Three sites implemented this convention before they were
    /// converged (#1359) and two of them already spelled `&#39;`; the third,
    /// `Platform/ServiceControl.cpp`'s launchd plist writer, spelled `&apos;`. Both
    /// are valid character references for U+0027 and every conforming XML and HTML
    /// parser reads either, so one table with no per-consumer column was chosen over
    /// two tables or a column nothing would ever branch on.
    ///
    /// Stated here because of WHERE the changed bytes land: a service registration
    /// replays its command line forever, so a byte change there is not the same kind
    /// of change as one in a page that is re-rendered per request. What makes it safe
    /// is the narrow fact that nothing reads the spelling back -- no test asserted
    /// either form, and launchd parses the plist rather than comparing it. It is NOT
    /// safe on the general ground that "both are valid", which would equally license
    /// changing a spelling something does depend on.
    /// The NAME has a reader outside this header even though no code spells it:
    /// `scripts/check-markup-entities.cmake` derives this rule's home by searching
    /// for `MarkupEscapes`, so renaming it reddens that check rather than silently
    /// leaving the census hunting a table that no longer exists. That is the intended
    /// fail-closed direction, and it is why `Detail` here does not mean *nothing
    /// depends on this name*.
    inline constexpr std::array MarkupEscapes {
        TextEscape { .byte = '&', .spelling = "&amp;" },  TextEscape { .byte = '<', .spelling = "&lt;" },
        TextEscape { .byte = '>', .spelling = "&gt;" },   TextEscape { .byte = '"', .spelling = "&quot;" },
        TextEscape { .byte = '\'', .spelling = "&#39;" },
    };

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
    /// EVERY such surface, and one escaper must meet the stricter of its consumers.
    /// The fleet page is `text/html`, where a browser repairs most of this quietly;
    /// a chart is `image/svg+xml`, which is XML, where a parser refuses the whole
    /// document rather than drawing it with a gap; and a launchd plist is XML that
    /// `launchd` refuses outright. Only chart labels this build writes itself reach
    /// the SVG path today -- which is exactly the kind of thing a later column
    /// labelled by toolchain would change, silently, with no reason for anyone to
    /// revisit this function.
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
        // `any_of` rather than a scan, unlike `EscapeFor` above: this one answers a
        // boolean and so has a spelling clang-tidy's `readability-use-anyofallof`
        // accepts, while that one returns a value out of the range and does not.
        return std::ranges::any_of(
            MarkupCarriable, [value](CodePointRange const& range) { return value >= range.first && value <= range.last; });
    }

} // namespace Detail

/// Escape text for an XML-family document -- HTML, SVG or a property list.
///
/// Every value the fleet surfaces came off a wire: a toolchain fingerprint and an
/// endpoint are whatever a peer sent, and the page and the charts both interpolate
/// them. A launchd plist interpolates a path, a launch argument and an account name.
/// Shared between all of them rather than copied, because N copies are N places for
/// one of them to be forgotten.
///
/// `apps/fastcache-cc/Stats.cpp` used to keep its own sibling of this, on the stated
/// ground that the launcher does not link this library. That ground was real and it
/// does not reach a header like this one: the launcher's rule is header-only AND
/// std-only, `Markup.hpp` pulls in only `Core/Ranges.hpp` and `Core/Utf8.hpp`, and
/// there is no `.cpp` between the three -- so the launcher spends no `_fc_cc_core`
/// row and no link to call this. It compiles one of those two headers already.
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

/// Where @p text stops being expressible in an XML-family document, if it does.
///
/// The question `EscapeMarkup` answers by REPLACING, asked instead of answered. Both
/// callers exist for a reason and the difference is what the text is FOR:
///
/// - A fleet page label is descriptive, so a replaced code point costs a glyph and
///   the page is still true. `EscapeMarkup` is right there.
/// - A plist field is OPERATIVE -- `ProgramArguments[0]` is a path launchd must open,
///   `Label` is the handle every `launchctl` subcommand takes. Replacing a byte there
///   yields a document that parses and names something that does not exist, which is
///   a registration that fails on every boot with nothing to diagnose it by. That one
///   has to refuse while the operator is still watching, so it asks first.
///
/// A byte OFFSET rather than a bool, so a refusal can say where to look without
/// quoting bytes that are not text back at a terminal.
/// @param text The text to check.
/// @return The offset of the first code point markup cannot carry -- or of the first
///         byte belonging to no valid UTF-8 sequence -- or nullopt when the whole of
///         @p text is expressible.
[[nodiscard]] constexpr std::optional<std::size_t> FirstUncarriableByte(std::string_view text) noexcept
{
    auto const size = text.size();
    while (!text.empty())
    {
        auto const decoded = DecodeUtf8(text);
        if (!decoded.has_value() || !Detail::MarkupMayCarry(decoded->value))
            return size - text.size();
        text.remove_prefix(decoded->length);
    }
    return std::nullopt;
}

} // namespace FastCache
