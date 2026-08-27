// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Utf8.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using FastCache::IsValidUtf8;
using FastCache::Utf8SequenceLength;
using namespace std::string_view_literals;

// Asserted at COMPILE time throughout, which is what `constexpr` on the decoder
// buys: a wrong answer is a build failure rather than a red run, and the same
// expressions then document that this is usable from a `constexpr` context.
//
// Every byte sequence is written as escapes rather than as source text, so this
// file stays ASCII and says on its face which bytes each case is about. The
// literals are split where an escape is followed by a hex digit -- `\xA9` and `a`
// juxtaposed is ONE escape, not two characters, which is a silent way to write a
// different test from the one intended.

TEST_CASE("Empty and ASCII text are valid UTF-8", "[core][utf8]")
{
    STATIC_REQUIRE(IsValidUtf8(""sv));
    STATIC_REQUIRE(IsValidUtf8("gcc-14-a1b2c3"sv));
    // The whole one-byte range, NUL included: a zero byte is a perfectly good code
    // point, and this decoder is asked about encoding rather than about whether a
    // string is C-shaped.
    STATIC_REQUIRE(IsValidUtf8("\x00\x01\x7F"sv));
    STATIC_REQUIRE(Utf8SequenceLength("A"sv) == 1);
}

TEST_CASE("Every sequence length is accepted at its own length", "[core][utf8]")
{
    STATIC_REQUIRE(Utf8SequenceLength("\xC3\xA9"sv) == 2);         // U+00E9 e-acute
    STATIC_REQUIRE(Utf8SequenceLength("\xE2\x82\xAC"sv) == 3);     // U+20AC euro sign
    STATIC_REQUIRE(Utf8SequenceLength("\xF0\x9F\x92\xA9"sv) == 4); // U+1F4A9

    STATIC_REQUIRE(IsValidUtf8("\xC3\xA9"
                               "clair"sv));
    STATIC_REQUIRE(IsValidUtf8("gcc \xE2\x86\x92 clang"sv));
    STATIC_REQUIRE(IsValidUtf8("\xF0\x9F\x92\xA9\xC3\xA9\xE2\x82\xAC"sv));
}

TEST_CASE("A continuation byte leads nothing", "[core][utf8]")
{
    // The bug this header exists for, in its simplest form: one stray byte from a
    // latin-1 host, with nothing before it to make it a tail.
    STATIC_REQUIRE_FALSE(IsValidUtf8("\x80"sv));
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xBF"sv));
    STATIC_REQUIRE_FALSE(IsValidUtf8("gcc-14-\x80"
                                     "1b2"sv));
    STATIC_REQUIRE(Utf8SequenceLength("\x80"sv) == 0);
}

TEST_CASE("Overlong forms are refused, at every length", "[core][utf8]")
{
    // Every code point has exactly ONE spelling. A second one is how a NUL or a
    // `/` gets past a scanner that only looks for the byte it expects.
    //
    // The two-byte forms are refused by the table's OMISSION of 0xC0-0xC1, and the
    // longer ones by the `lowest` column -- two mechanisms, because a lead byte
    // that can only ever be overlong is a different fact from one whose sequence
    // has to be assembled before anyone can tell.
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xC0\x80"sv));         // two-byte U+0000
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xC1\xBF"sv));         // two-byte U+007F
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xE0\x80\x80"sv));     // three-byte U+0000
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xE0\x9F\xBF"sv));     // three-byte U+07FF
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xF0\x80\x80\x80"sv)); // four-byte U+0000
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xF0\x8F\xBF\xBF"sv)); // four-byte U+FFFF

    // The shortest form of each of those boundaries IS valid -- which is what makes
    // the six above a rejection of the spelling rather than of the value.
    STATIC_REQUIRE(IsValidUtf8("\x00"sv));
    STATIC_REQUIRE(IsValidUtf8("\x7F"sv));
    STATIC_REQUIRE(IsValidUtf8("\xDF\xBF"sv));     // U+07FF
    STATIC_REQUIRE(IsValidUtf8("\xEF\xBF\xBF"sv)); // U+FFFF
}

TEST_CASE("Surrogates are refused, and the code points either side of them are not", "[core][utf8]")
{
    // A surrogate is half of a UTF-16 pair and never stands alone. CESU-8 spells
    // them anyway, so this is the shape a lenient re-encoder produces rather than
    // one a peer types.
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xED\xA0\x80"sv)); // U+D800
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xED\xBF\xBF"sv)); // U+DFFF

    // Both boundaries, because an off-by-one here rejects text that is fine or
    // accepts text that is not, and neither is visible without them.
    STATIC_REQUIRE(IsValidUtf8("\xED\x9F\xBF"sv)); // U+D7FF
    STATIC_REQUIRE(IsValidUtf8("\xEE\x80\x80"sv)); // U+E000
}

TEST_CASE("Nothing above U+10FFFF is accepted, however well-formed its shape", "[core][utf8]")
{
    STATIC_REQUIRE(IsValidUtf8("\xF4\x8F\xBF\xBF"sv));       // U+10FFFF, the last one there is
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xF4\x90\x80\x80"sv)); // U+110000, one past it
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xF5\x80\x80\x80"sv)); // the whole lead range above it

    // Assembles as a five-byte form in the pre-RFC-3629 encoding, and leads
    // nothing at all in this one.
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xF8\x88\x80\x80\x80"sv));
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xFE"sv));
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xFF"sv));
}

TEST_CASE("A truncated sequence is invalid rather than pending", "[core][utf8]")
{
    // Answering "valid so far" is what lets a chunked writer emit a document no
    // decoder accepts: each piece looked fine on its own.
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xC3"sv));
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xE2\x82"sv));
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xF0\x9F\x92"sv));
    STATIC_REQUIRE(Utf8SequenceLength("\xE2\x82"sv) == 0);
}

TEST_CASE("A sequence with a bad continuation byte is invalid", "[core][utf8]")
{
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xE2\x28\xA1"sv)); // '(' where a continuation must be
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xF0\x9F\x92\x28"sv));
    // A lead byte where a continuation must be: the second sequence is fine on its
    // own, which is why the first must not silently consume its first byte.
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xE2\xC3\xA9"sv));
}

TEST_CASE("Validity is a property of the whole text, not of its start", "[core][utf8]")
{
    // The realistic shape of the bug: a fingerprint that is ASCII for twenty bytes
    // and then is not.
    STATIC_REQUIRE_FALSE(IsValidUtf8("gcc-14-a1b2c3\xFF"sv));
    STATIC_REQUIRE_FALSE(IsValidUtf8("\xC3\xA9\x80"sv));
    STATIC_REQUIRE(IsValidUtf8("\xC3\xA9\x21"sv));
}
