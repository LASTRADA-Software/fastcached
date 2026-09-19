// SPDX-License-Identifier: Apache-2.0
#include "NodeProofResponder.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Distributed/NodeProof.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A `NodeChallenge` or `ProveNode` payload that would not decode into its fixed-width fields.
    ///
    /// Counted apart from the rejection below for `SchedulerCredentialsMalformed`'s reason: a peer
    /// that cannot form the frame is a version or client-library mismatch, and one forming it
    /// correctly and failing to verify is the security question. Summed, the second hides inside
    /// the first whenever an old client is in the fleet.
    constexpr Cc::SurfaceRefusal RefusedMalformed { .code = Wire::ErrorCode::MalformedFrame,
                                                    .counter = IMetricsSink::Counter::NodeProofsMalformed };

    /// A signature that did not verify under the key the proof presented.
    ///
    /// **One answer for a signature over another handshake, another id or another key**, and
    /// deliberately so: the verification cannot tell them apart, and a refusal naming which field
    /// was wrong would be an oracle. Asked BEFORE the roster, so a caller who cannot sign learns
    /// nothing about which ids and keys this cluster holds.
    constexpr Cc::SurfaceRefusal RefusedRejected { .code = Wire::ErrorCode::NodeProofRejected,
                                                   .counter = IMetricsSink::Counter::NodeProofsRejected };

    /// A signature that verified under a key this cluster does not hold for the id it named.
    ///
    /// The caller IS who it says -- it signed -- and is simply not admitted, which is an
    /// operator's to change rather than the caller's. Its own code and counter, so neither reads as
    /// a forgery.
    constexpr Cc::SurfaceRefusal RefusedUnknownKey { .code = Wire::ErrorCode::NodeKeyUnknown,
                                                     .counter = IMetricsSink::Counter::NodeProofsRefusedUnknownKey };

    /// A signature that verified under a key this cluster REVOKED: the forgotten machine itself.
    ///
    /// Refused, and the connection is MARKED rather than closed: every later verb on it is refused
    /// as the forgotten machine's, from any address. Closing it would let the machine simply redial
    /// and be judged by its address, which `--fleet-member` may still admit.
    constexpr Cc::SurfaceRefusal RefusedRevokedKey { .code = Wire::ErrorCode::NodeKeyRevoked,
                                                     .counter = IMetricsSink::Counter::NodeProofsRefusedRevokedKey };

    /// This node's generator could not draw a handshake (#1527).
    ///
    /// `NoCluster`: this node cannot run the exchange right now, the caller did nothing wrong, and
    /// a caller reading `NoCluster` goes on being judged by its address exactly as before the verb
    /// existed. Never answered with a nonce or a key drawn from anywhere else -- a weak challenge is
    /// the replay window #1527 closes.
    constexpr Cc::UncountedRefusal NoNonce {
        .code = Wire::ErrorCode::NoCluster,
        .rationale = "this node's own random source cannot draw a handshake, which is a fact about this machine "
                     "rather than about the caller or the fleet; the Error beside it names the primitive and what it "
                     "answered, and a counter could carry neither",
    };

    /// A verb routed here that this component does not own.
    constexpr Cc::UncountedRefusal NotThisComponentsVerb {
        .code = Wire::UnimplementedVerb,
        .rationale = "a healthy answer, not an event: MergedResponder routes only the NodeProof family here, so only "
                     "a direct caller reaches this, and a client steps over it",
    };

    /// A proof verb reaching `Answer` at all.
    ///
    /// The endpoint terminates both, because what they change is connection state. A rise here
    /// says something about this process's own wiring and nothing about the fleet, which is
    /// exactly `LiveStatsResponder`'s reason for the verb IT does not terminate here.
    constexpr Cc::UncountedRefusal NotThroughAnswer {
        .code = Wire::UnimplementedVerb,
        .rationale = "the node-proof verbs are answered by the endpoint, which owns the handshake, the proven identity "
                     "and the seal because all three are connection state; reaching this means a caller went round the "
                     "endpoint, which is a wiring fact rather than an event in the fleet",
    };

    /// Why a size or opcode refusal on this surface moves nothing on its own.
    ///
    /// Stated once because the two arms share the reason, and `UncountedRefusal::rationale` is a
    /// forcing function rather than a field: what it forces is that somebody answered *would a
    /// rise here mean something happened*, and the answer is the same for both.
    constexpr std::string_view ShapeRefusalRationale =
        "a size or opcode refusal says the peer is confused about the framing rather than about which machine it is; "
        "summed into the node-proof series it would bury the refusals that mean an identity is wrong somewhere";

    /// Why a credential refusal here belongs to the scheduler.
    constexpr std::string_view CredentialIsTheSchedulersRationale =
        "the credential is the scheduler's -- AUTH is a Session verb and MergedResponder routes it there -- so the "
        "peer that presented it is counted against the component that checked it, once";

    /// One row per `EndpointRefusal`: what this surface does about it.
    struct NodeProofEndpointRefusal
    {
        EndpointRefusal refusal;                  ///< Which endpoint decision this describes.
        std::optional<Cc::SurfaceRefusal> answer; ///< The row, or nothing where this surface counts none.
        std::string_view rationale;               ///< Why nothing is counted; read only when `answer` is absent.
    };

    /// What this surface does about each endpoint-decided refusal.
    ///
    /// **The unchallenged row is the one counted row, and it is the only endpoint-decided refusal
    /// this surface can produce**: every other way a proof fails is decided in `Challenge` or
    /// `Verify`, which encode and count it there.
    constexpr EnumTable<EndpointRefusal, NodeProofEndpointRefusal> EndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          .answer = std::nullopt,
          .rationale = "the byte budget says this surface is momentarily full, which the peer sees and retries; "
                       "summed into a series read as a wrong key somewhere it is what makes that series unreadable" },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSchedulersRationale },
        { .refusal = EndpointRefusal::CredentialRejected,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSchedulersRationale },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .answer = std::nullopt,
          .rationale = AnswerDeadlineIsTheEndpointsRationale },
        { .refusal = EndpointRefusal::NodeProofUnchallenged,
          .answer = Cc::SurfaceRefusal { .code = Wire::ErrorCode::NodeProofUnchallenged,
                                         .counter = IMetricsSink::Counter::NodeProofsUnchallenged },
          .rationale = {} },
    } };

    static_assert(RowsInEnumeratorOrder(EndpointRefusals, &NodeProofEndpointRefusal::refusal),
                  "EndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    static_assert(Cc::RowsStateOneRefusalClaim(EndpointRefusals,
                                               [](NodeProofEndpointRefusal const& row) {
                                                   return Cc::RefusalClaim { .counted = row.answer.has_value(),
                                                                             .rationale = row.rationale };
                                               }),
                  "every node-proof endpoint refusal must state either a counted answer or a rationale, not both");

    /// Whether @p opRaw is one of the two verbs this surface owns.
    /// @param opRaw The third header byte, as received.
    /// @return True for `NodeChallenge` and `ProveNode`.
    [[nodiscard]] constexpr bool IsProofVerb(std::uint8_t opRaw) noexcept
    {
        return opRaw == static_cast<std::uint8_t>(Wire::Op::NodeChallenge)
               || opRaw == static_cast<std::uint8_t>(Wire::Op::ProveNode);
    }

} // namespace

Task<FrameReply> NodeProofResponder::Answer(std::span<std::byte const> frame, PeerIdentity /*peer*/)
{
    // The peer is not consulted, and the parameter is unnamed to say so: `RefusePeer` admits
    // everybody here by decision, so a gate reading it would refuse the population this surface
    // exists for.
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Not this protocol at all; empty is CLOSE, exactly as every other surface answers a
        // header that will not decode.
        co_return std::vector<std::byte> {};

    if (!IsProofVerb(header->opRaw))
        co_return Cc::RefuseWithoutCounter(NotThisComponentsVerb, "this node serves no component for that verb");

    co_return Cc::RefuseWithoutCounter(
        NotThroughAnswer, "the node-proof verbs are answered on the connection they establish, not through Answer");
}

std::optional<std::vector<std::byte>> NodeProofResponder::RefusePeer(PeerIdentity const& /*peer*/,
                                                                     std::uint8_t /*opRaw*/) const
{
    // Nobody, and both parameters are unnamed to say that is a decision rather than an oversight.
    // The machine asking is on no list, which is the entire problem being solved; the signature stands
    // in place of the list, and `Verify` is where it is checked.
    return std::nullopt;
}

std::vector<std::byte> NodeProofResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                        std::uint8_t /*opRaw*/,
                                                        std::string_view detail) const
{
    // Two arms, and the counted one is the CAP: `ProveNode`'s own `OpTable` row bounds it to
    // `MaxNodeProofPayload`, and an id plus a key and a signature comes nowhere near, so a header
    // declaring more came from no client of this tree at any version -- which is the same thing a
    // malformed payload says, and it shares that counter rather than earning a third.
    if (decision == Wire::PrePayloadDecision::PayloadTooLarge)
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCodeFor(decision), .counter = IMetricsSink::Counter::NodeProofsMalformed },
                          detail);
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCodeFor(decision), .rationale = ShapeRefusalRationale }, detail);
}

std::vector<std::byte> NodeProofResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                std::uint8_t /*opRaw*/,
                                                                std::string_view detail) const
{
    auto const& row = EndpointRefusals[static_cast<std::size_t>(refusal)];
    return AnswerEndpointRefusal(_metrics, ErrorCodeFor(refusal), row.answer, row.rationale, detail);
}

std::expected<NodeChallengeIssued, std::vector<std::byte>> NodeProofResponder::Challenge(std::span<std::byte const> payload)
{
    auto const request = Wire::DecodeNodeChallengePayload(payload);
    if (!request.has_value())
        return std::unexpected(Cc::Refuse(
            _metrics, RefusedMalformed, "a node challenge is a 32-byte nonce and a 32-byte ephemeral key, in that order"));

    // Named on the way out: no counter can carry WHICH machine is broken. Not throttled, because
    // this path is behind the credential gate wherever one is configured.
    auto const cannotDraw = [this](SecureRandomError const& why) {
        _logger.Log(LogLevel::Error,
                    std::format("node proof: refused a challenge, because this node cannot draw one: {}", why.ToString()));
        return std::unexpected(
            Cc::RefuseWithoutCounter(NoNonce, "this node cannot draw a handshake from its random source right now"));
    };
    auto const nonce = DrawNonce(_random);
    if (!nonce.has_value())
        return cannotDraw(nonce.error());
    auto ephemeral = Distributed::DrawNodeEphemeral(_random);
    if (!ephemeral.has_value())
        return cannotDraw(ephemeral.error());

    auto reply = Distributed::AnswerNodeChallenge(
        _identity,
        *request,
        Distributed::ServerHello { .serverId = _nodeId, .nonce = *nonce, .ephemeral = ephemeral->publicKey });
    auto encoded = Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeNodeChallengeReply(reply));
    return NodeChallengeIssued {
        .handshake = NodeHandshake { .request = *request,
                                     .reply = std::move(reply),
                                     .ephemeralSecret = std::move(ephemeral->secret) },
        .reply = std::move(encoded),
    };
}

NodeProofVerdict NodeProofResponder::Verify(NodeHandshake const& handshake, std::span<std::byte const> payload)
{
    auto const proof = Wire::DecodeProveNodePayload(payload);
    if (!proof.has_value())
        return NodeProofVerdict {
            .identity = std::nullopt,
            .keys = std::nullopt,
            .reply = Cc::Refuse(_metrics,
                                RefusedMalformed,
                                "a node proof is an id, a 32-byte identity key and a 64-byte signature, in that order"),
        };

    // The session first: every answer from here on is sealed under it, whatever it says, so the
    // caller reads the answer with the one grammar it expects. A caller whose ephemeral key is
    // low-order agreed no key at all, and is answered in the clear as a proof that does not verify.
    auto keys = Distributed::DeriveNodeSessionKeys(
        handshake.ephemeralSecret, handshake.request, handshake.reply, proof->nodeId, /*callerSide=*/false);
    if (!keys.has_value())
        return NodeProofVerdict { .identity = std::nullopt,
                                  .keys = std::nullopt,
                                  .reply = Cc::Refuse(_metrics, RefusedRejected, "the handshake agreed no session key") };

    auto const sealed = [&keys](std::vector<std::byte> reply, std::optional<ProvenIdentity> identity) {
        return NodeProofVerdict { .identity = std::move(identity), .keys = std::move(keys), .reply = std::move(reply) };
    };

    // The signature under the key the caller presented, and the roster only after: a caller who
    // cannot sign learns nothing about which ids and keys this cluster holds.
    if (!Distributed::VerifyNodeProof(handshake.request, handshake.reply, *proof))
        return sealed(Cc::Refuse(_metrics,
                                 RefusedRejected,
                                 "the signature does not verify under the identity key this proof presented"),
                      std::nullopt);

    auto identity = ProvenIdentity { .id = proof->nodeId, .key = {} };
    std::ranges::copy(proof->publicKey, identity.key.begin());

    // The one door to what the cluster holds: the admission oracle's key question, so the answer a
    // proof gets here and the answer every later verb on the connection gets are one fold.
    auto const standing = _roster.ExplainKey(identity);
    if (standing.decidedBy.Has(Distributed::MembershipParticipant::KeyTombstone))
        return sealed(Cc::Refuse(_metrics,
                                 RefusedRevokedKey,
                                 "this cluster revoked that identity key; every request on this connection is refused "
                                 "as the forgotten machine's"),
                      std::move(identity));
    if (!Distributed::RestsOnProvenIdentity(standing))
        return sealed(Cc::Refuse(_metrics,
                                 RefusedUnknownKey,
                                 std::format("this cluster holds no such key for {}: admit it with --enroll-from or "
                                             "--cluster-admit-worker",
                                             identity.id)),
                      std::nullopt);

    _metrics.Increment(IMetricsSink::Counter::NodeProofsAccepted);
    return sealed(Wire::EncodeReply(Wire::Status::Ok, {}), std::move(identity));
}

} // namespace FastCache::Node
