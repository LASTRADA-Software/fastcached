// SPDX-License-Identifier: Apache-2.0
//
// The defects these cover were all reachable before the option table: a typo'd
// option was silently ignored and its default used, `Opt` would happily return
// the *next flag* as a value (`--key --port` yielded key "--port"), `--help`
// fell through to "unknown sub-command", and a non-numeric `--port` threw out
// of std::stoi.
#include "TestClientCli.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::TestClient;

namespace
{

/// Parse an argument list.
/// @param argv The arguments, without the program name.
/// @return The parse result.
[[nodiscard]] std::expected<Args, ConfigError> Parse(std::vector<char const*> const& argv)
{
    return ParseArgs(std::span<char const* const> { argv });
}

} // namespace

TEST_CASE("a sub-command and a port is the minimum accepted line", "[testclient][cli]")
{
    auto const parsed = Parse({ "store", "--port", "1234" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->action == Action::Store);
    CHECK(parsed->port == 1234);
    CHECK(parsed->host == "127.0.0.1");
    CHECK(parsed->prefetchGroup == "default");
    // Per-platform, because a default naming a compiler the host does not have
    // is a default nobody can use. Asserted against the same constant the option
    // table renders, so the two cannot drift.
    CHECK(parsed->compiler == DefaultCompiler);
}

TEST_CASE("fetch selects its own action", "[testclient][cli]")
{
    auto const parsed = Parse({ "fetch", "--port=1234" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->action == Action::Fetch);
}

TEST_CASE("an unknown sub-command is named, not assumed", "[testclient][cli]")
{
    auto const parsed = Parse({ "stroe", "--port", "1234" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::UnknownKey);
    CHECK(parsed.error().field == "stroe");
}

TEST_CASE("no arguments asks for a sub-command", "[testclient][cli]")
{
    auto const parsed = Parse({});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::MissingRequired);
}

TEST_CASE("a typo'd option is rejected rather than ignored", "[testclient][cli]")
{
    // Previously this fell through the linear scan and the default was used, so
    // the run silently probed something other than what was asked for.
    auto const parsed = Parse({ "store", "--port", "1234", "--sourse", "a.cpp" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::UnknownKey);
    CHECK(parsed.error().field == "--sourse");
}

TEST_CASE("a flag consumed as a value no longer passes silently", "[testclient][cli]")
{
    // `--key --port 1234` used to *succeed*: the old scan found "--key" and
    // took the next token, and a separate scan found "--port" independently, so
    // the run stored under the key "--port" and nobody noticed.
    //
    // `--key` still consumes the following token — that is ordinary two-token
    // option syntax, and the daemon reads `--bind --port` the same way — but
    // the "1234" left over is now an unrecognised argument rather than a
    // silently different run.
    auto const parsed = Parse({ "store", "--key", "--port", "1234" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::UnknownKey);
    CHECK(parsed.error().field == "1234");
}

TEST_CASE("a missing port is one clear diagnostic", "[testclient][cli]")
{
    auto const parsed = Parse({ "store", "--source", "a.cpp" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::MissingRequired);
    CHECK(parsed.error().field == "--port");
}

TEST_CASE("a non-numeric port is rejected, not thrown on", "[testclient][cli]")
{
    // std::stoi used to throw out of main here.
    auto const parsed = Parse({ "store", "--port", "abc" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::TypeMismatch);
}

TEST_CASE("an out-of-range port is rejected", "[testclient][cli]")
{
    auto const parsed = Parse({ "store", "--port", "99999" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::OutOfRange);
}

TEST_CASE("A --help works with and without a sub-command", "[testclient][cli]")
{
    auto const bare = Parse({ "--help" });
    REQUIRE(bare.has_value());
    CHECK(bare->action == Action::ShowHelp);

    auto const shortForm = Parse({ "-h" });
    REQUIRE(shortForm.has_value());
    CHECK(shortForm->action == Action::ShowHelp);

    // After a sub-command it still wins, and without needing --port.
    auto const afterCommand = Parse({ "store", "--help" });
    REQUIRE(afterCommand.has_value());
    CHECK(afterCommand->action == Action::ShowHelp);
}

TEST_CASE("both value spellings agree", "[testclient][cli]")
{
    auto const joined = Parse({ "store", "--port=1234", "--key=abc" });
    auto const split = Parse({ "store", "--port", "1234", "--key", "abc" });
    REQUIRE(joined.has_value());
    REQUIRE(split.has_value());
    CHECK(joined->port == split->port);
    CHECK(joined->key == split->key);
}

TEST_CASE("every option is documented", "[testclient][cli][help]")
{
    auto const help = HelpText();
    for (auto const& spec: TestClientOptions())
    {
        INFO("row: " << spec.primary);
        CHECK(help.contains(RenderFlagForms(spec)));
        CHECK_FALSE(spec.description.empty());
        CHECK((spec.arity == Arity::Value) == !spec.operand.empty());
    }
    CHECK(help.contains("COMMANDS"));
    CHECK(help.contains("store"));
    CHECK(help.contains("fetch"));
}

TEST_CASE("help colorizes without changing its text", "[testclient][cli][help][color]")
{
    CHECK_FALSE(HelpText(UsageColor::Plain).contains('\x1b'));
    CHECK(HelpText(UsageColor::Colored).contains('\x1b'));
}
