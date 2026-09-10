// SPDX-License-Identifier: Apache-2.0
#include "MemcachedClient.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace FastCache::Cli;
using namespace std::string_view_literals;

// Every reply string here was taken from the SERVER that writes it --
// `MemcachedText.cpp` and `MemcachedMeta.cpp` -- rather than from memcached's
// documentation. This client only has to read what this daemon sends, and a parser
// tested against the spec instead of against the implementation is tested on the wrong
// subject.

namespace
{
/// Parse @p bytes and require it completed.
/// @param bytes The reply.
/// @return The parsed reply.
[[nodiscard]] McReply Parsed(std::string_view bytes)
{
    auto const result = ParseMemcachedReply(bytes);
    REQUIRE(result.state == McParseState::Complete);
    REQUIRE(result.consumed == bytes.size());
    return result.reply;
}

/// The encoded form of a command with @p args.
/// @param verb The verb.
/// @param args The arguments.
/// @return The bytes.
[[nodiscard]] std::string Encoded(std::string_view verb, std::vector<std::string> const& args)
{
    return EncodeMemcachedCommand(verb, args);
}
} // namespace

TEST_CASE("memcached: a status line is its own answer", "[cli][memcached][parse]")
{
    // The whole storage and lifetime vocabulary, each one line and nothing else.
    for (auto const* word: { "STORED", "NOT_STORED", "EXISTS", "NOT_FOUND", "DELETED", "TOUCHED", "OK", "RESET" })
    {
        auto const reply = Parsed(std::string { word } + "\r\n");
        CHECK(reply.kind == McLeadToken::Status);
        CHECK(reply.status == word);
        CHECK(reply.values.empty());
        CHECK(reply.stats.empty());
    }
}

TEST_CASE("memcached: END with no values is a miss, not an empty success", "[cli][memcached][parse]")
{
    // `get` on a missing key replies END alone. The caller has to be able to tell that
    // from a value whose payload happens to be empty, which is the case below.
    auto const miss = Parsed("END\r\n"sv);
    CHECK(miss.kind == McLeadToken::Status);
    CHECK(miss.status == "END");
    CHECK(miss.values.empty());

    auto const empty = Parsed("VALUE k 0 0\r\n\r\nEND\r\n"sv);
    CHECK(empty.kind == McLeadToken::Value);
    REQUIRE(empty.values.size() == 1);
    CHECK(empty.values[0].key == "k");
    CHECK(empty.values[0].data.empty());
}

TEST_CASE("memcached: a value block carries its bytes verbatim", "[cli][memcached][parse]")
{
    // A payload holding CRLF is the case a line-based reader gets wrong: the length in
    // the header is what delimits it, never the next CRLF.
    auto const reply = Parsed("VALUE k 7 4\r\na\r\nb\r\nEND\r\n"sv);
    CHECK(reply.kind == McLeadToken::Value);
    REQUIRE(reply.values.size() == 1);
    CHECK(reply.values[0].key == "k");
    CHECK(reply.values[0].flags == 7);
    CHECK(reply.values[0].data == "a\r\nb");
    CHECK_FALSE(reply.values[0].hasCas);
    CHECK(reply.status == "END");
}

TEST_CASE("memcached: gets carries a cas token and get does not", "[cli][memcached][parse]")
{
    // The two headers differ by one field, and `hasCas` is what distinguishes "no cas
    // in this reply" from "a cas token of zero" -- which is a token a server can issue.
    auto const without = Parsed("VALUE k 0 1\r\nx\r\nEND\r\n"sv);
    REQUIRE(without.values.size() == 1);
    CHECK_FALSE(without.values[0].hasCas);
    CHECK(without.values[0].cas == 0);

    auto const with = Parsed("VALUE k 0 1 0\r\nx\r\nEND\r\n"sv);
    REQUIRE(with.values.size() == 1);
    CHECK(with.values[0].hasCas);
    CHECK(with.values[0].cas == 0);

    auto const real = Parsed("VALUE k 0 1 42\r\nx\r\nEND\r\n"sv);
    REQUIRE(real.values.size() == 1);
    CHECK(real.values[0].hasCas);
    CHECK(real.values[0].cas == 42);
}

TEST_CASE("memcached: several values arrive in order", "[cli][memcached][parse]")
{
    auto const reply = Parsed("VALUE a 0 1\r\n1\r\nVALUE b 0 2\r\n22\r\nEND\r\n"sv);
    REQUIRE(reply.values.size() == 2);
    CHECK(reply.values[0].key == "a");
    CHECK(reply.values[0].data == "1");
    CHECK(reply.values[1].key == "b");
    CHECK(reply.values[1].data == "22");
}

TEST_CASE("memcached: STAT lines keep a value that contains spaces", "[cli][memcached][parse]")
{
    // The name is one token; everything after it is the value. A splitter that took
    // the LAST token would lose the rest, and `stats settings` writes such rows.
    auto const reply = Parsed("STAT version 0.2.0\r\nSTAT growth_factor 1.25 approx\r\nEND\r\n"sv);
    CHECK(reply.kind == McLeadToken::Stat);
    REQUIRE(reply.stats.size() == 2);
    CHECK(reply.stats[0].name == "version");
    CHECK(reply.stats[0].value == "0.2.0");
    CHECK(reply.stats[1].name == "growth_factor");
    CHECK(reply.stats[1].value == "1.25 approx");
}

TEST_CASE("memcached: a STAT with no value is kept, not dropped", "[cli][memcached][parse]")
{
    // `stats conns` returns nothing at all here, and a name-only row is what a future
    // sub-command could write. Dropping it would make it invisible.
    auto const reply = Parsed("STAT lonely\r\nEND\r\n"sv);
    REQUIRE(reply.stats.size() == 1);
    CHECK(reply.stats[0].name == "lonely");
    CHECK(reply.stats[0].value.empty());
}

TEST_CASE("memcached: the me inspector's flags are split on the first equals", "[cli][memcached][meta]")
{
    // `MemcachedMeta.cpp` writes exactly this shape:
    //     ME <key> exp=<n> la=<n> cas=<n> fetch=1 cls=1 size=<n>
    auto const reply = Parsed("ME mykey exp=-1 la=3 cas=9 fetch=1 cls=1 size=11\r\n"sv);
    CHECK(reply.kind == McLeadToken::Meta);
    CHECK(reply.metaKey == "mykey");
    REQUIRE(reply.metaFlags.size() == 6);
    CHECK(reply.metaFlags[0].name == "exp");
    CHECK(reply.metaFlags[0].value == "-1");
    CHECK(reply.metaFlags[2].name == "cas");
    CHECK(reply.metaFlags[2].value == "9");
    CHECK(reply.metaFlags[5].name == "size");
    CHECK(reply.metaFlags[5].value == "11");
}

TEST_CASE("memcached: EN is a meta miss and stays a status line", "[cli][memcached][meta]")
{
    auto const reply = Parsed("EN\r\n"sv);
    CHECK(reply.kind == McLeadToken::Status);
    CHECK(reply.status == "EN");
    CHECK(reply.metaKey.empty());
}

TEST_CASE("memcached: all three error words are one kind", "[cli][memcached][parse]")
{
    // A caller acts on the SENTENCE, not on which of the three words carried it -- but
    // it must be able to tell an error from a status, because `ERROR` and `STORED` are
    // both one line and only one of them means the command worked.
    for (auto const* text: { "ERROR", "CLIENT_ERROR bad command line format", "SERVER_ERROR storage failure" })
    {
        auto const reply = Parsed(std::string { text } + "\r\n");
        CHECK(reply.kind == McLeadToken::Error);
        CHECK(reply.status == text);
    }
}

TEST_CASE("memcached: every word this daemon opens a reply with is recognised", "[cli][memcached][parse]")
{
    // The census the codec's `LeadWords` table refers to. It is asserted from OUTSIDE
    // the table, against the words read out of `MemcachedText.cpp` and
    // `MemcachedMeta.cpp` -- a test that walked the table would agree with it whatever
    // it holds, which is the one thing this case must not do.
    struct Case
    {
        std::string_view line;      ///< A reply opening with the word under test.
        std::string_view remainder; ///< What that shape needs after it to be whole.
        McLeadToken kind;           ///< What the word must be read as.
    };

    static constexpr Case cases[] = {
        { .line = "VALUE k 0 0\r\n\r\n"sv, .remainder = "END\r\n"sv, .kind = McLeadToken::Value },
        { .line = "STAT curr_items 1\r\n"sv, .remainder = "END\r\n"sv, .kind = McLeadToken::Stat },
        { .line = "ME k exp=-1\r\n"sv, .remainder = ""sv, .kind = McLeadToken::Meta },
        { .line = "ERROR\r\n"sv, .remainder = ""sv, .kind = McLeadToken::Error },
        { .line = "CLIENT_ERROR authentication required\r\n"sv, .remainder = ""sv, .kind = McLeadToken::Error },
        { .line = "SERVER_ERROR out of memory\r\n"sv, .remainder = ""sv, .kind = McLeadToken::Error },
    };

    for (auto const& one: cases)
    {
        auto const bytes = std::string { one.line } + std::string { one.remainder };
        auto const result = ParseMemcachedReply(bytes);
        INFO(one.line);
        REQUIRE(result.state == McParseState::Complete);
        CHECK(result.reply.kind == one.kind);
    }
}

TEST_CASE("memcached: an error in the middle of a value run ends the reply", "[cli][memcached][parse]")
{
    // A server that gives up midway says so. Returning the sentence beats returning a
    // truncated item list that reads like a complete answer.
    auto const result = ParseMemcachedReply("VALUE a 0 1\r\n1\r\nSERVER_ERROR out of memory\r\n"sv);
    REQUIRE(result.state == McParseState::Complete);
    CHECK(result.reply.kind == McLeadToken::Error);
    CHECK(result.reply.status == "SERVER_ERROR out of memory");
}

TEST_CASE("memcached: a partial reply is incomplete, never malformed", "[cli][memcached][parse]")
{
    // Every prefix of a real reply must ask for more bytes rather than be refused --
    // otherwise a reply split across two reads is a spurious protocol error.
    static constexpr auto whole = "VALUE k 0 5\r\nhello\r\nEND\r\n"sv;
    for (std::size_t cut = 1; cut < whole.size(); ++cut)
    {
        auto const result = ParseMemcachedReply(whole.substr(0, cut));
        INFO("cut at " << cut);
        CHECK(result.state == McParseState::Incomplete);
    }
    CHECK(ParseMemcachedReply(whole).state == McParseState::Complete);
}

TEST_CASE("memcached: a value longer than the cap is refused before it is sized", "[cli][memcached][limits]")
{
    // The header's byte count is peer-controlled. This client dials whatever address an
    // operator typed, so the number is checked against this side's cap BEFORE it is
    // used to size or wait for anything -- the refusal must not require the bytes to
    // arrive first.
    McParseLimits const tight { .maxValueBytes = 4 };
    auto const result = ParseMemcachedReply("VALUE k 0 1000000\r\n"sv, tight);
    CHECK(result.state == McParseState::Malformed);
    CHECK(result.diagnostic.contains("1000000"));
    CHECK(result.diagnostic.contains("cap"));
}

TEST_CASE("memcached: a line that will never terminate is refused, not awaited", "[cli][memcached][limits]")
{
    // Incomplete means "more bytes will help". A line already past the cap with no CRLF
    // is not that: waiting would buffer forever against a peer that sends no newline.
    McParseLimits const tight { .maxLineBytes = 8 };
    auto const result = ParseMemcachedReply("STORED but far too long to be a line"sv, tight);
    CHECK(result.state == McParseState::Malformed);
}

TEST_CASE("memcached: too many items in one reply is refused", "[cli][memcached][limits]")
{
    McParseLimits const tight { .maxItems = 2 };
    auto const result = ParseMemcachedReply("STAT a 1\r\nSTAT b 2\r\nSTAT c 3\r\nEND\r\n"sv, tight);
    CHECK(result.state == McParseState::Malformed);
    CHECK(result.diagnostic.contains("more than 2"));
}

TEST_CASE("memcached: a malformed VALUE header names what is wrong", "[cli][memcached][parse]")
{
    struct Case
    {
        std::string_view bytes;
        std::string_view mentions;
    };

    static constexpr Case cases[] = {
        { .bytes = "VALUE k 0\r\nEND\r\n"sv, .mentions = "tokens"sv },
        { .bytes = "VALUE k x 1\r\nz\r\nEND\r\n"sv, .mentions = "flags"sv },
        { .bytes = "VALUE k 0 x\r\nz\r\nEND\r\n"sv, .mentions = "byte count"sv },
        { .bytes = "VALUE k 0 1 x\r\nz\r\nEND\r\n"sv, .mentions = "cas"sv },
        { .bytes = "VALUE k 0 1\r\nzz\r\nEND\r\n"sv, .mentions = "CRLF"sv },
    };

    for (auto const& one: cases)
    {
        auto const result = ParseMemcachedReply(one.bytes);
        INFO(one.bytes);
        CHECK(result.state == McParseState::Malformed);
        CHECK(result.diagnostic.contains(one.mentions));
    }
}

TEST_CASE("memcached: a byte count that is not exactly an integer is refused", "[cli][memcached][parse]")
{
    // `12x` must not read as 12. The number delimits a payload, so a prefix parse is a
    // silently truncated value rather than a visible error.
    auto const result = ParseMemcachedReply("VALUE k 0 12x\r\nhello\r\n"sv);
    CHECK(result.state == McParseState::Malformed);
}

TEST_CASE("memcached: a mixed sequence is refused rather than half-read", "[cli][memcached][parse]")
{
    auto const result = ParseMemcachedReply("VALUE a 0 1\r\n1\r\nSTAT b 2\r\nEND\r\n"sv);
    CHECK(result.state == McParseState::Malformed);
    CHECK(result.diagnostic.contains("interrupted"));
}

TEST_CASE("memcached: a command line is verb then args then CRLF", "[cli][memcached][encode]")
{
    CHECK(Encoded("get", { "k" }) == "get k\r\n");
    CHECK(Encoded("touch", { "k", "60" }) == "touch k 60\r\n");
    CHECK(Encoded("stats", {}) == "stats\r\n");
    CHECK(Encoded("stats", { "settings" }) == "stats settings\r\n");
}

TEST_CASE("memcached: a storage command frames its payload by length", "[cli][memcached][encode]")
{
    // Header, payload, CRLF -- and the payload's own CRLFs are not escaped, because
    // the byte count is what delimits it. That is the encode side of the parse case
    // above, and the pair is what makes a round trip meaningful.
    CHECK(EncodeMemcachedStorage("set", "k", 0, 60, "hello") == "set k 0 60 5\r\nhello\r\n");
    CHECK(EncodeMemcachedStorage("append", "k", 0, 0, "a\r\nb") == "append k 0 0 4\r\na\r\nb\r\n");
    CHECK(EncodeMemcachedStorage("set", "k", 7, 0, "") == "set k 7 0 0\r\n\r\n");
}

TEST_CASE("memcached: only cas carries the sixth field", "[cli][memcached][encode]")
{
    // Keyed on the VERB, never on the token being non-zero: zero is a cas token a
    // server can legitimately issue, so a non-zero test would drop the field for it and
    // the command would be refused as malformed by the daemon.
    CHECK(EncodeMemcachedStorage("cas", "k", 0, 0, "v", 99) == "cas k 0 0 1 99\r\nv\r\n");
    CHECK(EncodeMemcachedStorage("cas", "k", 0, 0, "v", 0) == "cas k 0 0 1 0\r\nv\r\n");
    CHECK(EncodeMemcachedStorage("set", "k", 0, 0, "v", 99) == "set k 0 0 1\r\nv\r\n");
}

TEST_CASE("memcached: a round trip reads back what was encoded", "[cli][memcached][encode]")
{
    // The pair, rather than either alone: an encoder and a parser that agree with each
    // other and not with the wire is exactly what a same-tree test cannot see, so the
    // literals above are the anchor and this only proves the two are consistent.
    static constexpr auto payload = "line one\r\nline two"sv;
    auto const stored = EncodeMemcachedStorage("set", "k", 3, 0, payload);
    CHECK(stored == "set k 3 0 18\r\nline one\r\nline two\r\n");

    auto const served = "VALUE k 3 18\r\n" + std::string { payload } + "\r\nEND\r\n";
    auto const reply = Parsed(served);
    REQUIRE(reply.values.size() == 1);
    CHECK(reply.values[0].data == payload);
    CHECK(reply.values[0].flags == 3);
}

TEST_CASE("memcached: a token that cannot travel on this wire is refused", "[cli][memcached][encode]")
{
    // No quoting and no escaping: a key with a space would arrive as two tokens and
    // silently address a different key, and one with a CR or LF would end the line
    // early and inject a command. Both are refused before anything is sent.
    CHECK(ValidTextToken("ordinary-key"));
    CHECK(ValidTextToken("with:colons/and/slashes"));
    CHECK_FALSE(ValidTextToken(""));
    CHECK_FALSE(ValidTextToken("two words"));
    CHECK_FALSE(ValidTextToken("tab\there"));
    CHECK_FALSE(ValidTextToken("newline\nhere"));
    CHECK_FALSE(ValidTextToken("carriage\rreturn"));
    CHECK_FALSE(ValidTextToken(std::string_view { "nul\0byte", 8 }));
    CHECK_FALSE(ValidTextToken("delete\x7F"));
}

TEST_CASE("memcached: consumed says where the next reply starts", "[cli][memcached][parse]")
{
    // A pipelined pair: the first parse must report exactly its own length, or the
    // second reply is read from the wrong offset and every later one is garbage.
    static constexpr auto both = "STORED\r\nDELETED\r\n"sv;
    auto const first = ParseMemcachedReply(both);
    REQUIRE(first.state == McParseState::Complete);
    CHECK(first.reply.status == "STORED");
    CHECK(first.consumed == 8);

    auto const second = ParseMemcachedReply(both.substr(first.consumed));
    REQUIRE(second.state == McParseState::Complete);
    CHECK(second.reply.status == "DELETED");
}
