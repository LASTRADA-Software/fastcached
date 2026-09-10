// SPDX-License-Identifier: Apache-2.0
#include "CliVerbs.hpp"
#include "ScriptedExchange.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;

// The memcached-text verbs, against scripted replies written in the server's own
// vocabulary. Its own file rather than rows in `CliVerbs_test.cpp`: these all share one
// wire, one fake and one auth constraint, and the file that carries the RESP verbs is
// already the longest in the directory.

namespace
{
/// Run a memcached verb against a scripted exchange.
/// @param name The verb.
/// @param operands Its positional arguments.
/// @param exchange What the server will say.
/// @param options The modifiers.
/// @return The answer.
[[nodiscard]] Answer Run(std::string_view name,
                         std::vector<std::string> operands,
                         ScriptedMemcachedExchange& exchange,
                         VerbOptions options = {})
{
    auto const* const verb = FindVerb(name);
    REQUIRE(verb != nullptr);
    return RunVerb(*verb,
                   VerbContext { .operands = operands, .options = options, .memcached = &exchange, .stats = nullptr });
}

/// Whether any advisory contains @p needle.
/// @param answer The answer.
/// @param needle What to look for.
/// @return True when some advisory contains it.
[[nodiscard]] bool Mentions(Answer const& answer, std::string_view needle)
{
    return std::ranges::any_of(answer.advisories,
                               [needle](std::string const& advisory) { return advisory.contains(needle); });
}
} // namespace

TEST_CASE("every memcached verb names a lower-case protocol command", "[cli][verbs][memcached]")
{
    // The wire spells these in lower case and RESP in upper, and the column carries the
    // spelling rather than a handler. A row that got the case wrong would be refused by
    // the server as an unknown command -- visible only against a live daemon, which is
    // exactly the class of defect a table check is for.
    auto seen = std::size_t { 0 };
    for (auto const& verb: Verbs())
    {
        if (verb.wire != Wire::Memcached)
            continue;
        ++seen;
        INFO(verb.name);
        CHECK_FALSE(verb.protocolCommand.empty());
        CHECK(std::ranges::none_of(verb.protocolCommand, [](char ch) { return ch >= 'A' && ch <= 'Z'; }));
    }
    // A positive control: a census of zero rows would pass every check above.
    CHECK(seen >= 11);
}

TEST_CASE("a memcached verb is refused when its OWN wire is absent", "[cli][verbs][memcached]")
{
    auto const* const verb = FindVerb("touch");
    REQUIRE(verb != nullptr);

    // **Every OTHER collaborator is present and only `memcached` is null**, which is
    // what makes this case able to fail. With all three null it passes under any
    // reading of `available` -- including the two-way ternary the column replaced,
    // `verb.wire == Wire::Resp ? resp != nullptr : stats != nullptr`, which consults
    // `stats` for every wire that is not RESP. Bound this way that ternary answers
    // *available* and the handler dereferences a null pointer; bound with everything
    // null it answers *unavailable* for the right reason by accident.
    auto resp = ScriptedExchange { Answers({ Simple("OK") }) };
    auto stats = ScriptedGatherer { {} };
    auto const answer = RunVerb(*verb, VerbContext { .operands = {}, .resp = &resp, .memcached = nullptr, .stats = &stats });
    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK(Mentions(answer, "memcached"));
    // Neither open wire was touched: a refusal must not leak onto another connection.
    CHECK(resp.Sent().empty());
    CHECK(stats.Calls() == 0);
}

TEST_CASE("touch reports TOUCHED and NOT_FOUND differently", "[cli][verbs][memcached]")
{
    auto hit = ScriptedMemcachedExchange { { "TOUCHED\r\n" } };
    auto const touched = Run("touch", { "k", "60" }, hit);
    CHECK(touched.outcome == Outcome::Affirmative);
    CHECK(hit.Sent().front() == "touch k 60\r\n");
    CHECK(hit.Unused() == 0);

    auto miss = ScriptedMemcachedExchange { { "NOT_FOUND\r\n" } };
    auto const absent = Run("touch", { "k", "60" }, miss);
    CHECK(absent.outcome == Outcome::Negative);
    CHECK(Mentions(absent, "no such key"));
}

TEST_CASE("a status word this client has no reading for is a protocol failure", "[cli][verbs][memcached]")
{
    // **There is no default row in `McStatusTable` on purpose.** An unknown word mapped
    // to affirmative by a fallback would report success for whatever a future server
    // says, and nothing here could notice. It names the word so the report is actionable.
    auto exchange = ScriptedMemcachedExchange { { "PONDERED\r\n" } };
    auto const answer = Run("touch", { "k", "60" }, exchange);
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(Mentions(answer, "PONDERED"));
}

TEST_CASE("gat sends the expiry first, as the wire does", "[cli][verbs][memcached]")
{
    // `touch <key> <seconds>` and `gat <seconds> <key>` disagree on this daemon and on
    // memcached. The CLI mirrors the wire rather than tidying it, so a packet capture
    // and `--help` agree; this case is what pins that decision.
    auto exchange = ScriptedMemcachedExchange { { "VALUE a 0 1\r\nx\r\nEND\r\n" } };
    auto const answer = Run("gat", { "60", "a" }, exchange);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(exchange.Sent().front() == "gat 60 a\r\n");
}

TEST_CASE("gat says how many keys came back, because the wire skips misses", "[cli][verbs][memcached]")
{
    // This protocol does not name a miss -- the server simply omits the block -- so a
    // short table reads as a complete answer unless the count is said out loud. `mget`
    // gets this for free over RESP, which answers a null per key.
    auto partial = ScriptedMemcachedExchange { { "VALUE a 0 1\r\n1\r\nEND\r\n" } };
    auto const some = Run("gat", { "60", "a", "b", "c" }, partial);
    CHECK(some.outcome == Outcome::Affirmative);
    CHECK(Mentions(some, "1 of 3 keys exist"));
    CHECK(Mentions(some, "does not name the misses"));

    auto empty = ScriptedMemcachedExchange { { "END\r\n" } };
    auto const none = Run("gat", { "60", "a", "b" }, empty);
    CHECK(none.outcome == Outcome::Negative);
    CHECK(Mentions(none, "none of the keys exist"));
}

TEST_CASE("gats carries a cas column and gat does not", "[cli][verbs][memcached]")
{
    auto without = ScriptedMemcachedExchange { { "VALUE a 0 1\r\nx\r\nEND\r\n" } };
    auto const plain = Run("gat", { "60", "a" }, without);
    REQUIRE(plain.value.shape == Shape::Table);
    CHECK(plain.value.columns == std::vector<std::string> { "key", "value" });

    auto with = ScriptedMemcachedExchange { { "VALUE a 0 1 42\r\nx\r\nEND\r\n" } };
    auto const withCas = Run("gats", { "60", "a" }, with);
    REQUIRE(withCas.value.shape == Shape::Table);
    CHECK(withCas.value.columns == std::vector<std::string> { "key", "value", "cas" });
    REQUIRE(withCas.value.rows.size() == 1);
    REQUIRE(withCas.value.rows[0].size() == 3);
    CHECK(withCas.value.rows[0][2].lexical == "42");
}

TEST_CASE("the storage verbs differ only by the word they send", "[cli][verbs][memcached]")
{
    struct Case
    {
        std::string_view verb;   ///< As typed.
        std::string_view header; ///< The header line it must produce.
    };

    static constexpr Case cases[] = {
        { .verb = "add", .header = "add k 0 0 1\r\nv\r\n" },
        { .verb = "replace", .header = "replace k 0 0 1\r\nv\r\n" },
        { .verb = "append", .header = "append k 0 0 1\r\nv\r\n" },
        { .verb = "prepend", .header = "prepend k 0 0 1\r\nv\r\n" },
    };

    for (auto const& one: cases)
    {
        auto exchange = ScriptedMemcachedExchange { { "STORED\r\n" } };
        auto const answer = Run(one.verb, { "k", "v" }, exchange);
        INFO(one.verb);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(exchange.Sent().front() == one.header);
    }
}

TEST_CASE("NOT_STORED is a negative answer that says which condition failed", "[cli][verbs][memcached]")
{
    auto exchange = ScriptedMemcachedExchange { { "NOT_STORED\r\n" } };
    auto const answer = Run("add", { "k", "v" }, exchange);
    CHECK(answer.outcome == Outcome::Negative);
    CHECK(Mentions(answer, "`add` needs the key absent"));
}

TEST_CASE("cas sends its token and reports EXISTS as a changed value", "[cli][verbs][memcached]")
{
    auto stored = ScriptedMemcachedExchange { { "STORED\r\n" } };
    auto const ok = Run("cas", { "k", "v", "42" }, stored);
    CHECK(ok.outcome == Outcome::Affirmative);
    CHECK(stored.Sent().front() == "cas k 0 0 1 42\r\nv\r\n");

    auto changed = ScriptedMemcachedExchange { { "EXISTS\r\n" } };
    auto const stale = Run("cas", { "k", "v", "42" }, changed);
    CHECK(stale.outcome == Outcome::Negative);
    CHECK(Mentions(stale, "changed since that cas token"));

    auto gone = ScriptedMemcachedExchange { { "NOT_FOUND\r\n" } };
    auto const missing = Run("cas", { "k", "v", "42" }, gone);
    CHECK(missing.outcome == Outcome::Negative);
    CHECK(Mentions(missing, "no such key"));
}

TEST_CASE("a cas token of zero is sent, not dropped", "[cli][verbs][memcached]")
{
    // Zero is a token a server can legitimately issue, so the sixth field is keyed on
    // the VERB and never on the token being non-zero. Dropped, the command becomes a
    // five-field `cas` the daemon refuses as malformed.
    auto exchange = ScriptedMemcachedExchange { { "STORED\r\n" } };
    auto const answer = Run("cas", { "k", "v", "0" }, exchange);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(exchange.Sent().front() == "cas k 0 0 1 0\r\nv\r\n");
}

TEST_CASE("a cas token that is not a whole number is refused before anything is sent", "[cli][verbs][memcached]")
{
    auto exchange = ScriptedMemcachedExchange { {} };
    auto const answer = Run("cas", { "k", "v", "42x" }, exchange);
    CHECK(answer.outcome == Outcome::Usage);
    CHECK(Mentions(answer, "not a cas token"));
    // The point of refusing early: `42x` must not reach the wire as `42`.
    CHECK(exchange.Sent().empty());
}

TEST_CASE("--ttl reaches the storage verbs that honour it", "[cli][verbs][memcached]")
{
    auto exchange = ScriptedMemcachedExchange { { "STORED\r\n" } };
    auto const answer = Run("add", { "k", "v" }, exchange, VerbOptions { .ttlSeconds = 90 });
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(exchange.Sent().front() == "add k 0 90 1\r\nv\r\n");
}

TEST_CASE("append and prepend do not accept --ttl, because the server ignores it", "[cli][verbs][memcached]")
{
    // `CacheEngine::Append` and `Prepend` take no expiry at all, so a `--ttl` accepted
    // here would be silently discarded -- the defect the modifier column exists to
    // prevent. Asserted on the ROW rather than through a parse, because the row is what
    // the command line is checked against.
    for (auto const* name: { "append", "prepend" })
    {
        auto const* const verb = FindVerb(name);
        REQUIRE(verb != nullptr);
        INFO(name);
        CHECK((verb->modifiers & Modifier::Ttl) == 0);
    }
    for (auto const* name: { "add", "replace", "cas" })
    {
        auto const* const verb = FindVerb(name);
        REQUIRE(verb != nullptr);
        INFO(name);
        CHECK((verb->modifiers & Modifier::Ttl) != 0);
    }
}

TEST_CASE("a key that cannot travel on this wire is refused before dialling", "[cli][verbs][memcached]")
{
    // No quoting and no escaping: a key with a space arrives as two tokens and silently
    // addresses a different key, and one with a CR ends the line early and injects
    // whatever follows as a command. Sending it and letting the server complain is not
    // an option for the second.
    for (auto const* key: { "two words", "carriage\rreturn", "" })
    {
        auto exchange = ScriptedMemcachedExchange { {} };
        auto const answer = Run("touch", { key, "60" }, exchange);
        INFO(key);
        CHECK(answer.outcome == Outcome::Usage);
        CHECK(Mentions(answer, "no quoting"));
        CHECK(exchange.Sent().empty());
    }
}

TEST_CASE("inspect renames the me inspector's flags and keeps unknown ones", "[cli][verbs][memcached]")
{
    auto exchange = ScriptedMemcachedExchange { { "ME mykey exp=-1 la=3 cas=9 fetch=1 cls=1 size=11 novel=7\r\n" } };
    auto const answer = Run("inspect", { "mykey" }, exchange);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(exchange.Sent().front() == "me mykey\r\n");
    REQUIRE(answer.value.shape == Shape::Record);

    // The renamed ones a reader came for...
    REQUIRE(FindField(answer.value, "ttl_seconds") != nullptr);
    CHECK(FindField(answer.value, "ttl_seconds")->value.lexical == "-1");
    REQUIRE(FindField(answer.value, "last_access_seconds") != nullptr);
    CHECK(FindField(answer.value, "last_access_seconds")->value.lexical == "3");
    REQUIRE(FindField(answer.value, "value_bytes") != nullptr);
    CHECK(FindField(answer.value, "value_bytes")->value.lexical == "11");
    REQUIRE(FindField(answer.value, "cas") != nullptr);
    CHECK(FindField(answer.value, "cas")->value.lexical == "9");

    // ...and a flag with no row keeps its wire name rather than being dropped, so a
    // server that grows one is visible here the day it does.
    REQUIRE(FindField(answer.value, "novel") != nullptr);
    CHECK(FindField(answer.value, "novel")->value.lexical == "7");

    // The wire spellings are gone, or the rename did not happen.
    CHECK(FindField(answer.value, "exp") == nullptr);
    CHECK(FindField(answer.value, "la") == nullptr);
}

TEST_CASE("inspect reads EN as a miss and anything else as a protocol failure", "[cli][verbs][memcached]")
{
    auto miss = ScriptedMemcachedExchange { { "EN\r\n" } };
    auto const absent = Run("inspect", { "k" }, miss);
    CHECK(absent.outcome == Outcome::Negative);
    CHECK(Mentions(absent, "no such key: k"));

    // Checked as *not a meta reply* AND *not EN*, rather than by assuming everything
    // that is not `ME` is the miss -- which would read an unexpected status as a hit
    // with no flags.
    auto odd = ScriptedMemcachedExchange { { "OK\r\n" } };
    auto const surprising = Run("inspect", { "k" }, odd);
    CHECK(surprising.outcome == Outcome::Protocol);
    CHECK(Mentions(surprising, "OK"));
}

TEST_CASE("mc-stats returns a name/value table and keeps a value with spaces", "[cli][verbs][memcached]")
{
    auto exchange = ScriptedMemcachedExchange { { "STAT curr_items 3\r\nSTAT growth_factor 1.25 approx\r\nEND\r\n" } };
    auto const answer = Run("mc-stats", { "settings" }, exchange);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(exchange.Sent().front() == "stats settings\r\n");
    REQUIRE(answer.value.shape == Shape::Table);
    REQUIRE(answer.value.rows.size() == 2);
    CHECK(answer.value.rows[0][0].lexical == "curr_items");
    CHECK(answer.value.rows[1][1].lexical == "1.25 approx");
}

TEST_CASE("mc-stats with no family asks for the default set", "[cli][verbs][memcached]")
{
    auto exchange = ScriptedMemcachedExchange { { "STAT cmd_get 1\r\nEND\r\n" } };
    auto const answer = Run("mc-stats", {}, exchange);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(exchange.Sent().front() == "stats\r\n");
}

TEST_CASE("an empty stats family renders an empty table, not nothing", "[cli][verbs][memcached]")
{
    // `stats conns` answers a bare `END` on this daemon, which is an empty RESULT
    // rather than a status. Rendered as an empty table so every format says *nothing
    // here* in its own vocabulary rather than printing nothing, which reads as the
    // command having failed.
    auto exchange = ScriptedMemcachedExchange { { "END\r\n" } };
    auto const answer = Run("mc-stats", { "conns" }, exchange);
    CHECK(answer.outcome == Outcome::Negative);
    REQUIRE(answer.value.shape == Shape::Table);
    CHECK(answer.value.rows.empty());
    CHECK(answer.value.columns == std::vector<std::string> { "name", "value" });
    CHECK(Mentions(answer, "no rows for `conns`"));
}

TEST_CASE("mc-stats refuses a family this client does not offer", "[cli][verbs][memcached]")
{
    // `reset` is the one that matters: `MemcachedText.cpp` answers it `RESET` while
    // resetting nothing -- its own comment says it acknowledges the command rather than
    // lying about state -- so relaying it would report a reset that did not happen.
    auto exchange = ScriptedMemcachedExchange { {} };
    auto const answer = Run("mc-stats", { "reset" }, exchange);
    CHECK(answer.outcome == Outcome::Usage);
    CHECK(Mentions(answer, "settings"));
    CHECK(exchange.Sent().empty());
}

TEST_CASE("cache-memlimit says the new limit is not persisted", "[cli][verbs][memcached]")
{
    auto exchange = ScriptedMemcachedExchange { { "OK\r\n" } };
    auto const answer = Run("cache-memlimit", { "512" }, exchange);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(exchange.Sent().front() == "cache_memlimit 512\r\n");
    CHECK(Mentions(answer, "NOT persisted"));
}

TEST_CASE("the auth refusal is explained, not relayed bare", "[cli][verbs][memcached]")
{
    // The server's own sentence does not say that this protocol has no AUTH verb, so an
    // operator reads `authentication required` as *supply a credential* and there is no
    // way to. The explanation is keyed on the WIRE's `authenticable` column, since it is
    // the protocol that lacks the verb rather than the verb or the deployment.
    auto exchange = ScriptedMemcachedExchange { { "CLIENT_ERROR authentication required\r\n" } };
    auto const answer = Run("touch", { "k", "60" }, exchange);
    CHECK(answer.outcome == Outcome::Refused);
    CHECK(Mentions(answer, "CLIENT_ERROR authentication required"));
    CHECK(Mentions(answer, "has no AUTH verb"));
    CHECK(Mentions(answer, "touch"));
}

TEST_CASE("an ordinary refusal gets no auth explanation", "[cli][verbs][memcached]")
{
    // The control for the case above: without it, *explain the auth refusal* and
    // *explain every refusal* are the same passing test.
    auto exchange = ScriptedMemcachedExchange { { "CLIENT_ERROR bad command line format\r\n" } };
    auto const answer = Run("touch", { "k", "60" }, exchange);
    CHECK(answer.outcome == Outcome::Refused);
    CHECK(Mentions(answer, "bad command line format"));
    CHECK_FALSE(Mentions(answer, "has no AUTH verb"));
}

TEST_CASE("a broken connection is unreachable, never refused", "[cli][verbs][memcached]")
{
    // The distinction a script acts on: *nothing answered* exits 3 and *the server
    // declined* exits 4, and they send an operator to different places -- a dead port
    // or a wrong port against a daemon that answered and said no.
    auto broken = ScriptedMemcachedExchange { { McFailure(ExchangeFailure::Transport,
                                                          "the server closed the connection part-way through a reply") } };
    auto const answer = Run("touch", { "k", "60" }, broken);
    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK(Mentions(answer, "part-way through a reply"));

    // The control, so *report a transport break as unreachable* and *report every
    // failure as unreachable* are not the same passing test.
    auto declined = ScriptedMemcachedExchange { { "CLIENT_ERROR bad command line format\r\n" } };
    auto const refused = Run("touch", { "k", "60" }, declined);
    CHECK(refused.outcome == Outcome::Refused);
}

TEST_CASE("bytes this client cannot read are a protocol failure", "[cli][verbs][memcached]")
{
    // Distinct from both of the above, and it has its own exit code: a reply this
    // client cannot parse is neither an unreachable server nor a refusal, and it is
    // fixed by changing this client rather than the deployment.
    auto garbled = ScriptedMemcachedExchange { { McFailure(ExchangeFailure::Malformed,
                                                           "VALUE header's byte count is not an integer") } };
    auto const answer = Run("touch", { "k", "60" }, garbled);
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(Mentions(answer, "byte count"));
}
