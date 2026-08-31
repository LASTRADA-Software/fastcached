// SPDX-License-Identifier: Apache-2.0
#include "LauncherCli.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
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
        FlagSpec { .action = Action::HtmlStats,
                   .primary = "--html-stats",
                   .aliases = NoAliases,
                   .arity = Arity::None,
                   .operands = " [options]",
                   .summary = "Render cache statistics as a self-contained HTML dashboard." },
        FlagSpec { .action = Action::ZeroStats,
                   .primary = "--zero-stats",
                   .aliases = ZeroStatsAliases,
                   .arity = Arity::None,
                   .operands = "",
                   .summary = "Discard the statistics log." },
        FlagSpec { .action = Action::PrintFingerprint,
                   .primary = "--print-toolchain-fingerprint",
                   .aliases = NoAliases,
                   .arity = Arity::Value,
                   .operands = " <compiler>",
                   .summary = "Print the toolchain fingerprint a dispatched compile would use." },
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
        FlagSpec { .action = Action::PrefetchGroup,
                   .primary = "--prefetch-group",
                   .aliases = NoAliases,
                   .arity = Arity::Value,
                   .operands = " <id>",
                   .summary = "Report only this prefetch group." },
    };

    /// The sub-options accepted after `--html-stats`.
    constexpr std::array HtmlStatsOptionTable {
        FlagSpec { .action = Action::PrefetchGroup,
                   .primary = "--prefetch-group",
                   .aliases = NoAliases,
                   .arity = Arity::Value,
                   .operands = " <id>",
                   .summary = "Report only this prefetch group." },
        FlagSpec { .action = Action::OutputPath,
                   .primary = "--out",
                   .aliases = NoAliases,
                   .arity = Arity::Value,
                   .operands = " <path>",
                   .summary = "Write the dashboard here instead of the default report path." },
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
        return { .action = action, .groupFilter = {}, .outputPath = {}, .compiler = {}, .diagnostic = {} };
    }

    /// A rejected command line.
    /// @param diagnostic Why the arguments could not be used.
    /// @return A `UsageError` command carrying `diagnostic`.
    [[nodiscard]] Command Rejected(std::string diagnostic)
    {
        return { .action = Action::UsageError,
                 .groupFilter = {},
                 .outputPath = {},
                 .compiler = {},
                 .diagnostic = std::move(diagnostic) };
    }

    /// The `FASTCACHE_*` variables, in the order `--help` documents them.
    ///
    /// One row per variable, each naming itself through `EnvName` so it cannot
    /// drift from what the launcher actually reads. The prose below is what a
    /// table genuinely cannot carry: it explains *why* a setting matters rather
    /// than merely listing it.
    constexpr std::array EnvironmentTable {
        EnvVarSpec { .name = EnvName::Addr,
                     .summary = "host:port of the cache. Defaults to 127.0.0.1:6674,\n"
                                "where both fastcached and a local\n"
                                "fastcache-compile-node listen, so a local daemon is\n"
                                "found with no configuration. Set it EMPTY to run\n"
                                "every compile uncached -- the build still succeeds\n"
                                "either way, so check this before concluding the\n"
                                "cache is working." },
        EnvVarSpec { .name = EnvName::SourceDir, .summary = "Checkout source root, for keying and path canonicalization." },
        EnvVarSpec { .name = EnvName::BinaryDir, .summary = "Build output root." },
        EnvVarSpec { .name = EnvName::PrefetchGroup,
                     .summary = "Prefetch grouping id (default \"default\"). Not part of the\n"
                                "cache key, so it never partitions the cache." },
        EnvVarSpec { .name = EnvName::Verbose, .summary = "Print HIT/MISS and fall-back diagnostics to stderr." },
        EnvVarSpec { .name = EnvName::NoStats, .summary = "Do not record invocations to the statistics log." },
        EnvVarSpec { .name = EnvName::Verify,
                     .summary = "Verify one cache hit in every N by compiling the\n"
                                "translation unit again and comparing the object\n"
                                "byte for byte. Unset or 0 is off, which is the\n"
                                "default: a verified hit costs a whole compile.\n"
                                "On a mismatch the FRESHLY compiled object is\n"
                                "used and the key is named on stderr, whether or\n"
                                "not FASTCACHE_VERBOSE is set -- a wrong object\n"
                                "under a correct key is the one thing nobody\n"
                                "should have to have opted in to hearing about.\n"
                                "Which hits are sampled is decided by hashing the\n"
                                "key, not by chance, so the rate holds over a\n"
                                "build and a unit that verified verifies again." },
        EnvVarSpec { .name = EnvName::NoDirect,
                     .summary = "Disable direct mode (always preprocess to derive the key).\n"
                                "Direct mode is on by default: it reaches a cached object by\n"
                                "re-hashing the project headers a previous compile recorded,\n"
                                "which is far cheaper than preprocessing the translation unit." },
        EnvVarSpec { .name = EnvName::ConnectTimeoutMs,
                     .summary = "Deadline, in milliseconds, for OPENING a connection to the\n"
                                "daemon -- name resolution included (default 1000; 0 leaves\n"
                                "the platform's own, which runs to minutes).\n"
                                "\n"
                                "Separate from FASTCACHE_TIMEOUT_MS because they bound\n"
                                "different things and neither implies the other. This one is\n"
                                "short on purpose: a cache that has not accepted within a\n"
                                "second is one the build is better off without, and a name\n"
                                "lookup that hangs would otherwise stall every translation\n"
                                "unit with nothing to say why.\n" },
        EnvVarSpec { .name = EnvName::TimeoutMs,
                     .summary = "Deadline, in milliseconds, for one WHOLE exchange with the\n"
                                "daemon -- or with a scheduler's lease and release verbs --\n"
                                "measured from the request to the last byte of the reply\n"
                                "(default 10000; 0 removes the bound). A daemon that accepts\n"
                                "the connection and then stalls -- or dribbles one byte at a\n"
                                "time, which no per-call ceiling catches -- would otherwise\n"
                                "block the compile forever, which would make the cache\n"
                                "load-bearing. On expiry the launcher gives up on the cache\n"
                                "and compiles for real, like any other cache error. Raise it\n"
                                "if a heavily loaded daemon is legitimately slow.\n"
                                "This bounds one exchange, not the whole invocation: direct\n"
                                "mode makes a separate manifest round-trip, so one compile\n"
                                "against a wedged daemon can wait up to twice this before\n"
                                "falling back. It does NOT bound a remote compile -- see\n"
                                "FASTCACHE_DISPATCH_TIMEOUT_MS." },
        EnvVarSpec { .name = EnvName::DispatchTimeoutMs,
                     .summary = "Deadline, in milliseconds, for one whole COMPILE exchange\n"
                                "with a worker (default 600000 = 10 minutes; 0 removes the\n"
                                "bound).\n"
                                "\n"
                                "Separate from FASTCACHE_TIMEOUT_MS, and far larger, because\n"
                                "the two bound different shapes of conversation. A worker\n"
                                "writes nothing until the compiler has finished, so the\n"
                                "client waits out the entire remote compile in one read:\n"
                                "this is bounded by how long a compiler runs, not by a round\n"
                                "trip. While the two shared a number, every translation unit\n"
                                "taking longer than the cache deadline was abandoned and\n"
                                "rebuilt locally -- exactly the ones worth distributing --\n"
                                "while the worker finished the job anyway.\n"
                                "\n"
                                "Ten minutes because that is the scheduler's own lease\n"
                                "timeout: waiting longer means waiting on a lease it has\n"
                                "already reclaimed. Lower it only if you know your slowest\n"
                                "translation unit, and remember that this is a FLAT ceiling\n"
                                "-- it is also how long a genuinely dead worker takes to be\n"
                                "noticed. On expiry the launcher compiles locally and hands\n"
                                "the lease back, so the cost is one slow translation unit,\n"
                                "not a failed build and not a pinned key.\n"
                                "\n"
                                "The launcher is one process per translation unit, so this\n"
                                "variable is a RUNTIME setting: change it and the next\n"
                                "compile picks it up. Nothing needs reloading or\n"
                                "restarting." },
        EnvVarSpec { .name = EnvName::MaxStoreBytes,
                     .summary = "Largest compiled result, in bytes, the launcher will offer\n"
                                "to the daemon (default 268435456 = 256 MiB; 0 means no\n"
                                "limit). A bigger result is left uncached: the compile still\n"
                                "succeeds and nothing is sent. The default matches the\n"
                                "daemon's own --storage-max-value default, which is a\n"
                                "deliberate coincidence and not a negotiation -- there is no\n"
                                "handshake, so a launcher cannot ask. Raise BOTH to cache a\n"
                                "result larger than this; raising only one leaves the other\n"
                                "refusing." },
        EnvVarSpec { .name = EnvName::Scheduler,
                     .summary = "host:port of a fastcached scheduling endpoint (its\n"
                                "--listen-scheduler port). Unset means every miss compiles\n"
                                "locally, which is the behaviour without this feature.\n"
                                "On a miss the launcher asks the scheduler for a worker and\n"
                                "sends it the preprocessed translation unit. EVERY refusal\n"
                                "-- no matching toolchain, no free slot, another client\n"
                                "already compiling this key, an unreachable node -- falls\n"
                                "back to a local compile, so distribution cannot fail a\n"
                                "build. A worker that reports a FAILED compile is retried\n"
                                "locally before its diagnostics are believed." },
        EnvVarSpec { .name = EnvName::Token,
                     .summary = "Shared secret presented to a daemon started with\n"
                                "--requirepass. Unset means no credential is sent, which is\n"
                                "correct against a daemon that requires none. Setting it\n"
                                "against a daemon that requires none is also fine -- such a\n"
                                "daemon accepts the credential and ignores it. Costs no\n"
                                "round trip: it is pipelined ahead of the real command\n"
                                "rather than awaited." },
        EnvVarSpec { .name = EnvName::User,
                     .summary = "Username to accompany FASTCACHE_TOKEN. Unset (the usual\n"
                                "case) authenticates against the secret alone, which is what\n"
                                "--requirepass configures. Ignored without a token." },
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

    /// Lead-in to the state-directory rows. Its nesting is the block's `indent`,
    /// not spaces baked into the literal.
    constexpr std::string_view StateDirectoryNote =
        "The statistics log lives under a per-user state directory, located from the\n"
        "usual platform variables rather than one of our own:";

    /// The two closing paragraphs, each its own block so a blank line separates
    /// them the same way one separates any other pair of blocks.
    constexpr std::string_view RequiredVariablesNote =
        "ADDR, SOURCE_DIR and BINARY_DIR must ALL be set to cache; any missing one makes\n"
        "the launcher run the real compiler and report\n"
        "\"missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR\" under FASTCACHE_VERBOSE.";

    /// The promise the whole launcher is built around, and the limit of it.
    ///
    /// The second sentence is here because the first one used to be read as
    /// covering more than it says: a cache error fell back to a LOCAL compile,
    /// dispatch included, so an unreachable cache silently switched a healthy
    /// fleet off (issue #236).
    constexpr std::string_view FallbackNote =
        "Any cache error falls back to a real compile: caching is an optimization and\n"
        "never breaks a build. It does not stop distribution -- a cache that is\n"
        "unreachable or that refuses is reported, and the compile is still dispatched.";

} // namespace

Command ParseStatsOptions(std::span<std::string const> args, std::span<FlagSpec const> options, Action baseAction)
{
    Command cmd = Selected(baseAction);

    // Walk a shrinking span rather than an index so consuming a flag's value
    // is an explicit step; a value that is never consumed has already been a
    // bug (`--show-stats --prefetch-group --zero-stats` used to both filter on
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

        // An empty filter means "every prefetch group", so accepting one here would
        // silently answer a different question than the one asked.
        if (value.empty())
            return Rejected(std::format("option '{}' requires a non-empty value", option->primary));

        if (option->action == Action::PrefetchGroup)
            cmd.groupFilter = value;
        else if (option->action == Action::OutputPath)
            cmd.outputPath = value;
    }
    return cmd;
}

std::span<FlagSpec const> TopLevelFlags() noexcept
{
    return TopLevelTable;
}

std::span<FlagSpec const> HtmlStatsOptions() noexcept
{
    return HtmlStatsOptionTable;
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
        if (flag->action == Action::HtmlStats)
            return ParseStatsOptions(args.subspan(1), HtmlStatsOptions(), Action::HtmlStats);
        if (flag->action == Action::PrintFingerprint)
        {
            // The operand is required: without a compiler there is nothing to
            // fingerprint, and defaulting to something plausible ("cc") would
            // print a digest for a toolchain the operator did not ask about --
            // which, in the one command whose purpose is diagnosing a mismatch,
            // is worse than refusing.
            if (args.size() < 2)
                return Rejected("--print-toolchain-fingerprint needs a compiler");
            auto selected = Selected(flag->action);
            selected.compiler = args[1];
            return selected;
        }
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
    UsageRows usageRows;
    // The compile form is the default rather than a flag, so it has no table row.
    usageRows.Add("fastcache-cc <compiler> <args...>", "Front a compile (as CMAKE_<LANG>_COMPILER_LAUNCHER).");
    for (auto const& spec: TopLevelFlags())
        usageRows.Add(std::format("fastcache-cc {}", RenderForms(spec)), spec.summary);

    UsageRows statsRows;
    for (auto const& spec: StatsOptions())
        statsRows.Add(RenderForms(spec), spec.summary);

    UsageRows htmlStatsRows;
    for (auto const& spec: HtmlStatsOptions())
        htmlStatsRows.Add(RenderForms(spec), spec.summary);

    UsageRows environmentRows;
    for (auto const& spec: LauncherEnvironment())
        environmentRows.Add(std::string { spec.name }, spec.summary);

    auto const blocks = std::to_array<UsageBlock>({
        { .entries = usageRows.Rows() },
        { .entries = statsRows.Rows() },
        { .entries = htmlStatsRows.Rows() },
        { .entries = environmentRows.Rows() },
        { .text = StateDirectoryNote, .textIndent = 2 },
        { .entries = StateDirectoryRows },
        { .text = RequiredVariablesNote },
        { .text = FallbackNote },
    });

    std::span<UsageBlock const> const allBlocks { blocks };
    auto const sections = std::to_array<UsageSection>({
        { .subject = "fastcache-cc - a compiler launcher over the fastcached compile cache." },
        { .title = "USAGE", .blocks = allBlocks.subspan(0, 1) },
        { .title = "STATS OPTIONS", .blocks = allBlocks.subspan(1, 1) },
        { .title = "HTML STATS OPTIONS", .blocks = allBlocks.subspan(2, 1) },
        // The three ENVIRONMENT blocks share one section so its two runs of rows
        // keep a common column even though prose sits between them.
        { .title = "ENVIRONMENT", .blocks = allBlocks.subspan(3, 3) },
        { .blocks = allBlocks.subspan(6, 2) },
    });

    return RenderUsage({ .sections = sections }, color);
}

} // namespace FastCache::Cc
