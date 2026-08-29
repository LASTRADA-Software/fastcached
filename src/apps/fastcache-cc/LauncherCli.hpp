// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// What the launcher was asked to do.
///
/// Every value except `Prefetch group` and `OutputPath` is a possible
/// `Command::action`; those two name sub-options (`--show-stats`'s and
/// `--html-stats`'s respectively) and are only ever seen inside
/// `StatsOptions()`/`HtmlStatsOptions()`.
enum class Action : std::uint8_t
{
    Compile,   ///< The default: front a real compile.
    Help,      ///< Print the usage text.
    Version,   ///< Print the launcher version.
    ShowStats, ///< Report the recorded statistics as plain text.
    HtmlStats, ///< Render the recorded statistics as a self-contained HTML dashboard.
    ZeroStats, ///< Discard the statistics log.
    /// Print the toolchain fingerprint for a named compiler and exit.
    ///
    /// Exists because switching dispatch from the compiler's `--version` banner to
    /// an include-tree digest takes the fingerprint out of what an operator can
    /// derive by hand. A compile node computes its own, so this is not how a fleet
    /// is normally configured -- it is how a fingerprint MISMATCH is diagnosed,
    /// which is otherwise invisible: the scheduler simply reports no worker, and
    /// nothing tells you the two digests that failed to match.
    PrintFingerprint,
    PrefetchGroup, ///< Stats sub-option: restrict the report to one prefetch group.
    OutputPath,    ///< `--html-stats` sub-option: where to write the dashboard.
    NoArguments,   ///< Invoked with nothing at all — usage, to stderr.
    UsageError,    ///< Unknown option or a missing option value.
};

/// One accepted flag.
///
/// This is the single source of truth for the CLI: `ParseTopLevel` matches
/// against these rows and `HelpText` renders its synopsis from them, so a flag
/// cannot be accepted without also being documented. Adding a flag is adding a
/// row.
///
/// Distinct from the shared `OptionSpec` because the launcher's flags carry
/// several aliases each and select an `Action` rather than writing a field.
struct FlagSpec
{
    Action action;                             ///< What the flag selects.
    std::string_view primary;                  ///< The documented spelling.
    std::span<std::string_view const> aliases; ///< Accepted synonyms, documented alongside.
    Arity arity;                               ///< Whether a value follows.
    std::string_view operands;                 ///< Display-only suffix, e.g. " <id>"; may be empty.
    std::string_view summary;                  ///< One-line description for the usage text.
};

/// The top-level flags, in the order they are documented.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<FlagSpec const> TopLevelFlags() noexcept;

/// The sub-options accepted after `--show-stats`.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<FlagSpec const> StatsOptions() noexcept;

/// The sub-options accepted after `--html-stats`.
///
/// A separate table from `StatsOptions()` rather than a superset: `--out`
/// means nothing after `--show-stats` (it writes to stdout, always), so
/// accepting it there would silently ignore a flag the caller thought did
/// something.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<FlagSpec const> HtmlStatsOptions() noexcept;

/// Look a token up in a flag table, matching the primary spelling or any alias.
/// @param table The table to search.
/// @param token The command-line token to match.
/// @return The matching row, or nullptr when the token names no flag. The
///         pointer is non-owning and refers to static storage.
[[nodiscard]] FlagSpec const* FindFlag(std::span<FlagSpec const> table, std::string_view token) noexcept;

/// What one launcher invocation resolved to.
struct Command
{
    Action action { Action::Compile }; ///< The selected action.
    std::string groupFilter;           ///< From `--prefetch-group`; empty means no filtering.
    std::string outputPath;            ///< From `--html-stats`'s `--out`; empty means the default path.
    std::string compiler;              ///< From `--print-toolchain-fingerprint`; the compiler to interrogate.
    std::string diagnostic;            ///< Why parsing failed; set iff `action == UsageError`.
};

/// Classify a launcher argument vector.
///
/// Pure: it reads no environment, touches no filesystem, and performs no I/O, so
/// the whole CLI surface is unit-testable without a process or a daemon.
///
/// Only the leading token selects a launcher action; anything unrecognized that
/// does not look like an option is the compiler to front, which is why
/// `g++ --help` still reaches the compiler.
///
/// @param args The arguments with `argv[0]` (the launcher itself) already removed.
/// @return The resolved command.
[[nodiscard]] Command ParseTopLevel(std::span<std::string const> args);

/// Parse the sub-options that may follow `--show-stats` or `--html-stats`.
///
/// The option table is a parameter rather than a lookup so the generic
/// table-driven paths stay exercisable independently of which options happen to
/// exist today; `ParseTopLevel` passes `StatsOptions()` or `HtmlStatsOptions()`.
/// @param args The arguments after the `--show-stats`/`--html-stats` token itself.
/// @param options The table to match each token against.
/// @param baseAction The action the returned command carries on success —
///        `ShowStats` for `--show-stats`, `HtmlStats` for `--html-stats`.
/// @return A command with the given action, or a usage error.
[[nodiscard]] Command ParseStatsOptions(std::span<std::string const> args,
                                        std::span<FlagSpec const> options,
                                        Action baseAction = Action::ShowStats);

/// The `FASTCACHE_*` variable names, spelled once.
///
/// Both the help table and `LoadConfig()` in main.cpp name their variables
/// through these, so the two cannot drift. That mattered the moment they were
/// renamed: the table alone drives `--help`, so changing it without the reader
/// would have left the help text looking correct while the launcher went on
/// reading names nothing sets any more — caching silently off, and no test in
/// the default suite to say so, since only the `smoke`-labelled end-to-end
/// script exercises the reader.
/// Where the cache is when `FASTCACHE_ADDR` says nothing.
///
/// Localhost, so `fastcache-cc` works with no configuration at all: whichever of
/// `fastcached` or `fastcache-compile-node` a developer runs answers here, and the
/// node's `--listen-cache` defaults to this same address for exactly that reason.
///
/// Defaulting to a *remote* address would be indefensible — every translation unit
/// on a machine with nothing listening would pay a connect timeout, in silence. A
/// loopback connect to a closed port is not that: it is refused immediately, with no
/// timeout and no round trip, so a machine running neither daemon pays microseconds
/// per compile rather than seconds. That asymmetry is the whole argument, and it is
/// why this default is localhost and could not be anything else.
///
/// `FASTCACHE_ADDR=` (set but empty) still means *off*, which is what a build that
/// wants no cache at all sets — see `EnvOr`, which treats set-but-empty as unset,
/// and `cmake/portable/CompileCache.cmake`, which passes an empty value to opt out.
constexpr std::string_view DefaultAddr = "127.0.0.1:6674";

namespace EnvName
{
    constexpr std::string_view Addr = "FASTCACHE_ADDR";
    constexpr std::string_view SourceDir = "FASTCACHE_SOURCE_DIR";
    constexpr std::string_view BinaryDir = "FASTCACHE_BINARY_DIR";
    constexpr std::string_view PrefetchGroup = "FASTCACHE_PREFETCH_GROUP";
    constexpr std::string_view Verbose = "FASTCACHE_VERBOSE";
    constexpr std::string_view NoStats = "FASTCACHE_NO_STATS";
    constexpr std::string_view NoDirect = "FASTCACHE_NO_DIRECT";
    constexpr std::string_view TimeoutMs = "FASTCACHE_TIMEOUT_MS";
    constexpr std::string_view DispatchTimeoutMs = "FASTCACHE_DISPATCH_TIMEOUT_MS";
    constexpr std::string_view ConnectTimeoutMs = "FASTCACHE_CONNECT_TIMEOUT_MS";
    constexpr std::string_view MaxStoreBytes = "FASTCACHE_MAX_STORE_BYTES";
    constexpr std::string_view Scheduler = "FASTCACHE_SCHEDULER";
    constexpr std::string_view Token = "FASTCACHE_TOKEN";
    constexpr std::string_view User = "FASTCACHE_USER";
} // namespace EnvName

/// One environment variable the launcher reads.
///
/// The single source of truth for what `--help` documents under ENVIRONMENT.
/// Before this table the names lived in three unlinked places — the reader in
/// main.cpp, a hand-written block of help prose, and a file-header comment —
/// so a variable could be read but undocumented, or documented but never read.
struct EnvVarSpec
{
    std::string_view name;    ///< The variable, spelled exactly as it is read.
    std::string_view summary; ///< Help text; '\n' starts a continuation line.
};

/// The `FASTCACHE_*` variables, in the order `--help` documents them.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<EnvVarSpec const> LauncherEnvironment() noexcept;

/// Render the usage text.
///
/// Every section is generated from the tables above; only the prose explaining
/// *why* a setting matters is written by hand, because a table cannot carry it.
///
/// Returns a string rather than writing to a stream so the caller picks both
/// the destination and the color at one place, and so this module keeps the
/// no-I/O promise `ParseTopLevel` above makes.
/// @param color Whether to emit ANSI SGR escapes; see StdoutSupportsColor.
/// @return The complete usage text, ending in a newline.
[[nodiscard]] std::string HelpText(UsageColor color = UsageColor::Plain);

} // namespace FastCache::Cc
