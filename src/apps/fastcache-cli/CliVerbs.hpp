// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "RespClient.hpp"
#include "StatsSource.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// @file CliVerbs.hpp
/// The verb table. **Adding a command is adding a row**, and the row carries
/// everything about it: what it is called, which wire it needs, how many operands it
/// takes, how it is documented, and what it does.
///
/// One table rather than a table plus a dispatch switch, because two structures keyed
/// on the same set is not a cross-check -- it is a second thing to be wrong, and the
/// failure is a verb that parses and then does nothing, or one documented and not
/// accepted.

/// Which wire a verb needs to reach the server.
///
/// A private enum. Two entries today; the memcached-text verbs (`touch`, `gat`,
/// `append`, `cas`, the `stats` sub-commands) and the scheduler's `0xFC` cluster verbs
/// are each a further enumerator plus rows, which is the point of the column.
enum class Wire : std::uint8_t
{
    Resp,  ///< RESP on the data port.
    Stats, ///< Whatever the stats ladder chooses; see StatsSource.hpp.
    Last,
};

/// One wire's fixed properties.
struct WireSpec
{
    Wire wire;                    ///< The enumerator this row describes.
    std::string_view name;        ///< Stable lower-case name, for diagnostics.
    std::string_view unavailable; ///< What to tell an operator when it could not be opened.
};

/// The wires, one row per enumerator, in enumerator order.
inline constexpr EnumTable<Wire, WireSpec> WireTable { {
    { .wire = Wire::Resp, .name = "resp", .unavailable = "no connection to the cache was opened" },
    { .wire = Wire::Stats, .name = "stats", .unavailable = "no stats source was configured" },
} };

static_assert(RowsInEnumeratorOrder(WireTable, &WireSpec::wire),
              "WireTable must hold one row per Wire, in enumerator order");

/// The value `VerbOptions::ttlSeconds` carries when the operator named no TTL.
///
/// A named sentinel rather than a bare `-1`, because `-1` is also a *meaningful* TTL
/// answer from the server (`the key exists and has no expiry`), and the two must not
/// be confusable at a glance.
constexpr std::int64_t TtlUnset = -1;

/// Modifiers a verb reads from the command line.
struct VerbOptions
{
    std::int64_t ttlSeconds { TtlUnset }; ///< From `--ttl`; `TtlUnset` when not given.
    bool onlyIfAbsent { false };          ///< From `--nx`.
    bool onlyIfPresent { false };         ///< From `--xx`.
    bool raw { false };                   ///< From `--raw`: write the value's bytes verbatim.
    bool everything { false };            ///< From `--all`: `flush` clears every database.
};

struct VerbSpec;

/// Everything a handler is given.
///
/// The collaborators are pointers and may be null: a verb whose wire could not be
/// opened is refused by name rather than dereferencing. Both are seams rather than
/// concrete clients, which is what makes every handler testable against a scripted
/// exchange -- and the refusal arms are the half that matters, since provoking a real
/// `NOAUTH` or a real crossed reply needs a daemon configured to misbehave.
struct VerbContext
{
    VerbSpec const* verb { nullptr };         ///< The row that was matched; set by `RunVerb`.
    std::span<std::string const> operands {}; ///< The positional arguments after the verb.
    VerbOptions options {};                   ///< The modifiers.
    IExchange* resp { nullptr };              ///< The RESP connection, or null.
    IStatsGatherer* stats { nullptr };        ///< The stats ladder, or null.
};

/// What a verb does.
///
/// A plain function pointer so the table stays `constexpr` and every handler is a free
/// function with no captured state -- the same shape `RedisResp`'s own `CommandTable`
/// uses for its handlers.
using VerbHandler = Answer (*)(VerbContext const&);

/// The command-line modifiers a verb can honour, as bits of `VerbSpec::modifiers`.
///
/// **A column, so a modifier that means nothing for a verb is REFUSED rather than
/// ignored.** The launcher already learned this one the expensive way: `--out` means
/// nothing after `--show-stats`, and accepting it there would silently discard a flag
/// the caller believed did something. Same rule, same reason -- `fastcache-cli set k v
/// --raw` should say so, not quietly store and print nothing.
namespace Modifier
{
    constexpr std::uint8_t None = 0;
    constexpr std::uint8_t Ttl = 1U << 0U;         ///< `--ttl`
    constexpr std::uint8_t Exclusivity = 1U << 1U; ///< `--nx` and `--xx`
    constexpr std::uint8_t Raw = 1U << 2U;         ///< `--raw`
    constexpr std::uint8_t Everything = 1U << 3U;  ///< `--all`
} // namespace Modifier

/// `VerbSpec::maxOperands` for a verb that takes any number.
constexpr std::uint8_t VariadicOperands = 0xFF;

/// One command.
struct VerbSpec
{
    std::string_view name;     ///< As typed.
    Wire wire;                 ///< What it needs to reach the server.
    std::uint8_t minOperands;  ///< Fewest positional arguments accepted.
    std::uint8_t maxOperands;  ///< Most accepted, or `VariadicOperands`.
    std::string_view operands; ///< Display-only operand list, e.g. ` <key> <value>`.
    std::string_view summary;  ///< One-line help; `\n` starts a continuation line.

    /// The protocol command this verb sends, or empty for one that sends none directly.
    ///
    /// **A column rather than a literal inside the handler**, which is what lets one
    /// handler serve a family: `del`, `exists`, `incr`, `decr`, `incrby` and `decrby`
    /// differ only in this word and in their operand bounds, and both are already
    /// columns. Written the way the wire spells it, in upper case.
    std::string_view protocolCommand;

    /// Which of `Modifier`'s bits this verb honours.
    std::uint8_t modifiers;

    VerbHandler handler; ///< What it does.
};

/// The verbs, in the order `--help` documents them.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<VerbSpec const> Verbs() noexcept;

/// The verb @p name spells.
/// @param name The token as typed.
/// @return The matching row, or nullptr.
[[nodiscard]] VerbSpec const* FindVerb(std::string_view name) noexcept;

/// Whether @p count positional arguments satisfies @p verb.
/// @param verb The verb.
/// @param count How many operands were given.
/// @return True when the count is within the verb's bounds.
[[nodiscard]] bool OperandCountAccepted(VerbSpec const& verb, std::size_t count) noexcept;

/// How many operands @p verb wants, in words.
///
/// Derived from the row's own bounds rather than written out per verb, so a refusal
/// cannot describe an arity the parser does not enforce.
/// @param verb The verb.
/// @return A phrase such as `exactly 2 operands` or `at least 1 operand`.
[[nodiscard]] std::string DescribeOperandArity(VerbSpec const& verb);

/// Run @p verb, having checked its wire is available.
///
/// The one place a null collaborator is turned into a refusal, so no handler has to.
/// @param verb The verb.
/// @param context What to run it against.
/// @return The answer.
[[nodiscard]] Answer RunVerb(VerbSpec const& verb, VerbContext const& context);

} // namespace FastCache::Cli
