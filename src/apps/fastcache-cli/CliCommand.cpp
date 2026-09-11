// SPDX-License-Identifier: Apache-2.0
#include "CliCommand.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <deque>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// One colour choice's spelling.
    struct ColorChoiceSpec
    {
        ColorChoice choice;    ///< The enumerator this row describes.
        std::string_view name; ///< The `--color=` spelling.
    };

    /// The colour choices, one row per enumerator, in enumerator order.
    inline constexpr EnumTable<ColorChoice, ColorChoiceSpec> ColorChoiceTable { {
        { .choice = ColorChoice::Auto, .name = "auto" },
        { .choice = ColorChoice::Always, .name = "always" },
        { .choice = ColorChoice::Never, .name = "never" },
    } };

    static_assert(RowsInEnumeratorOrder(ColorChoiceTable, &ColorChoiceSpec::choice),
                  "ColorChoiceTable must hold one row per ColorChoice, in enumerator order");

    /// Every accepted `--color=` spelling, comma-separated.
    /// @return The list.
    [[nodiscard]] std::string ColorChoiceNames()
    {
        std::string out;
        for (auto const& row: ColorChoiceTable)
        {
            if (!out.empty())
                out += ", ";
            out += row.name;
        }
        return out;
    }

    /// Parse a `--format` value.
    ///
    /// Names no field: `ApplyOneOption` stamps the row's own spelling into an error
    /// whose field the parser left empty, which is what stops a shared parser from
    /// claiming to be a flag it was merely reached through.
    /// @param value The value text.
    /// @return The format, or why it is not one.
    [[nodiscard]] std::expected<OutputFormat, ConfigError> ParseFormatName(std::string_view value)
    {
        if (auto const format = FormatFromName(value); format.has_value())
            return *format;
        return std::unexpected(
            ArgvError(ConfigErrorCode::ParseError, {}, std::format("expected one of: {}", FormatNames())));
    }

    /// Parse a `--color` value.
    /// @param value The value text.
    /// @return The choice, or why it is not one.
    [[nodiscard]] std::expected<ColorChoice, ConfigError> ParseColorChoice(std::string_view value)
    {
        for (auto const& row: ColorChoiceTable)
            if (row.name == value)
                return row.choice;
        return std::unexpected(
            ArgvError(ConfigErrorCode::ParseError, {}, std::format("expected one of: {}", ColorChoiceNames())));
    }

    /// Parse a non-negative count of something.
    /// @param value The value text.
    /// @return The value, or why it is not a count.
    [[nodiscard]] std::expected<std::int64_t, ConfigError> ParseCount(std::string_view value)
    {
        if (value.empty() || !std::ranges::all_of(value, [](char ch) { return ch >= '0' && ch <= '9'; }))
            return std::unexpected(ArgvError(ConfigErrorCode::ParseError, {}, "expected a non-negative whole number"));
        std::int64_t parsed = 0;
        auto const* const first = value.data();
        auto const* const last = first + value.size();
        auto const [ptr, ec] = std::from_chars(first, last, parsed);
        if (ec != std::errc {} || ptr != last)
            return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, {}, "the number is too large"));
        return parsed;
    }

    /// An applier that parses `host:port` into an `Endpoint` member of `Command`.
    ///
    /// `ParseDialEndpoint`, and not `SplitHostPort` or `ParseEndpoint`: this value is
    /// something the tool will DIAL, and a bare port names no machine. That is the
    /// project's own rule about which predicate an address flag takes, and the reason
    /// it exists is that the opened-surface and dialled-surface grammars differ.
    /// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
    template <auto Field>
    [[nodiscard]] constexpr auto AssignEndpoint() noexcept
    {
        return [](Command& command, std::string_view value) -> std::expected<void, ConfigError> {
            auto const parsed = ParseDialEndpoint(value);
            if (!parsed.has_value())
                return std::unexpected(
                    ArgvError(ConfigErrorCode::ParseError, {}, "expected host:port (a bare port names no machine)"));
            (command.*Field) = Endpoint { .host = parsed->first, .port = parsed->second };
            return {};
        };
    }

    /// An applier that parses milliseconds into a nested `DialTimeouts` member.
    ///
    /// A bespoke applier because the target is two levels down (`timeouts.connect`),
    /// which `AssignFrom`'s member pointer cannot name.
    /// @return The applier.
    template <auto Field>
    [[nodiscard]] constexpr auto AssignMilliseconds() noexcept
    {
        return [](Command& command, std::string_view value) -> std::expected<void, ConfigError> {
            auto const parsed = ParseCount(value);
            if (!parsed.has_value())
                return std::unexpected(parsed.error());
            (command.timeouts.*Field) = std::chrono::milliseconds { *parsed };
            return {};
        };
    }

    /// An applier that parses a TTL in seconds into the verb options.
    /// @return The applier.
    [[nodiscard]] constexpr auto AssignTtl() noexcept
    {
        return [](Command& command, std::string_view value) -> std::expected<void, ConfigError> {
            auto const parsed = ParseCount(value);
            if (!parsed.has_value())
                return std::unexpected(parsed.error());
            command.verbOptions.ttlSeconds = *parsed;
            return {};
        };
    }

    /// An applier that parses the AUTH username into the nested credential.
    /// @return The applier.
    [[nodiscard]] constexpr auto AssignUsername() noexcept
    {
        return [](Command& command, std::string_view value) -> std::expected<void, ConfigError> {
            command.credential.username = std::string { value };
            return {};
        };
    }

    /// An applier that sets one of the verb options' flags.
    /// @return The applier.
    template <auto Field>
    [[nodiscard]] constexpr auto SetVerbFlag() noexcept
    {
        return [](Command& command, std::string_view) -> std::expected<void, ConfigError> {
            (command.verbOptions.*Field) = true;
            return {};
        };
    }

    /// The accepted options, in the order `--help` documents them.
    constexpr auto Options = std::to_array<OptionSpec<Command>>({
        { .primary = "--addr",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AssignEndpoint<&Command::cache>(),
          .description = "the cache's data port (default {addr}, or $FASTCACHE_ADDR)" },
        { .primary = "--admin-addr",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AssignEndpoint<&Command::admin>(),
          .description = "the admin surface, where /metrics is. An OVERRIDE: against\n"
                         "a node the port is discovered over 0xFC, and `stats`\n"
                         "names whichever rung answered either way" },
        { .primary = "--format",
          .arity = Arity::Value,
          .operand = "=<name>",
          .apply = AssignFrom<&Command::format, ParseFormatName>(),
          .description = "how to write the answer; see FORMATS (default human)" },
        { .primary = "--absent",
          .arity = Arity::Value,
          .operand = "=<text>",
          .apply = AssignFrom<&Command::absentOverride, ParseText>(),
          .description = "what an absent cell renders as, for the line-oriented\n"
                         "formats. JSON always uses null and ignores this" },
        { .primary = "--color",
          .arity = Arity::Value,
          .operand = "=<when>",
          .apply = AssignFrom<&Command::color, ParseColorChoice>(),
          .description = "auto, always or never (default auto; NO_COLOR is honoured)" },
        { .primary = "--quiet",
          .alias = "-q",
          .apply = SetTrue<&Command::quiet>(),
          .description = "drop the remarks that would go to stderr" },
        { .primary = "--token-file",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&Command::tokenFile, ParseText>(),
          .description = "read the credential from this file rather than\n"
                         "$FASTCACHE_TOKEN, which is visible in the environment" },
        { .primary = "--user",
          .arity = Arity::Value,
          .operand = "=<name>",
          .apply = AssignUsername(),
          .description = "username for the two-argument AUTH form; rarely needed" },
        { .primary = "--ttl",
          .arity = Arity::Value,
          .operand = "=<seconds>",
          .apply = AssignTtl(),
          .description = "expiry for `set`" },
        { .primary = "--nx",
          .apply = SetVerbFlag<&VerbOptions::onlyIfAbsent>(),
          .description = "`set` only if the key does not exist" },
        { .primary = "--xx",
          .apply = SetVerbFlag<&VerbOptions::onlyIfPresent>(),
          .description = "`set` only if the key already exists" },
        { .primary = "--raw",
          .apply = SetVerbFlag<&VerbOptions::raw>(),
          .description = "`get` writes the value's bytes to stdout untouched,\n"
                         "with no formatting and no trailing newline" },
        { .primary = "--all",
          .apply = SetVerbFlag<&VerbOptions::everything>(),
          .description = "`flush` clears every database, not just the current one" },
        { .primary = "--connect-timeout",
          .arity = Arity::Value,
          .operand = "=<ms>",
          .apply = AssignMilliseconds<&DialTimeouts::connect>(),
          .description = "cap on establishing the connection (default 5000)" },
        { .primary = "--timeout",
          .arity = Arity::Value,
          .operand = "=<ms>",
          .apply = AssignMilliseconds<&DialTimeouts::io>(),
          .description = "cap on each read and write (default 10000)" },
        { .primary = "--help",
          .alias = "-h",
          .select = SelectOutcome<&Command::action, Action::ShowHelp>(),
          .flow = ParseFlow::Stop,
          .description = "show this help and exit" },
        { .primary = "--version",
          .alias = "-V",
          .select = SelectOutcome<&Command::action, Action::ShowVersion>(),
          .flow = ParseFlow::Stop,
          .description = "print this client's version and exit, without dialling" },
    });

    static_assert(TableIsWellFormed<Command>(Options),
                  "fastcache-cli option table is malformed: a row is undocumented, a value flag has no operand, "
                  "a row does nothing, or a spelling is claimed twice");

    /// The environment variables, in the order `--help` documents them.
    constexpr auto Environment = std::to_array<EnvVarSpec>({
        { .name = "FASTCACHE_ADDR",
          .summary = "the cache's data port, as host:port. The same variable\n"
                     "fastcache-cc reads, so a machine configured for one is\n"
                     "configured for both. Set but empty means 'use the default'" },
        { .name = "FASTCACHE_ADMIN_ADDR", .summary = "the admin surface, as host:port; see --admin-addr" },
        { .name = "FASTCACHE_TOKEN", .summary = "the credential to present. --token-file is preferable" },
        { .name = "FASTCACHE_USER", .summary = "username for the two-argument AUTH form" },
        { .name = "NO_COLOR",
          .summary = "set to anything non-empty to suppress colour. It governs the\n"
                     "DEFAULT, so an explicit --color=always still colours, which\n"
                     "is what the NO_COLOR convention asks for" },
    });

    /// Prose the tables cannot carry.
    constexpr auto Notes = std::to_array<std::string_view>({
        "The output FORMAT does not change when stdout is a pipe. Only colour does.\n"
        "A command whose shape depends on whether it is piped is one that works by\n"
        "hand and breaks in the script somebody wrote by copying it.",

        "An absent cell is not a zero. A tier the cache does not run, or a field the\n"
        "chosen stats source cannot supply, renders as a dash in the human format and\n"
        "as null in JSON -- never as 0, which would be a claim. A counter that has not\n"
        "counted anything does render 0, because that is the truth about events that\n"
        "did not happen.",

        "Remarks go to stderr in every format, so stdout stays parseable. Redirect it\n"
        "away with 2>/dev/null, or pass --quiet.",

        "Reading TSV with `IFS=$'\\t' read` does not work when a field can be absent:\n"
        "tab is IFS whitespace, so an empty field collapses and shifts every field\n"
        "after it. Use --absent to name a placeholder, or prefer csv or json.",
    });

    /// The verb's invocation form, for the left column of the COMMANDS block.
    /// @param verb The verb.
    /// @return The term.
    [[nodiscard]] std::string RenderVerb(VerbSpec const& verb)
    {
        return std::format("{}{}", verb.name, verb.operands);
    }

    /// One modifier's flag spelling and the bit it sets, for the applicability check.
    struct ModifierSpec
    {
        std::uint8_t bit;        ///< The `Modifier` bit.
        std::string_view flag;   ///< The flag an operator typed.
        bool VerbOptions::* set; ///< The field to read; null for `--ttl`, which is not a bool.
    };

    /// The modifiers, so the applicability refusal names the flag the operator typed.
    ///
    /// A table rather than four `if`s, and the flag spelling comes from it rather than
    /// being written into a message -- a refusal naming a flag that has been renamed is
    /// worse than no refusal, because it sends the reader looking for something that is
    /// not there.
    constexpr auto Modifiers = std::to_array<ModifierSpec>({
        // `--ttl` had no row although the struct's own `set` field was documented as
        // *"null for `--ttl`, which is not a bool"* -- the column anticipated the row and
        // the row was never written. Nothing was wrong while `UnhonouredModifier` was the
        // only reader, because it carries a second, hand-written `--ttl` clause below; it
        // goes wrong the moment a SECOND reader treats the table as the modifier list,
        // which `HelpTopicText` does. `set` staying null is what keeps the two readers
        // honest: the applicability loop skips the row it cannot read, and the help
        // renderer, which only ever wants the spelling, does not.
        { .bit = Modifier::Ttl, .flag = "--ttl", .set = nullptr },
        { .bit = Modifier::Exclusivity, .flag = "--nx", .set = &VerbOptions::onlyIfAbsent },
        { .bit = Modifier::Exclusivity, .flag = "--xx", .set = &VerbOptions::onlyIfPresent },
        { .bit = Modifier::Raw, .flag = "--raw", .set = &VerbOptions::raw },
        { .bit = Modifier::Everything, .flag = "--all", .set = &VerbOptions::everything },
    });

    /// One connection a verb's wire needs, and the word the help calls it.
    struct ConnectionSpec
    {
        bool WireSpec::* needed; ///< The `WireSpec` column that says whether it is opened.
        std::string_view name;   ///< What to call it in the help.
    };

    /// The connections, so the per-verb help reads the three columns rather than `wire`.
    ///
    /// `stats` is why this is not derived from the wire name: its row needs the RESP port
    /// AND the `0xFC` one, because the ladder's `/metrics` rung asks the node where its
    /// admin surface is. A reader told only *wire: stats* cannot see that two addresses
    /// are in play, which is the first thing they need when one of the two is wrong.
    constexpr auto Connections = std::to_array<ConnectionSpec>({
        { .needed = &WireSpec::needsResp, .name = "resp" },
        { .needed = &WireSpec::needsMemcached, .name = "memcached-text" },
        { .needed = &WireSpec::needsNode, .name = "0xFC" },
    });

    /// What a compile node does with @p verb, for its page's last row.
    ///
    /// THREE states, and reading `nodeFallback` alone gives two. That column answers *is
    /// there a SECOND answer once the primary wire has already failed against a compile
    /// node*, which is a question a verb whose primary wire IS the node does not have --
    /// so `node` and `node-metrics`, the two verbs whose whole subject is a compile node,
    /// rendered as *refuses it by name*. A confident wrong answer, on the page written to
    /// stop an operator having to dial a machine to find out.
    /// @param verb The verb.
    /// @return The cell's text.
    [[nodiscard]] std::string_view NodeAnswerFor(VerbSpec const& verb) noexcept
    {
        if (verb.wire == Wire::Node)
            return "answered: this is a node verb";
        if (verb.nodeFallback != nullptr)
            return "answered: the row carries a fallback";
        return "refused by name";
    }

    /// Refuse a modifier the verb does not honour.
    /// @param command The parsed command.
    /// @param verb The verb it named.
    /// @return The diagnostic, or empty when every modifier given is applicable.
    [[nodiscard]] std::string UnhonouredModifier(Command const& command, VerbSpec const& verb)
    {
        for (auto const& modifier: Modifiers)
        {
            // The `--ttl` row carries no bool to read; whether it was given is a
            // comparison against `TtlUnset`, which the clause after this loop makes.
            if (modifier.set == nullptr)
                continue;
            if (!(command.verbOptions.*modifier.set))
                continue;
            if ((verb.modifiers & modifier.bit) == 0)
                return std::format("{} means nothing for `{}`", modifier.flag, verb.name);
        }
        if (command.verbOptions.ttlSeconds != TtlUnset && (verb.modifiers & Modifier::Ttl) == 0)
            return std::format("--ttl means nothing for `{}`", verb.name);
        return {};
    }
} // namespace

std::span<OptionSpec<Command> const> CliToolOptions() noexcept
{
    return Options;
}

std::span<EnvVarSpec const> CliEnvironment() noexcept
{
    return Environment;
}

void ApplyEnvironment(Command& command, std::optional<std::string> (*lookup)(std::string_view))
{
    /// Set-but-empty counts as unset, the way `fastcache-cc`'s own reader treats it: a
    /// build that wants no configuration should not have to unset a variable it never
    /// set.
    auto const value = [lookup](std::string_view name) -> std::optional<std::string> {
        auto found = lookup(name);
        if (found.has_value() && found->empty())
            return std::nullopt;
        return found;
    };

    auto const endpoint = [&command, &value](std::string_view name, Endpoint& target) {
        auto const text = value(name);
        if (!text.has_value())
            return;
        auto const parsed = ParseDialEndpoint(*text);
        if (!parsed.has_value())
        {
            command.action = Action::UsageError;
            command.diagnostic = std::format("${} is not host:port: {}", name, *text);
            return;
        }
        target = Endpoint { .host = parsed->first, .port = parsed->second };
    };

    endpoint("FASTCACHE_ADDR", command.cache);
    endpoint("FASTCACHE_ADMIN_ADDR", command.admin);

    if (auto const token = value("FASTCACHE_TOKEN"); token.has_value())
        command.credential.secret = *token;
    if (auto const user = value("FASTCACHE_USER"); user.has_value())
        command.credential.username = *user;
}

namespace
{

    /// A bare word that asks the same question as a flag.
    struct WordAlias
    {
        std::string_view word; ///< What the operator typed in the command position.
        Action action;         ///< The action it selects.
        std::string_view flag; ///< The flag it is an alias FOR, for the help text.
    };

    /// Bare words accepted in the command position.
    ///
    /// **`help` is the word people type**, and answering it with *unknown command* while
    /// printing the help text anyway was the worst of both: exit 2 for a question that was
    /// understood, the text on stderr where a pipe swallows it, and uncoloured because the
    /// usage-error path is deliberately plain. The text appearing made it look like it had
    /// worked, so nothing about the output said the exit code was 2.
    ///
    /// A TABLE rather than a comparison, so the next such word -- `--version`'s bare
    /// spelling is the obvious candidate, and is deliberately NOT here because `version` is
    /// already a VERB that dials a server and reports both ends -- is a row rather than a
    /// branch in a parse loop.
    constexpr std::array WordAliases {
        WordAlias { .word = "help", .action = Action::ShowHelp, .flag = "--help" },
    };

    /// A question about this BINARY, and what a bare word after it may name.
    ///
    /// `--help get` and `help get` ask for help ABOUT `get`; `--version get` asks nothing
    /// at all. So the arity of what follows is a property of the settled ACTION, and it
    /// is a row rather than a comparison so the next such question has to state its own
    /// answer instead of inheriting `ShowHelp`'s by being written nearby.
    struct SettledQuestion
    {
        Action action;             ///< The action that settled the parse.
        std::string_view spelling; ///< Its canonical spelling, for the diagnostic.
        bool takesTopic;           ///< Whether ONE trailing bare word is meaningful.
        std::string_view subject;  ///< What that word names; empty iff `!takesTopic`.
    };

    /// The settled actions, and what each accepts after it.
    ///
    /// Deliberately NOT an `EnumTable` over `Action`: the subject is *the actions that
    /// settle*, which is two of the four, and rows for `RunVerb` and `UsageError` would
    /// be statements nothing can reach or test.
    constexpr auto SettledQuestions = std::to_array<SettledQuestion>({
        { .action = Action::ShowHelp, .spelling = "help", .takesTopic = true, .subject = "command" },
        { .action = Action::ShowVersion, .spelling = "--version", .takesTopic = false, .subject = "" },
    });

    /// The row for @p action, or nullptr when it has none.
    ///
    /// A missing row means a trailing word is REFUSED rather than discarded, which is the
    /// safe direction: a new stopping flag that nobody gave a row gets a diagnostic
    /// naming the operand, not the silence this whole change is about.
    /// @param action The settled action.
    /// @return Its row, or nullptr.
    [[nodiscard]] SettledQuestion const* FindSettledQuestion(Action action) noexcept
    {
        for (auto const& row: SettledQuestions)
            if (row.action == action)
                return &row;
        return nullptr;
    }

    /// Refuse or accept the bare words that followed a settled action.
    ///
    /// @param command The parsed command, whose `operands` hold the trailing words.
    /// @return The diagnostic, or empty when what followed is acceptable.
    [[nodiscard]] std::string UnacceptableTopic(Command const& command)
    {
        auto const* const question = FindSettledQuestion(command.action);
        if (question == nullptr || !question->takesTopic)
        {
            if (command.operands.empty())
                return {};
            auto const spelling = question == nullptr ? std::string_view { "that option" } : question->spelling;
            return std::format("{} takes no operand, and `{}` was given", spelling, command.operands.front());
        }

        if (command.operands.empty())
            return {};
        if (command.operands.size() > 1)
            return std::format("{} explains one {} at a time, and {} were given",
                               question->spelling,
                               question->subject,
                               command.operands.size());
        if (FindVerb(command.operands.front()) == nullptr)
            return std::format("no help for `{}`: it is not a command (try --help)", command.operands.front());
        return {};
    }

    /// The alias @p word names, or nullptr.
    ///
    /// A plain loop rather than `std::ranges::find`: `std::array`'s iterator is a raw pointer
    /// on libstdc++ and libc++ and a class type on MSVC, so no one spelling of `auto`
    /// satisfies both the compilers and `readability-qualified-auto`. The same collision
    /// `FindVerb` records.
    /// @param word What was typed in the command position.
    /// @return Its row, or nullptr when the word is not an alias.
    [[nodiscard]] WordAlias const* FindWordAlias(std::string_view word) noexcept
    {
        for (auto const& row: WordAliases)
            if (row.word == word)
                return &row;
        return nullptr;
    }

} // namespace

Command ParseCommand(std::span<std::string const> argv, Command seed)
{
    auto command = std::move(seed);

    if (argv.empty())
    {
        command.action = Action::UsageError;
        command.diagnostic = "expected a command (try --help)";
        return command;
    }

    // The option kit works over `char const*`, so a view is built rather than the
    // arguments being copied. `argv`'s strings own the bytes and outlive this.
    std::vector<char const*> raw;
    raw.reserve(argv.size());
    for (auto const& arg: argv)
        raw.push_back(arg.c_str());
    std::span<char const* const> const args { raw };

    auto optionsEnded = false;

    // Set once `--help` or `--version` has chosen the action. What follows can still say
    // how to RENDER it and can no longer say what it is.
    auto settled = false;

    for (std::size_t index = 0; index < args.size(); ++index)
    {
        std::string_view const token { args[index] };

        // `--` ends option parsing, so a key or a value that begins with a dash can
        // still be named. Without it `set -- --weird v` has no spelling.
        if (!optionsEnded && token == "--")
        {
            optionsEnded = true;
            continue;
        }

        auto const looksLikeOption = !optionsEnded && token.size() > 1 && token.front() == '-';
        if (looksLikeOption)
        {
            auto const flow = ApplyOneOption(CliToolOptions(), args, index, command);
            if (!flow.has_value())
            {
                // A bad flag AFTER `--help` cannot change the answer, so it must not
                // replace it with a diagnostic. `--help --bogus` printed the help before
                // this change -- the loop had already returned -- and goes on doing so.
                if (settled)
                    continue;

                command.action = Action::UsageError;
                auto const& error = flow.error();
                command.diagnostic = error.field.empty() ? error.context : std::format("{}: {}", error.field, error.context);
                // A token that begins with a dash AFTER the verb is far more often a
                // value than a mistyped flag -- `set counter -5` and `append k -suffix`
                // are ordinary, and a cache stores arbitrary bytes. `--` is the answer
                // and the bare refusal does not mention it, so the operator is left
                // reading a flag list for a flag they never meant to type.
                if (!command.verb.empty())
                    command.diagnostic +=
                        std::format(" (if `{}` is a value rather than a flag, put `--` before the operands: `{} -- ...`)",
                                    token,
                                    command.verb);
                return command;
            }
            // `--help` and `--version` are questions about this BINARY, so nothing
            // after them may change the answer. That is what `ParseFlow::Stop` says and
            // it stays true -- but *the answer* and *how the answer is rendered* are two
            // questions, and returning here collapsed them: `--help --color=always` threw
            // the flag away in silence, while `--color=always --help` honoured it. A flag
            // that works in one position and is ignored in the other, with no diagnostic
            // either way, is the worst of the three possible behaviours.
            //
            // So the action is SETTLED and scanning continues. Presentation flags still
            // land; nothing can select a different action, because the rows that select
            // one are exactly the rows that set this.
            if (*flow == ParseFlow::Stop)
                settled = true;
            continue;
        }

        // Once the action is settled a bare word is not a verb -- `--help get` asks for
        // the help, not for a `get`. It is not NOTHING either, which is what this arm
        // used to make it: `help nosuchverb` printed 114 lines and exited **0**, while
        // the bare `nosuchverb` exits 2. A question that was not understood answered
        // with a confident success is the same defect `WordAliases` was written to fix,
        // one level down -- the operand went the way the exit code had.
        //
        // So it is a TOPIC of the settled question, kept in `operands` because that is
        // where positional words already live, and judged after the loop by
        // `UnacceptableTopic` -- which needs the whole tail, so it cannot be judged here.
        if (settled)
        {
            command.operands.emplace_back(token);
            continue;
        }

        if (command.verb.empty())
        {
            // A bare word that asks a flag's question. Checked BEFORE `FindVerb`, so a
            // row here shadows a verb of the same name rather than racing it -- and the
            // table's own comment records why `version` is not one.
            auto const* const alias = FindWordAlias(token);
            if (alias != nullptr)
            {
                command.action = alias->action;
                settled = true;
                continue;
            }
            command.verb = token;
        }
        else
            command.operands.emplace_back(token);
    }

    // A settled action needs no verb and no VERB operand arity: it was a question about
    // this binary. Returning here is what keeps `--help` from falling into *expected a
    // command* now that the loop no longer returns early. What it does still owe is an
    // answer about its own tail, which is the one thing the loop could not decide.
    if (settled)
    {
        if (auto const unacceptable = UnacceptableTopic(command); !unacceptable.empty())
        {
            command.action = Action::UsageError;
            command.diagnostic = unacceptable;
        }
        return command;
    }

    if (command.verb.empty())
    {
        command.action = Action::UsageError;
        command.diagnostic = "expected a command (try --help)";
        return command;
    }

    auto const* const verb = FindVerb(command.verb);
    if (verb == nullptr)
    {
        command.action = Action::UsageError;
        command.diagnostic = std::format("unknown command `{}` (try --help)", command.verb);
        return command;
    }

    if (!OperandCountAccepted(*verb, command.operands.size()))
    {
        command.action = Action::UsageError;
        command.diagnostic =
            std::format("`{}` takes {} and was given {}", verb->name, DescribeOperandArity(*verb), command.operands.size());
        return command;
    }

    if (command.verbOptions.onlyIfAbsent && command.verbOptions.onlyIfPresent)
    {
        command.action = Action::UsageError;
        command.diagnostic = "--nx and --xx contradict each other";
        return command;
    }

    if (auto const unhonoured = UnhonouredModifier(command, *verb); !unhonoured.empty())
    {
        command.action = Action::UsageError;
        command.diagnostic = unhonoured;
        return command;
    }

    return command;
}

std::string HelpTopicText(VerbSpec const& verb, UsageColor color)
{
    auto const& wire = WireTable[static_cast<std::size_t>(verb.wire)];

    // `UsageRows::Add` takes the description as a VIEW, so a computed one has to outlive
    // the render. A deque, for the reason `UsageRows` itself keeps one: growing a vector
    // reseats its strings and every entry already added would view freed bytes.
    std::deque<std::string> cells;
    auto const cell = [&cells](std::string text) -> std::string_view {
        return cells.emplace_back(std::move(text));
    };

    UsageRows details;
    details.Add("wire", cell(std::string { wire.name }));

    std::string opens;
    for (auto const& connection: Connections)
    {
        if (!(wire.*connection.needed))
            continue;
        if (!opens.empty())
            opens += ", ";
        opens += connection.name;
    }
    details.Add("opens", cell(std::move(opens)));

    // An empty `protocolCommand` is NOT an absent cell. The column's own documentation
    // says empty means *sends none directly*, which is a fact this client knows -- and a
    // dash, which is how this tool spells a value it could not obtain, would claim the
    // opposite. Two states, two renderings.
    details.Add("sends",
                cell(verb.protocolCommand.empty() ? std::string { "no single command; the source is chosen at run time" }
                                                  : std::string { verb.protocolCommand }));
    details.Add("operands", cell(DescribeOperandArity(verb)));

    std::string honoured;
    for (auto const& modifier: Modifiers)
    {
        if ((verb.modifiers & modifier.bit) == 0)
            continue;
        if (!honoured.empty())
            honoured += ", ";
        honoured += modifier.flag;
    }
    details.Add("modifiers", cell(honoured.empty() ? std::string { "none" } : std::move(honoured)));

    // The question #1275 was about, asked per verb rather than discovered by dialling a
    // node and reading the refusal.
    details.Add("on a compile node", NodeAnswerFor(verb));

    auto const invocation = std::format(" fastcache-cli [options] {}", RenderVerb(verb));

    auto const blocks = std::to_array<UsageBlock>({
        UsageBlock { .text = verb.summary, .textIndent = 2 },
        UsageBlock { .entries = details.Rows() },
        UsageBlock { .text = "fastcache-cli --help lists every command, the options, the formats, the exit\n"
                             "codes and the environment variables.",
                     .textIndent = 2 },
    });

    std::span<UsageBlock const> const all { blocks };
    auto const sections = std::to_array<UsageSection>({
        { .title = "usage:", .subject = invocation },
        { .blocks = all.subspan(0, 1) },
        { .title = "DETAILS", .blocks = all.subspan(1, 1) },
        { .title = "SEE ALSO", .blocks = all.subspan(2, 1) },
    });

    return RenderUsage({ .sections = sections }, color);
}

std::string HelpText(UsageColor color)
{
    UsageRows commandRows;
    for (auto const& verb: Verbs())
        commandRows.Add(RenderVerb(verb), verb.summary);

    UsageRows optionRows;
    AddOptionRows(optionRows, CliToolOptions());

    UsageRows formatRows;
    for (auto const& format: FormatTable)
        formatRows.Add(std::string { format.name }, format.summary);

    UsageRows exitRows;
    for (auto const& outcome: OutcomeTable)
        exitRows.Add(std::format("{}  {}", outcome.code, outcome.name), outcome.meaning);

    UsageRows environmentRows;
    for (auto const& variable: CliEnvironment())
        environmentRows.Add(std::string { variable.name }, variable.summary);

    std::vector<UsageBlock> blocks;
    blocks.push_back(UsageBlock { .entries = commandRows.Rows() });
    blocks.push_back(UsageBlock { .entries = optionRows.Rows() });
    blocks.push_back(UsageBlock { .entries = formatRows.Rows() });
    blocks.push_back(UsageBlock { .entries = exitRows.Rows() });
    blocks.push_back(UsageBlock { .entries = environmentRows.Rows() });
    for (auto const note: Notes)
        blocks.push_back(UsageBlock { .text = note, .textIndent = 2 });

    std::span<UsageBlock const> const all { blocks };
    auto const notesFirst = std::size_t { 5 };
    auto const sections = std::to_array<UsageSection>({
        { .subject = "fastcache-cli - operate a fastcached cache and a compile fleet from a terminal." },
        { .title = "usage:", .subject = " fastcache-cli [options] <command> [operands]" },
        { .title = "COMMANDS", .blocks = all.subspan(0, 1) },
        { .title = "OPTIONS", .blocks = all.subspan(1, 1) },
        { .title = "FORMATS", .blocks = all.subspan(2, 1) },
        { .title = "EXIT CODES", .blocks = all.subspan(3, 1) },
        { .title = "ENVIRONMENT", .blocks = all.subspan(4, 1) },
        { .title = "NOTES", .blocks = all.subspan(notesFirst, Notes.size()) },
    });

    auto const substitutions = std::to_array<UsageSubstitution>({
        { .token = "{addr}", .value = "127.0.0.1:6674" },
    });

    return RenderUsage({ .sections = sections }, color, substitutions);
}

} // namespace FastCache::Cli
