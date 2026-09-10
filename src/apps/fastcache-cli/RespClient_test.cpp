// SPDX-License-Identifier: Apache-2.0
#include "RespClient.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;

namespace
{
/// The encoded form of one command, as text.
/// @param argv The command and its arguments.
/// @return The bytes as a string, for comparison against a literal.
[[nodiscard]] std::string Encoded(std::vector<std::string> const& argv)
{
    auto const bytes = EncodeCommand(argv);
    std::string out;
    out.reserve(bytes.size());
    for (auto const byte: bytes)
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    return out;
}
} // namespace

TEST_CASE("a command encodes as an array of bulk strings", "[cli][resp]")
{
    // Always an array. An inline command would begin with a letter, and
    // ProtocolAutodetect classifies a connection from its first byte -- so it would be
    // handed to the memcached text handler and answered ERROR.
    CHECK(Encoded({ "GET", "k" }) == "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    CHECK(Encoded({ "PING" }) == "*1\r\n$4\r\nPING\r\n");
    // The first byte is one RESP is recognised by. Asserted directly, because it is
    // the property the whole encoding choice rests on.
    CHECK(Encoded({ "PING" }).front() == '*');
}

TEST_CASE("an empty argument and a binary one both survive encoding", "[cli][resp]")
{
    CHECK(Encoded({ "SET", "k", "" }) == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n");
    // A value containing CRLF is exactly what a length-prefixed encoding is for; an
    // encoder that relied on the terminator would corrupt it.
    auto const value = std::string { "a\r\nb" };
    CHECK(Encoded({ "SET", "k", value }) == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\na\r\nb\r\n");
}

TEST_CASE("each RESP2 reply type parses to its own kind", "[cli][resp]")
{
    auto const simple = ParseReply("+OK\r\n");
    REQUIRE(simple.state == ParseState::Complete);
    CHECK(simple.value.type == RespType::SimpleString);
    CHECK(simple.value.text == "OK");
    CHECK(simple.consumed == 5);

    auto const error = ParseReply("-NOAUTH Authentication required.\r\n");
    REQUIRE(error.state == ParseState::Complete);
    CHECK(IsError(error.value));
    CHECK(ErrorCodeWord(error.value.text) == "NOAUTH");

    auto const integer = ParseReply(":-3\r\n");
    REQUIRE(integer.state == ParseState::Complete);
    CHECK(integer.value.type == RespType::Integer);
    CHECK(integer.value.integer == -3);

    auto const bulk = ParseReply("$5\r\nhello\r\n");
    REQUIRE(bulk.state == ParseState::Complete);
    CHECK(bulk.value.type == RespType::BulkString);
    CHECK(bulk.value.text == "hello");
    CHECK(bulk.consumed == 11);
}

TEST_CASE("every spelling of nothing is a null", "[cli][resp]")
{
    // RESP2 and RESP3 disagree about which byte means it, and a caller testing for one
    // reads a miss as a value on the other version.
    for (auto const* bytes: { "$-1\r\n", "*-1\r\n", "_\r\n" })
    {
        auto const parsed = ParseReply(bytes);
        REQUIRE(parsed.state == ParseState::Complete);
        CHECK(IsNull(parsed.value));
    }
    // An empty bulk string is NOT a null: it is a value of zero length. Collapsing the
    // two makes `set k ""` indistinguishable from a miss.
    auto const empty = ParseReply("$0\r\n\r\n");
    REQUIRE(empty.state == ParseState::Complete);
    CHECK_FALSE(IsNull(empty.value));
    CHECK(empty.value.type == RespType::BulkString);
}

TEST_CASE("a bulk string carrying CRLF is read by its length", "[cli][resp]")
{
    auto const parsed = ParseReply("$4\r\na\r\nb\r\n");
    REQUIRE(parsed.state == ParseState::Complete);
    CHECK(parsed.value.text == "a\r\nb");
    CHECK(parsed.consumed == 10);
}

TEST_CASE("incomplete is distinct from malformed at every truncation point", "[cli][resp]")
{
    // The distinction the read loop is built on: *needs more bytes* and *will never be
    // a reply* are opposite diagnoses, and collapsing them makes a slow network look
    // like a broken server. Every proper prefix of a valid reply must say Incomplete.
    std::string_view const whole = "*2\r\n$3\r\nabc\r\n:7\r\n";
    for (std::size_t length = 0; length < whole.size(); ++length)
    {
        auto const parsed = ParseReply(whole.substr(0, length));
        CHECK(parsed.state == ParseState::Incomplete);
    }
    auto const complete = ParseReply(whole);
    REQUIRE(complete.state == ParseState::Complete);
    CHECK(complete.consumed == whole.size());
}

TEST_CASE("a malformed reply says so rather than asking for more bytes", "[cli][resp]")
{
    for (auto const* bytes: {
             "!nonsense\r\n",  // no such type marker
             "$abc\r\nxx\r\n", // a length that is not a number
             ":12x\r\n",       // an integer with a trailing character
             "$-2\r\n",        // a negative length that is not the null spelling
             "#maybe\r\n",     // a boolean that is neither t nor f
             ",notanumber\r\n" // a double that is not one
         })
    {
        auto const parsed = ParseReply(bytes);
        CHECK(parsed.state == ParseState::Malformed);
        CHECK_FALSE(parsed.diagnostic.empty());
    }
}

TEST_CASE("a length that is not exactly a number is refused, not truncated", "[cli][resp]")
{
    // `from_chars` alone would read `12x` as 12 and stop. A peer-controlled length
    // silently truncated to something plausible is how a malformed frame becomes a
    // wrong answer, so the parse is strict.
    auto const parsed = ParseReply("$12x\r\nhello\r\n");
    CHECK(parsed.state == ParseState::Malformed);
}

TEST_CASE("a payload that is not CRLF-terminated is malformed", "[cli][resp]")
{
    auto const parsed = ParseReply("$2\r\nabXX");
    CHECK(parsed.state == ParseState::Malformed);
}

TEST_CASE("a declared length above this side's cap is refused", "[cli][resp]")
{
    // A peer-declared length sizes nothing until it has been checked against a bound
    // this side chose. Driven with three bytes rather than gigabytes, which is what
    // injecting the limits buys.
    ParseLimits const tiny { .maxBulkBytes = 4 };
    auto const parsed = ParseReply("$5\r\nhello\r\n", tiny);
    CHECK(parsed.state == ParseState::Malformed);
    CHECK(parsed.diagnostic.contains("cap"));

    // The positive control: at the cap it is accepted, so the refusal is about the
    // bound and not about bulk strings in general.
    ParseLimits const exact { .maxBulkBytes = 5 };
    CHECK(ParseReply("$5\r\nhello\r\n", exact).state == ParseState::Complete);
}

TEST_CASE("a declared element count above the cap is refused before any allocation", "[cli][resp]")
{
    ParseLimits const tiny { .maxElements = 2 };
    // Declares three elements and supplies none. A parser that reserved first would
    // have to be given the bytes to be caught; this one refuses on the header.
    auto const parsed = ParseReply("*3\r\n", tiny);
    CHECK(parsed.state == ParseState::Malformed);

    CHECK(ParseReply("*2\r\n:1\r\n:2\r\n", tiny).state == ParseState::Complete);
}

TEST_CASE("nesting deeper than the cap is refused rather than recursing", "[cli][resp]")
{
    // A recursive-descent parser turns declared nesting into stack frames, so this is
    // the bound that makes a hostile reply a refusal rather than a crash.
    ParseLimits const shallow { .maxDepth = 3 };
    std::string deep;
    for (auto index = 0; index < 10; ++index)
        deep += "*1\r\n";
    deep += ":1\r\n";
    CHECK(ParseReply(deep, shallow).state == ParseState::Malformed);

    CHECK(ParseReply("*1\r\n*1\r\n:1\r\n", shallow).state == ParseState::Complete);
}

TEST_CASE("an array parses its elements and reports the bytes it used", "[cli][resp]")
{
    auto const parsed = ParseReply("*3\r\n$1\r\na\r\n$-1\r\n:5\r\n");
    REQUIRE(parsed.state == ParseState::Complete);
    REQUIRE(parsed.value.items.size() == 3);
    CHECK(parsed.value.items[0].text == "a");
    CHECK(IsNull(parsed.value.items[1]));
    CHECK(parsed.value.items[2].integer == 5);
    CHECK(parsed.consumed == 20);
}

TEST_CASE("a map declares pairs, so it consumes two elements per count", "[cli][resp]")
{
    // `%1` is one key and one value. A parser reading it as one element would leave the
    // value in the buffer to be mistaken for the next reply -- which is a crossed
    // answer, not a parse failure, so nothing would report it.
    auto const parsed = ParseReply("%1\r\n$1\r\nk\r\n$1\r\nv\r\n");
    REQUIRE(parsed.state == ParseState::Complete);
    CHECK(parsed.value.type == RespType::Map);
    REQUIRE(parsed.value.items.size() == 2);
    CHECK(parsed.value.items[0].text == "k");
    CHECK(parsed.value.items[1].text == "v");
    CHECK(parsed.consumed == 18);
}

TEST_CASE("the RESP3 scalar types parse", "[cli][resp]")
{
    auto const boolean = ParseReply("#t\r\n");
    REQUIRE(boolean.state == ParseState::Complete);
    CHECK(boolean.value.type == RespType::Boolean);
    CHECK(boolean.value.boolean);

    auto const real = ParseReply(",3.5\r\n");
    REQUIRE(real.state == ParseState::Complete);
    CHECK(real.value.type == RespType::Double);
    CHECK(real.value.real == 3.5);

    auto const verbatim = ParseReply("=8\r\ntxt:abcd\r\n");
    REQUIRE(verbatim.state == ParseState::Complete);
    CHECK(verbatim.value.type == RespType::Verbatim);
    CHECK(verbatim.value.text == "txt:abcd");
}

TEST_CASE("consumed lets a second reply be read from the same buffer", "[cli][resp]")
{
    // The property the read loop depends on: a reply can arrive in the same segment as
    // its predecessor's tail, and dropping the remainder would make the next command
    // read this one's leftovers.
    std::string_view const both = "+FIRST\r\n+SECOND\r\n";
    auto const first = ParseReply(both);
    REQUIRE(first.state == ParseState::Complete);
    CHECK(first.value.text == "FIRST");

    auto const second = ParseReply(both.substr(first.consumed));
    REQUIRE(second.state == ParseState::Complete);
    CHECK(second.value.text == "SECOND");
}

TEST_CASE("ErrorCodeWord takes the leading token and not the sentence", "[cli][resp]")
{
    CHECK(ErrorCodeWord("WRONGPASS invalid username-password pair") == "WRONGPASS");
    CHECK(ErrorCodeWord("ERR unknown command") == "ERR");
    CHECK(ErrorCodeWord("SINGLE") == "SINGLE");
    CHECK(ErrorCodeWord("").empty());
}

TEST_CASE("every reply type has a name for a diagnostic", "[cli][resp]")
{
    for (auto const& row: RespTypeTable)
    {
        CHECK(RespTypeName(row.type) == row.name);
        CHECK_FALSE(row.name.empty());
    }
}
