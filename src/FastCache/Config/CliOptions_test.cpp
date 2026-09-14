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
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>

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
    { .flag = "--no-log-timestamps", .value = "" },
    { .flag = "--log-source", .value = "" },
    { .flag = "--log-everything", .value = "" },
    { .flag = "--storage", .value = "cache.db" },
    { .flag = "--storage-durability", .value = "fsync" },
    { .flag = "--storage-max-value", .value = "2m" },
    { .flag = "--storage-max-disk", .value = "3m" },
    { .flag = "--compression", .value = "none" },
    { .flag = "--compression-level", .value = "5" },
    { .flag = "--compression-min-bytes", .value = "128" },
    // `none` on every codec row, because this table asks only whether the value
    // PARSES and `Identity` is the one codec available in every build --
    // `ParseCompression` refuses `zstd` where FASTCACHED_ENABLE_COMPRESSION is off.
    { .flag = "--memory-compression", .value = "none" },
    { .flag = "--memory-compression-level", .value = "5" },
    { .flag = "--memory-compression-min-bytes", .value = "128" },
    { .flag = "--lru-mode", .value = "strict" },
    { .flag = "--cpu-affinity", .value = "none" },
    { .flag = "--threads", .value = "4" },
    { .flag = "--listen-backlog", .value = "128" },
    { .flag = "--storage-shards", .value = "2" },
    { .flag = "--expiry-interval", .value = "250" },
    { .flag = "--expiry-scan", .value = "64" },
    { .flag = "--daemon", .value = "" },
    { .flag = "--install-service", .value = "" },
    { .flag = "--uninstall-service", .value = "" },
    { .flag = "--service-scope", .value = "system" },
    { .flag = "--healthcheck", .value = "" },
    { .flag = "--seed-config", .value = "template.yaml" },
    { .flag = "--migrate-storage", .value = "" },
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

/// A spelling one file-carried row is driven with besides its accepted sample.
struct FileSpelling
{
    std::string_view flag; ///< The row's primary spelling.
    std::string_view text; ///< Text to hand the row, accepted or not.
};

/// Spellings that must reach the configuration identically from a file and from argv.
///
/// **Refusals mostly, and chosen where two parsers disagreed.** The first rows are the
/// ones measured accepted by the daemon's old file reader and refused by argv; the rest
/// are each row's own edges. A path, a secret or free text accepts anything, so its row
/// carries the empty string, which a second parser could as easily have refused.
///
/// No `$`: a path row expands environment references from a file and deliberately not
/// from argv (`FileValue`), which `ConfigMerge_test` asserts on its own.
constexpr auto FileSpellings = std::to_array<FileSpelling>({
    { .flag = "--port", .text = "0x50" },
    { .flag = "--port", .text = "+80" },
    { .flag = "--port", .text = "0" },
    { .flag = "--port", .text = "65536" },
    { .flag = "--bind", .text = "" },
    { .flag = "--bind", .text = "a b" },
    { .flag = "--threads", .text = "0x10" },
    { .flag = "--threads", .text = "-1" },
    { .flag = "--listen-backlog", .text = "0x10" },
    { .flag = "--listen-backlog", .text = "0" },
    { .flag = "--expiry-interval", .text = "0x10" },
    { .flag = "--expiry-interval", .text = "+5" },
    { .flag = "--expiry-interval", .text = "0" },
    { .flag = "--expiry-interval", .text = "86400001" },
    { .flag = "--expiry-scan", .text = "0" },
    { .flag = "--expiry-scan", .text = "1e3" },
    { .flag = "--storage-shards", .text = "0x2" },
    { .flag = "--max-memory", .text = "64k" },
    { .flag = "--max-memory", .text = "5x" },
    { .flag = "--max-memory", .text = "50%" },
    { .flag = "--metrics-port", .text = "0" },
    { .flag = "--metrics-port", .text = " 9999" },
    { .flag = "--metrics-bind", .text = "" },
    { .flag = "--log-level", .text = "Info" },
    { .flag = "--log-level", .text = "verbose" },
    { .flag = "--requirepass", .text = "" },
    { .flag = "--auth-username", .text = "" },
    { .flag = "--tls-cert", .text = "" },
    { .flag = "--tls-key", .text = "" },
    { .flag = "--listen", .text = "127.0.0.1" },
    { .flag = "--listen", .text = "127.0.0.1:0" },
    { .flag = "--listen", .text = "::1:6674" },
    { .flag = "--listen", .text = "[::1]:6674" },
    { .flag = "--listen-tls", .text = "[::1]:70000" },
    { .flag = "--listen-tls", .text = "" },
    { .flag = "--notify-keyspace-events", .text = "" },
    { .flag = "--storage", .text = "" },
    { .flag = "--storage-durability", .text = "FSYNC" },
    { .flag = "--storage-max-value", .text = "1.5m" },
    { .flag = "--storage-max-disk", .text = "-1" },
    { .flag = "--compression", .text = "brotli" },
    { .flag = "--compression-level", .text = "0" },
    { .flag = "--compression-level", .text = "23" },
    { .flag = "--compression-min-bytes", .text = "4q" },
    { .flag = "--memory-compression", .text = "" },
    { .flag = "--memory-compression-level", .text = "+3" },
    { .flag = "--memory-compression-min-bytes", .text = "0x10" },
    { .flag = "--lru-mode", .text = "exact" },
    { .flag = "--cpu-affinity", .text = "per_core" },
});

/// Which door a setting arrives through.
enum class Source : std::uint8_t
{
    File,        ///< A configuration file naming the row's key.
    CommandLine, ///< The row's flag on the command line.
};

/// @param text Text to put in a YAML document as one double-quoted scalar.
/// @return The quoted scalar, so every spelling arrives as the text itself and never
///         as whatever YAML would have made of it bare.
[[nodiscard]] std::string YamlQuoted(std::string_view text)
{
    auto quoted = std::string { "\"" };
    for (auto const c: text)
    {
        if (c == '"' || c == '\\')
            quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

/// Assemble a configuration naming one row once, through one door.
///
/// Through `AssembleEffectiveConfig` for both, because that is what `main` and the
/// reloader call: a helper that applied the file layer directly would pass while the
/// daemon did something else.
/// @param scratch Where the file goes.
/// @param spec The row.
/// @param text The value; ignored for a presence flag, which a file spells `true`.
/// @param source Which door.
/// @return The assembly, or its refusal.
[[nodiscard]] std::expected<EffectiveConfig, ConfigError> AssembleFrom(FastCache::Testing::ScratchDirectory const& scratch,
                                                                       OptionSpec<CliResult> const& spec,
                                                                       std::string_view text,
                                                                       Source source)
{
    auto const presence = spec.arity == Arity::None;
    if (source == Source::CommandLine)
    {
        auto token = presence ? std::string { spec.primary } : std::format("{}={}", spec.primary, text);
        return AssembleEffectiveConfig({}, ConfigSources { .args = { std::move(token) }, .metricsPortEnv = std::nullopt });
    }

    scratch.Write("setting.yaml",
                  std::format("{}: {}\n", spec.yamlKey, presence ? std::string { "true" } : YamlQuoted(text)));
    return AssembleEffectiveConfig(scratch / "setting.yaml", ConfigSources {});
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

TEST_CASE("every setting a file can carry reaches the configuration exactly as its flag does",
          "[config][cli][options][file]")
{
    // **The property #1437 exists for, driven over the whole table.** The daemon's file
    // used to be parsed by a second reader with yaml-cpp's own conversions, and measured
    // on 0.2.0-568 it accepted `port: 0x50`, `port: +80`, `bind: ""`, `bind: "a b"`,
    // `threads: 0x10` and `active_expiry_interval_ms: +5` -- every one refused on argv.
    //
    // Each row is driven with its accepted sample AND with every spelling below, once as
    // a file and once as a command line, and the two must agree: both refuse with the
    // same code, or both accept with the same configuration and the same provenance.
    // Agreement alone would pass a table that refused everything, which is what the
    // accepted sample is for.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-file-argv-parity" };

    auto keyed = std::size_t { 0 };
    for (auto const& spec: CliOptions())
    {
        if (spec.yamlKey.empty())
            continue;
        ++keyed;

        auto texts = std::vector<std::string_view> {};
        for (auto const& sample: Samples)
            if (sample.flag == spec.primary)
                texts.push_back(sample.value);
        REQUIRE(texts.size() == 1);
        for (auto const& spelling: FileSpellings)
            if (spelling.flag == spec.primary)
                texts.push_back(spelling.text);

        // A value row with nothing but its accepted sample would assert agreement on the
        // one input both sides were always going to accept.
        INFO("row: " << spec.primary);
        CHECK((spec.arity == Arity::None || texts.size() > 1));

        for (auto const text: texts)
        {
            INFO("text: `" << text << "`");
            auto const fromFile = AssembleFrom(scratch, spec, text, Source::File);
            auto const fromArgv = AssembleFrom(scratch, spec, text, Source::CommandLine);
            REQUIRE(fromFile.has_value() == fromArgv.has_value());
            if (!fromFile.has_value())
            {
                CHECK(fromFile.error().code == fromArgv.error().code);
                continue;
            }
            // The snapshot names the file it came from and a command line names none:
            // the one difference between the two doors that is correct.
            auto fileConfig = fromFile->Configuration();
            fileConfig.configPath.clear();
            CHECK(fileConfig == fromArgv->Configuration());
            if (spec.explicitBit != nullptr)
            {
                CHECK(fromFile->Named(spec.explicitBit));
                CHECK(fromArgv->Named(spec.explicitBit));
            }
        }
    }

    // Two empty walks agree perfectly, so the walk must have found the rows it is about.
    CHECK(keyed == ConfigFileSettings().size());
    CHECK(keyed > 30);
}

TEST_CASE("every spelling above names a row a file can carry", "[config][cli][options][file]")
{
    // The table is written by hand, so a row renamed out from under it would leave its
    // spellings asserting nothing -- driven at no row, and silently.
    for (auto const& spelling: FileSpellings)
    {
        INFO("flag: " << spelling.flag);
        auto const* const row = FindIfOrNull(
            CliOptions(), [&spelling](OptionSpec<CliResult> const& spec) { return spec.primary == spelling.flag; });
        REQUIRE(row != nullptr);
        CHECK_FALSE(row->yamlKey.empty());
        CHECK(row->arity == Arity::Value);
    }
}

TEST_CASE("a key no option row declares is refused, and so is every key a release retired", "[config][cli][options][file]")
{
    // A key a file carries and nothing reads is a setting an operator believes is in
    // force forever. The walk that applies a key is the walk that refuses one, so there
    // is no gate to disagree with the appliers.
    //
    // The retired keys are the sharper half: `listeners` is where the daemon's endpoints
    // lived until they became `listen:` and `listen_tls:`, and a file still naming it
    // must be told so at start rather than serve the defaults.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-unknown-keys" };
    constexpr auto keys = std::to_array<std::string_view>({
        "prot",
        "--port",
        "storage",
        "listeners",
        "execution_model",
        "threading_model",
        "roles",
    });
    for (auto const key: keys)
    {
        INFO("key: " << key);
        scratch.Write("unknown.yaml", std::format("\"{}\": 1\n", key));
        auto const assembled = AssembleEffectiveConfig(scratch / "unknown.yaml", ConfigSources {});
        REQUIRE_FALSE(assembled.has_value());
        CHECK(assembled.error().code == ConfigErrorCode::UnknownKey);
        CHECK(assembled.error().field == key);
    }
}

TEST_CASE("every setting a file can carry can be compared", "[config][cli][options][reload]")
{
    // `UnreloadableChanges` calls `setting.same` unconditionally for an immutable
    // setting, so a null one there is a crash rather than a missed guard. The
    // option-table half cannot happen -- `yamlKey.empty() == (same == nullptr)` is
    // a static_assert beside the table -- but the PROJECTION is what a caller sees,
    // and one that dropped a comparator would satisfy that assertion perfectly.
    //
    // "A reloadable row is one a file can carry" is NOT restated here: it is the
    // fourth static_assert beside the table, and a runtime copy of a compile-time
    // fact only tells you the build you are running already passed.
    REQUIRE(!ConfigFileSettings().empty());
    for (auto const& setting: ConfigFileSettings())
    {
        INFO("key: " << setting.key);
        CHECK_FALSE(setting.key.empty());
        CHECK(setting.same != nullptr);
    }
}
