// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

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
    std::string advertised;
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
///
/// Its refusal writes a field BY HAND -- `ArgvError` takes none, so this builds the error itself
/// -- because that is the one way left for a parser to try, and the cases below assert that the
/// row's own spelling overwrites it.
/// @param sv The value text.
/// @return The port, or a ConfigError.
[[nodiscard]] std::expected<std::uint16_t, ConfigError> ParseTestPort(std::string_view sv)
{
    std::uint16_t value = 0;
    auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc {} || ptr != sv.data() + sv.size())
        return std::unexpected(ConfigError { .code = ConfigErrorCode::TypeMismatch,
                                             .source = "argv",
                                             .line = 0,
                                             .field = "port",
                                             .context = std::format("not a number: {}", sv) });
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
    // A flag whose value other machines will read, so it has to be text.
    { .primary = "--advertise",
      .arity = Arity::Value,
      .operand = "=<host>",
      .apply = AssignFrom<&TestConfig::advertised, ParseUtf8Text>(),
      .description = "what to tell other machines" },
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
    // The row's own spelling, stamped by `ApplyOneOption` like every other refusal.
    CHECK(parsed.error().field == "--port");
}

TEST_CASE("a bad value surfaces the parser's own error, attributed to the row", "[cli][options]")
{
    auto const parsed = Parse({ "--port=abc" });
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == ConfigErrorCode::TypeMismatch);
    CHECK(parsed.error().context == "not a number: abc");
    CHECK(parsed.error().field == "--port");
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

TEST_CASE("a value other machines will read is accepted as text, in any script", "[cli][options]")
{
    // The rule is about ENCODING and not about ASCII. #141 narrowed nothing else,
    // and a fleet whose members may only name themselves in ASCII would be a second
    // restriction nobody announced.
    auto const ascii = Parse({ "--advertise=build-3.example:6676" });
    REQUIRE(ascii.has_value());
    CHECK(ascii->config.advertised == "build-3.example:6676");

    auto const multiByte = Parse({ "--advertise=gr\xC3\xBC"
                                   "n:6676" });
    REQUIRE(multiByte.has_value());
    CHECK(multiByte->config.advertised
          == "gr\xC3\xBC"
             "n:6676");
}

TEST_CASE("a value other machines will read is refused when it is not text", "[cli][options]")
{
    // Refused where a person typed it. Accepted here it reaches
    // `SchedulerService::Register`, is refused there on every heartbeat forever, and
    // the operator's only recovery is to rename the thing (issue #155).
    auto const legacy = Parse({ "--advertise=gr\xFC"
                                "n:6676" });
    REQUIRE_FALSE(legacy.has_value());

    // The FLAG, which the value parser cannot know: it is a free function shared by
    // every row that uses it, so `ApplyOneOption` stamps the row's own spelling.
    CHECK(legacy.error().field == "--advertise");
    CHECK(legacy.error().source == "argv");

    // And WHICH byte. "not valid UTF-8" about a string the console has already
    // re-rendered tells nobody which character was the problem.
    CHECK(legacy.error().context.contains("0xFC"));
    CHECK(legacy.error().context.contains("offset 2"));

    // Strict in the sense RFC 3629 is, because Core/Utf8.hpp is: a form that only
    // LOOKS like UTF-8 is not text either, and the far end would refuse it too.
    CHECK_FALSE(Parse({ "--advertise=\xED\xA0\x80" }).has_value()); // lone surrogate
    CHECK_FALSE(Parse({ "--advertise=\xC0\x80" }).has_value());     // overlong NUL
}

TEST_CASE("a name other machines will read is refused when empty and otherwise judged as text", "[cli][options]")
{
    // The empty refusal is its OWN refusal, told apart from the encoding one by what it says.
    auto const empty = ParseNonEmptyUtf8Text("");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == ConfigErrorCode::ParseError);
    CHECK(empty.error().context.contains("value is empty"));
    // It names no field: the parser cannot know which row reached it, so the caller stamps it.
    CHECK(empty.error().field.empty());

    // A non-empty value that is not text still meets the encoding refusal, not the empty one.
    auto const legacy = ParseNonEmptyUtf8Text("gr\xFC"
                                              "n");
    REQUIRE_FALSE(legacy.has_value());
    CHECK(legacy.error().context.contains("not valid UTF-8"));
    CHECK_FALSE(legacy.error().context.contains("value is empty"));

    // And a name that is text is taken as it stands, however short.
    auto const oneByte = ParseNonEmptyUtf8Text("f");
    REQUIRE(oneByte.has_value());
    CHECK(*oneByte == "f");
}

TEST_CASE("a field a value parser wrote by hand is overwritten by the row's own spelling", "[cli][options]")
{
    // `ParseTestPort` names `port` by hand, the way a dozen parsers did before `ArgvError` lost
    // its field parameter -- each in a spelling no row used, or naming another row outright. The
    // stamp is UNCONDITIONAL, so what an operator reads is the row they typed, whatever the
    // parser tried to say.
    auto const bad = Parse({ "--port=nope" });
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().field == "--port");
}

namespace
{
/// One file's calls of `UnrowedArgvError`, and why each is about no row.
struct UnrowedCallRow
{
    std::string_view file; ///< Relative to the repository root, forward slashes.
    std::size_t calls;     ///< How many times `UnrowedArgvError(` appears in it.
    std::string_view why;  ///< What the refusals are about, since it is no row's value.
};

/// Every first-party, non-test spelling of `UnrowedArgvError(`: the ONE constructor that still names
/// a field by hand, so the one a literal field could come back through.
///
/// Its stated blind spot, and its direction: a value parser that builds a `ConfigError` itself, with a
/// field, is not counted -- and needs not be, because `ApplyOneOption` overwrites that field (the case
/// above). What this cannot miss is `UnrowedArgvError` in a file listed or not.
constexpr std::array UnrowedCalls {
    UnrowedCallRow { .file = "src/FastCache/Cli/Options.hpp",
                     .calls = 2,
                     .why = "its definition, and ApplyOneOption's argument that matched no row" },
    UnrowedCallRow { .file = "src/apps/compile-cache-testclient/TestClientCli.cpp",
                     .calls = 3,
                     .why = "a missing or unknown sub-command, dispatched before any row; and --port required after "
                            "the rows ran" },
};

/// @param text A whole source file.
/// @return How many times `UnrowedArgvError(` appears in it.
[[nodiscard]] std::size_t UnrowedCallsIn(std::string const& text)
{
    auto count = std::size_t { 0 };
    auto at = text.find("UnrowedArgvError(");
    while (at != std::string::npos)
    {
        ++count;
        at = text.find("UnrowedArgvError(", at + 1);
    }
    return count;
}
} // namespace

TEST_CASE("Every call that names a command-line field by hand is a row saying why it is about no row", "[cli][options]")
{
    auto const root = std::filesystem::path { FASTCACHED_SOURCE_DIR };
    auto filesRead = std::size_t { 0 };
    auto hits = std::vector<std::pair<std::string, std::size_t>> {};
    for (auto const& entry: std::filesystem::recursive_directory_iterator { root / "src" })
    {
        auto const& path = entry.path();
        auto const isSource = path.extension() == ".cpp" || path.extension() == ".hpp";
        if (!entry.is_regular_file() || !isSource || path.filename().string().ends_with("_test.cpp")
            || path.generic_string().contains("/src/tests/"))
            continue;
        ++filesRead;
        std::ifstream input { path, std::ios::binary };
        std::ostringstream read;
        read << input.rdbuf();
        auto const text = std::move(read).str();
        if (auto const calls = UnrowedCallsIn(text); calls != 0)
            hits.emplace_back(std::filesystem::relative(path, root).generic_string(), calls);
    }

    // The walk read the tree: a census that looked at nothing finds nothing.
    CHECK(filesRead > 200);

    for (auto const& [file, calls]: hits)
    {
        INFO(file << " calls UnrowedArgvError " << calls
                  << " time(s). A value parser or an applier refuses with ArgvError, which names no field: "
                     "ApplyOneOption stamps the row's own spelling. If this refusal really is about no row, add an "
                     "UnrowedCalls row saying what it is about.");
        auto const* const row = core::findOrNull(UnrowedCalls, std::string_view { file }, &UnrowedCallRow::file);
        REQUIRE(row != nullptr);
        CHECK(row->calls == calls);
    }
    for (auto const& row: UnrowedCalls)
    {
        INFO(row.file << ": " << row.why);
        CHECK(std::ranges::contains(hits, row.file, [](auto const& hit) { return std::string_view { hit.first }; }));
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
