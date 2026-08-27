// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Distributed/FleetText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <string>
#include <string_view>

using FastCache::IsValidUtf8;
using FastCache::Distributed::AppendJsonText;
using FastCache::Distributed::EscapeMarkup;
using FastCache::Distributed::JsonReplacement;
using FastCache::Distributed::MarkupReplacement;
using namespace std::string_view_literals;

// Every expectation is built from the header's own constants rather than from a
// second spelling of them, and this file stays ASCII throughout: a replacement is
// SIX characters in the output, and a test written with the character instead
// would pass against an encoder that emitted the wrong one of the two.

namespace
{
/// One JSON string literal, quotes included.
[[nodiscard]] std::string Json(std::string_view text)
{
    std::string out;
    AppendJsonText(out, text);
    return out;
}

/// How JSON spells one control byte.
[[nodiscard]] std::string UnicodeEscape(unsigned value)
{
    return std::format("{}u{:04x}", '\\', value);
}
} // namespace

TEST_CASE("The escapes each format requires are still written", "[distributed][fleettext]")
{
    CHECK(EscapeMarkup("a & b < c > d \" e ' f"sv) == "a &amp; b &lt; c &gt; d &quot; e &#39; f");

    // Bound to a name rather than written inside the `CHECK`, and that is
    // portability rather than taste: MSVC's traditional preprocessor mis-tokenizes
    // a raw string containing backslashes when it appears as a macro ARGUMENT --
    // it rescans the `\"` as an escape, ends the literal early, and then reports
    // an "illegal escape sequence" and an invalid literal suffix on the remains.
    // Outside the macro the same literal is read correctly.
    constexpr auto EscapedJson = R"("a\"b\\c\nd\re\tf")"sv;
    CHECK(Json("a\"b\\c\nd\re\tf"sv) == EscapedJson);
}

TEST_CASE("A control byte is written as its JSON escape, not carried", "[distributed][fleettext]")
{
    // Already true before this file learned about UTF-8, and asserted because the
    // byte-below-0x20 branch and the invalid-sequence branch now sit next to each
    // other: reversing them would send every control byte through the replacement.
    CHECK(Json("\x01\x1F"sv) == "\"" + UnicodeEscape(0x01) + UnicodeEscape(0x1F) + "\"");
}

TEST_CASE("Markup replaces the control bytes XML forbids, and keeps the three it allows", "[distributed][fleettext]")
{
    // The byte class below the one this file is about, and it fails the same way:
    // XML 1.0 admits only tab, LF and CR below 0x20 and forbids the rest outright,
    // so a chart carrying a NUL is an SVG a browser refuses to render -- the whole
    // fleet page, over one peer's byte.
    //
    // Checked against the OUTPUT rather than through `IsValidUtf8`, deliberately: a
    // raw NUL is perfectly good UTF-8, so the encoding assertion elsewhere in this
    // file passes on a document no XML parser will take. That gap is why this case
    // exists.
    CHECK(EscapeMarkup("a\x00"
                       "b"sv)
          == "a" + std::string { MarkupReplacement } + "b");
    CHECK(EscapeMarkup("a\x0B\x0C\x1F"
                       "b"sv)
          == "a" + std::string { MarkupReplacement } + std::string { MarkupReplacement } + std::string { MarkupReplacement }
                 + "b");

    // The three XML does allow are carried, because a value containing one is not
    // malformed and rewriting it would be this escaper deciding the value.
    CHECK(EscapeMarkup("a\tb\nc\rd"sv) == "a\tb\nc\rd");

    // JSON is unaffected: every control byte has a legal escape there, so nothing
    // is replaced and the reader gets the byte back.
    CHECK(Json("a\x00"
               "b"sv)
          == "\"a" + UnicodeEscape(0x00) + "b\"");
}

TEST_CASE("Markup replaces the code points XML forbids at the other end too", "[distributed][fleettext]")
{
    // U+FFFE and U+FFFF are perfectly good UTF-8 -- `IsValidUtf8` says so, and it
    // is right -- and XML 1.0's `Char` production stops at U+FFFD, so a chart
    // carrying either is an SVG a browser refuses.
    //
    // This is the case a byte-level check cannot see at all, which is why the
    // rule is a table of CODE POINTS: nothing about these three bytes is wrong
    // until they are decoded. It is also invisible to `IsValidUtf8` on the
    // rendered document, exactly as the NUL case above is.
    CHECK(EscapeMarkup("\xEF\xBF\xBE"sv) == std::string { MarkupReplacement }); // U+FFFE
    CHECK(EscapeMarkup("\xEF\xBF\xBF"sv) == std::string { MarkupReplacement }); // U+FFFF

    // One replacement for the whole sequence, not one per byte: unlike an
    // undecodable byte, there IS a code point here to consume.
    CHECK(EscapeMarkup("a\xEF\xBF\xBF"
                       "b"sv)
          == "a" + std::string { MarkupReplacement } + "b");

    // U+FFFD itself is the last one the production admits, and it is carried --
    // otherwise a replacement this function wrote earlier could not survive being
    // escaped a second time.
    CHECK(EscapeMarkup("\xEF\xBF\xBD"sv) == "\xEF\xBF\xBD");
    // And the boundary below the surrogate hole, which `DecodeUtf8` already
    // refuses, so only the lower side of it can be reached from here.
    CHECK(EscapeMarkup("\xED\x9F\xBF"sv) == "\xED\x9F\xBF"); // U+D7FF

    // JSON carries all of them: its own spec puts no such hole in the character
    // range, so replacing here would be this encoder inventing a restriction.
    CHECK(Json("\xEF\xBF\xBF"sv) == "\"\xEF\xBF\xBF\"");
}

TEST_CASE("Valid UTF-8 is carried through byte for byte", "[distributed][fleettext]")
{
    // The rule is about ENCODING, not about ASCII. A fingerprint an operator pinned
    // by hand may legitimately be any text, and mangling it here would make the page
    // disagree with the value the scheduler matches leases against.
    constexpr auto Text = "gcc \xE2\x86\x92 \xC3\xA9\xF0\x9F\x92\xA9"sv;
    CHECK(EscapeMarkup(Text) == Text);
    CHECK(Json(Text) == "\"" + std::string { Text } + "\"");
}

TEST_CASE("A byte that starts no sequence becomes a replacement, in both formats", "[distributed][fleettext]")
{
    // The defect this closes. `AppendJsonText` escaped the quotes and every control
    // byte and passed everything from 0x80 up through verbatim -- half of what JSON
    // requires of a byte, applied to half of the bytes.
    CHECK(Json("gcc-\x80"sv) == "\"gcc-" + std::string { JsonReplacement } + "\"");
    CHECK(EscapeMarkup("gcc-\x80"sv) == "gcc-" + std::string { MarkupReplacement });

    // One replacement per BYTE, which is what makes the output length predictable:
    // this is a three-byte sequence that stops after two, so it is two of them.
    auto const twiceJson = std::string { JsonReplacement } + std::string { JsonReplacement };
    auto const twiceMarkup = std::string { MarkupReplacement } + std::string { MarkupReplacement };
    CHECK(Json("\xE2\x82"sv) == "\"" + twiceJson + "\"");
    CHECK(EscapeMarkup("\xE2\x82"sv) == twiceMarkup);
}

TEST_CASE("A repaired encoding is still an escaped one", "[distributed][fleettext]")
{
    // The two halves are independent, and a walker that consumed a byte for the
    // encoding check without asking the escape table first would carry a raw `<`
    // straight into a page.
    CHECK(EscapeMarkup("<\x80>"sv) == "&lt;" + std::string { MarkupReplacement } + "&gt;");

    // Bound to a name for the reason the first case records: a raw string carrying
    // backslashes must not be a macro argument.
    auto const quoted = std::string { R"("\")" } + std::string { JsonReplacement } + R"(\"")";
    CHECK(Json("\"\xFF\""sv) == quoted);
}

TEST_CASE("Whatever these are given, what comes out is UTF-8", "[distributed][fleettext]")
{
    // The property, rather than any one repair. RFC 8259 section 8.1 requires UTF-8
    // of JSON exchanged between systems, and an SVG is XML, which a strict parser
    // refuses outright rather than rendering with a gap.
    //
    // Refusing a registration cannot be the whole answer, which is why this is
    // asserted as a property here as well: a value can still arrive through a door
    // this build does not control -- a consensus entry replicated by a peer built
    // before that refusal existed, and applied after it was committed, when there
    // is nobody left to refuse it.
    constexpr std::array Poison {
        "\x80"sv,              // a continuation byte leading nothing
        "\xC0\x80"sv,          // an overlong NUL
        "\xED\xA0\x80"sv,      // a lone surrogate
        "\xF4\x90\x80\x80"sv,  // one past U+10FFFF
        "\xE2\x82"sv,          // truncated
        "gcc-14-a1b2c3\xFF"sv, // ASCII that stops being text at the very end
        "\xFF\xFE\xFD\xFC"sv,  // nothing that leads anything
    };

    for (auto const& text: Poison)
    {
        CHECK(IsValidUtf8(Json(text)));
        CHECK(IsValidUtf8(EscapeMarkup(text)));
    }
}
