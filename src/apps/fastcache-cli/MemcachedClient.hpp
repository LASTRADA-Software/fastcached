// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "RespClient.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file MemcachedClient.hpp
/// The memcached **text** protocol, client side: an encoder and a reply parser.
///
/// ## Why this exists at all, when RESP already works
///
/// The two surfaces are not supersets of one another. `touch`, `gat`/`gats`, `append`,
/// `prepend`, `cas`, `add`, `replace`, the `stats` sub-commands, `cache_memlimit` and
/// the meta `me` inspector exist ONLY here -- and `me` is the only way anything reaches
/// a key's last-access time, its cas token and its stored size, which is the most
/// useful per-key diagnostic this daemon has.
///
/// ## Why it is harder than RESP, stated so nobody re-litigates it
///
/// A RESP reply is type-prefixed, so ONE parser reads every command including ones
/// added later. A memcached reply is ad hoc per command family, and the only thing
/// they share is that the FIRST TOKEN says what the rest of the reply is. That token
/// is what this parser dispatches on (`McLeadToken`), which is as close to
/// type-prefixed as this wire gets: a new command whose reply opens with a token
/// already in the table needs no parser change.
///
/// ## The constraint that shapes every caller
///
/// **There is no AUTH verb on this protocol.** `MemcachedText.cpp` replies
/// `CLIENT_ERROR authentication required` and then ENDS THE SESSION for every verb but
/// `version` and `quit` when `--requirepass` is set -- deliberately, because a refused
/// storage command still has its data block sitting unread and continuing would parse
/// those bytes as the next command. So these verbs are unavailable against a
/// password-protected daemon, and a client must refuse them BY NAME rather than dial
/// and hang. `WireSpec::credentialRefusal` in `CliVerbs.hpp` is where that is said.

/// The first token of a reply, which says what the rest of it is.
///
/// A private enum: nothing persists or transmits its ordinals. The WORDS are the wire
/// contract and they are spelled as literals in `LeadTokenTable`.
enum class McLeadToken : std::uint8_t
{
    Value,  ///< `VALUE <key> <flags> <bytes>[ <cas>]` then data, repeating, then `END`.
    Stat,   ///< `STAT <name> <value>`, repeating, then `END`.
    Meta,   ///< `ME <key> <flag>=<value>...` -- one line, the `me` inspector's answer.
    Error,  ///< `ERROR`, `CLIENT_ERROR <text>`, `SERVER_ERROR <text>`.
    Status, ///< Everything else: one line whose whole text is the answer.
    Last,
};

/// One lead token's fixed properties.
struct McLeadTokenSpec
{
    McLeadToken token;     ///< The enumerator this row describes.
    std::string_view name; ///< Stable lower-case name, for diagnostics.
};

/// The lead tokens, one row per enumerator, in enumerator order.
inline constexpr EnumTable<McLeadToken, McLeadTokenSpec> LeadTokenTable { {
    { .token = McLeadToken::Value, .name = "value" },
    { .token = McLeadToken::Stat, .name = "stat" },
    { .token = McLeadToken::Meta, .name = "meta" },
    { .token = McLeadToken::Error, .name = "error" },
    { .token = McLeadToken::Status, .name = "status" },
} };

static_assert(RowsInEnumeratorOrder(LeadTokenTable, &McLeadTokenSpec::token),
              "LeadTokenTable must hold one row per McLeadToken, in enumerator order");

/// A `name value` pair out of a `STAT` line or an `ME` flag.
struct McPair
{
    std::string name;  ///< The name as the server spelled it.
    std::string value; ///< The rest of the line, verbatim.
};

/// One `VALUE` block.
struct McValue
{
    std::string key;           ///< The key the server echoed.
    std::uint32_t flags { 0 }; ///< The client-chosen flags word.
    std::uint64_t cas { 0 };   ///< The cas token; meaningful only when `hasCas`.
    bool hasCas { false };     ///< Whether the header carried a cas token (`gets`/`gats`).
    std::string data;          ///< The value's bytes, exactly `<bytes>` of them.
};

/// A parsed reply.
///
/// One struct rather than a variant, because a caller almost always wants the status
/// line even on a shape that also carries items -- `END` after zero `VALUE` blocks is
/// a miss and is worth reporting as such.
struct McReply
{
    McLeadToken kind { McLeadToken::Status }; ///< What the first token said this is.
    std::string status;                       ///< The terminating or only line, verbatim.
    std::vector<McValue> values;              ///< `VALUE` blocks, in order.
    std::vector<McPair> stats;                ///< `STAT` pairs, in order.
    std::string metaKey;                      ///< `ME`'s key.
    std::vector<McPair> metaFlags;            ///< `ME`'s `name=value` flags, in order.
};

/// Caps this side imposes on a reply, independent of anything the peer claims.
///
/// **A peer-declared length sizes nothing until it is checked against these.** A
/// `VALUE` header names its own byte count, and this client dials whatever address an
/// operator typed -- which is not a peer it should trust with its address space.
struct McParseLimits
{
    std::size_t maxValueBytes { 16U * 1024U * 1024U }; ///< Largest single value accepted.
    std::size_t maxItems { 100000 };                   ///< Most `VALUE`/`STAT` items accepted.
    std::size_t maxLineBytes { 8192 };                 ///< Longest single line accepted.
};

/// How a parse ended.
enum class McParseState : std::uint8_t
{
    Complete,   ///< A whole reply was read.
    Incomplete, ///< More bytes are needed; nothing is wrong.
    Malformed,  ///< The bytes cannot be a reply, and reading more will not help.
    Last,
};

/// The outcome of one parse attempt.
struct McParseResult
{
    McParseState state { McParseState::Incomplete }; ///< How it ended.
    McReply reply {};                                ///< Valid only when `Complete`.
    std::size_t consumed { 0 };                      ///< Bytes the reply occupied.
    std::string diagnostic {};                       ///< Why, when `Malformed`.
};

/// Parse one reply from the front of @p bytes.
///
/// @param bytes Everything read so far.
/// @param limits Caps this side imposes.
/// @return The result; `Incomplete` when more bytes are needed.
[[nodiscard]] McParseResult ParseMemcachedReply(std::string_view bytes, McParseLimits const& limits = {});

/// Encode a command line: `<verb> <arg>...\r\n`.
///
/// No storage payload. Arguments are joined with single spaces and sent verbatim --
/// this protocol has no quoting, so a key containing a space is not expressible and is
/// refused before it reaches here (`ValidTextToken`).
/// @param verb The verb, lower case as the wire spells it.
/// @param args The arguments.
/// @return The bytes to send.
[[nodiscard]] std::string EncodeMemcachedCommand(std::string_view verb, std::span<std::string const> args);

/// Encode a storage command: its header line, the payload, and the trailing CRLF.
///
/// @param verb One of `set`, `add`, `replace`, `append`, `prepend`, `cas`.
/// @param key The key.
/// @param flags The client-chosen flags word.
/// @param exptime Expiry in seconds, or 0 for none.
/// @param value The payload bytes.
/// @param casToken The cas token for `cas`; ignored for every other verb.
/// @return The bytes to send.
[[nodiscard]] std::string EncodeMemcachedStorage(std::string_view verb,
                                                 std::string_view key,
                                                 std::uint32_t flags,
                                                 std::int64_t exptime,
                                                 std::string_view value,
                                                 std::uint64_t casToken = 0);

/// Parse a non-negative integer that occupies the whole of @p text.
///
/// Exported so this wire has ONE strict-integer rule: the parser judges a `VALUE`
/// header's byte count with it, and a `cas` token typed on a command line is judged
/// identically. Strict on purpose -- a prefix parse would read `12x` as 12, which for
/// the byte count is a silently truncated value and for a cas token is a compare against
/// a number nobody typed.
/// @param text The token.
/// @param out Receives the value; untouched unless the whole token is an integer.
/// @return True when the whole token is an integer.
[[nodiscard]] bool ParseWholeUnsigned(std::string_view text, std::uint64_t& out) noexcept;

/// Whether @p text can travel as one token on this protocol.
///
/// The text protocol is space-separated and line-terminated with no quoting or
/// escaping, so a key carrying a space, a control character or a CR/LF is not
/// expressible. Refusing it here is what stops it being sent as two tokens and
/// silently addressing a different key -- the same class of defect as a length a peer
/// declares, pointing the other way.
/// @param text The candidate token.
/// @return True when it is expressible.
[[nodiscard]] bool ValidTextToken(std::string_view text) noexcept;

/// A memcached-text connection, as the verb handlers see it.
///
/// The seam exists for the same reason `IExchange` does: every handler is then
/// testable against a scripted exchange, and the REFUSAL arms are the half that
/// matters, since provoking a real `SERVER_ERROR` or a truncated value block needs a
/// daemon configured to misbehave.
class IMemcachedExchange
{
  public:
    IMemcachedExchange() = default;
    IMemcachedExchange(IMemcachedExchange const&) = delete;
    IMemcachedExchange(IMemcachedExchange&&) = delete;
    IMemcachedExchange& operator=(IMemcachedExchange const&) = delete;
    IMemcachedExchange& operator=(IMemcachedExchange&&) = delete;
    virtual ~IMemcachedExchange() = default;

    /// Send @p request and read one reply.
    /// @param request The already-encoded bytes.
    /// @return The reply, or why not.
    [[nodiscard]] virtual std::expected<McReply, ExchangeError> Send(std::string_view request) = 0;
};

} // namespace FastCache::Cli
