// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "MemcachedClient.hpp"
#include "NodeClient.hpp"
#include "RespClient.hpp"
#include "SocketExchange.hpp"
#include "StatsSource.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
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
/// A private enum. The scheduler's `0xFC` cluster verbs are a further enumerator plus
/// rows, which is the point of the column.
enum class Wire : std::uint8_t
{
    Resp,      ///< RESP on the data port.
    Memcached, ///< The memcached text protocol on the same port.
    /// The `0xFC` compile-cache wire, which is the ONLY thing
    /// `fastcache-compile-node` speaks. Measured against the running node: RESP
    /// `PING`, RESP `INFO` and memcached `version` each closed having sent nothing,
    /// while a `0xFC` header got a proper refusal.
    Node,
    Stats, ///< Whatever the stats ladder chooses; see StatsSource.hpp.
    Last,
};

struct VerbContext;

/// Whether a context carries the collaborator a wire needs.
///
/// A **column** rather than a condition inside `RunVerb`, which read
/// `verb.wire == Wire::Resp ? ctx.resp != nullptr : ctx.stats != nullptr` -- a ternary
/// that is exhaustive over two wires and silently wrong over three: a third wire would
/// have taken the `stats` arm and reported *no stats source was configured* for a
/// memcached verb, or dereferenced a null. The next wire is a row, and a row that
/// forgets this field does not compile.
using WireAvailable = bool (*)(VerbContext const&);

/// One wire's fixed properties.
struct WireSpec
{
    Wire wire;                    ///< The enumerator this row describes.
    std::string_view name;        ///< Stable lower-case name, for diagnostics.
    std::string_view unavailable; ///< What to tell an operator when it could not be opened.

    /// The sub-heading `--help` prints above this wire's verbs.
    ///
    /// `--help` groups `COMMANDS` by this column, so *which server answers this* is
    /// read off the page rather than discovered by dialling one and reading the
    /// refusal. A COLUMN rather than a list beside the renderer, for the reason every
    /// other property of a wire is one: a list is a second set keyed on the same enum,
    /// which agrees on the day it is written and silently stops agreeing afterwards.
    ///
    /// It names the wire's OWN server and claims no exclusivity, deliberately. A row
    /// carrying a `nodeFallback` -- `version` is the one today -- is also answered by a
    /// compile node, so a heading reading *the only verbs a daemon answers* would be
    /// false for it. Which verbs have that second answer is a per-verb fact and is on
    /// the verb's own page, where `NodeAnswerFor` states it in three values.
    std::string_view heading;

    WireAvailable available; ///< Which collaborator says this wire is open.

    /// What this wire can add when a value could not be shown as text.
    ///
    /// A COLUMN because the reassurance is true of ONE wire: a memcached key is a
    /// byte string and the protocol never promised otherwise, so base64 there is
    /// ordinary rather than a fault. Said on every wire it is a claim about keys to
    /// an answer that has no keys -- `fleet`, `cluster settings` and `node info` all
    /// reach the same advisory through `RunVerb`.
    ///
    /// EMPTY is a real answer rather than an absence standing in for one: every wire
    /// gets the general sentence, and this is only what a particular one adds to it.
    std::string_view binaryNote;

    /// Whether this wire can present a credential at all.
    ///
    /// **False for `Memcached`, and that is a property of the PROTOCOL rather than of
    /// any deployment**: the memcached text surface has no AUTH verb, so under
    /// `--requirepass` the server answers every verb but `version` and `quit` with
    /// `CLIENT_ERROR authentication required` and ends the session. A client cannot fix
    /// that by trying harder, so when such a refusal arrives it is explained rather
    /// than relayed bare -- `FromMemcachedError` in `CliVerbs.cpp` reads this column to
    /// decide whether to add the explanation.
    ///
    /// It is deliberately NOT used to refuse before dialling. A credential being
    /// CONFIGURED does not mean the server REQUIRES one -- that is the same false
    /// inference `SocketExchange::Open`'s AUTH advisory exists to avoid -- so refusing
    /// on it would decline verbs that would have worked, against every daemon with no
    /// password. Asking costs one round trip and the answer is the server's own.
    bool authenticable;

    /// Whether a caller must open a RESP connection for a verb on this wire.
    ///
    /// A **column** rather than a ladder in `main`, for the reason `main` holds nothing
    /// testable: it is in no test target (#370, #909), so a decision made there is a
    /// rule nothing can be held to. `Stats` says yes although it is not RESP, because
    /// `INFO` is the stats ladder's fallback rung and lives on that connection.
    bool needsResp;

    /// Whether a caller must open a memcached-text connection for a verb on this wire.
    ///
    /// Stated per row rather than derived from `wire`, so a future wire needing BOTH
    /// connections is a row that says so rather than a special case at the call site.
    bool needsMemcached;

    /// Whether a caller must open a `0xFC` connection for a verb on this wire.
    ///
    /// `Stats` says **yes**, which is the column doing real work rather than repeating
    /// `wire`: the ladder's `/metrics` rung needs an admin address, and since #431 the
    /// way it gets one when the operator named none is to ask the node over `0xFC`
    /// where its admin surface is. So `stats` opens three connections and each rung
    /// says which it needed, rather than `main` deciding.
    bool needsNode;
};

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
    VerbSpec const* verb { nullptr };          ///< The row that was matched; set by `RunVerb`.
    std::span<std::string const> operands {};  ///< The positional arguments after the verb.
    VerbOptions options {};                    ///< The modifiers.
    IExchange* resp { nullptr };               ///< The RESP connection, or null.
    IMemcachedExchange* memcached { nullptr }; ///< The memcached-text connection, or null.
    INodeExchange* node { nullptr };           ///< The `0xFC` connection, or null.
    IStatsGatherer* stats { nullptr };         ///< The stats ladder, or null.
    IAdminDocument* admin { nullptr };         ///< The endpoint's admin surface, or null.
};

/// The wires, one row per enumerator, in enumerator order.
///
/// Below `VerbContext` rather than beside `WireSpec`, because `available` is a function
/// of one: a column that answers a question about the context has to be able to see it.
inline constexpr EnumTable<Wire, WireSpec> WireTable { {
    { .wire = Wire::Resp,
      .name = "resp",
      .unavailable = "no connection to the cache was opened",
      .heading = "a cache daemon, over RESP",
      .available = [](VerbContext const& context) { return context.resp != nullptr; },
      .binaryNote = "",
      .authenticable = true,
      .needsResp = true,
      .needsMemcached = false,
      .needsNode = false },
    { .wire = Wire::Memcached,
      .name = "memcached",
      .unavailable = "no memcached-text connection to the cache was opened",
      .heading = "a cache daemon, over the memcached text protocol",
      .available = [](VerbContext const& context) { return context.memcached != nullptr; },
      .binaryNote = "a memcached key is a byte string, so this is ordinary rather than a fault",
      .authenticable = false,
      .needsResp = false,
      .needsMemcached = true,
      .needsNode = false },
    { .wire = Wire::Node,
      .name = "node",
      .unavailable = "no 0xFC connection to the node was opened",
      .heading = "a compile node, over the 0xFC wire",
      .available = [](VerbContext const& context) { return context.node != nullptr; },
      .binaryNote = "",
      // `AUTH` IS a `0xFC` verb, unlike on the memcached wire -- so this wire can
      // present a credential and a refusal about one is about the credential.
      .authenticable = true,
      .needsResp = false,
      .needsMemcached = false,
      .needsNode = true },
    { .wire = Wire::Stats,
      .name = "stats",
      .unavailable = "no stats source was configured",
      .heading = "either, over whichever surface answers",
      .available = [](VerbContext const& context) { return context.stats != nullptr; },
      .binaryNote = "",
      .authenticable = true,
      .needsResp = true,
      .needsMemcached = false,
      .needsNode = true },
} };

static_assert(RowsInEnumeratorOrder(WireTable, &WireSpec::wire),
              "WireTable must hold one row per Wire, in enumerator order");

/// Whether every wire states the heading its verbs are grouped under.
///
/// `RowsInEnumeratorOrder` cannot see this: a row omitting `.heading` from its
/// designated initializers is still a row, at the right index, describing the right
/// enumerator -- it just renders a run of verbs under a blank line, which is the flat
/// list this grouping replaced, for one wire, with nothing to say which.
///
/// A `static_assert` rather than a render-time fallback because the obligation is DO
/// SOMETHING rather than SAY WHY: there is no reason a wire could have for being
/// unnamed, so this is the half of that rule the type system answers. Written out
/// rather than left to a missing-field-initializer warning: which compilers carry that
/// warning, and under which flags, is a question this guard then would not have to
/// depend on -- and a guard that holds only where a flag is on reports clean wherever
/// it is not.
///
/// @param table The wire table.
/// @return True when no row's heading is empty.
[[nodiscard]] consteval bool EveryWireIsHeaded(EnumTable<Wire, WireSpec> const& table) noexcept
{
    return std::ranges::none_of(table, [](WireSpec const& row) { return row.heading.empty(); });
}

static_assert(EveryWireIsHeaded(WireTable), "every Wire row must carry the heading --help groups its verbs under");

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
    constexpr std::uint8_t Ttl = 0b0001;         ///< `--ttl`
    constexpr std::uint8_t Exclusivity = 0b0010; ///< `--nx` and `--xx`
    constexpr std::uint8_t Raw = 0b0100;         ///< `--raw`
    constexpr std::uint8_t Everything = 0b1000;  ///< `--all`
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
    /// columns. Written **the way its own wire spells it**, which is upper case on RESP
    /// and lower case on the memcached text protocol -- the row's `wire` says which, and
    /// a verb sending the wrong case is refused by the server rather than by this table.
    std::string_view protocolCommand;

    /// Which of `Modifier`'s bits this verb honours.
    std::uint8_t modifiers;

    VerbHandler handler; ///< What it does.

    /// How to answer this verb when the endpoint turns out to be a compile node.
    ///
    /// **Null on almost every row, and that is the point rather than an omission.** A
    /// compile node holds no user keyspace, so `get` has no `0xFC` equivalent and never
    /// will -- the honest answer there is a refusal naming what the endpoint IS, not a
    /// second attempt that cannot work. A row carrying one is a verb whose QUESTION a
    /// node can also answer: `version` asks *what are you running*, which every
    /// fastcache binary knows about itself.
    ///
    /// Reached only after the primary wire failed AND the endpoint was probed, so it
    /// costs the common case nothing. `RunNodeFallback` is the one door.
    VerbHandler nodeFallback;
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

/// Answer @p verb again, now that the endpoint has been identified.
///
/// **The pure half of the fallback**, so every arm is testable without a socket. What
/// `main` contributes is acquisition alone -- opening the `0xFC` connection and running
/// the probe -- because that file is in no test target (#370, #909) and a decision taken
/// there is a rule nothing can be held to.
///
/// Three outcomes, and the middle one is the reason this is not a `bool`:
///   - the row carries a `nodeFallback` and the endpoint is a **compile node**: run it,
///     and the operator gets an ANSWER rather than a better-worded failure;
///   - the endpoint was identified and there is no fallback: @p primary is returned with
///     the identification added as an advisory, so the exit code still says what
///     happened and the sentence says why;
///   - the probe explained nothing: @p primary is returned unchanged, because inventing
///     a second sentence for one fault makes it read as two.
///
/// @param verb The verb that could not be answered.
/// @param context What to run against; its `node` must be the probed connection.
/// @param kind What the endpoint turned out to be.
/// @param primary What the verb's own wire concluded.
/// @return The answer to report.
[[nodiscard]] Answer RunNodeFallback(VerbSpec const& verb, VerbContext const& context, RemoteKind kind, Answer primary);

} // namespace FastCache::Cli
