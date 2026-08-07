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

    /// One rendered usage line.
    struct UsageRow
    {
        std::string form;         ///< Invocation form (the left column).
        std::string_view summary; ///< Its one-line description.
    };

    /// Write a titled, column-aligned block of usage lines.
    /// @param stream Destination.
    /// @param title Section heading.
    /// @param rows The lines; must not be empty.
    void EmitSection(std::ostream& stream, std::string_view title, std::span<UsageRow const> rows)
    {
        auto const width =
            std::ranges::max(rows | std::views::transform([](UsageRow const& row) { return row.form.size(); }));
        stream << title << '\n';
        for (auto const& row: rows)
            stream << std::format("  {:<{}}  {}\n", row.form, width, row.summary);
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

    /// The parts of the usage text that describe *why* a setting matters, which a
    /// table cannot carry. Kept as prose deliberately; the flag synopsis above it
    /// is generated.
    constexpr std::string_view EnvironmentHelp = R"(
ENVIRONMENT
  FASTCACHE_ADDR        host:port of the fastcached daemon. Unset means every
                        compile runs uncached -- the build still succeeds, so
                        check this before concluding the cache is working.
  FASTCACHE_SRCROOT     Checkout source root, for keying and path canonicalization.
  FASTCACHE_BUILDTREE   Build output root.
  FASTCACHE_COHORT      Prefetch grouping id (default "default"). Not part of the
                        cache key, so it never partitions the cache.
  FASTCACHE_VERBOSE     Print HIT/MISS and fall-back diagnostics to stderr.
  FASTCACHE_NO_STATS    Do not record invocations to the statistics log.
  FASTCACHE_NO_DIRECT   Disable direct mode (always preprocess to derive the key).
                        Direct mode is on by default: it reaches a cached object by
                        re-hashing the project headers a previous compile recorded,
                        which is far cheaper than preprocessing the translation unit.
  FASTCACHE_TIMEOUT_MS  Per-call deadline, in milliseconds, for every send/recv to
                        the daemon (default 10000; 0 disables it). A daemon that
                        accepts the connection and then stalls mid-reply would
                        otherwise block the compile forever, which would make the
                        cache load-bearing. On expiry the launcher gives up on the
                        cache and compiles for real, like any other cache error.
                        Raise it if a heavily loaded daemon is legitimately slow.
                        This bounds each call, not the whole invocation: direct
                        mode makes a separate manifest round-trip, so one compile
                        against a wedged daemon can wait up to twice this before
                        falling back.

  The statistics log lives under a per-user state directory, located from the
  usual platform variables rather than one of our own:

  LOCALAPPDATA          (Windows) Base for the log directory,
                        %LOCALAPPDATA%\fastcache-cc.
  XDG_STATE_HOME, HOME  (POSIX) Base for the log directory, in that order of
                        preference: $XDG_STATE_HOME/fastcache-cc, else
                        $HOME/.local/state/fastcache-cc. With neither set there is
                        nowhere to record, and statistics are silently disabled.

ADDR, SRCROOT and BUILDTREE must ALL be set to cache; any missing one makes the
launcher run the real compiler and report "missing FASTCACHE_ADDR/SRCROOT/BUILDTREE"
under FASTCACHE_VERBOSE.

Any cache error falls back to a plain real compile: caching is an optimization
and never breaks a build.
)";

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

void PrintHelp(std::ostream& stream)
{
    stream << "fastcache-cc - a compiler launcher over the fastcached compile cache.\n\n";

    std::vector<UsageRow> usage;
    usage.reserve(TopLevelFlags().size() + 1);
    // The compile form is the default rather than a flag, so it has no table row.
    usage.emplace_back("fastcache-cc <compiler> <args...>", "Front a compile (as CMAKE_<LANG>_COMPILER_LAUNCHER).");
    for (auto const& spec: TopLevelFlags())
        usage.emplace_back(std::format("fastcache-cc {}", RenderForms(spec)), spec.summary);
    EmitSection(stream, "USAGE", usage);

    std::vector<UsageRow> options;
    options.reserve(StatsOptions().size());
    for (auto const& spec: StatsOptions())
        options.emplace_back(RenderForms(spec), spec.summary);
    stream << '\n';
    EmitSection(stream, "STATS OPTIONS", options);

    stream << EnvironmentHelp;
}

} // namespace FastCache::Cc
