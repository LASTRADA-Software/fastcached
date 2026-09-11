// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "CliValue.hpp"
#include "RespClient.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file NodeClient.hpp
/// The client half of the `0xFC` compile-cache wire, for the operator verbs.
///
/// ## Why this exists
///
/// `fastcache-compile-node` speaks `0xFC` and **nothing else**. Measured against the
/// running node on this machine: RESP `PING`, RESP `INFO` and memcached `version` each
/// closed the connection having sent nothing at all, while a `0xFC` header got a proper
/// refusal. So `fastcache-cli version` reported *the server closed the connection
/// without answering* -- a true sentence and a useless one, because it describes what
/// happened and not what to do.
///
/// ## The THREE refusal states, which is the whole of the design
///
/// A client that reads a refusal as a binary -- worked, did not work -- gets exactly
/// the failure this tree has already paid for twice (#283, #340): a `FASTCACHE_TOKEN`
/// launcher took `DispatchNotPermitted` for *not implemented*, stepped over it, and
/// earned a permanent 0% hit rate that presented as a cold cache. Correct objects,
/// green build, no counter moving.
///
///   - **`UnimplementedVerb`** (`0x02`) -- *this endpoint does not implement that*. The
///     verb may be perfectly implementable and this build simply has no component for
///     it. A client STEPS OVER it: the operator is told which endpoint lacks which verb
///     and the exit code says *refused*, never *unreachable*.
///   - **`DispatchNotPermitted`** (`0x0C`) -- *that job is done somewhere else*. A
///     routing fact, so the remedy is a different address rather than a different
///     build. Fatal, and it must never be reported as *your node is too old*.
///   - **Everything else** -- an ordinary refusal, relayed BY NAME with whatever the
///     server said. `NotAMember` is the one an operator meets in practice, and its
///     remedy (`--fleet-member`) is on the node rather than here.
///
/// The distinction is a TABLE column below, not a `switch` at a call site, for the
/// reason the rulebook gives: three surfaces spelling one refusal separately is exactly
/// how they came to disagree.

/// What a client does about one refusal code.
enum class NodeRefusalKind : std::uint8_t
{
    /// The endpoint does not implement this verb. Step over it and say so.
    Unimplemented,
    /// The verb is served somewhere else. Fatal, and the remedy is an address.
    ServedElsewhere,
    /// An ordinary refusal, relayed with the server's own words.
    Reported,
    Last,
};

/// One reply from a `0xFC` endpoint.
///
/// The payload is OWNED rather than a view onto the read buffer. `CallerContext`'s
/// lesson: a struct a decoder returns by value must not borrow from the bytes it
/// decoded, because `Decode(Encode(x))` is the obvious spelling and is a use-after-free
/// the moment one member becomes a view. Here the result outlives the buffer in
/// practice -- a handler holds it across statements -- so the rule selects OWN.
struct NodeReply
{
    CompileCacheWire::Status status { CompileCacheWire::Status::Ok }; ///< What the reply said.
    std::vector<std::byte> payload {};                                ///< Its body.

    /// The refusal this carries, for a non-`Ok` status.
    std::optional<CompileCacheWire::ErrorCode> code {};

    /// What the server said about it; empty when it said nothing.
    std::string detail {};
};

/// How a client should treat @p code.
///
/// @param code What the server answered.
/// @return Which of the three states it is.
[[nodiscard]] NodeRefusalKind ClassifyRefusal(CompileCacheWire::ErrorCode code) noexcept;

/// A sentence for an operator about @p reply, or empty when it needs none.
///
/// Separate from `ClassifyRefusal` so the wording is testable without a socket, and so
/// the two cannot be spelled differently at two call sites.
/// @param verb The verb as an operator typed it.
/// @param endpoint Where it was sent, as `host:port`.
/// @param reply The refusal.
/// @return The diagnostic.
[[nodiscard]] std::string ExplainRefusal(std::string_view verb, std::string_view endpoint, NodeReply const& reply);

/// The one thing a node verb needs from the server.
///
/// A seam rather than a concrete client, so every handler is a pure function over a
/// scripted exchange -- which is the only way the refusal arms get covered, since
/// provoking a real `DispatchNotPermitted` needs a node configured to misbehave.
class INodeExchange
{
  public:
    INodeExchange() = default;
    INodeExchange(INodeExchange const&) = delete;
    INodeExchange(INodeExchange&&) = delete;
    INodeExchange& operator=(INodeExchange const&) = delete;
    INodeExchange& operator=(INodeExchange&&) = delete;
    virtual ~INodeExchange() = default;

    /// Send one already-framed request and read its reply.
    ///
    /// The caller frames it, because the wire header owns the encoders and a second
    /// framing here would be a second place for the version byte to be wrong.
    /// @param request The framed request.
    /// @return The reply, or why there was none.
    [[nodiscard]] virtual std::expected<NodeReply, ExchangeError> Send(std::span<std::byte const> request) = 0;

    /// Where this exchange is pointed, as `host:port`, for a diagnostic.
    ///
    /// Spelled `Address` and not `Endpoint` deliberately: a member function called
    /// `Endpoint` HIDES the `Endpoint` type inside every class that implements this, so
    /// `Open(Endpoint const&, ...)` stops naming a type and the error points at the
    /// parameter rather than at the accessor.
    /// @return The endpoint text.
    [[nodiscard]] virtual std::string_view Address() const = 0;
};

/// What an endpoint turned out to be, when a cache verb could not reach it.
///
/// **Three states, and the third is the one that gets collapsed.** A client that can
/// only say *reachable* or *not* reports `the server closed the connection without
/// answering` for a compile node -- a true sentence describing what happened and not
/// what to do, which is what sent an operator looking for a crashed daemon that was
/// never there.
enum class RemoteKind : std::uint8_t
{
    /// It answered `NodeStatus`: a `fastcache-compile-node`, which holds no user
    /// keyspace and serves no RESP and no memcached text.
    CompileNode,
    /// It speaks `0xFC` and does not implement the operator verbs. That is the cache
    /// DAEMON -- whose `0xFC` surface serves the compile-cache verbs and nothing about
    /// a node -- and it is also what a compile node older than these verbs looks like.
    /// This client cannot tell those two apart over the wire, so it says so rather than
    /// picking one.
    FastcacheWireOnly,
    /// Nothing recognisable came back. The port is open and is not this protocol.
    NotFastcacheWire,
    Last,
};

/// One `RemoteKind`'s rows: what it says to a person, and what it PROVES to a caller.
struct RemoteKindSpec
{
    RemoteKind kind; ///< The enumerator this row describes.

    /// The outcome this kind ESTABLISHES, or disengaged when it establishes nothing.
    ///
    /// **A probe answer is evidence about reachability, not only text for a human.**
    /// `ProbeRemote` reaches `CompileNode` and `FastcacheWireOnly` only when a frame came
    /// BACK, so each of them proves the endpoint was reached and answered -- which is the
    /// one thing `Outcome::Unreachable` denies. Identifying the endpoint in an advisory
    /// and leaving the outcome alone fixes the half an operator reads by eye and leaves a
    /// SCRIPT -- which reads the exit code and nothing else -- told that the port is dead.
    /// That is this enum's own state collapse surviving in the half nobody looks at, and
    /// the exit code is the published half.
    ///
    /// `Refused` rather than `Protocol`: bytes this client understood perfectly came
    /// back, and they said this endpoint does not serve that verb. It is also what the
    /// node verbs already exit with when a node answers `UnimplementedVerb`, so one
    /// address does not report two codes for one fact.
    ///
    /// `NotFastcacheWire` is disengaged, and that is a reading rather than an omission:
    /// no frame came back, so nothing was established that the caller's own transport
    /// diagnostic does not already say, and there is nothing here to correct it with.
    std::optional<Outcome> established;
};

/// What each kind establishes, one row per enumerator, in enumerator order.
inline constexpr EnumTable<RemoteKind, RemoteKindSpec> RemoteKindTable { {
    { .kind = RemoteKind::CompileNode, .established = Outcome::Refused },
    { .kind = RemoteKind::FastcacheWireOnly, .established = Outcome::Refused },
    { .kind = RemoteKind::NotFastcacheWire, .established = std::nullopt },
} };

static_assert(RowsInEnumeratorOrder(RemoteKindTable, &RemoteKindSpec::kind),
              "RemoteKindTable must hold one row per RemoteKind, in enumerator order");

/// What @p kind establishes about the endpoint.
/// @param kind What the probe turned out to find.
/// @return The outcome it proves, or nothing when it proves nothing new.
[[nodiscard]] std::optional<Outcome> EstablishedBy(RemoteKind kind) noexcept;

/// Ask an endpoint what it is.
///
/// One round trip, on the FAILURE path only: the common case never pays for it, which
/// is why this is a probe a caller reaches for rather than a handshake every command
/// performs.
/// @param node The `0xFC` connection to ask on.
/// @return What it turned out to be.
[[nodiscard]] RemoteKind ProbeRemote(INodeExchange& node);

/// What to tell an operator whose verb could not be answered by @p kind.
///
/// Pure over the classification, so every sentence is testable without a socket -- and
/// so the wording cannot be spelled one way here and another way at a second call site.
/// @param verb The verb as an operator typed it.
/// @param endpoint Where it was sent, as `host:port`.
/// @param kind What the endpoint turned out to be.
/// @return The diagnostic, or empty when the probe explains nothing the caller does not
///         already know.
[[nodiscard]] std::string ExplainRemoteKind(std::string_view verb, std::string_view endpoint, RemoteKind kind);

/// Read a `NodeMetrics` body back as a record.
///
/// Lives here rather than in the verb handler because TWO callers need it: the
/// `node-metrics` verb and the stats ladder's `0xFC` rung, which is the rung that makes
/// `stats` work against a node at all. A second copy would be a second place for the
/// field naming to drift, and the field names are what an operator greps.
///
/// **Every row the node sent is reported, zeroes included.** A counter is a tally, so
/// zero is the truth about events that never happened; dropping the zero rows here
/// would undo the one distinction the encoder went out of its way to preserve.
/// @param payload The reply body.
/// @return The record in the order the node sent it, or nullopt when malformed.
[[nodiscard]] std::optional<Value> DecodeNodeCounters(std::span<std::byte const> payload);

/// Read one framed reply out of @p bytes.
///
/// **Loops to a TERMINAL status at the transport layer, never here.** This decodes one
/// frame; `Status::Progress` is a pulse and is a terminal-status question the caller's
/// read loop answers, which is why `IsTerminalStatus` lives in the wire header with
/// four other readers.
///
/// @param bytes One whole reply frame, header included.
/// @return The reply, or why it is not one.
[[nodiscard]] std::expected<NodeReply, ExchangeError> DecodeNodeReply(std::span<std::byte const> bytes);

} // namespace FastCache::Cli
