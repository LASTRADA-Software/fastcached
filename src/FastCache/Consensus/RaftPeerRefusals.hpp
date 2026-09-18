// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache::Consensus
{

/// Why the accepting end of a Raft peer connection refused it, or ended it (#1308, #178).
///
/// Private: never transmitted. Each is its own counter because each names a different
/// thing to go and fix -- the rule `.agent/rules/metrics-and-observability.md` states as
/// "the row is the REFUSAL, not the code".
enum class AcceptorRefusal : std::uint8_t
{
    NoHandshake,      ///< The first frame was not a proof this build reads.
    HandshakeTimeout, ///< No proof within `RaftWire::HandshakeBound`.
    Proof,            ///< The proof's signature did not verify under the key the roster holds for its id.
    UnknownKey,       ///< The roster holds no key for the id the proof claims.
    RevokedKey,       ///< The proof's signature verified under a key the roster has revoked.
    WrongTarget,      ///< Proved its id, but dialled another member here.
    OwnId,            ///< Proved this node's own id: it holds this node's private key.
    FrameTag,         ///< A session frame's tag did not verify.
    FrameSender,      ///< A verified message named a sender other than the proven dialler.
    KeyWithdrawn,     ///< The roster stopped naming the key the session was proved with.
    Full,             ///< The listener already served as many connections as it holds.
    Last,             ///< Not a refusal, and has no row.
};

/// Why the dialling end of a Raft peer connection gave it up (#1308, #178).
///
/// Private: never transmitted.
enum class DiallerRefusal : std::uint8_t
{
    Timeout,            ///< No challenge, or no verdict, within `RaftWire::HandshakeBound`.
    NoChallenge,        ///< The acceptor opened with something other than a challenge this build reads.
    AcceptorProof,      ///< The verdict's signature did not verify under the key held for the member that answered.
    AcceptorKeyUnknown, ///< The roster holds no key for the member that answered.
    AcceptorKeyRevoked, ///< The member that answered signed with a key the roster has revoked.
    WrongTarget,        ///< A verified verdict: another member answers at this address.
    OwnId,              ///< A verified verdict: the acceptor proved this node's own id from this node.
    OwnKeyRevoked,      ///< A verified verdict: the acceptor's roster has revoked this node's key.
    EndedByAcceptor,    ///< The acceptor closed after the proof, with no signed verdict.
    KeyWithdrawn,       ///< The roster stopped naming the key the acceptor proved the session with.
    Last,               ///< Not a refusal, and has no row.
};

/// One refusal: the counter it moves and what the log line says about it.
struct AcceptorRefusalRow
{
    AcceptorRefusal refusal;       ///< The refusal this row describes.
    IMetricsSink::Counter counter; ///< The series it moves.
    std::string_view says;         ///< The log line's reason, completing "refused ... because ".
};

/// One refusal: the counter it moves and what the log line says about it.
struct DiallerRefusalRow
{
    DiallerRefusal refusal;        ///< The refusal this row describes.
    IMetricsSink::Counter counter; ///< The series it moves.
    std::string_view says;         ///< The log line's reason, completing "gave up ... because ".
};

/// One row per `AcceptorRefusal`, in enumerator order.
inline constexpr EnumTable<AcceptorRefusal, AcceptorRefusalRow> AcceptorRefusals { {
    { .refusal = AcceptorRefusal::NoHandshake,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedNoHandshake,
      .says = "its first frame was not a handshake proof this build reads" },
    { .refusal = AcceptorRefusal::HandshakeTimeout,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedHandshakeTimeout,
      .says = "it did not prove its id within the handshake bound" },
    { .refusal = AcceptorRefusal::Proof,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedProof,
      .says = "its proof's signature did not verify under the key this node holds for the id it claims: another machine "
              "claiming that id" },
    { .refusal = AcceptorRefusal::UnknownKey,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedUnknownKey,
      .says = "this node holds no key for the id it claims: give that member's key with @<key> on --raft-peer, or admit "
              "it with --cluster-admit=<id>=<host>:<port>@<key>" },
    { .refusal = AcceptorRefusal::RevokedKey,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedRevokedKey,
      .says = "it proved itself with a key the cluster has revoked: a machine that was removed" },
    { .refusal = AcceptorRefusal::WrongTarget,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedWrongTarget,
      .says = "it proved its id but dialled another member at this address, so its record of that member is stale" },
    { .refusal = AcceptorRefusal::OwnId,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedOwnId,
      .says = "it proved this node's own id, so it holds this node's private key: a copied --cluster-dir" },
    { .refusal = AcceptorRefusal::FrameTag,
      .counter = IMetricsSink::Counter::RaftPeerFramesRefusedTag,
      .says = "a frame's tag did not verify: changed, injected, replayed, reordered or from another connection" },
    { .refusal = AcceptorRefusal::FrameSender,
      .counter = IMetricsSink::Counter::RaftPeerFramesRefusedSender,
      .says = "a verified message named a sender other than the member the connection proved" },
    { .refusal = AcceptorRefusal::KeyWithdrawn,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsEndedKeyWithdrawn,
      .says = "the cluster no longer holds the key this connection was proved with: it was revoked or replaced" },
    { .refusal = AcceptorRefusal::Full,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedFull,
      .says = "the listener already serves as many peer connections as it holds" },
} };

static_assert(RowsInEnumeratorOrder(AcceptorRefusals, &AcceptorRefusalRow::refusal),
              "AcceptorRefusals must hold one row per AcceptorRefusal, in enumerator order");

/// One row per `DiallerRefusal`, in enumerator order.
inline constexpr EnumTable<DiallerRefusal, DiallerRefusalRow> DiallerRefusals { {
    { .refusal = DiallerRefusal::Timeout,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedTimeout,
      .says = "it sent no challenge or no verdict within the handshake bound: a build from before this handshake, or "
              "not a Raft port" },
    { .refusal = DiallerRefusal::NoChallenge,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedNoChallenge,
      .says = "it opened with something other than a challenge this build reads" },
    { .refusal = DiallerRefusal::AcceptorProof,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorProof,
      .says = "its verdict's signature did not verify under the key this node holds for the member that answered: "
              "another machine answering under that id" },
    { .refusal = DiallerRefusal::AcceptorKeyUnknown,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorKeyUnknown,
      .says = "this node holds no key for the member that answered: give its key with @<key> on --raft-peer" },
    { .refusal = DiallerRefusal::AcceptorKeyRevoked,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorKeyRevoked,
      .says = "the member that answered signed with a key the cluster has revoked: a machine that was removed" },
    { .refusal = DiallerRefusal::WrongTarget,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedWrongTarget,
      .says = "another member answers at that address, so this node's record of the peer is stale" },
    { .refusal = DiallerRefusal::OwnId,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedOwnId,
      .says = "it holds this node's own private key: a copied --cluster-dir" },
    { .refusal = DiallerRefusal::OwnKeyRevoked,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedOwnKeyRevoked,
      .says = "the cluster has revoked this node's key: this machine was removed, and must mint a new identity (a fresh "
              "--cluster-dir) and be admitted under it" },
    { .refusal = DiallerRefusal::EndedByAcceptor,
      .counter = IMetricsSink::Counter::RaftPeerDialsEndedByAcceptor,
      .says = "it closed after this node's proof without a signed verdict: most likely it holds no key for this node's "
              "id, or a different one" },
    { .refusal = DiallerRefusal::KeyWithdrawn,
      .counter = IMetricsSink::Counter::RaftPeerDialsEndedKeyWithdrawn,
      .says = "the cluster no longer holds the key the peer proved this connection with: it was revoked or replaced" },
} };

static_assert(RowsInEnumeratorOrder(DiallerRefusals, &DiallerRefusalRow::refusal),
              "DiallerRefusals must hold one row per DiallerRefusal, in enumerator order");

/// The row for @p refusal.
/// @param refusal Never `Last`.
/// @return Its row.
[[nodiscard]] constexpr AcceptorRefusalRow const& RowFor(AcceptorRefusal refusal) noexcept
{
    return AcceptorRefusals[static_cast<std::size_t>(refusal)];
}

/// The row for @p refusal.
/// @param refusal Never `Last`.
/// @return Its row.
[[nodiscard]] constexpr DiallerRefusalRow const& RowFor(DiallerRefusal refusal) noexcept
{
    return DiallerRefusals[static_cast<std::size_t>(refusal)];
}

} // namespace FastCache::Consensus
