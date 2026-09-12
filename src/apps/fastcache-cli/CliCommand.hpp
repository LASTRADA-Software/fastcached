// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "CliFormat.hpp"
#include "CliVerbs.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file CliCommand.hpp
/// The command line: one option table, one parse, and a help text rendered from both
/// tables rather than written out.
///
/// **Pure.** It reads no environment, touches no filesystem and opens no socket, so
/// every accepted and refused command line is a unit test. The environment is read by
/// `main` and folded in *before* this runs, which is also what keeps the precedence
/// rule to one sentence: defaults, then the environment, then argv.

/// Where colour comes from.
///
/// A three-way choice rather than a boolean because `auto` is the useful default and
/// is not expressible as one: it means *decide from whether stdout is a terminal*,
/// which is a question only `main` can ask.
///
/// Note that colour is the ONLY thing that varies with a TTY here. The output
/// *format* deliberately does not -- a command whose shape changes when it is piped
/// is one that works interactively and breaks in the script somebody wrote by copying
/// it.
enum class ColorChoice : std::uint8_t
{
    Auto,   ///< Colour when stdout is a terminal.
    Always, ///< Colour regardless.
    Never,  ///< No colour.
    Last,
};

/// What the tool was asked to do.
enum class Action : std::uint8_t
{
    RunVerb,     ///< Run `Command::verb`.
    ShowHelp,    ///< Print the usage text.
    ShowVersion, ///< Print this binary's version and exit, without dialling.
    UsageError,  ///< The command line was wrong; `Command::diagnostic` says how.
    Last,
};

/// One parsed command line.
struct Command
{
    Action action { Action::RunVerb };    ///< What to do.
    std::string verb {};                  ///< The verb, when `action == RunVerb`.
    std::vector<std::string> operands {}; ///< Its positional arguments.

    VerbOptions verbOptions {};                   ///< `--ttl`, `--nx`, `--xx`, `--raw`, `--all`.
    OutputFormat format { OutputFormat::Human };  ///< From `--format`.
    std::optional<std::string> absentOverride {}; ///< From `--absent`.
    ColorChoice color { ColorChoice::Auto };      ///< From `--color`.
    bool quiet { false };                         ///< From `--quiet`: drop the advisories.

    /// The cache's data port, where the RESP verbs go.
    Endpoint cache { .host = "127.0.0.1", .port = 6674 };

    /// The admin surface, where `/metrics` is. Unset means the richest stats source is
    /// not asked -- which is reported as *not asked*, never as *did not answer*.
    Endpoint admin {};

    Credential credential {}; ///< From `--token-file` and `--user`.
    DialTimeouts timeouts {}; ///< From `--connect-timeout` and `--timeout`.

    std::string tokenFile {};  ///< From `--token-file`; read by `main`, not here.
    std::string diagnostic {}; ///< Why parsing failed; set iff `action == UsageError`.
};

/// The accepted options, in the order `--help` documents them.
///
/// The single source of truth for this tool's flags: the parser matches these rows and
/// the help text's left column is derived from them, so a flag cannot be accepted
/// without being documented or documented without being accepted.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<OptionSpec<Command> const> CliToolOptions() noexcept;

/// Parse a command line.
///
/// The verb is positional and comes first; everything after it is either an option
/// from the table or one of the verb's operands. An unknown option is an error rather
/// than something quietly ignored.
///
/// @param argv The arguments with the program name already removed.
/// @param seed The command to start from -- defaults, with the environment already
///        folded in. **Taken as a parameter rather than built here so that "the
///        command line wins" is which step runs second**, rather than a per-field
///        merge with a per-field explicit bit. That second shape is the one this
///        repository has shipped a flag that parsed and never merged with, four
///        times.
/// @return The parsed command. Never fails outright: a bad command line comes back as
///         `Action::UsageError` with a diagnostic, because the caller wants to print
///         the help text alongside it either way.
[[nodiscard]] Command ParseCommand(std::span<std::string const> argv, Command seed = {});

/// Apply the `FASTCACHE_*` environment to a command before argv is parsed.
///
/// **A separate step, run first, so "the command line wins" is which one runs second**
/// rather than a per-field merge -- the shape that has shipped a flag that parsed and
/// never merged four times in this repository.
///
/// @param command The command to seed. Modified in place.
/// @param lookup Answers a variable name; returns nullopt when unset. Injected so this
///        is testable without touching the real environment.
void ApplyEnvironment(Command& command, std::optional<std::string> (*lookup)(std::string_view));

/// One wire's verbs, as `--help` prints them.
struct VerbGroup
{
    Wire wire;                          ///< The wire every verb in this group names.
    std::string_view heading;           ///< That wire's `WireSpec::heading`, never spelled here.
    std::vector<VerbSpec const*> verbs; ///< Its verbs, in the verb table's own order; NEVER empty.
};

/// Group @p verbs by the wire each one declares.
///
/// The `COMMANDS` list used to be flat, so *does this verb work against what I am
/// pointed at* -- the one question that decides whether a verb can work at all -- was
/// answered by dialling and reading the refusal, though the table knew it statically.
/// Both orders come from tables and neither is written here: the groups follow `Wire`'s
/// enumerator order, which `RowsInEnumeratorOrder` already pins, and the verbs inside a
/// group follow @p verbs.
///
/// **A wire that no row names produces NO group**, which is what keeps the renderer
/// from printing a heading over nothing. The decision is HERE, in the data, rather than
/// as a skip at the one call site: a group cannot be empty, so there is no line the
/// renderer could forget the check on. It is also the only reason that behaviour is
/// observable at all -- every wire has verbs in this tree, so a check living in
/// `HelpText` could be watched neither accepting nor refusing.
///
/// @param verbs The verb table to group; `Verbs()` in production, a synthetic set in
///        the tests that need a wire with no verbs.
/// @return One group per wire that @p verbs reaches, in `Wire` enumerator order.
[[nodiscard]] std::vector<VerbGroup> GroupVerbsByWire(std::span<VerbSpec const> verbs);

/// Render the usage text.
///
/// Every section is generated: the verbs from `Verbs()` grouped by `GroupVerbsByWire`,
/// the options from `CliToolOptions()`, the formats from `FormatTable`, and the exit
/// codes from `OutcomeTable`. Only the prose explaining *why* a setting matters is
/// written by hand, because a table cannot carry it.
/// @param color Whether to emit ANSI SGR escapes; see StdoutSupportsColor.
/// @return The complete usage text, ending in a newline.
[[nodiscard]] std::string HelpText(UsageColor color = UsageColor::Plain);

/// Render the help for ONE verb.
///
/// Every cell is derived from the verb's own row -- its wire, the connections that wire
/// opens, the protocol command it sends, its operand bounds, the modifiers it honours and
/// whether a compile node answers it -- so a verb cannot be documented here as doing
/// something `VerbTable` does not say it does.
///
/// Reached by `help <command>` and `--help <command>`. The word that names no verb is a
/// USAGE ERROR rather than a page about nothing, which is the half that was missing: the
/// operand used to be discarded, so `help nosuchverb` printed the whole text and exited
/// **0** while the bare `nosuchverb` exits 2.
/// @param verb The verb to explain.
/// @param color Whether to emit ANSI SGR escapes; see StdoutSupportsColor.
/// @return The page, ending in a newline.
[[nodiscard]] std::string HelpTopicText(VerbSpec const& verb, UsageColor color = UsageColor::Plain);

/// The `FASTCACHE_*` variables this tool reads.
struct EnvVarSpec
{
    std::string_view name;    ///< The variable, spelled exactly as it is read.
    std::string_view summary; ///< Help text; `\n` starts a continuation line.
};

/// The variables, in the order `--help` documents them.
///
/// One table driving both the reader and the help, so a variable cannot be read and
/// undocumented or documented and never read -- the failure the launcher's own
/// equivalent table exists to prevent.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<EnvVarSpec const> CliEnvironment() noexcept;

} // namespace FastCache::Cli
