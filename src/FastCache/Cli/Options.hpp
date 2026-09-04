// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>
#include <FastCache/Core/Utf8.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace FastCache
{

/// Whether a flag takes a value.
enum class Arity : std::uint8_t
{
    None,  ///< The flag stands alone, e.g. `--metrics`.
    Value, ///< A value follows, spelled `--flag=v` or `--flag v`.
};

/// What the parser does once a flag has been applied.
enum class ParseFlow : std::uint8_t
{
    Continue, ///< Keep reading the remaining arguments.
    Stop,     ///< Return immediately, discarding whatever follows (`--help`, `--version`).
};

/// Apply one flag's effect to the result being built.
///
/// The single type-erased shape every flag's effect takes, so one row type can
/// hold heterogeneous parsers and target fields. Rows are never written by
/// hand: the AssignFrom / AppendFrom / SetTrue / SelectOutcome factories
/// generate a captureless lambda, which converts to this pointer and stays
/// usable in a `constexpr` table.
///
/// @param result The result being populated.
/// @param value The flag's value text; empty for an `Arity::None` flag.
/// @return Nothing on success; the value parser's ConfigError verbatim otherwise.
template <typename Result>
using ApplyFlag = std::expected<void, ConfigError> (*)(Result& result, std::string_view value);

/// The configuration type a `Result` carries, for the reload column below.
///
/// Mirrors `TargetOf`'s branch and must keep mirroring it: a parse result either
/// wraps its configuration in a `config` member (the daemon's `CliResult`) or IS the
/// configuration (the node's `NodeConfig`). A comparator typed on the wrong one of
/// those does not fail at the column -- it fails at every row, as
/// `'config': is not a member of NodeConfig` -- so the two must be derived from one
/// question asked the same way.
template <typename Result>
struct ConfigOfImpl
{
    using type = Result;
};

template <typename Result>
    requires requires(Result& r) { r.config; }
struct ConfigOfImpl<Result>
{
    using type = std::remove_cvref_t<decltype(std::declval<Result&>().config)>;
};

template <typename Result>
using ConfigOf = ConfigOfImpl<Result>::type;

/// Compares one field of two configurations. See `OptionSpec::same`.
template <typename Result>
using SameFieldFn = bool (*)(ConfigOf<Result> const&, ConfigOf<Result> const&);

/// Whether a setting can take effect without restarting the process.
///
/// **Opt-in, and that default is the guard.** The daemon's reloader used to decide
/// this in a hand-written ladder, which meant a flag added that day was *silently
/// reloadable* -- measured at 10 of 38 members guarded, which is what
/// [#406](https://github.com/LASTRADA-Software/fastcached/issues/406) was and what it
/// closed by giving the daemon this column too. That pre-fix figure is kept because it
/// is still the argument: defaulting to `No` inverts that failure, so a new flag is
/// silently IMMUTABLE, and the worst a forgotten column costs is a reload refused with
/// a name attached, never a live-wired object left disagreeing with the configuration
/// that claims to describe it.
enum class Reloadable : std::uint8_t
{
    /// Live-wired at startup; a change requires a restart. **The default, and it is
    /// the safe direction rather than the conservative one.**
    ///
    /// The instinct on reading this is that `Yes` would be friendlier. It is not, and
    /// the two failure directions are not equivalent:
    ///
    /// - A row that should be reloadable and is left `No` costs a **reload refused
    ///   with the field named**. The operator sees exactly what did not apply and why,
    ///   and restarts. Nothing is wrong afterwards.
    /// - A row that should be immutable and is silently `Yes` costs a **live-wired
    ///   object disagreeing with the configuration that claims to describe it** --
    ///   `Current()` reporting one thing and the running sockets, threads or toolchain
    ///   table doing another, with nothing anywhere saying so.
    ///
    /// The second is unbounded and invisible; the first is a sentence on a terminal.
    /// So a forgotten column fails closed. It is the inverse of what #406 found: a
    /// hand-written ladder beside a table makes a new flag silently RELOADABLE, which
    /// is the direction with no bound on what it costs. Both binaries answer this
    /// column now, so neither has such a ladder left.
    No,
    Yes, ///< Takes effect on reload.
};

/// One accepted command-line option.
///
/// The single source of truth for a binary's CLI: the parser matches these rows
/// and the help renderer builds its left column from `primary`/`alias`/`operand`
/// and its right column from `description`. A flag therefore cannot be accepted
/// without being documented, nor documented without being accepted. Adding a
/// flag is adding a row; there is no second place to edit.
///
/// `apply` and `select` are separate because a flag may do both: `--seed-config`
/// stores its path *and* selects an alternative to running the daemon. Either
/// may be null.
template <typename Result>
struct OptionSpec
{
    std::string_view primary;               ///< The documented spelling, e.g. `--port`.
    std::string_view alias {};              ///< Accepted synonym (`-h`), or empty for none.
    Arity arity { Arity::None };            ///< Whether a value follows.
    std::string_view operand {};            ///< Display-only value suffix, e.g. `=<num>`; empty iff `arity == None`.
    ApplyFlag<Result> apply { nullptr };    ///< What the flag's value does, or null.
    ApplyFlag<Result> select { nullptr };   ///< The outcome the flag selects, or null.
    bool Result::* explicitBit { nullptr }; ///< The "user typed this" tracker a config merge consults, or null.
    ParseFlow flow { ParseFlow::Continue }; ///< Whether parsing continues after this flag.
    std::string_view description {};        ///< Help text; '\n' wraps, `{token}`s expand at render time.

    /// The key this setting carries in a YAML configuration file, or empty when it
    /// may not come from one.
    ///
    /// **A column rather than a derivation, because the mapping is not derivable.**
    /// Measured on the daemon: 48 flag rows, 34 of which carry a key, diverging four
    /// ways -- `--storage` is `storage_path`, `--expiry-scan` is `active_expiry_scan`,
    /// `--expiry-interval` is `active_expiry_interval_ms` (renamed *and* carrying a
    /// unit the flag does not), and `--listen`/`--listen-tls` collapse into a single
    /// `listeners:` key. There is no rule with exceptions there, only a mapping, and
    /// a convention derived from flag names would silently rename three existing
    /// keys the day somebody generalised it.
    ///
    /// Empty is a decision rather than an omission: a one-shot verb has no business
    /// in a file, because a file is read at every start and would replay one
    /// operator's decision forever. Which rows those are is a named list beside the
    /// table, and a guard walks the table requiring every other row to carry a key.
    std::string_view yamlKey {};

    /// Whether a reload may apply a change to this setting.
    ///
    /// **A column of THIS table rather than a list beside it.** A second list is not a
    /// cross-check, it is a second thing to be wrong -- and the reload path is where
    /// that costs most, because the failure is an operator editing a file, seeing no
    /// error, and believing the change took.
    ///
    /// The reason most rows are `No` is stated once, in the node's own table, and it
    /// is the argument `fastcache-compile-node/main.cpp` used to give for handling no
    /// SIGHUP at all: a worker's toolchain table is what its registration advertised,
    /// so re-reading it would leave the scheduler dispatching against a set this
    /// worker no longer serves.
    Reloadable reloadable { Reloadable::No };

    /// Whether two configurations agree about this row's field.
    ///
    /// Paired with `reloadable` and derived from the SAME member pointer the applier
    /// uses -- `FieldEq<&NodeConfig::slots>()` beside
    /// `AssignFrom<&NodeConfig::slots, ...>()` -- so the two mentions sit adjacent on
    /// one line and a mismatch is visible rather than silent.
    ///
    /// Null for a row that is not configuration state at all: a one-shot verb has no
    /// field to compare, exactly as it has no `yamlKey`. A guard requires every row
    /// with a `yamlKey` to carry one, because those are precisely the rows a file can
    /// change.
    SameFieldFn<Result> same { nullptr };

    /// Empties this row's target before command-line values replace file-sourced
    /// ones, or null for a row whose value is not a list.
    ///
    /// Only repeatable rows need it, and only because their applier APPENDS. A
    /// command line naming any value for a list setting **replaces** what the file
    /// declared rather than extending it -- the daemon's rule for `listeners:`, and
    /// its reasoning is what decides it: mixing partial file values with partial
    /// command-line values makes precedence depend on declaration order, which is
    /// not something an operator can reason about.
    ApplyFlag<Result> clear { nullptr };
};

/// Check at compile time that a table says what it must.
///
/// Every row documented, every value flag carrying an operand to display, every
/// row doing *something*, and no spelling claimed twice — so a malformed row is
/// a build error rather than a flag that silently shadows another at runtime.
///
/// Generic rather than written once per binary: the checks are a property of the
/// row type, and a per-table copy is a table that quietly gets none. `consteval`
/// so it cannot be called at runtime and mistaken for a test.
/// @param table The rows to check.
/// @return True when every row is well formed.
template <typename Result>
[[nodiscard]] consteval bool TableIsWellFormed(std::span<OptionSpec<Result> const> table)
{
    auto const shapeOk = std::ranges::all_of(table, [](OptionSpec<Result> const& spec) {
        return spec.primary.starts_with("--") && !spec.description.empty()
               && (spec.arity == Arity::Value) == !spec.operand.empty() && (spec.apply != nullptr || spec.select != nullptr);
    });
    if (!shapeOk)
        return false;

    // A row reachable from a config file must be one a file can actually express.
    // `select` marks a row choosing what the process DOES instead of running --
    // `--help`, `--install-service`, a cluster question -- and `flow == Stop` ends
    // parsing; both are incoherent read from a file at every start. A `clear` on a
    // row with no key is a row somebody half-converted to a list. Checked here
    // rather than at runtime because a row that is both is not a condition to
    // report, it is one that should not compile.
    //
    // Arity is deliberately NOT checked. An `Arity::None` row is a flag whose
    // meaning is its presence, and a file spells presence as a boolean: the key
    // takes `true` or `false`, and `apply` runs on `true` alone. That reading is
    // exact for both polarities -- `raft_join: true` passes `--raft-join`, and
    // `no_toolchain_discovery: false` passes nothing, which is discovery left on.
    // The alternative, a positively-named key with an applier no flag has, is a
    // setting reachable from a file and not from argv -- two mechanisms for one
    // setting, which is the shape this column exists to remove.
    auto const fileRowsOk = std::ranges::all_of(table, [](OptionSpec<Result> const& spec) {
        if (spec.yamlKey.empty())
            return spec.clear == nullptr;
        return spec.apply != nullptr && spec.select == nullptr && spec.flow == ParseFlow::Continue;
    });
    if (!fileRowsOk)
        return false;

    auto const indices = std::views::iota(std::size_t { 0 }, table.size());
    return std::ranges::all_of(indices, [table, indices](std::size_t a) {
        return std::ranges::all_of(indices | std::views::drop(a + 1), [table, a](std::size_t b) {
            // The key joins the spelling checks for the same reason: two rows
            // answering to one key means whichever the walk reaches second wins,
            // silently.
            return table[a].primary != table[b].primary && (table[a].alias.empty() || table[a].alias != table[b].alias)
                   && (table[a].yamlKey.empty() || table[a].yamlKey != table[b].yamlKey);
        });
    });
}

/// Build a ConfigError attributed to the command line.
/// @param code The error category.
/// @param field The flag or setting at fault.
/// @param context Human-readable detail.
/// @return The populated error, with `source` stamped "argv".
[[nodiscard]] inline ConfigError ArgvError(ConfigErrorCode code, std::string field, std::string context)
{
    return ConfigError {
        .code = code, .source = "argv", .line = 0, .field = std::move(field), .context = std::move(context)
    };
}

/// Whether `arg` names `flag`, either exactly or as the `flag=value` form.
/// @param arg The command-line token.
/// @param flag The flag spelling to test.
/// @return True when `arg` names `flag`.
[[nodiscard]] constexpr bool FlagMatches(std::string_view arg, std::string_view flag) noexcept
{
    return arg == flag || (arg.starts_with(flag) && arg.size() > flag.size() && arg[flag.size()] == '=');
}

/// Whether `arg` names this row, by primary spelling or alias.
///
/// Valueless flags match exactly: `--daemon=1` has never been an accepted
/// spelling, and admitting it here would silently discard the value.
/// @param arg The command-line token.
/// @param spec The row to test.
/// @return True when the row claims `arg`.
template <typename Result>
[[nodiscard]] constexpr bool Matches(std::string_view arg, OptionSpec<Result> const& spec) noexcept
{
    auto const names = [&](std::string_view flag) {
        return !flag.empty() && (spec.arity == Arity::None ? arg == flag : FlagMatches(arg, flag));
    };
    return names(spec.primary) || names(spec.alias);
}

/// Render one row's left help column, e.g. `--port=<num>` or `--help, -h`.
///
/// Derived from the row rather than restated, so the documented spelling is by
/// construction the accepted one.
/// @param primary The documented spelling.
/// @param alias The synonym, or empty.
/// @param operand The display-only value suffix, or empty.
/// @return The flag column text.
[[nodiscard]] inline std::string RenderFlagForms(std::string_view primary, std::string_view alias, std::string_view operand)
{
    std::string forms { primary };
    if (!alias.empty())
    {
        forms += ", ";
        forms += alias;
    }
    forms += operand;
    return forms;
}

/// Render one row's left help column.
/// @param spec The row to render.
/// @return The flag column text.
template <typename Result>
[[nodiscard]] std::string RenderFlagForms(OptionSpec<Result> const& spec)
{
    return RenderFlagForms(spec.primary, spec.alias, spec.operand);
}

/// Pull the value out of `args[i]` for a `--flag=value` or `--flag value`
/// shape, advancing `i` past the value when it is a separate argv element.
/// @param args The full argument span.
/// @param i Index of the argument under inspection; advanced on the two-token form.
/// @param flag The flag spelling, for the error message.
/// @return The value text, or a ConfigError when the value is missing.
[[nodiscard]] inline std::expected<std::string_view, ConfigError> TakeValue(std::span<char const* const> args,
                                                                            std::size_t& i,
                                                                            std::string_view flag)
{
    auto const arg = std::string_view { args[i] };
    if (auto const eq = arg.find('='); eq != std::string_view::npos)
        return arg.substr(eq + 1);
    if (i + 1 >= args.size())
        return std::unexpected(ArgvError(ConfigErrorCode::ParseError, std::string { flag }, "missing value"));
    ++i;
    return std::string_view { args[i] };
}

/// Resolve the object a flag's target member pointer designates.
///
/// Accepts a pointer-to-member of the result itself or of a nested `config`
/// member, so one set of factories serves both the settings that take part in a
/// config merge and the install-time ones that must not.
///
/// The nested member must be spelled `config` — that name is part of the
/// contract a `Result` signs up to, not a coincidence. A result type that calls
/// its settings something else gets a hard error from inside a factory.
/// @param result The result being populated.
/// @return Reference to the field `Field` names.
template <auto Field, typename Result>
[[nodiscard]] constexpr auto& TargetOf(Result& result) noexcept
{
    if constexpr (requires { result.config.*Field; })
        return result.config.*Field;
    else
        return result.*Field;
}

/// An applier that parses the value with `Parse` and assigns it to `Field`.
/// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
template <auto Field, auto Parse>
[[nodiscard]] constexpr auto AssignFrom() noexcept
{
    return [](auto& result, std::string_view value) -> std::expected<void, ConfigError> {
        return Parse(value).transform(
            [&result](auto&& parsed) { TargetOf<Field>(result) = std::forward<decltype(parsed)>(parsed); });
    };
}

/// A comparator for `OptionSpec::same`, over the same field an applier assigns.
///
/// Takes the member pointer rather than deriving one from the applier, because an
/// applier is a lambda by the time the row holds it and there is nothing left to ask.
/// Written beside the applier on the same row so the two spellings of the field are
/// adjacent.
/// @return The comparator, usable as an OptionSpec::same in a `constexpr` table.
template <auto Field>
[[nodiscard]] constexpr auto FieldEq() noexcept
{
    return [](auto const& previous, auto const& candidate) -> bool {
        return previous.*Field == candidate.*Field;
    };
}

/// As AssignFrom, but appends — the shape a repeatable flag needs.
/// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
template <auto Field, auto Parse>
[[nodiscard]] constexpr auto AppendFrom() noexcept
{
    return [](auto& result, std::string_view value) -> std::expected<void, ConfigError> {
        return Parse(value).transform(
            [&result](auto&& parsed) { TargetOf<Field>(result).push_back(std::forward<decltype(parsed)>(parsed)); });
    };
}

/// An applier that EMPTIES `Field` — the reset a repeatable row needs before
/// command-line values replace file-sourced ones.
///
/// The same `ApplyFlag<Result>` type as `apply` and `select`, so the column costs a
/// field of a type the row already has rather than a new concept. Its value
/// argument is ignored: there is nothing to parse in "forget what you were told".
/// @return The applier, usable as an OptionSpec::clear in a `constexpr` table.
template <auto Field>
[[nodiscard]] constexpr auto ClearList() noexcept
{
    return [](auto& result, std::string_view) -> std::expected<void, ConfigError> {
        TargetOf<Field>(result).clear();
        return {};
    };
}

/// An applier that stores a fixed value in `Field` — how a flag selects what
/// the process will do instead of its default action.
/// @return The applier, usable as an OptionSpec::select in a `constexpr` table.
template <auto Field, auto Value>
[[nodiscard]] constexpr auto SelectOutcome() noexcept
{
    return [](auto& result, std::string_view) -> std::expected<void, ConfigError> {
        TargetOf<Field>(result) = Value;
        return {};
    };
}

/// An applier for a valueless switch: sets `Field` to true.
///
/// Named rather than spelled `SelectOutcome<Field, true>()` at each call site,
/// because a table of flags reads better when the common case has a name — but it
/// IS that, rather than a second copy of it. `apply` and `select` are the same type
/// (`ApplyFlag<Result>`), so there was never a reason for two bodies.
///
/// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
template <auto Field>
[[nodiscard]] constexpr auto SetTrue() noexcept
{
    return SelectOutcome<Field, true>();
}

/// An applier for a valueless switch that turns something OFF: sets `Field` to
/// false.
///
/// The counterpart to `SetTrue`, for the flag whose default is on. Spelling the
/// field positively and clearing it here is what keeps the double negative out of
/// the config struct — `toolchainDiscovery = false` reads, `noToolchainDiscovery =
/// true` has to be decoded at every use.
///
/// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
template <auto Field>
[[nodiscard]] constexpr auto SetFalse() noexcept
{
    return SelectOutcome<Field, false>();
}

/// The identity parser, for flags whose value is taken verbatim.
/// @param sv The value text.
/// @return `sv` as an owned string; never fails.
[[nodiscard]] inline std::expected<std::string, ConfigError> ParseText(std::string_view sv)
{
    return std::string { sv };
}

/// As ParseText, for a flag whose value OTHER MACHINES will read.
///
/// Which flags those are is a column of the option table, the same way the set of
/// verbs reachable before authentication is: a node's advertised endpoint and its
/// cluster id travel to peers, are rendered into `/fleet.json` and onto the fleet
/// page, and #141 made the scheduler refuse a registration carrying a field that
/// is not valid UTF-8. A path or a local-only setting is deliberately NOT in this
/// column -- on a host that transcodes nothing, a legacy filename is a perfectly
/// good filename, and refusing it would break a working build to satisfy a rule
/// about a different thing.
///
/// Refused HERE, where a person typed it, rather than only at the far end. A value
/// that gets past this is refused by `SchedulerService::Register` on every
/// heartbeat, forever, with the operator's only recovery being to rename the
/// thing -- and on Windows the byte they typed was never going to be the byte the
/// check wanted (issue #155). The offending offset is named because "not valid
/// UTF-8" about a string a console already re-rendered tells nobody which
/// character was the problem.
///
/// @param sv The value text.
/// @return `sv` as an owned string, or why it is not text. The error names no
///         field; `ApplyOneOption` stamps the flag, which this cannot know.
[[nodiscard]] inline std::expected<std::string, ConfigError> ParseUtf8Text(std::string_view sv)
{
    std::size_t offset = 0;
    while (offset < sv.size())
    {
        auto const length = Utf8SequenceLength(sv.substr(offset));
        if (length == 0)
            return std::unexpected(ArgvError(
                ConfigErrorCode::ParseError,
                {},
                std::format("value is not valid UTF-8: byte 0x{:02X} at offset {} starts no valid sequence, so this "
                            "value cannot travel to the rest of the fleet",
                            static_cast<unsigned>(static_cast<unsigned char>(sv[offset])),
                            offset)));
        offset += length;
    }
    return std::string { sv };
}

/// Apply the one option named by `args[i]`.
///
/// @param table The rows to match against.
/// @param args The full argument span.
/// @param i Index of the argument under inspection; advanced past a consumed value.
/// @param result The result being populated.
/// @return Whether to keep parsing, or a ConfigError when the token names no
///         row or its value was rejected.
template <typename Result>
[[nodiscard]] std::expected<ParseFlow, ConfigError> ApplyOneOption(std::span<OptionSpec<Result> const> table,
                                                                   std::span<char const* const> args,
                                                                   std::size_t& i,
                                                                   Result& result)
{
    std::string_view const arg { args[i] };
    auto const match = std::ranges::find_if(table, [arg](OptionSpec<Result> const& spec) { return Matches(arg, spec); });
    if (match == std::ranges::end(table))
        return std::unexpected(ArgvError(ConfigErrorCode::UnknownKey, std::string { arg }, "unrecognised argument"));

    if (match->apply != nullptr)
    {
        std::string_view value;
        if (match->arity == Arity::Value)
        {
            auto const taken = TakeValue(args, i, match->primary);
            if (!taken.has_value())
                return std::unexpected(taken.error());
            value = *taken;
        }
        if (auto const applied = (*match->apply)(result, value); !applied.has_value())
        {
            // The flag stamped here rather than by the value parser, which cannot
            // know it: a parser is a free function reached through a member pointer
            // in the table, shared by every row that uses it. A row's own spelling
            // is the only one that could ever be right, and a hand-written field is
            // one that drifts when a flag is renamed.
            //
            // Only when the parser left it empty, so a parser with something more
            // specific to say -- the node's log-level parser names `log-level`, and
            // its cluster appliers name the action they were reached through --
            // keeps saying it.
            auto error = applied.error();
            if (error.field.empty())
                error.field = std::string { match->primary };
            return std::unexpected(std::move(error));
        }
    }
    if (match->select != nullptr)
    {
        if (auto const selected = (*match->select)(result, {}); !selected.has_value())
            return std::unexpected(selected.error());
    }
    if (match->explicitBit != nullptr)
        result.*match->explicitBit = true;
    return match->flow;
}

/// Parse a whole argument vector into an already-constructed result.
///
/// Separate from ParseOptions because a caller that must seed its result first
/// (a sub-command selecting the action, say) would otherwise re-spell this loop.
/// The returned flow tells such a caller whether parsing stopped early, so it
/// can skip the checks a `--help` run should not fail.
/// @param table The rows to match against.
/// @param args The arguments, with the program name already removed.
/// @param result The result to populate; left partially applied on error.
/// @return Stop when a row ended parsing, Continue when the arguments ran out,
///         or the first ConfigError encountered.
template <typename Result>
[[nodiscard]] std::expected<ParseFlow, ConfigError> ParseOptionsInto(std::span<OptionSpec<Result> const> table,
                                                                     std::span<char const* const> args,
                                                                     Result& result)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        auto const flow = ApplyOneOption(table, args, i, result);
        if (!flow.has_value())
            return std::unexpected(flow.error());
        if (*flow == ParseFlow::Stop)
            return ParseFlow::Stop;
    }
    return ParseFlow::Continue;
}

/// Parse a whole argument vector against an option table.
/// @param table The rows to match against.
/// @param args The arguments, with the program name already removed.
/// @return The populated result, or the first ConfigError encountered.
template <typename Result>
[[nodiscard]] std::expected<Result, ConfigError> ParseOptions(std::span<OptionSpec<Result> const> table,
                                                              std::span<char const* const> args)
{
    Result result {};
    auto const flow = ParseOptionsInto(table, args, result);
    if (!flow.has_value())
        return std::unexpected(flow.error());
    return result;
}

/// Append one help row per option, deriving both columns from the row.
/// @param rows Destination.
/// @param table The options to document, in table order.
template <typename Result>
void AddOptionRows(UsageRows& rows, std::span<OptionSpec<Result> const> table)
{
    for (auto const& spec: table)
        rows.Add(RenderFlagForms(spec), spec.description);
}

} // namespace FastCache
