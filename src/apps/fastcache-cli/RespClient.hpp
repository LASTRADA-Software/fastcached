// SPDX-License-Identifier: Apache-2.0
#pragma once

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

/// @file RespClient.hpp
/// A client-side RESP codec: request encoder and reply parser.
///
/// **This is written here because the tree has no client half.** `Protocol/RedisResp.cpp`
/// parses requests and writes replies; nothing anywhere reads a reply. The existing
/// tests compare whole literal reply strings by hand, so there was nothing to lift.
///
/// The parser is a pure function over bytes with an explicit *incomplete* answer,
/// rather than a coroutine over an `ISocket`. That is what lets every reply
/// shape -- including the malformed and truncated ones, which are the interesting
/// half and the ones a real server will not produce on demand -- be tested against
/// a byte string with no socket and no daemon.

/// A RESP reply's type.
///
/// A private enum: nothing here persists or transmits an ordinal of it. The *bytes*
/// are the contract and they live in `TypeMarker` below.
///
/// RESP3 types are parsed even though this client does not send `HELLO 3`. Making
/// the parser total costs a few lines now; meeting an unexpected type later and
/// reporting it as *malformed* would send somebody hunting a corruption that is not
/// there.
enum class RespType : std::uint8_t
{
    SimpleString, ///< `+OK`
    Error,        ///< `-ERR ...`
    Integer,      ///< `:42`
    BulkString,   ///< `$5\r\nhello`
    Null,         ///< `$-1`, `*-1` (RESP2) or `_` (RESP3)
    Array,        ///< `*2\r\n...`
    Boolean,      ///< `#t` / `#f` (RESP3)
    Double,       ///< `,3.14` (RESP3)
    BigNumber,    ///< `(123...` (RESP3)
    Verbatim,     ///< `=15\r\ntxt:...` (RESP3); what `INFO` returns under RESP3
    Map,          ///< `%2\r\n...` (RESP3); items alternate key, value
    Set,          ///< `~3\r\n...` (RESP3)
    Push,         ///< `>3\r\n...` (RESP3); an out-of-band pub/sub message
    Last,
};

/// One reply type's name, for a diagnostic an operator can act on.
struct RespTypeSpec
{
    RespType type;         ///< The enumerator this row describes.
    std::string_view name; ///< What to call it in a message.
};

/// The reply types, one row per enumerator, in enumerator order.
inline constexpr EnumTable<RespType, RespTypeSpec> RespTypeTable { {
    { .type = RespType::SimpleString, .name = "simple string" },
    { .type = RespType::Error, .name = "error" },
    { .type = RespType::Integer, .name = "integer" },
    { .type = RespType::BulkString, .name = "bulk string" },
    { .type = RespType::Null, .name = "null" },
    { .type = RespType::Array, .name = "array" },
    { .type = RespType::Boolean, .name = "boolean" },
    { .type = RespType::Double, .name = "double" },
    { .type = RespType::BigNumber, .name = "big number" },
    { .type = RespType::Verbatim, .name = "verbatim string" },
    { .type = RespType::Map, .name = "map" },
    { .type = RespType::Set, .name = "set" },
    { .type = RespType::Push, .name = "push message" },
} };

static_assert(RowsInEnumeratorOrder(RespTypeTable, &RespTypeSpec::type),
              "RespTypeTable must hold one row per RespType, in enumerator order");

/// What to call @p type in a diagnostic.
/// @param type The reply type.
/// @return Its name, or `unknown` for a value outside the table.
[[nodiscard]] std::string_view RespTypeName(RespType type) noexcept;

/// A parsed reply.
struct RespValue
{
    RespType type { RespType::Null }; ///< Which member below is meaningful.
    std::string text {};              ///< SimpleString, Error, BulkString, BigNumber, Verbatim.
    std::int64_t integer { 0 };       ///< Integer.
    double real { 0.0 };              ///< Double.
    bool boolean { false };           ///< Boolean.
    std::vector<RespValue> items {};  ///< Array, Map, Set, Push.
};

/// Whether @p value is an error reply.
/// @param value The reply.
/// @return True for `RespType::Error`.
[[nodiscard]] bool IsError(RespValue const& value) noexcept;

/// Whether @p value is any of RESP's several spellings of nothing.
///
/// One predicate because RESP2 and RESP3 disagree about which byte means it -- a
/// `$-1`, a `*-1` and a `_` are the same fact -- and a caller testing for one of them
/// reads a miss as a value on the other protocol version.
/// @param value The reply.
/// @return True when the reply is a null.
[[nodiscard]] bool IsNull(RespValue const& value) noexcept;

/// The leading token of an error reply, which is its machine-readable code.
///
/// RESP errors are `-<CODE> <sentence>`, and the code is the part a client may branch
/// on -- `WRONGPASS`, `NOAUTH`, `ERR`. The sentence is the server's to reword, so a
/// client that matches on it breaks at the next wording change; one that matches the
/// code word does not.
/// @param text The error text, without the leading `-`.
/// @return The first whitespace-delimited word, or the whole text when there is one word.
[[nodiscard]] std::string_view ErrorCodeWord(std::string_view text) noexcept;

/// The error code words this client branches on.
///
/// Shared vocabulary rather than a literal at each site: the AUTH gate reads
/// `WrongPass` when deciding whether a refusal is fatal, and the verb handlers read
/// `NoAuth` when deciding whether to tell the operator about `FASTCACHE_TOKEN`. Two
/// files spelling the same word separately is how they come to disagree.
namespace ErrorCode
{
    /// The credential was wrong. About the credential, so it is fatal.
    constexpr std::string_view WrongPass = "WRONGPASS";
    /// A credential is required and none was accepted on this connection.
    constexpr std::string_view NoAuth = "NOAUTH";
} // namespace ErrorCode

/// Caps this side imposes on a reply, regardless of what the peer declares.
///
/// **A peer-declared length sizes nothing until it has been checked against a bound
/// this side chose.** The daemon is not automatically the trusted half here: this
/// tool dials whatever `--host`/`FASTCACHE_ADDR` names, and in the fleet case an
/// endpoint a scheduler named. Injected rather than baked in so a test can drive the
/// refusal with three bytes instead of gigabytes.
struct ParseLimits
{
    /// Largest single bulk string or verbatim payload accepted.
    ///
    /// Matches the daemon's own 16 MiB value cap, so a value the server was willing to
    /// store is a value this client is willing to read back.
    std::size_t maxBulkBytes { 16U * 1024U * 1024U };

    /// Largest element count accepted for one aggregate.
    std::size_t maxElements { 1024U * 1024U };

    /// Deepest nesting accepted.
    ///
    /// A recursive-descent parser turns declared nesting into stack frames, so this is
    /// the bound that stops a hostile `*1\r\n` repeated a million times from being a
    /// crash rather than a refusal.
    std::size_t maxDepth { 32 };
};

/// How far a parse got.
enum class ParseState : std::uint8_t
{
    /// A whole reply was read. `consumed` says how many bytes it took.
    Complete,
    /// The bytes are a valid prefix of a reply; read more and try again.
    ///
    /// Distinct from `Malformed` on purpose, and it is the distinction the read loop
    /// is built on: *needs more bytes* and *will never be a reply* are opposite
    /// diagnoses, and collapsing them makes a slow network look like a broken server.
    Incomplete,
    /// The bytes cannot be a reply, or exceed a `ParseLimits` bound.
    Malformed,
    Last,
};

/// The outcome of one parse attempt.
struct ParseResult
{
    ParseState state { ParseState::Incomplete }; ///< How far it got.
    RespValue value {};                          ///< Meaningful iff `state == Complete`.
    std::size_t consumed { 0 };                  ///< Bytes the reply occupied; meaningful iff `state == Complete`.
    std::string diagnostic {};                   ///< Why; non-empty iff `state == Malformed`.
};

/// Parse one reply from the front of @p bytes.
///
/// @param bytes The bytes read so far. Not required to be a whole reply.
/// @param limits Caps this side imposes.
/// @return How far the parse got; see ParseState.
[[nodiscard]] ParseResult ParseReply(std::string_view bytes, ParseLimits const& limits = {});

/// Encode one command as a RESP array of bulk strings.
///
/// **Always an array, never the inline form**, and that is not a style choice:
/// `ProtocolAutodetect` classifies a connection from its first byte, and RESP is
/// recognised by one of `* + - : $`. An inline command starts with a letter, so it
/// would be handed to the memcached text handler -- which would answer `ERROR` to a
/// perfectly good `GET`, on the first command of every connection.
///
/// @param argv The command and its arguments.
/// @return The bytes to send.
[[nodiscard]] std::vector<std::byte> EncodeCommand(std::span<std::string const> argv);

/// Why an exchange did not produce a reply.
///
/// Three failures rather than one because they are fixed by three different people:
/// nothing is listening, the connection broke mid-answer, or the answer was
/// unintelligible. They map onto three distinct exit codes for the same reason.
enum class ExchangeFailure : std::uint8_t
{
    Unreachable, ///< The connection could not be established.
    Transport,   ///< It was established and then failed, or timed out.
    Malformed,   ///< Bytes arrived and are not a reply this client can read.
    Last,
};

/// An exchange that produced no reply.
struct ExchangeError
{
    ExchangeFailure kind { ExchangeFailure::Unreachable }; ///< What went wrong.
    std::string detail {};                                 ///< Human-readable specifics.
};

/// The one thing a verb handler needs from the server.
///
/// A seam rather than a concrete client, so every verb handler is a pure function
/// over a scripted exchange in the tests -- which is the only way the refusal arms
/// get covered, since provoking a real `NOAUTH` or a real crossed reply needs a
/// daemon configured to misbehave.
class IExchange
{
  public:
    IExchange() = default;
    IExchange(IExchange const&) = delete;
    IExchange(IExchange&&) = delete;
    IExchange& operator=(IExchange const&) = delete;
    IExchange& operator=(IExchange&&) = delete;
    virtual ~IExchange() = default;

    /// Send one command and read its reply.
    /// @param argv The command and its arguments.
    /// @return The reply, or why there was none.
    [[nodiscard]] virtual std::expected<RespValue, ExchangeError> Call(std::span<std::string const> argv) = 0;
};

/// Send one command, spelled as string literals.
///
/// A convenience over `IExchange::Call` for the handlers, whose arguments are mostly
/// fixed words plus an operand or two.
/// @param exchange The connection.
/// @param argv The command and its arguments.
/// @return The reply, or why there was none.
[[nodiscard]] std::expected<RespValue, ExchangeError> Call(IExchange& exchange,
                                                           std::initializer_list<std::string_view> argv);

} // namespace FastCache::Cli
