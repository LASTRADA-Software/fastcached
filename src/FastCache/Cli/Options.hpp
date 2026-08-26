// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

    auto const indices = std::views::iota(std::size_t { 0 }, table.size());
    return std::ranges::all_of(indices, [table, indices](std::size_t a) {
        return std::ranges::all_of(indices | std::views::drop(a + 1), [table, a](std::size_t b) {
            return table[a].primary != table[b].primary && (table[a].alias.empty() || table[a].alias != table[b].alias);
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

/// An applier for a valueless switch: sets `Field` to true.
/// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
template <auto Field>
[[nodiscard]] constexpr auto SetTrue() noexcept
{
    return [](auto& result, std::string_view) -> std::expected<void, ConfigError> {
        TargetOf<Field>(result) = true;
        return {};
    };
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
    return [](auto& result, std::string_view) -> std::expected<void, ConfigError> {
        TargetOf<Field>(result) = false;
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

/// The identity parser, for flags whose value is taken verbatim.
/// @param sv The value text.
/// @return `sv` as an owned string; never fails.
[[nodiscard]] inline std::expected<std::string, ConfigError> ParseText(std::string_view sv)
{
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
            return std::unexpected(applied.error());
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
