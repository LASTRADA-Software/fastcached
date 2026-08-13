// SPDX-License-Identifier: Apache-2.0
#include "LauncherCli.hpp"

#include <FastCache/Cli/UsageTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache::Cc;
using FastCache::Arity;
using FastCache::UsageColor;
using FastCache::Testing::StripAnsi;

namespace
{

Command Parse(std::vector<std::string> const& argv)
{
    return ParseTopLevel(std::span<std::string const> { argv });
}

} // namespace

// --- the drift guard --------------------------------------------------------
//
// The original defect was that dispatch and the usage text were independent, so
// -h, /? and a top-level --reset were accepted but undocumented. These two cases
// fail if that ever separates again.

TEST_CASE("every accepted flag and alias appears in the help text")
{
    auto const help = HelpText();
    for (auto const& table: { TopLevelFlags(), StatsOptions() })
    {
        for (auto const& spec: table)
        {
            INFO("flag " << spec.primary);
            CHECK(help.contains(spec.primary));
            for (auto const alias: spec.aliases)
            {
                INFO("alias " << alias << " of " << spec.primary);
                CHECK(help.contains(alias));
            }
        }
    }
}

TEST_CASE("every documented top-level flag and alias dispatches to its own action")
{
    for (auto const& spec: TopLevelFlags())
    {
        INFO("flag " << spec.primary);
        CHECK(Parse({ std::string { spec.primary } }).action == spec.action);
        for (auto const alias: spec.aliases)
        {
            INFO("alias " << alias << " of " << spec.primary);
            CHECK(Parse({ std::string { alias } }).action == spec.action);
        }
    }
}

// --- the flag surface -------------------------------------------------------

TEST_CASE("the short stats aliases match their long forms")
{
    CHECK(Parse({ "-s" }).action == Action::ShowStats);
    CHECK(Parse({ "--show-stats" }).action == Action::ShowStats);
    CHECK(Parse({ "-z" }).action == Action::ZeroStats);
    CHECK(Parse({ "--zero-stats" }).action == Action::ZeroStats);
}

TEST_CASE("help is reachable by all three spellings")
{
    CHECK(Parse({ "--help" }).action == Action::Help);
    CHECK(Parse({ "-h" }).action == Action::Help);
    CHECK(Parse({ "/?" }).action == Action::Help);
}

TEST_CASE("the retired sccache-era names are diagnosed, not spawned as a compiler")
{
    // These used to work; removing them silently would leave a caller spawning
    // "--stats" as if it were a compiler and failing with exit 1.
    for (auto const* retired: { "--stats", "--clear-stats", "--reset" })
    {
        INFO("retired flag " << retired);
        auto const cmd = Parse({ std::string { retired } });
        CHECK(cmd.action == Action::UsageError);
        CHECK(cmd.diagnostic.contains(retired));
    }
}

TEST_CASE("the reset spelling is gone entirely, including as a stats sub-option")
{
    auto const cmd = Parse({ "--show-stats", "--reset" });
    CHECK(cmd.action == Action::UsageError);
}

TEST_CASE("an unknown option is named rather than treated as a compiler")
{
    auto const cmd = Parse({ "--frobnicate" });
    CHECK(cmd.action == Action::UsageError);
    CHECK(cmd.diagnostic.contains("--frobnicate"));
}

// --- the compile path -------------------------------------------------------

TEST_CASE("no arguments at all is its own outcome")
{
    CHECK(Parse({}).action == Action::NoArguments);
}

TEST_CASE("a compiler line is fronted, whatever it contains")
{
    CHECK(Parse({ "g++", "-c", "a.cpp", "-o", "a.o" }).action == Action::Compile);
    CHECK(Parse({ "cl.exe", "/c", "a.cpp" }).action == Action::Compile);
    // The compiler's own flags must reach the compiler, not the launcher.
    CHECK(Parse({ "g++", "--help" }).action == Action::Compile);
    CHECK(Parse({ "g++", "--show-stats" }).action == Action::Compile);
}

TEST_CASE("a leading slash is an absolute path, not an option")
{
    // Deciding option-vs-path from the host rather than from the token has
    // already been a bug; /usr/bin/g++ must stay a compiler on every platform.
    CHECK(Parse({ "/usr/bin/g++", "-c", "a.cpp" }).action == Action::Compile);
    CHECK(Parse({ "/opt/llvm/bin/clang++" }).action == Action::Compile);
}

// --- stats sub-options ------------------------------------------------------

TEST_CASE("the cohort filter accepts both the separated and the joined form")
{
    auto const separated = Parse({ "--show-stats", "--cohort", "ci-main" });
    CHECK(separated.action == Action::ShowStats);
    CHECK(separated.cohortFilter == "ci-main");

    auto const joined = Parse({ "--show-stats", "--cohort=ci-main" });
    CHECK(joined.action == Action::ShowStats);
    CHECK(joined.cohortFilter == "ci-main");

    CHECK(Parse({ "-s", "--cohort", "ci-main" }).cohortFilter == "ci-main");
}

TEST_CASE("a stats report with no options reports every cohort")
{
    auto const cmd = Parse({ "--show-stats" });
    CHECK(cmd.action == Action::ShowStats);
    CHECK(cmd.cohortFilter.empty());
}

TEST_CASE("the cohort filter consumes its value")
{
    // Regression: the value used to be read without being consumed, so the next
    // iteration saw it again. With the old zero-stats spelling in that position
    // this both filtered on the flag name and wiped the log.
    auto const cmd = Parse({ "--show-stats", "--cohort", "--zero-stats" });
    CHECK(cmd.action == Action::ShowStats);
    CHECK(cmd.cohortFilter == "--zero-stats");
}

TEST_CASE("a cohort filter without a value is an error rather than a silent no-op")
{
    auto const cmd = Parse({ "--show-stats", "--cohort" });
    CHECK(cmd.action == Action::UsageError);
    CHECK(cmd.diagnostic.contains("--cohort"));
}

TEST_CASE("a cohort filter with an empty value is an error")
{
    // An empty filter means "every cohort", so accepting it would answer a
    // different question than the one asked.
    CHECK(Parse({ "--show-stats", "--cohort=" }).action == Action::UsageError);
    CHECK(Parse({ "--show-stats", "--cohort", "" }).action == Action::UsageError);
}

TEST_CASE("an unknown stats sub-option is diagnosed")
{
    auto const cmd = Parse({ "--show-stats", "--nope" });
    CHECK(cmd.action == Action::UsageError);
    CHECK(cmd.diagnostic.contains("--nope"));
}

TEST_CASE("a value on a valueless flag is diagnosed")
{
    CHECK(Parse({ "--show-stats=1" }).action == Action::UsageError);
}

TEST_CASE("a valueless stats option is accepted, and rejects a joined value")
{
    // Every stats option that exists today takes a value, so inject a table with
    // a valueless row to exercise the generic path a future option would use.
    static constexpr std::array<std::string_view, 0> NoAliases {};
    static constexpr std::array Options {
        FlagSpec { .action = Action::Cohort,
                   .primary = "--verbose",
                   .aliases = NoAliases,
                   .arity = Arity::None,
                   .operands = "",
                   .summary = "Test-only valueless option." },
    };

    std::vector<std::string> const bare { "--verbose" };
    auto const accepted = ParseStatsOptions(bare, Options);
    CHECK(accepted.action == Action::ShowStats);
    CHECK(accepted.cohortFilter.empty());

    std::vector<std::string> const withValue { "--verbose=1" };
    auto const rejected = ParseStatsOptions(withValue, Options);
    CHECK(rejected.action == Action::UsageError);
    CHECK(rejected.diagnostic.contains("takes no value"));
}

// --- table lookup ------------------------------------------------------------

TEST_CASE("FindFlag matches primaries and aliases only")
{
    CHECK(FindFlag(TopLevelFlags(), "--show-stats") != nullptr);
    CHECK(FindFlag(TopLevelFlags(), "-s") != nullptr);
    CHECK(FindFlag(TopLevelFlags(), "--cohort") == nullptr);
    CHECK(FindFlag(StatsOptions(), "--cohort") != nullptr);
    CHECK(FindFlag(StatsOptions(), "--show-stats") == nullptr);
    CHECK(FindFlag(TopLevelFlags(), "") == nullptr);
}

TEST_CASE("the help text renders the sections it promises")
{
    auto const help = HelpText();
    CHECK(help.contains("USAGE"));
    CHECK(help.contains("STATS OPTIONS"));
    CHECK(help.contains("ENVIRONMENT"));
    CHECK(help.contains("fastcache-cc <compiler> <args...>"));
    // The joined form is documented from the arity, not per row.
    CHECK(help.contains("--cohort=<id>"));
}

TEST_CASE("every environment row is named and described")
{
    REQUIRE_FALSE(LauncherEnvironment().empty());
    for (auto const& spec: LauncherEnvironment())
    {
        INFO("variable " << spec.name);
        CHECK(spec.name.starts_with("FASTCACHE_"));
        CHECK_FALSE(spec.summary.empty());
    }
}

TEST_CASE("plain help carries no ANSI escapes")
{
    // Help on stderr and help through a pipe must stay plain: the e2e scripts
    // grep this output, and a build log is no place for escapes.
    CHECK_FALSE(HelpText(UsageColor::Plain).contains('\x1b'));
}

TEST_CASE("colorized help adds escapes but not one character of text")
{
    auto const plain = HelpText(UsageColor::Plain);
    auto const colored = HelpText(UsageColor::Colored);
    CHECK(colored.contains('\x1b'));
    CHECK(StripAnsi(colored) == plain);
}

TEST_CASE("the two ENVIRONMENT row groups share one column")
{
    // They sit in one section precisely so the prose between them cannot let
    // the halves drift apart; XDG_STATE_HOME, HOME is as wide as the widest
    // FASTCACHE_ name, so a regression here is visible immediately.
    // Anchored on each row's own indent: FASTCACHE_ADDR is also named in the
    // prose below the rows, and a bare search would find whichever came first.
    auto const help = HelpText();
    auto const first = FastCache::Testing::DescriptionColumnOf(help, "FASTCACHE_ADDR");
    auto const second = FastCache::Testing::DescriptionColumnOf(help, "XDG_STATE_HOME, HOME");
    REQUIRE(first != std::string_view::npos);
    REQUIRE(second != std::string_view::npos);
    CHECK(first == second);
}

TEST_CASE("the help text documents every environment variable the launcher reads")
{
    // The hardcoded list is a deliberate independent oracle: help is rendered
    // from LauncherEnvironment(), so checking it against that table would be
    // tautological, while this fails if a row is ever dropped.
    auto const help = HelpText();
    for (auto const* name: { "FASTCACHE_ADDR",
                             "FASTCACHE_SRCROOT",
                             "FASTCACHE_BUILDTREE",
                             "FASTCACHE_COHORT",
                             "FASTCACHE_VERBOSE",
                             "FASTCACHE_NO_STATS",
                             "FASTCACHE_NO_DIRECT",
                             "FASTCACHE_TIMEOUT_MS",
                             "LOCALAPPDATA",
                             "XDG_STATE_HOME",
                             "HOME" })
    {
        INFO("variable " << name);
        CHECK(help.contains(name));
    }
}
