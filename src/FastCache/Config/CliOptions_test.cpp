// SPDX-License-Identifier: Apache-2.0
//
// Properties of the daemon's CLI option table itself, as opposed to the
// behaviour of individual flags (which CliParser_test.cpp covers).
//
// These are the assertions the single-table design makes possible: before it,
// a flag's spelling, its parser, its explicit tracker and its help text lived
// in separate structures, so "every accepted flag is documented" and "parsing
// one flag touches only its own tracker" could not be stated at all.
#include <FastCache/Cli/UsageTestUtils.hpp>
#include <FastCache/Config/CliParser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{

/// A flag paired with a value that parses, so every row can be exercised.
struct FlagSample
{
    std::string_view flag;  ///< The row's primary spelling.
    std::string_view value; ///< A valid value, or empty for a valueless flag.
};

/// One sample per row. Asserted below to cover `CliOptions()` exactly, so a new
/// flag cannot be added without also being exercised here.
constexpr auto Samples = std::to_array<FlagSample>({
    { .flag = "--config", .value = "fastcached.yaml" },
    { .flag = "--bind", .value = "127.0.0.1" },
    { .flag = "--port", .value = "1234" },
    { .flag = "--max-memory", .value = "1m" },
    { .flag = "--log-level", .value = "debug" },
    { .flag = "--requirepass", .value = "s3cret" },
    { .flag = "--auth-username", .value = "operator" },
    { .flag = "--metrics", .value = "" },
    { .flag = "--metrics-bind", .value = "127.0.0.1" },
    { .flag = "--metrics-port", .value = "9999" },
    { .flag = "--tls", .value = "" },
    { .flag = "--tls-cert", .value = "cert.pem" },
    { .flag = "--tls-key", .value = "key.pem" },
    { .flag = "--listen", .value = "127.0.0.1:1234" },
    { .flag = "--listen-tls", .value = "127.0.0.1:1235" },
    { .flag = "--notify-keyspace-events", .value = "KEA" },
    { .flag = "--log-timestamps", .value = "" },
    { .flag = "--log-source", .value = "" },
    { .flag = "--log-everything", .value = "" },
    { .flag = "--storage", .value = "cache.db" },
    { .flag = "--storage-durability", .value = "fsync" },
    { .flag = "--storage-max-value", .value = "2m" },
    { .flag = "--storage-max-disk", .value = "3m" },
    { .flag = "--compression", .value = "none" },
    { .flag = "--compression-level", .value = "5" },
    { .flag = "--compression-min-bytes", .value = "128" },
    { .flag = "--lru-mode", .value = "strict" },
    { .flag = "--cpu-affinity", .value = "none" },
    { .flag = "--threads", .value = "4" },
    { .flag = "--listen-backlog", .value = "128" },
    { .flag = "--storage-shards", .value = "2" },
    { .flag = "--daemon", .value = "" },
    { .flag = "--install-service", .value = "" },
    { .flag = "--uninstall-service", .value = "" },
    { .flag = "--service-scope", .value = "system" },
    { .flag = "--healthcheck", .value = "" },
    { .flag = "--seed-config", .value = "template.yaml" },
    { .flag = "--pidfile", .value = "fastcached.pid" },
    { .flag = "--service-name", .value = "FastCached" },
    { .flag = "--help", .value = "" },
    { .flag = "--version", .value = "" },
});

/// Parse a single flag in its `--flag=value` form.
/// @param sample The flag and a value that parses.
/// @return The parse result.
[[nodiscard]] std::expected<CliResult, ConfigError> ParseOne(FlagSample const& sample)
{
    auto const joined = sample.value.empty() ? std::string { sample.flag } : std::format("{}={}", sample.flag, sample.value);
    std::array<char const*, 1> const argv { joined.c_str() };
    return ParseCli(argv);
}

/// Every explicit tracker the table knows about.
/// @return The member pointers, one per row that carries a tracker.
[[nodiscard]] std::vector<bool CliResult::*> AllTrackers()
{
    std::vector<bool CliResult::*> trackers;
    for (auto const& spec: CliOptions())
        if (spec.explicitBit != nullptr)
            trackers.push_back(spec.explicitBit);
    return trackers;
}

} // namespace

TEST_CASE("every option row is well formed", "[config][cli][options]")
{
    REQUIRE_FALSE(CliOptions().empty());
    for (auto const& spec: CliOptions())
    {
        INFO("row: " << spec.primary);
        CHECK(spec.primary.starts_with("--"));
        CHECK_FALSE(spec.description.empty());
        // A value flag must show its operand in help; a valueless one must not.
        CHECK((spec.arity == Arity::Value) == !spec.operand.empty());
        // A row that does nothing at all would be an accepted no-op.
        CHECK((spec.apply != nullptr || spec.select != nullptr));
    }
}

TEST_CASE("no spelling is claimed by two rows", "[config][cli][options]")
{
    for (auto const& outer: CliOptions())
    {
        auto primaries = 0;
        auto aliases = 0;
        for (auto const& inner: CliOptions())
        {
            if (outer.primary == inner.primary)
                ++primaries;
            if (!outer.alias.empty() && outer.alias == inner.alias)
                ++aliases;
        }
        INFO("row: " << outer.primary);
        CHECK(primaries == 1);
        if (!outer.alias.empty())
            CHECK(aliases == 1);
    }
}

TEST_CASE("every accepted flag appears in the help text", "[config][cli][options][help]")
{
    // The property the single table exists to create, direction one.
    auto const usage = CliUsage();
    for (auto const& spec: CliOptions())
    {
        INFO("row: " << spec.primary);
        CHECK(usage.contains(RenderFlagForms(spec)));
    }
}

TEST_CASE("every documented flag is an accepted flag", "[config][cli][options][help]")
{
    // Direction two: nothing in the option column is undocumented prose or a
    // flag the parser would reject.
    auto const usage = CliUsage();

    // Rendered once: rebuilding every row's forms inside the scan would be a
    // quadratic pile of throwaway strings.
    std::vector<std::string> forms;
    forms.reserve(CliOptions().size());
    for (auto const& spec: CliOptions())
        forms.push_back(RenderFlagForms(spec));

    auto documented = std::size_t { 0 };
    for (auto const& text: FastCache::Testing::UsageLines(usage))
    {
        if (!text.starts_with("  --"))
            continue;
        ++documented;
        auto const term = std::string_view { text }.substr(2, text.find("  ", 2) - 2);
        auto const known = std::ranges::contains(forms, term);
        INFO("documented term: " << term);
        CHECK(known);
    }
    CHECK(documented == CliOptions().size());
}

TEST_CASE("every row parses its sample value", "[config][cli][options]")
{
    // The sample table must cover the option table exactly, so a newly added
    // flag cannot slip through unexercised.
    REQUIRE(Samples.size() == CliOptions().size());
    for (auto const& spec: CliOptions())
    {
        auto const covered = std::ranges::any_of(Samples, [&spec](FlagSample const& s) { return s.flag == spec.primary; });
        INFO("row: " << spec.primary);
        CHECK(covered);
    }

    for (auto const& sample: Samples)
    {
        INFO("flag: " << sample.flag);
        auto const parsed = ParseOne(sample);
        CHECK(parsed.has_value());
    }
}

TEST_CASE("a flag sets its own tracker and no other", "[config][cli][options]")
{
    // Impossible to state before the table: it catches the copy-paste tracker
    // mixup that the merge layer records having been retrofitted repeatedly.
    auto const trackers = AllTrackers();
    for (auto const& sample: Samples)
    {
        auto const spec = std::ranges::find_if(
            CliOptions(), [&sample](auto const& candidate) { return candidate.primary == sample.flag; });
        REQUIRE(spec != std::ranges::end(CliOptions()));

        auto const parsed = ParseOne(sample);
        REQUIRE(parsed.has_value());

        for (auto const tracker: trackers)
        {
            INFO("flag: " << sample.flag);
            CHECK(parsed.value().*tracker == (tracker == spec->explicitBit));
        }
    }
}

TEST_CASE("both value spellings produce the same result", "[config][cli][options]")
{
    for (auto const& sample: Samples)
    {
        if (sample.value.empty())
            continue;
        INFO("flag: " << sample.flag);

        auto const joined = std::format("{}={}", sample.flag, sample.value);
        std::array<char const*, 1> const joinedArgv { joined.c_str() };
        auto const fromJoined = ParseCli(joinedArgv);

        std::string const flag { sample.flag };
        std::string const value { sample.value };
        std::array<char const*, 2> const splitArgv { flag.c_str(), value.c_str() };
        auto const fromSplit = ParseCli(splitArgv);

        REQUIRE(fromJoined.has_value());
        REQUIRE(fromSplit.has_value());
        CHECK(fromJoined->config == fromSplit->config);
        CHECK(fromJoined->outcome == fromSplit->outcome);
        CHECK(fromJoined->seedConfigTemplate == fromSplit->seedConfigTemplate);
    }
}

TEST_CASE("a value flag at the end of argv names itself in the error", "[config][cli][options]")
{
    for (auto const& spec: CliOptions())
    {
        if (spec.arity != Arity::Value)
            continue;
        std::string const flag { spec.primary };
        std::array<char const*, 1> const argv { flag.c_str() };
        auto const parsed = ParseCli(argv);

        INFO("row: " << spec.primary);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == ConfigErrorCode::ParseError);
        // The dashed spelling is deliberate: a value parser's own errors name
        // the setting ("port"), a missing value names the flag as typed.
        CHECK(parsed.error().field == spec.primary);
    }
}

TEST_CASE("A --help wins over whatever follows it", "[config][cli][options]")
{
    std::array<char const*, 2> const argv { "--help", "--nonsense" };
    auto const parsed = ParseCli(argv);
    REQUIRE(parsed.has_value());
    CHECK(parsed->outcome == CliOutcome::ShowHelp);
}

TEST_CASE("a flag before --help is still applied", "[config][cli][options]")
{
    std::array<char const*, 2> const argv { "--port=4321", "--help" };
    auto const parsed = ParseCli(argv);
    REQUIRE(parsed.has_value());
    CHECK(parsed->outcome == CliOutcome::ShowHelp);
    CHECK(parsed->config.port == 4321);
}
