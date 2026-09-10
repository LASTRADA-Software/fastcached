// SPDX-License-Identifier: Apache-2.0
#include "CliCommand.hpp"

#include <FastCache/Cli/UsageTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Testing;

namespace
{
/// Parse a command line spelled as literals.
/// @param argv The arguments, program name already removed.
/// @return The parsed command.
[[nodiscard]] Command Parse(std::initializer_list<std::string_view> argv)
{
    std::vector<std::string> args;
    args.reserve(argv.size());
    for (auto const arg: argv)
        args.emplace_back(arg);
    return ParseCommand(args);
}

/// A scripted environment holding one variable.
///
/// A function pointer, because that is what `ApplyEnvironment` takes -- and it
/// takes one precisely so this substitution is possible without touching the real
/// environment, which a test must never depend on.
std::string envName;
std::string envValue;

/// The scripted lookup.
/// @param name The variable asked for.
/// @return Its value, when it is the scripted one.
[[nodiscard]] std::optional<std::string> ScriptedLookup(std::string_view name)
{
    if (name == envName)
        return envValue;
    return std::nullopt;
}

/// Apply one scripted variable to a fresh command.
/// @param name The variable.
/// @param value Its value.
/// @return The seeded command.
[[nodiscard]] Command WithEnvironment(std::string name, std::string value)
{
    envName = std::move(name);
    envValue = std::move(value);
    Command command;
    ApplyEnvironment(command, &ScriptedLookup);
    return command;
}
} // namespace

TEST_CASE("the option table is well formed and every row is documented", "[cli][command]")
{
    // `TableIsWellFormed` is a static_assert in the implementation, so this asserts the
    // properties a compile-time check cannot: that the help's left column is derived
    // from the row rather than restated.
    CHECK_FALSE(CliToolOptions().empty());
    auto const help = HelpText();
    for (auto const& option: CliToolOptions())
    {
        CHECK_FALSE(option.description.empty());
        // A flag that is accepted and undocumented is the failure this derivation
        // exists to prevent.
        CHECK(help.contains(option.primary));
    }
}

TEST_CASE("the help text names every verb, format, exit code and variable", "[cli][command]")
{
    auto const help = HelpText();
    for (auto const& verb: Verbs())
        CHECK(help.contains(verb.name));
    for (auto const& format: FormatTable)
        CHECK(help.contains(format.name));
    for (auto const& outcome: OutcomeTable)
        CHECK(help.contains(outcome.name));
    for (auto const& variable: CliEnvironment())
        CHECK(help.contains(variable.name));
    CHECK(help.back() == '\n');
}

TEST_CASE("the help text substitutes the default address rather than printing the token", "[cli][command]")
{
    auto const help = HelpText();
    CHECK(help.contains("127.0.0.1:6674"));
    // An unsubstituted token would be a visible defect in the one text everybody reads.
    CHECK_FALSE(help.contains("{addr}"));
}

TEST_CASE("colour changes no column in the help text", "[cli][command]")
{
    auto const plain = HelpText(UsageColor::Plain);
    auto const colored = HelpText(UsageColor::Colored);
    CHECK(StripAnsi(colored) == plain);
    CHECK(colored != plain);
}

TEST_CASE("no arguments is a usage error rather than a default action", "[cli][command]")
{
    auto const command = Parse({});
    CHECK(command.action == Action::UsageError);
    CHECK_FALSE(command.diagnostic.empty());
}

TEST_CASE("a verb and its operands are separated from the options", "[cli][command]")
{
    auto const command = Parse({ "set", "k", "v" });
    CHECK(command.action == Action::RunVerb);
    CHECK(command.verb == "set");
    CHECK(command.operands == std::vector<std::string> { "k", "v" });
}

TEST_CASE("options are accepted before and after the verb", "[cli][command]")
{
    // Both orders, because an operator types whichever occurs to them and a tool that
    // accepts only one is a tool that looks broken.
    auto const before = Parse({ "--format=json", "get", "k" });
    CHECK(before.action == Action::RunVerb);
    CHECK(before.format == OutputFormat::Json);
    CHECK(before.operands == std::vector<std::string> { "k" });

    auto const after = Parse({ "get", "k", "--format=json" });
    CHECK(after.action == Action::RunVerb);
    CHECK(after.format == OutputFormat::Json);
    CHECK(after.operands == std::vector<std::string> { "k" });
}

TEST_CASE("a separated option value is taken and not mistaken for an operand", "[cli][command]")
{
    auto const command = Parse({ "get", "--format", "json", "k" });
    CHECK(command.format == OutputFormat::Json);
    // `json` must not have become the key.
    CHECK(command.operands == std::vector<std::string> { "k" });
}

TEST_CASE("a bare -- ends option parsing so a dashed operand can be named", "[cli][command]")
{
    auto const command = Parse({ "get", "--", "--weird-key" });
    CHECK(command.action == Action::RunVerb);
    CHECK(command.operands == std::vector<std::string> { "--weird-key" });
}

TEST_CASE("an unknown option is refused rather than ignored", "[cli][command]")
{
    auto const command = Parse({ "get", "k", "--nope" });
    CHECK(command.action == Action::UsageError);
    CHECK(command.diagnostic.contains("--nope"));
}

TEST_CASE("an unknown verb is refused and named", "[cli][command]")
{
    auto const command = Parse({ "frobnicate" });
    CHECK(command.action == Action::UsageError);
    CHECK(command.diagnostic.contains("frobnicate"));
}

TEST_CASE("the wrong operand count is refused with the arity from the row", "[cli][command]")
{
    auto const tooFew = Parse({ "set", "k" });
    CHECK(tooFew.action == Action::UsageError);
    CHECK(tooFew.diagnostic.contains("exactly 2 operands"));

    auto const tooMany = Parse({ "get", "a", "b" });
    CHECK(tooMany.action == Action::UsageError);
    CHECK(tooMany.diagnostic.contains("exactly 1 operand"));

    // Variadic accepts many, so this must NOT be refused -- the positive control for
    // the two above.
    auto const variadic = Parse({ "mget", "a", "b", "c" });
    CHECK(variadic.action == Action::RunVerb);
}

TEST_CASE("help and version answer without reading the rest", "[cli][command]")
{
    CHECK(Parse({ "--help" }).action == Action::ShowHelp);
    CHECK(Parse({ "-h" }).action == Action::ShowHelp);
    CHECK(Parse({ "--version" }).action == Action::ShowVersion);
    CHECK(Parse({ "-V" }).action == Action::ShowVersion);
    // They are questions about this binary, not about the command line, so nonsense
    // after them is not an error.
    CHECK(Parse({ "--help", "--nonsense" }).action == Action::ShowHelp);
}

TEST_CASE("the addr flag parses host:port and refuses a bare port", "[cli][command]")
{
    auto const good = Parse({ "--addr=example:7000", "ping" });
    CHECK(good.action == Action::RunVerb);
    CHECK(good.cache.host == "example");
    CHECK(good.cache.port == 7000);

    // A bare port names no machine. This is the dialled-endpoint grammar, and it
    // differs from the one a member list takes on purpose.
    auto const bare = Parse({ "--addr=7000", "ping" });
    CHECK(bare.action == Action::UsageError);
    CHECK(bare.diagnostic.contains("--addr"));
}

TEST_CASE("an IPv6 literal keeps its address and loses its brackets", "[cli][command]")
{
    // `rfind(':')` finds the wrong colon here, which is exactly why the shared
    // predicate is used rather than a split written at this call site.
    auto const command = Parse({ "--addr=[::1]:7000", "ping" });
    CHECK(command.action == Action::RunVerb);
    CHECK(command.cache.host == "::1");
    CHECK(command.cache.port == 7000);
}

TEST_CASE("a refusal from a shared value parser is stamped with the flag it came through", "[cli][command]")
{
    // The parser names no field; `ApplyOneOption` stamps the row's own spelling. Two
    // flags share `ParseFormatName`-shaped appliers, so a hand-written field would be
    // wrong for one of them.
    auto const cache = Parse({ "--addr=nonsense", "ping" });
    CHECK(cache.diagnostic.contains("--addr"));
    auto const admin = Parse({ "--admin-addr=nonsense", "ping" });
    CHECK(admin.diagnostic.contains("--admin-addr"));
}

TEST_CASE("an unknown format is refused and the message lists the real ones", "[cli][command]")
{
    auto const command = Parse({ "--format=yaml", "ping" });
    CHECK(command.action == Action::UsageError);
    for (auto const& row: FormatTable)
        CHECK(command.diagnostic.contains(row.name));
}

TEST_CASE("the color flag takes its three spellings and refuses a fourth", "[cli][command]")
{
    CHECK(Parse({ "--color=auto", "ping" }).color == ColorChoice::Auto);
    CHECK(Parse({ "--color=always", "ping" }).color == ColorChoice::Always);
    CHECK(Parse({ "--color=never", "ping" }).color == ColorChoice::Never);
    CHECK(Parse({ "--color=sometimes", "ping" }).action == Action::UsageError);
}

TEST_CASE("a modifier a verb does not honour is refused, not ignored", "[cli][command]")
{
    // Silently ignoring it would discard a flag the caller believed did something --
    // the failure the launcher's separate sub-option tables exist to prevent.
    auto const raw = Parse({ "set", "k", "v", "--raw" });
    CHECK(raw.action == Action::UsageError);
    CHECK(raw.diagnostic.contains("--raw"));
    CHECK(raw.diagnostic.contains("set"));

    auto const ttl = Parse({ "get", "k", "--ttl=30" });
    CHECK(ttl.action == Action::UsageError);
    CHECK(ttl.diagnostic.contains("--ttl"));

    auto const all = Parse({ "get", "k", "--all" });
    CHECK(all.action == Action::UsageError);

    auto const nx = Parse({ "get", "k", "--nx" });
    CHECK(nx.action == Action::UsageError);
}

TEST_CASE("the modifiers each verb does honour are accepted", "[cli][command]")
{
    // The positive control. Without it, a check that refused every modifier
    // everywhere would pass the case above.
    CHECK(Parse({ "get", "k", "--raw" }).action == Action::RunVerb);
    CHECK(Parse({ "set", "k", "v", "--ttl=30" }).action == Action::RunVerb);
    CHECK(Parse({ "set", "k", "v", "--nx" }).action == Action::RunVerb);
    CHECK(Parse({ "set", "k", "v", "--xx" }).action == Action::RunVerb);
    CHECK(Parse({ "flush", "--all" }).action == Action::RunVerb);

    auto const command = Parse({ "set", "k", "v", "--ttl=30", "--nx" });
    CHECK(command.verbOptions.ttlSeconds == 30);
    CHECK(command.verbOptions.onlyIfAbsent);
}

TEST_CASE("nx and xx together are refused", "[cli][command]")
{
    auto const command = Parse({ "set", "k", "v", "--nx", "--xx" });
    CHECK(command.action == Action::UsageError);
    CHECK(command.diagnostic.contains("contradict"));
}

TEST_CASE("a ttl that is not a number is refused", "[cli][command]")
{
    CHECK(Parse({ "set", "k", "v", "--ttl=soon" }).action == Action::UsageError);
    CHECK(Parse({ "set", "k", "v", "--ttl=-5" }).action == Action::UsageError);
}

TEST_CASE("the timeouts parse into their own fields", "[cli][command]")
{
    auto const command = Parse({ "--connect-timeout=250", "--timeout=750", "ping" });
    CHECK(command.action == Action::RunVerb);
    CHECK(command.timeouts.connect == std::chrono::milliseconds { 250 });
    CHECK(command.timeouts.io == std::chrono::milliseconds { 750 });
}

TEST_CASE("the environment seeds the defaults", "[cli][command]")
{
    auto const command = WithEnvironment("FASTCACHE_ADDR", "elsewhere:9999");
    CHECK(command.action == Action::RunVerb);
    CHECK(command.cache.host == "elsewhere");
    CHECK(command.cache.port == 9999);
}

TEST_CASE("the command line beats the environment", "[cli][command]")
{
    // "The command line wins" is which step runs second, not a per-field merge with a
    // per-field explicit bit -- the shape this repository has shipped a flag that
    // parsed and never merged with, four times.
    auto const seed = WithEnvironment("FASTCACHE_ADDR", "elsewhere:9999");
    std::vector<std::string> const args { "--addr=chosen:1234", "ping" };
    auto const command = ParseCommand(args, seed);

    CHECK(command.cache.host == "chosen");
    CHECK(command.cache.port == 1234);
}

TEST_CASE("the environment still applies to what the command line did not name", "[cli][command]")
{
    // The other half of the precedence rule, and the one a naive "argv overwrites
    // everything" implementation breaks.
    auto const seed = WithEnvironment("FASTCACHE_ADDR", "elsewhere:9999");
    std::vector<std::string> const args { "--format=json", "ping" };
    auto const command = ParseCommand(args, seed);

    CHECK(command.format == OutputFormat::Json);
    CHECK(command.cache.host == "elsewhere");
}

TEST_CASE("a set-but-empty variable counts as unset", "[cli][command]")
{
    // What a build that wants no configuration sets, and the launcher's own reader
    // treats it the same way.
    auto const command = WithEnvironment("FASTCACHE_ADDR", "");
    CHECK(command.cache.host == "127.0.0.1");
    CHECK(command.cache.port == 6674);
}

TEST_CASE("a malformed address in the environment is refused, naming the variable", "[cli][command]")
{
    // Refused rather than ignored: an operator who exported a typo would otherwise
    // reach the default and wonder why their setting did nothing.
    auto const command = WithEnvironment("FASTCACHE_ADDR", "nonsense");
    CHECK(command.action == Action::UsageError);
    CHECK(command.diagnostic.contains("FASTCACHE_ADDR"));
}

TEST_CASE("the credential comes from the environment", "[cli][command]")
{
    auto const token = WithEnvironment("FASTCACHE_TOKEN", "s3cret");
    CHECK(token.credential.secret == "s3cret");
    CHECK(token.credential.Configured());

    auto const user = WithEnvironment("FASTCACHE_USER", "someone");
    CHECK(user.credential.username == "someone");
    // A username with no secret is not a configured credential: there is nothing to
    // present.
    CHECK_FALSE(user.credential.Configured());
}

TEST_CASE("the admin address is unset by default and says so", "[cli][command]")
{
    // Unset means the richest stats source is not asked, which is reported as *not
    // asked* rather than as a failure. `Configured()` is what the gatherer branches on.
    CHECK_FALSE(Command {}.admin.Configured());
    CHECK(Parse({ "--admin-addr=here:9000", "stats" }).admin.Configured());
}

TEST_CASE("every outcome has a distinct exit code", "[cli][command]")
{
    // One code answering two questions is the defect this table exists to prevent.
    for (auto const& outer: OutcomeTable)
        for (auto const& inner: OutcomeTable)
            if (outer.outcome != inner.outcome)
                CHECK(outer.code != inner.code);

    // The pairs the table's own documentation promises to keep apart.
    CHECK(ExitCodeOf(Outcome::Negative) != ExitCodeOf(Outcome::Unreachable));
    CHECK(ExitCodeOf(Outcome::Refused) != ExitCodeOf(Outcome::Protocol));
    CHECK(ExitCodeOf(Outcome::Affirmative) == 0);
    CHECK(ExitCodeOf(Outcome::Usage) == 2);
}
