// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace FastCache;

namespace
{

/// What a parse of the test table resolved to.
enum class TestOutcome : std::uint8_t
{
    Run,      ///< The default.
    ShowHelp, ///< `--help` was seen.
    Install,  ///< `--install` was seen.
};

/// Stands in for a daemon Config: the nested settings a merge would consult.
struct TestConfig
{
    std::uint16_t port { 11 };
    bool verbose { false };
    std::string name;
    std::vector<std::string> listeners;
};

/// Stands in for a CliResult: the nested config plus install-time settings that
/// deliberately live outside it.
struct TestResult
{
    TestOutcome outcome { TestOutcome::Run };
    TestConfig config {};
    std::string seed;
    bool portExplicit { false };
    bool verboseExplicit { false };
};

/// Parse a decimal port, rejecting anything else.
/// @param sv The value text.
/// @return The port, or a ConfigError.
[[nodiscard]] std::expected<std::uint16_t, ConfigError> ParseTestPort(std::string_view sv)
{
    std::uint16_t value = 0;
    auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc {} || ptr != sv.data() + sv.size())
        return std::unexpected(ArgvError(ConfigErrorCode::TypeMismatch, "port", std::format("not a number: {}", sv)));
    return value;
}

/// One row of every shape the daemon needs, so the generic paths are exercised
/// independently of which flags happen to exist today.
constexpr auto TestOptions = std::to_array<OptionSpec<TestResult>>({
    // A typed flag with a tracker.
    { .primary = "--port",
      .arity = Arity::Value,
      .operand = "=<num>",
      .apply = AssignFrom<&TestConfig::port, ParseTestPort>(),
      .explicitBit = &TestResult::portExplicit,
      .description = "TCP port (default {port})" },
    // A valueless switch with a tracker.
    { .primary = "--verbose",
      .apply = SetTrue<&TestConfig::verbose>(),
      .explicitBit = &TestResult::verboseExplicit,
      .description = "talk more" },
    // A string flag with no tracker.
    { .primary = "--name",
      .arity = Arity::Value,
      .operand = "=<text>",
      .apply = AssignFrom<&TestConfig::name, ParseText>(),
      .description = "a name" },
    // A repeatable flag.
    { .primary = "--listen",
      .arity = Arity::Value,
      .operand = "=<host>",
      .apply = AppendFrom<&TestConfig::listeners, ParseText>(),
      .description = "extra listener; repeatable" },
    // An action flag that both stores a value and selects an outcome, and whose
    // target is the result itself rather than the nested config.
    { .primary = "--install",
      .arity = Arity::Value,
      .operand = "=<path>",
      .apply = AssignFrom<&TestResult::seed, ParseText>(),
      .select = SelectOutcome<&TestResult::outcome, TestOutcome::Install>(),
      .description = "install from <path>" },
    // A control flag: no state, selects an outcome, stops parsing.
    { .primary = "--help",
      .alias = "-h",
      .select = SelectOutcome<&TestResult::outcome, TestOutcome::ShowHelp>(),
      .flow = ParseFlow::Stop,
      .description = "show this help and exit" },
});

/// Parse an argument list against the test table.
/// @param args The arguments, without a program name.
/// @return The parse result.
[[nodiscard]] std::expected<TestResult, ConfigError> Parse(std::vector<char const*> const& args)
{
    return ParseOptions(std::span<OptionSpec<TestResult> const> { TestOptions }, std::span<char const* const> { args });
}

} // namespace

TEST_CASE("option table assigns a typed value and marks it explicit", "[cli][options]")
{
    auto const joined = Parse({ "--port=8080" });
    REQUIRE(joined.has_value());
    CHECK(joined->config.port == 8080);
    CHECK(joined->portExplicit);

    // The separate-token spelling must produce an identical result.
    auto const split = Parse({ "--port", "8080" });
    REQUIRE(split.has_value());
    CHECK(split->config.port == 8080);
    CHECK(split->portExplicit);
}

TEST_CASE("a typed value equal to the default is still marked explicit", "[cli][options]")
{
    // The whole reason the tracker exists: 11 is the field's default, so
    // without the bit a merge could not tell this from "flag absent".
    auto const parsed = Parse({ "--port=11" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->config.port == 11);
    CHECK(parsed->portExplicit);
}

TEST_CASE("a valueless switch sets its field and tracker", "[cli][options]")
{
    auto const parsed = Parse({ "--verbose" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->config.verbose);
    CHECK(parsed->verboseExplicit);
}

TEST_CASE("a valueless flag does not accept an attached value", "[cli][options]")
{
    // `--verbose=1` has never been an accepted spelling; admitting it would
    // silently discard the value.
    auto const parsed = Parse({ "--verbose=1" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::UnknownKey);
}

TEST_CASE("a repeatable flag appends in order", "[cli][options]")
{
    auto const parsed = Parse({ "--listen", "a", "--listen=b" });
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->config.listeners.size() == 2);
    CHECK(parsed->config.listeners[0] == "a");
    CHECK(parsed->config.listeners[1] == "b");
}

TEST_CASE("an action flag stores its value and selects its outcome", "[cli][options]")
{
    auto const parsed = Parse({ "--install=/tmp/x.yaml" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->seed == "/tmp/x.yaml");
    CHECK(parsed->outcome == TestOutcome::Install);
}

TEST_CASE("an action flag keeps parsing the flags after it", "[cli][options]")
{
    // Install-style flags must not stop the loop: the remaining flags are
    // captured into the config that gets baked into a service command line.
    auto const parsed = Parse({ "--install=/tmp/x.yaml", "--port=99" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->outcome == TestOutcome::Install);
    CHECK(parsed->config.port == 99);
}

TEST_CASE("a Stop flag discards whatever follows it", "[cli][options]")
{
    auto const parsed = Parse({ "--help", "--nonsense" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->outcome == TestOutcome::ShowHelp);
}

TEST_CASE("an alias selects the same row as its primary", "[cli][options]")
{
    auto const parsed = Parse({ "-h" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->outcome == TestOutcome::ShowHelp);
}

TEST_CASE("an unknown argument is rejected by name", "[cli][options]")
{
    auto const parsed = Parse({ "--bogus" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::UnknownKey);
    CHECK(parsed.error().source == "argv");
    CHECK(parsed.error().field == "--bogus");
}

TEST_CASE("a flag at the end of argv with no value is rejected", "[cli][options]")
{
    auto const parsed = Parse({ "--port" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::ParseError);
    // The dashed spelling is deliberate: value-parser errors name the setting
    // ("port"), a missing value names the flag as typed.
    CHECK(parsed.error().field == "--port");
}

TEST_CASE("a bad value surfaces the parser's own error verbatim", "[cli][options]")
{
    auto const parsed = Parse({ "--port=abc" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::TypeMismatch);
    CHECK(parsed.error().field == "port");
}

TEST_CASE("flag forms are derived from the row", "[cli][options]")
{
    CHECK(RenderFlagForms("--port", "", "=<num>") == "--port=<num>");
    CHECK(RenderFlagForms("--help", "-h", "") == "--help, -h");
    CHECK(RenderFlagForms("--daemon", "", "") == "--daemon");
}

TEST_CASE("every row is documented and spelled once", "[cli][options]")
{
    // The property the table exists to create: a flag cannot be accepted
    // without being documented, nor documented without being accepted.
    for (auto const& spec: TestOptions)
    {
        INFO("row: " << spec.primary);
        CHECK(spec.primary.starts_with("--"));
        CHECK_FALSE(spec.description.empty());
        CHECK((spec.arity == Arity::Value) == !spec.operand.empty());
    }

    for (auto const& outer: TestOptions)
    {
        auto duplicates = 0;
        for (auto const& inner: TestOptions)
            if (outer.primary == inner.primary)
                ++duplicates;
        INFO("row: " << outer.primary);
        CHECK(duplicates == 1);
    }
}

TEST_CASE("a longer flag is not claimed by a shorter one", "[cli][options]")
{
    // `--listen` is a prefix of nothing here, but the guard that makes a flat
    // table order-independent is that a value flag only matches on `=`.
    CHECK(FlagMatches("--listen=a", "--listen"));
    CHECK(FlagMatches("--listen", "--listen"));
    CHECK_FALSE(FlagMatches("--listen-tls=a", "--listen"));
    CHECK_FALSE(FlagMatches("--listenx", "--listen"));
}
