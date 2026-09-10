// SPDX-License-Identifier: Apache-2.0
#include "CliVerbs.hpp"
#include "ScriptedExchange.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;

namespace
{
/// Run a verb against a scripted exchange.
/// @param name The verb.
/// @param operands Its positional arguments.
/// @param exchange What the server will say.
/// @param options The modifiers.
/// @return The answer.
[[nodiscard]] Answer Run(std::string_view name,
                         std::vector<std::string> operands,
                         ScriptedExchange& exchange,
                         VerbOptions options = {})
{
    auto const* const verb = FindVerb(name);
    REQUIRE(verb != nullptr);
    return RunVerb(*verb, VerbContext { .operands = operands, .options = options, .resp = &exchange, .stats = nullptr });
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

TEST_CASE("the verb table is internally consistent", "[cli][verbs]")
{
    CHECK_FALSE(Verbs().empty());
    for (auto const& verb: Verbs())
    {
        CHECK_FALSE(verb.name.empty());
        CHECK_FALSE(verb.summary.empty());
        CHECK(verb.handler != nullptr);
        // A verb reachable by name is the only kind the parser can dispatch.
        CHECK(FindVerb(verb.name) == &verb);
        // Bounds that cannot be satisfied would make a verb permanently unusable.
        CHECK((verb.maxOperands == VariadicOperands || verb.minOperands <= verb.maxOperands));
        // A RESP verb needs something to send; the stats verb deliberately does not.
        if (verb.wire == Wire::Resp)
            CHECK_FALSE(verb.protocolCommand.empty());
    }
    CHECK(FindVerb("no-such-verb") == nullptr);
}

TEST_CASE("no two verbs claim the same name", "[cli][verbs]")
{
    for (auto const& verb: Verbs())
    {
        auto const matches =
            std::ranges::count_if(Verbs(), [&verb](VerbSpec const& other) { return other.name == verb.name; });
        CHECK(matches == 1);
    }
}

TEST_CASE("operand bounds are enforced and described from the row", "[cli][verbs]")
{
    auto const* const get = FindVerb("get");
    REQUIRE(get != nullptr);
    CHECK_FALSE(OperandCountAccepted(*get, 0));
    CHECK(OperandCountAccepted(*get, 1));
    CHECK_FALSE(OperandCountAccepted(*get, 2));
    CHECK(DescribeOperandArity(*get) == "exactly 1 operand");

    auto const* const mget = FindVerb("mget");
    REQUIRE(mget != nullptr);
    CHECK_FALSE(OperandCountAccepted(*mget, 0));
    CHECK(OperandCountAccepted(*mget, 1));
    CHECK(OperandCountAccepted(*mget, 50));
    CHECK(DescribeOperandArity(*mget) == "at least 1 operand");

    auto const* const ping = FindVerb("ping");
    REQUIRE(ping != nullptr);
    CHECK(OperandCountAccepted(*ping, 0));
    CHECK_FALSE(OperandCountAccepted(*ping, 1));
    CHECK(DescribeOperandArity(*ping) == "exactly 0 operands");
}

TEST_CASE("a verb whose wire was not opened is refused rather than crashing", "[cli][verbs]")
{
    auto const* const get = FindVerb("get");
    REQUIRE(get != nullptr);
    auto const operands = std::vector<std::string> { "k" };

    auto const answer = RunVerb(*get, VerbContext { .operands = operands, .resp = nullptr, .stats = nullptr });
    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK(Mentions(answer, "no connection to the cache"));
}

TEST_CASE("get sends GET and reports the value", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Bulk("hello") }) };
    auto const answer = Run("get", { "k" }, exchange);

    // The request is asserted, not just the answer: a handler that sent the wrong
    // command would still produce a plausible-looking result.
    REQUIRE(exchange.Sent().size() == 1);
    CHECK(exchange.Sent()[0] == std::vector<std::string> { "GET", "k" });

    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(answer.value.shape == Shape::Scalar);
    CHECK(answer.value.scalar.lexical == "hello");
    CHECK_FALSE(answer.rawPayload.has_value());
}

TEST_CASE("a get miss is a negative answer and not a failure", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Nil() }) };
    auto const answer = Run("get", { "k" }, exchange);

    // The discrimination the exit-code table exists for: a miss and a dead daemon must
    // not share a code. Here it is Negative, and the value is absent rather than an
    // empty string -- an empty string is a value somebody could have stored.
    CHECK(answer.outcome == Outcome::Negative);
    REQUIRE(answer.value.shape == Shape::Scalar);
    CHECK(answer.value.scalar.kind == CellKind::Absent);
    CHECK(Mentions(answer, "no such key"));
}

TEST_CASE("get of a stored empty string is affirmative, not a miss", "[cli][verbs]")
{
    // The positive control for the case above: a handler that treated an empty payload
    // as a miss would pass that test and fail this one.
    ScriptedExchange exchange { Answers({ Bulk("") }) };
    auto const answer = Run("get", { "k" }, exchange);

    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(answer.value.scalar.kind == CellKind::Text);
    CHECK(answer.value.scalar.lexical.empty());
}

TEST_CASE("a value that is not UTF-8 is shown base64 and said to be", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Bulk(std::string { 'a', '\x80', 'b' }) }) };
    auto const answer = Run("get", { "k" }, exchange);

    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(answer.value.scalar.kind == CellKind::Binary);
    CHECK(answer.value.scalar.lexical == "YYBi");
    // Told, rather than left to guess. A silent repair is the failure that is quiet.
    CHECK(Mentions(answer, "base64"));
}

TEST_CASE("get --raw hands over the bytes and renders nothing", "[cli][verbs]")
{
    auto const bytes = std::string { 'a', '\x80', 'b' };
    ScriptedExchange exchange { Answers({ Bulk(bytes) }) };
    auto const answer = Run("get", { "k" }, exchange, VerbOptions { .raw = true });

    REQUIRE(answer.rawPayload.has_value());
    // Untouched: the one escape from the text/base64 classification.
    CHECK(FastCache::Testing::Unwrap(answer.rawPayload) == bytes);
    CHECK(answer.value.shape == Shape::Empty);
}

TEST_CASE("mget pairs every key with its value and marks the misses absent", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Array({ Bulk("1"), Nil(), Bulk("3") }) }) };
    auto const answer = Run("mget", { "a", "b", "c" }, exchange);

    REQUIRE(exchange.Sent().size() == 1);
    CHECK(exchange.Sent()[0] == std::vector<std::string> { "MGET", "a", "b", "c" });

    REQUIRE(answer.value.shape == Shape::Table);
    CHECK(answer.value.columns == std::vector<std::string> { "key", "value" });
    REQUIRE(answer.value.rows.size() == 3);
    CHECK(answer.value.rows[0][0].lexical == "a");
    CHECK(answer.value.rows[0][1].lexical == "1");
    // The miss is absent, so JSON renders null and the human format a dash -- never a
    // zero or an empty string, either of which is a value.
    CHECK(answer.value.rows[1][1].kind == CellKind::Absent);
    CHECK(answer.value.rows[2][1].lexical == "3");
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(Mentions(answer, "2 of 3 keys exist"));
    CHECK(WellFormed(answer.value));
}

TEST_CASE("mget where nothing exists is negative", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Array({ Nil(), Nil() }) }) };
    auto const answer = Run("mget", { "a", "b" }, exchange);
    CHECK(answer.outcome == Outcome::Negative);
    CHECK(Mentions(answer, "none of the keys exist"));
}

TEST_CASE("mget refuses a reply that answers about the wrong number of keys", "[cli][verbs]")
{
    // A crossed or truncated reply. The server answered, and what it said cannot be
    // matched to the request -- so it is Protocol, not Refused.
    ScriptedExchange exchange { Answers({ Array({ Bulk("1") }) }) };
    auto const answer = Run("mget", { "a", "b" }, exchange);
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(Mentions(answer, "answered about 1"));
}

TEST_CASE("set sends the modifiers it was given and nothing it was not", "[cli][verbs]")
{
    SECTION("plain")
    {
        ScriptedExchange exchange { Answers({ Simple("OK") }) };
        auto const answer = Run("set", { "k", "v" }, exchange);
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "SET", "k", "v" });
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(answer.value.shape == Shape::Empty);
    }
    SECTION("with a ttl")
    {
        ScriptedExchange exchange { Answers({ Simple("OK") }) };
        (void) Run("set", { "k", "v" }, exchange, VerbOptions { .ttlSeconds = 30 });
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "SET", "k", "v", "EX", "30" });
    }
    SECTION("only if absent")
    {
        ScriptedExchange exchange { Answers({ Simple("OK") }) };
        (void) Run("set", { "k", "v" }, exchange, VerbOptions { .onlyIfAbsent = true });
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "SET", "k", "v", "NX" });
    }
}

TEST_CASE("a set the server declined on nx is negative and says which condition", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Nil() }) };
    auto const answer = Run("set", { "k", "v" }, exchange, VerbOptions { .onlyIfAbsent = true });

    // An answer, and it is `no`. Not a failure -- the server did exactly what it was
    // asked.
    CHECK(answer.outcome == Outcome::Negative);
    CHECK(Mentions(answer, "already exists"));
}

TEST_CASE("del reports the count and calls zero a negative answer", "[cli][verbs]")
{
    SECTION("some deleted")
    {
        ScriptedExchange exchange { Answers({ Integer(2) }) };
        auto const answer = Run("del", { "a", "b" }, exchange);
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "DEL", "a", "b" });
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(answer.value.scalar.lexical == "2");
    }
    SECTION("none deleted")
    {
        ScriptedExchange exchange { Answers({ Integer(0) }) };
        auto const answer = Run("del", { "a" }, exchange);
        CHECK(answer.outcome == Outcome::Negative);
        // The number is still reported, so a script does not have to infer it.
        CHECK(answer.value.scalar.lexical == "0");
    }
}

TEST_CASE("an incr that lands on zero is not a negative answer", "[cli][verbs]")
{
    // The row's own name decides whether zero means `no`. An `incr` reaching zero has
    // succeeded; a `del` of nothing has not deleted anything. One handler serves both,
    // so this is the case that keeps them apart.
    ScriptedExchange exchange { Answers({ Integer(0) }) };
    auto const answer = Run("incr", { "k" }, exchange);
    CHECK(exchange.Sent()[0] == std::vector<std::string> { "INCR", "k" });
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(answer.value.scalar.lexical == "0");
}

TEST_CASE("the arithmetic family each send their own command", "[cli][verbs]")
{
    // The `protocolCommand` column is what lets one handler serve six rows; if a row
    // carried the wrong word, every one of them would still answer plausibly.
    struct Expectation
    {
        std::string_view verb;
        std::vector<std::string> operands;
        std::vector<std::string> sent;
    };
    auto const expectations = std::vector<Expectation> {
        { .verb = "incr", .operands = { "k" }, .sent = { "INCR", "k" } },
        { .verb = "decr", .operands = { "k" }, .sent = { "DECR", "k" } },
        { .verb = "incrby", .operands = { "k", "5" }, .sent = { "INCRBY", "k", "5" } },
        { .verb = "decrby", .operands = { "k", "5" }, .sent = { "DECRBY", "k", "5" } },
        { .verb = "exists", .operands = { "k" }, .sent = { "EXISTS", "k" } },
    };

    for (auto const& expectation: expectations)
    {
        ScriptedExchange exchange { Answers({ Integer(1) }) };
        (void) Run(expectation.verb, expectation.operands, exchange);
        REQUIRE(exchange.Sent().size() == 1);
        CHECK(exchange.Sent()[0] == expectation.sent);
    }
}

TEST_CASE("ttl reports its three states distinguishably on stdout", "[cli][verbs]")
{
    // The whole reason `ttl` is a record. Rendered as a bare scalar, *no such key* and
    // *exists with no expiry* both come out as an absent cell and are separable only
    // by the exit code -- a state collapse in the one verb whose job is that
    // distinction.
    SECTION("a live ttl")
    {
        ScriptedExchange exchange { Answers({ Integer(45) }) };
        auto const answer = Run("ttl", { "k" }, exchange);
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "TTL", "k" });
        CHECK(answer.outcome == Outcome::Affirmative);
        REQUIRE(FindField(answer.value, "exists") != nullptr);
        CHECK(FindField(answer.value, "exists")->value.lexical == "true");
        REQUIRE(FindField(answer.value, "ttl") != nullptr);
        CHECK(FindField(answer.value, "ttl")->value.lexical == "45");
    }
    SECTION("exists with no expiry")
    {
        ScriptedExchange exchange { Answers({ Integer(-1) }) };
        auto const answer = Run("ttl", { "k" }, exchange);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(FindField(answer.value, "exists")->value.lexical == "true");
        CHECK(FindField(answer.value, "ttl")->value.kind == CellKind::Absent);
        CHECK(Mentions(answer, "no expiry"));
    }
    SECTION("no such key")
    {
        ScriptedExchange exchange { Answers({ Integer(-2) }) };
        auto const answer = Run("ttl", { "k" }, exchange);
        CHECK(answer.outcome == Outcome::Negative);
        // `exists` is what separates this from the section above. Both have an absent
        // `ttl`.
        CHECK(FindField(answer.value, "exists")->value.lexical == "false");
        CHECK(FindField(answer.value, "ttl")->value.kind == CellKind::Absent);
    }
}

TEST_CASE("expire and persist report whether they changed anything", "[cli][verbs]")
{
    SECTION("it did")
    {
        ScriptedExchange exchange { Answers({ Integer(1) }) };
        auto const answer = Run("expire", { "k", "30" }, exchange);
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "EXPIRE", "k", "30" });
        CHECK(answer.outcome == Outcome::Affirmative);
    }
    SECTION("it did not")
    {
        ScriptedExchange exchange { Answers({ Integer(0) }) };
        auto const answer = Run("persist", { "k" }, exchange);
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "PERSIST", "k" });
        CHECK(answer.outcome == Outcome::Negative);
    }
}

TEST_CASE("flush picks its command from --all", "[cli][verbs]")
{
    SECTION("this database")
    {
        ScriptedExchange exchange { Answers({ Simple("OK") }) };
        auto const answer = Run("flush", {}, exchange);
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "FLUSHDB" });
        CHECK(answer.outcome == Outcome::Affirmative);
    }
    SECTION("every database")
    {
        ScriptedExchange exchange { Answers({ Simple("OK") }) };
        (void) Run("flush", {}, exchange, VerbOptions { .everything = true });
        CHECK(exchange.Sent()[0] == std::vector<std::string> { "FLUSHALL" });
    }
}

TEST_CASE("ping and echo report what came back", "[cli][verbs]")
{
    ScriptedExchange ping { Answers({ Simple("PONG") }) };
    auto const pinged = Run("ping", {}, ping);
    CHECK(ping.Sent()[0] == std::vector<std::string> { "PING" });
    CHECK(pinged.value.scalar.lexical == "PONG");

    ScriptedExchange echo { Answers({ Bulk("shout") }) };
    auto const echoed = Run("echo", { "shout" }, echo);
    CHECK(echo.Sent()[0] == std::vector<std::string> { "ECHO", "shout" });
    CHECK(echoed.value.scalar.lexical == "shout");
}

TEST_CASE("info parses the payload into a record", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Bulk("# Server\r\nfastcached_version:1.2.3\r\nused_memory:9\r\n") }) };
    auto const answer = Run("info", {}, exchange);

    CHECK(exchange.Sent()[0] == std::vector<std::string> { "INFO" });
    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(FindField(answer.value, "fastcached_version") != nullptr);
    CHECK(FindField(answer.value, "fastcached_version")->value.lexical == "1.2.3");
}

TEST_CASE("info accepts a verbatim payload as well as a bulk one", "[cli][verbs]")
{
    // A verbatim string is what INFO answers under RESP3. Accepting both costs one
    // comparison and means this does not break the day the client sends HELLO 3.
    ScriptedExchange exchange { Answers({ Verbatim("txt:fastcached_version:1.2.3\r\n") }) };
    auto const answer = Run("info", {}, exchange);
    CHECK(answer.outcome == Outcome::Affirmative);
}

TEST_CASE("version reports both ends", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Bulk("fastcached_version:9.9.9\r\nredis_version:6.0.0-fastcached\r\n") }) };
    auto const answer = Run("version", {}, exchange);

    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(FindField(answer.value, "client") != nullptr);
    // Compiled in, so it cannot have drifted from the build.
    CHECK(FindField(answer.value, "client")->value.lexical == FASTCACHE_CLI_VERSION);
    REQUIRE(FindField(answer.value, "server") != nullptr);
    CHECK(FindField(answer.value, "server")->value.lexical == "9.9.9");
    CHECK(FindField(answer.value, "resp_dialect")->value.lexical == "6.0.0-fastcached");
}

TEST_CASE("version still reports the client half when the server cannot be reached", "[cli][verbs]")
{
    // The half that is knowable without a server is still reported. Answering with
    // nothing but the failure would throw away a fact this process already had.
    ScriptedExchange exchange { Failure(ExchangeFailure::Unreachable, "connection refused") };
    auto const answer = Run("version", {}, exchange);

    CHECK(answer.outcome == Outcome::Unreachable);
    REQUIRE(FindField(answer.value, "client") != nullptr);
    CHECK(FindField(answer.value, "client")->value.lexical == FASTCACHE_CLI_VERSION);
    // And the server half is absent rather than blank or zero.
    REQUIRE(FindField(answer.value, "server") != nullptr);
    CHECK(FindField(answer.value, "server")->value.kind == CellKind::Absent);
    CHECK(Mentions(answer, "connection refused"));
}

TEST_CASE("a server error is a refusal and carries what the server said", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Error("ERR unknown command 'GET'") }) };
    auto const answer = Run("get", { "k" }, exchange);

    // Refused, not Protocol: the server answered and declined. Those are fixed by
    // different people.
    CHECK(answer.outcome == Outcome::Refused);
    CHECK(Mentions(answer, "unknown command"));
}

TEST_CASE("NOAUTH is a refusal that names how to supply a credential", "[cli][verbs]")
{
    ScriptedExchange exchange { Answers({ Error("NOAUTH Authentication required.") }) };
    auto const answer = Run("get", { "k" }, exchange);

    CHECK(answer.outcome == Outcome::Refused);
    // The bare server sentence does not say how to supply one, and this is the most
    // likely first-run failure.
    CHECK(Mentions(answer, "FASTCACHE_TOKEN"));
}

TEST_CASE("each exchange failure maps to its own outcome", "[cli][verbs]")
{
    SECTION("unreachable")
    {
        ScriptedExchange exchange { Failure(ExchangeFailure::Unreachable, "refused") };
        CHECK(Run("get", { "k" }, exchange).outcome == Outcome::Unreachable);
    }
    SECTION("transport")
    {
        ScriptedExchange exchange { Failure(ExchangeFailure::Transport, "reset") };
        CHECK(Run("get", { "k" }, exchange).outcome == Outcome::Unreachable);
    }
    SECTION("malformed")
    {
        // A reply this client cannot read is NOT the same as not getting one, and it
        // must not share an exit code with it.
        ScriptedExchange exchange { Failure(ExchangeFailure::Malformed, "nonsense") };
        CHECK(Run("get", { "k" }, exchange).outcome == Outcome::Protocol);
    }
}

TEST_CASE("an unexpected reply shape is a protocol error naming both shapes", "[cli][verbs]")
{
    // The outcome that suggests a version mismatch rather than a misconfiguration.
    ScriptedExchange exchange { Answers({ Integer(7) }) };
    auto const answer = Run("get", { "k" }, exchange);
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(Mentions(answer, "integer"));
    CHECK(Mentions(answer, "a value"));
}

TEST_CASE("stats goes through the gatherer and is refused without one", "[cli][verbs]")
{
    auto const* const stats = FindVerb("stats");
    REQUIRE(stats != nullptr);
    CHECK(stats->wire == Wire::Stats);

    SECTION("with a gatherer")
    {
        auto gatherer = ScriptedGatherer { std::vector<StatsAttempt> { StatsAttempt {
            .origin = StatsOrigin::Metrics,
            .asked = true,
            .record = RecordValue({ Field { .name = "fastcached_items", .value = NumberCell(std::uint64_t { 3 }) } }) } } };
        auto const answer = RunVerb(*stats, VerbContext { .resp = nullptr, .stats = &gatherer });
        CHECK(gatherer.Calls() == 1);
        CHECK(answer.outcome == Outcome::Affirmative);
        REQUIRE(FindField(answer.value, "source") != nullptr);
        CHECK(FindField(answer.value, "source")->value.lexical == "metrics");
    }
    SECTION("without one")
    {
        auto const answer = RunVerb(*stats, VerbContext { .resp = nullptr, .stats = nullptr });
        CHECK(answer.outcome == Outcome::Unreachable);
        CHECK(Mentions(answer, "no stats source"));
    }
}

TEST_CASE("every wire has a row explaining its absence", "[cli][verbs]")
{
    for (auto const& row: WireTable)
    {
        CHECK_FALSE(row.name.empty());
        // The sentence an operator reads when the wire could not be opened. An empty
        // one would make the refusal unactionable.
        CHECK_FALSE(row.unavailable.empty());
    }
}
