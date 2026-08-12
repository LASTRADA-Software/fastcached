// SPDX-License-Identifier: Apache-2.0
#include "LauncherCli.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

namespace
{

    constexpr std::array<std::string_view, 2> HelpAliases { "-h", "/?" };
    constexpr std::array<std::string_view, 1> ShowStatsAliases { "-s" };
    constexpr std::array<std::string_view, 1> ZeroStatsAliases { "-z" };
    constexpr std::array<std::string_view, 0> NoAliases {};

    /// The accepted top-level flags, in the order `--help` documents them.
    constexpr std::array TopLevelTable {
        FlagSpec { .action = Action::ShowStats,
                   .primary = "--show-stats",
                   .aliases = ShowStatsAliases,
                   .arity = Arity::None,
                   .operands = " [options]",
                   .summary = "Report cache statistics for this machine." },
        FlagSpec { .action = Action::ZeroStats,
                   .primary = "--zero-stats",
                   .aliases = ZeroStatsAliases,
                   .arity = Arity::None,
                   .operands = "",
                   .summary = "Discard the statistics log." },
        FlagSpec { .action = Action::Help,
                   .primary = "--help",
                   .aliases = HelpAliases,
                   .arity = Arity::None,
                   .operands = "",
                   .summary = "This text." },
        FlagSpec { .action = Action::Version,
                   .primary = "--version",
                   .aliases = NoAliases,
                   .arity = Arity::None,
                   .operands = "",
                   .summary = "The launcher version." },
    };

    /// The sub-options accepted after `--show-stats`.
    constexpr std::array StatsOptionTable {
        FlagSpec { .action = Action::Cohort,
                   .primary = "--cohort",
                   .aliases = NoAliases,
                   .arity = Arity::Value,
                   .operands = " <id>",
                   .summary = "Report only this cohort." },
    };

    /// True when `token` was meant as a launcher option rather than as the
    /// compiler to front.
    ///
    /// A leading `/` is deliberately *not* an option: on POSIX it starts an
    /// absolute path (`/usr/bin/g++`), and deciding that from the host rather
    /// than from the token has already been a bug. Windows' `/?` is the single
    /// spelled-out exception.
    /// @param token The leading command-line token.
    /// @return True when the token should be matched against the flag tables.
    [[nodiscard]] constexpr bool LooksLikeOption(std::string_view token) noexcept
    {
        return token.starts_with('-') || token == "/?";
    }

    /// Render one flag's accepted spellings, e.g. `--show-stats | -s [options]`.
    /// @param spec The flag to render.
    /// @return The left column of its usage line.
    [[nodiscard]] std::string RenderForms(FlagSpec const& spec)
    {
        std::string forms { spec.primary };
        for (auto const alias: spec.aliases)
        {
            forms += " | ";
            forms += alias;
        }
        forms += spec.operands;

        // Every value-taking flag also accepts the joined spelling, so document
        // it from the arity rather than repeating it in each row's summary.
        if (spec.arity == Arity::Value)
        {
            auto const start = spec.operands.find_first_not_of(' ');
            auto const operand =
                start == std::string_view::npos ? std::string_view { "<value>" } : spec.operands.substr(start);
            forms += std::format(" | {}={}", spec.primary, operand);
        }
        return forms;
    }

    /// A command that carries nothing beyond the action it selects.
    /// @param action The resolved action.
    /// @return The command.
    [[nodiscard]] Command Selected(Action action)
    {
        return { .action = action, .cohortFilter = {}, .diagnostic = {} };
    }

    /// A rejected command line.
    /// @param diagnostic Why the arguments could not be used.
    /// @return A `UsageError` command carrying `diagnostic`.
    [[nodiscard]] Command Rejected(std::string diagnostic)
    {
        return { .action = Action::UsageError, .cohortFilter = {}, .diagnostic = std::move(diagnostic) };
    }

    /// The `FASTCACHE_*` variables, in the order `--help` documents them.
    ///
    /// One row per variable, so the names cannot drift from what the launcher
    /// actually reads. The prose below is what a table genuinely cannot carry:
    /// it explains *why* a setting matters rather than merely listing it.
    constexpr std::array EnvironmentTable {
        EnvVarSpec { .name = "FASTCACHE_ADDR",
                     .summary = "host:port of the fastcached daemon. Unset means every\n"
                                "compile runs uncached -- the build still succeeds, so\n"
                                "check this before concluding the cache is working." },
        EnvVarSpec { .name = "FASTCACHE_SRCROOT", .summary = "Checkout source root, for keying and path canonicalization." },
        EnvVarSpec { .name = "FASTCACHE_BUILDTREE", .summary = "Build output root." },
        EnvVarSpec { .name = "FASTCACHE_COHORT",
                     .summary = "Prefetch grouping id (default \"default\"). Not part of the\n"
                                "cache key, so it never partitions the cache." },
        EnvVarSpec { .name = "FASTCACHE_VERBOSE", .summary = "Print HIT/MISS and fall-back diagnostics to stderr." },
        EnvVarSpec { .name = "FASTCACHE_NO_STATS", .summary = "Do not record invocations to the statistics log." },
        EnvVarSpec { .name = "FASTCACHE_NO_DIRECT",
                     .summary = "Disable direct mode (always preprocess to derive the key).\n"
                                "Direct mode is on by default: it reaches a cached object by\n"
                                "re-hashing the project headers a previous compile recorded,\n"
                                "which is far cheaper than preprocessing the translation unit." },
        EnvVarSpec { .name = "FASTCACHE_TIMEOUT_MS",
                     .summary = "Per-call deadline, in milliseconds, for every send/recv to\n"
                                "the daemon (default 10000; 0 disables it). A daemon that\n"
                                "accepts the connection and then stalls mid-reply would\n"
                                "otherwise block the compile forever, which would make the\n"
                                "cache load-bearing. On expiry the launcher gives up on the\n"
                                "cache and compiles for real, like any other cache error.\n"
                                "Raise it if a heavily loaded daemon is legitimately slow.\n"
                                "This bounds each call, not the whole invocation: direct\n"
                                "mode makes a separate manifest round-trip, so one compile\n"
                                "against a wedged daemon can wait up to twice this before\n"
                                "falling back." },
    };

    /// Where the statistics log goes.
    ///
    /// Deliberately UsageEntry rows rather than EnvVarSpec: one row documents
    /// two variables, and Stats.cpp chooses between them behind a platform
    /// `#if` that no flat list of names describes. These are also not ours —
    /// they are the usual per-user state locations.
    constexpr std::array StateDirectoryRows {
        UsageEntry { .term = "LOCALAPPDATA",
                     .description = "(Windows) Base for the log directory,\n"
                                    "%LOCALAPPDATA%\\fastcache-cc." },
        UsageEntry { .term = "XDG_STATE_HOME, HOME",
                     .description = "(POSIX) Base for the log directory, in that order of\n"
                                    "preference: $XDG_STATE_HOME/fastcache-cc, else\n"
                                    "$HOME/.local/state/fastcache-cc. With neither set there is\n"
                                    "nowhere to record, and statistics are silently disabled." },
    };

    /// Lead-in to the state-directory rows.
    constexpr std::string_view StateDirectoryNote =
        "  The statistics log lives under a per-user state directory, located from the\n"
        "  usual platform variables rather than one of our own:";

    /// The two closing paragraphs, each its own block so a blank line separates
    /// them the same way one separates any other pair of blocks.
    constexpr std::string_view RequiredVariablesNote =
        "ADDR, SRCROOT and BUILDTREE must ALL be set to cache; any missing one makes the\n"
        "launcher run the real compiler and report \"missing FASTCACHE_ADDR/SRCROOT/BUILDTREE\"\n"
        "under FASTCACHE_VERBOSE.";

    /// The promise the whole launcher is built around.
    constexpr std::string_view FallbackNote =
        "Any cache error falls back to a plain real compile: caching is an optimization\n"
        "and never breaks a build.";

} // namespace

Command ParseStatsOptions(std::span<std::string const> args, std::span<FlagSpec const> options)
{
    Command cmd = Selected(Action::ShowStats);

    // Walk a shrinking span rather than an index so consuming a flag's value
    // is an explicit step; a value that is never consumed has already been a
    // bug (`--show-stats --cohort --zero-stats` used to both filter on
    // "--zero-stats" and wipe the log).
    auto rest = args;
    while (!rest.empty())
    {
        std::string_view const token = rest.front();
        rest = rest.subspan(1);

        auto const equals = token.find('=');
        auto const name = equals == std::string_view::npos ? token : token.substr(0, equals);
        auto const* const option = FindFlag(options, name);
        if (option == nullptr)
            return Rejected(std::format("unknown option '{}'", token));

        if (option->arity == Arity::None)
        {
            if (equals != std::string_view::npos)
                return Rejected(std::format("option '{}' takes no value", option->primary));
            continue;
        }

        std::string_view value;
        if (equals != std::string_view::npos)
            value = token.substr(equals + 1);
        else if (!rest.empty())
        {
            value = rest.front();
            rest = rest.subspan(1);
        }
        else
            return Rejected(std::format("option '{}' requires a value", option->primary));

        // An empty filter means "every cohort", so accepting one here would
        // silently answer a different question than the one asked.
        if (value.empty())
            return Rejected(std::format("option '{}' requires a non-empty value", option->primary));

        if (option->action == Action::Cohort)
            cmd.cohortFilter = value;
    }
    return cmd;
}

std::span<FlagSpec const> TopLevelFlags() noexcept
{
    return TopLevelTable;
}

std::span<FlagSpec const> StatsOptions() noexcept
{
    return StatsOptionTable;
}

FlagSpec const* FindFlag(std::span<FlagSpec const> table, std::string_view token) noexcept
{
    auto const match = std::ranges::find_if(table, [token](FlagSpec const& spec) {
        return spec.primary == token || std::ranges::find(spec.aliases, token) != spec.aliases.end();
    });
    return match != table.end() ? std::to_address(match) : nullptr;
}

Command ParseTopLevel(std::span<std::string const> args)
{
    if (args.empty())
        return Selected(Action::NoArguments);

    std::string_view const head = args.front();
    if (auto const* const flag = FindFlag(TopLevelFlags(), head))
    {
        if (flag->action == Action::ShowStats)
            return ParseStatsOptions(args.subspan(1), StatsOptions());
        return Selected(flag->action);
    }

    // Not a launcher flag. An option-looking token is a mistake worth naming;
    // anything else is the compiler to front, which is why `g++ --help` still
    // reaches the compiler.
    if (LooksLikeOption(head))
        return Rejected(std::format("unknown option '{}'", head));

    return Selected(Action::Compile);
}

std::span<EnvVarSpec const> LauncherEnvironment() noexcept
{
    return EnvironmentTable;
}

std::string HelpText(UsageColor color)
{
    // Every owning container is filled to completion before any span or view
    // over it is taken: a later push_back would reallocate and leave the
    // document pointing at freed storage.
    std::vector<std::string> forms;
    forms.reserve(TopLevelFlags().size() + StatsOptions().size() + 1);
    // The compile form is the default rather than a flag, so it has no table row.
    forms.emplace_back("fastcache-cc <compiler> <args...>");
    for (auto const& spec: TopLevelFlags())
        forms.push_back(std::format("fastcache-cc {}", RenderForms(spec)));
    for (auto const& spec: StatsOptions())
        forms.push_back(RenderForms(spec));

    std::vector<UsageEntry> usageRows;
    usageRows.reserve(TopLevelFlags().size() + 1);
    usageRows.push_back({ .term = forms.front(), .description = "Front a compile (as CMAKE_<LANG>_COMPILER_LAUNCHER)." });
    for (auto const index: std::views::iota(std::size_t { 0 }, TopLevelFlags().size()))
        usageRows.push_back({ .term = forms[index + 1], .description = TopLevelFlags()[index].summary });

    std::vector<UsageEntry> statsRows;
    statsRows.reserve(StatsOptions().size());
    for (auto const index: std::views::iota(std::size_t { 0 }, StatsOptions().size()))
        statsRows.push_back(
            { .term = forms[TopLevelFlags().size() + 1 + index], .description = StatsOptions()[index].summary });

    std::vector<UsageEntry> environmentRows;
    environmentRows.reserve(LauncherEnvironment().size());
    for (auto const& spec: LauncherEnvironment())
        environmentRows.push_back({ .term = spec.name, .description = spec.summary });

    auto const blocks = std::to_array<UsageBlock>({
        { .entries = usageRows },
        { .entries = statsRows },
        { .entries = environmentRows },
        { .text = StateDirectoryNote },
        { .entries = StateDirectoryRows },
        { .text = RequiredVariablesNote },
        { .text = FallbackNote },
    });

    std::span<UsageBlock const> const allBlocks { blocks };
    auto const sections = std::to_array<UsageSection>({
        { .subject = "fastcache-cc - a compiler launcher over the fastcached compile cache." },
        { .title = "USAGE", .blocks = allBlocks.subspan(0, 1) },
        { .title = "STATS OPTIONS", .blocks = allBlocks.subspan(1, 1) },
        // The three ENVIRONMENT blocks share one section so its two runs of rows
        // keep a common column even though prose sits between them.
        { .title = "ENVIRONMENT", .blocks = allBlocks.subspan(2, 3) },
        { .blocks = allBlocks.subspan(5, 2) },
    });

    return RenderUsage({ .sections = sections }, color);
}

} // namespace FastCache::Cc
