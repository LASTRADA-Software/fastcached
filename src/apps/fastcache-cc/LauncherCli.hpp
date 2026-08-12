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
/// Every value except `Cohort` is a possible `Command::action`; `Cohort` names a
/// `--show-stats` sub-option and is only ever seen inside `StatsOptions()`.
enum class Action : std::uint8_t
{
    Compile,     ///< The default: front a real compile.
    Help,        ///< Print the usage text.
    Version,     ///< Print the launcher version.
    ShowStats,   ///< Report the recorded statistics.
    ZeroStats,   ///< Discard the statistics log.
    Cohort,      ///< Stats sub-option: restrict the report to one cohort.
    NoArguments, ///< Invoked with nothing at all — usage, to stderr.
    UsageError,  ///< Unknown option or a missing option value.
};

/// One accepted flag.
///
/// This is the single source of truth for the CLI: `ParseTopLevel` matches
/// against these rows and `PrintHelp` renders its synopsis from them, so a flag
/// cannot be accepted without also being documented. Adding a flag is adding a
/// row.
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
    std::string cohortFilter;          ///< From `--cohort`; empty means no filtering.
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

/// Parse the sub-options that may follow `--show-stats`.
///
/// The option table is a parameter rather than a lookup so the generic
/// table-driven paths stay exercisable independently of which options happen to
/// exist today; `ParseTopLevel` passes `StatsOptions()`.
/// @param args The arguments after the `--show-stats` token itself.
/// @param options The table to match each token against.
/// @return A `ShowStats` command, or a usage error.
[[nodiscard]] Command ParseStatsOptions(std::span<std::string const> args, std::span<FlagSpec const> options);

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
