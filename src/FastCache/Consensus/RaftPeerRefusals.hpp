// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache::Consensus
{

/// Why the accepting end of a Raft peer connection refused it (#1308).
///
/// Private: never transmitted. Each is its own counter because each names a different
/// thing to go and fix -- the rule `.agent/rules/metrics-and-observability.md` states as
/// "the row is the REFUSAL, not the code".
enum class AcceptorRefusal : std::uint8_t
{
    NoHandshake,      ///< The first frame was not a proof this build reads.
    HandshakeTimeout, ///< No proof within `RaftWire::HandshakeBound`.
    Proof,            ///< The proof did not verify: no key, or another one.
    WrongTarget,      ///< Proved the key, but dialled another member here.
    OwnId,            ///< Proved the key, under this node's own id.
    FrameTag,         ///< A session frame's tag did not verify.
    FrameSender,      ///< A verified message named a sender other than the proven dialler.
    Full,             ///< The listener already served as many connections as it holds.
    Last,             ///< Not a refusal, and has no row.
};

/// Why the dialling end of a Raft peer connection gave it up (#1308).
///
/// Private: never transmitted.
enum class DiallerRefusal : std::uint8_t
{
    Timeout,         ///< No challenge, or no verdict, within `RaftWire::HandshakeBound`.
    NoChallenge,     ///< The acceptor opened with something other than a challenge this build reads.
    AcceptorProof,   ///< The acceptor's verdict did not verify: it does not hold this key.
    WrongTarget,     ///< A verified verdict: another member answers at this address.
    OwnId,           ///< A verified verdict: the acceptor holds this node's own id.
    EndedByAcceptor, ///< The acceptor closed after the proof, with no signed verdict.
    Last,            ///< Not a refusal, and has no row.
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
      .says = "it did not prove the cluster key within the handshake bound" },
    { .refusal = AcceptorRefusal::Proof,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedProof,
      .says = "its proof did not verify: it holds a different cluster key, or none" },
    { .refusal = AcceptorRefusal::WrongTarget,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedWrongTarget,
      .says = "it proved the key but dialled another member at this address, so its record of that member is stale" },
    { .refusal = AcceptorRefusal::OwnId,
      .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedOwnId,
      .says = "it proved the key under this node's own id: a copied --cluster-dir or a duplicated --node-id" },
    { .refusal = AcceptorRefusal::FrameTag,
      .counter = IMetricsSink::Counter::RaftPeerFramesRefusedTag,
      .says = "a frame's tag did not verify: changed, injected, replayed, reordered or from another connection" },
    { .refusal = AcceptorRefusal::FrameSender,
      .counter = IMetricsSink::Counter::RaftPeerFramesRefusedSender,
      .says = "a verified message named a sender other than the member the connection proved" },
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
      .says = "it sent no challenge or no verdict within the handshake bound: a build from before the handshake, or "
              "not a Raft port" },
    { .refusal = DiallerRefusal::NoChallenge,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedNoChallenge,
      .says = "it opened with something other than a challenge this build reads" },
    { .refusal = DiallerRefusal::AcceptorProof,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorProof,
      .says = "its verdict did not verify: whatever answers there does not hold this cluster key" },
    { .refusal = DiallerRefusal::WrongTarget,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedWrongTarget,
      .says = "another member answers at that address, so this node's record of the peer is stale" },
    { .refusal = DiallerRefusal::OwnId,
      .counter = IMetricsSink::Counter::RaftPeerDialsRefusedOwnId,
      .says = "it answers to this node's own id: a copied --cluster-dir or a duplicated --node-id" },
    { .refusal = DiallerRefusal::EndedByAcceptor,
      .counter = IMetricsSink::Counter::RaftPeerDialsEndedByAcceptor,
      .says = "it closed after this node's proof without a signed verdict: most likely this node's cluster key is "
              "not the peer's" },
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
