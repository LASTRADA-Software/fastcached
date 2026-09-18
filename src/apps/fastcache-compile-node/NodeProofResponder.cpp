// SPDX-License-Identifier: Apache-2.0
#include "NodeProofResponder.hpp"

#include <FastCache/Core/EnumTable.hpp>
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

    /// A `ProveNode` payload that would not decode into an id and a fixed-width tag.
    ///
    /// Counted apart from the rejection below for `SchedulerCredentialsMalformed`'s reason: a peer
    /// that cannot form the frame is a version or client-library mismatch, and one forming it
    /// correctly with the wrong key is the security question. Summed, the second hides inside the
    /// first whenever an old client is in the fleet.
    constexpr Cc::SurfaceRefusal RefusedMalformed { .code = Wire::ErrorCode::MalformedFrame,
                                                    .counter = IMetricsSink::Counter::NodeProofsMalformed };

    /// A tag that did not authenticate.
    ///
    /// **One answer for a wrong key, a tag over another challenge and a tag over another id**, and
    /// deliberately so: the verification is a single MAC comparison and cannot tell them apart, and
    /// a refusal naming which field was wrong would be an oracle a caller grinds one field at a
    /// time. `Cluster::VerifyFields` is where that reasoning lives for every verifier here.
    constexpr Cc::SurfaceRefusal RefusedRejected { .code = Wire::ErrorCode::NodeProofRejected,
                                                   .counter = IMetricsSink::Counter::NodeProofsRejected };

    /// This node's own key file has stopped being readable.
    ///
    /// `NoCluster`, not `NodeProofRejected`: the caller's tag was never examined, and telling it
    /// the key did not match would send whoever reads that refusal to check the key on the machine
    /// that is fine. Uncounted, and the Warn beside it is the diagnosis -- a counter would say
    /// *proofs are failing* without saying that the failure is on THIS machine, which is the only
    /// part an operator can act on.
    constexpr Cc::UncountedRefusal KeyUnreadable {
        .code = Wire::ErrorCode::NoCluster,
        .rationale = "this node's own --cluster-key-file has stopped being readable, which is a fact about this "
                     "machine rather than about the caller or the fleet; the Warn beside it names the path and the "
                     "error, and a counter could carry neither",
    };

    /// This node's generator could not draw a challenge (#1527).
    ///
    /// `NoCluster` for `KeyUnreadable`'s reason, and the two are the same shape: this node cannot
    /// run the exchange right now, the caller did nothing wrong, and a caller reading `NoCluster`
    /// goes on being judged by its address exactly as before the verb existed. Never answered with
    /// a nonce drawn from anywhere else -- a weak challenge is the replay window #1527 closes.
    constexpr Cc::UncountedRefusal NoNonce {
        .code = Wire::ErrorCode::NoCluster,
        .rationale = "this node's own random source cannot draw a challenge, which is a fact about this machine "
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
        .rationale = "the node-proof verbs are answered by the endpoint, which owns the challenge and the proven id "
                     "because both are connection state; reaching this means a caller went round the endpoint, which "
                     "is a wiring fact rather than an event in the fleet",
    };

    /// Why a size or opcode refusal on this surface moves nothing on its own.
    ///
    /// Stated once because the two arms share the reason, and `UncountedRefusal::rationale` is a
    /// forcing function rather than a field: what it forces is that somebody answered *would a
    /// rise here mean something happened*, and the answer is the same for both.
    constexpr std::string_view ShapeRefusalRationale =
        "a size or opcode refusal says the peer is confused about the framing rather than about this cluster's key; "
        "summed into the node-proof series it would bury the two refusals that mean a key is wrong somewhere";

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
    /// this surface can produce**: every other way a proof fails is decided in `Verify`, which
    /// encodes and counts it there.
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
    // The machine asking is on no list, which is the entire problem being solved; the MAC stands
    // in place of the list, and `Verify` is where it is checked.
    return std::nullopt;
}

std::vector<std::byte> NodeProofResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                        std::uint8_t /*opRaw*/,
                                                        std::string_view detail) const
{
    // Two arms, and the counted one is the CAP: `ProveNode`'s own `OpTable` row bounds it to
    // `MaxNodeProofPayload`, and an id plus a 32-byte tag comes nowhere near, so a header
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

std::expected<Nonce, std::vector<std::byte>> NodeProofResponder::IssueChallenge()
{
    auto nonce = DrawNonce(_random);
    if (nonce.has_value())
        return *nonce;

    // Named on the way out, for `KeyUnreadable`'s reason: no counter can carry WHICH machine is
    // broken. Not throttled, because this path is behind the credential gate wherever one is
    // configured, and one line per challenge asked is the rate the key-file line is said at too.
    _logger.Log(
        LogLevel::Error,
        std::format("node proof: refused a challenge, because this node cannot draw one: {}", nonce.error().ToString()));
    return std::unexpected(
        Cc::RefuseWithoutCounter(NoNonce, "this node cannot draw a challenge from its random source right now"));
}

std::expected<std::string, std::vector<std::byte>> NodeProofResponder::Verify(std::span<std::byte const> challenge,
                                                                              std::span<std::byte const> payload)
{
    auto const presented = Wire::DecodeProveNodePayload(payload);
    if (!presented.has_value())
        return std::unexpected(
            Cc::Refuse(_metrics, RefusedMalformed, "a node proof is an id and a 32-byte tag, in that order"));

    // The key is read HERE, at the moment it is used, and never captured at construction: that is
    // this project's rule for a credential, and it is what makes a rotated key file reach a
    // running node at all. A few dozen bytes, on a path taken once per connection.
    auto key = _key.ClusterKey();
    if (!key.has_value())
    {
        // Named on the way out, because no counter can carry WHICH machine is broken.
        _logger.Log(LogLevel::Warn,
                    std::format("node proof: this node's cluster key could not be read, so no proof can be verified "
                                "until it can: {}",
                                key.error()));
        return std::unexpected(Cc::RefuseWithoutCounter(
            KeyUnreadable, "this node cannot read its own cluster key, so it can verify no proof right now"));
    }

    auto const nodeId = Wire::AsStringView(presented->nodeId);
    auto tag = Sha256::Digest {};
    std::ranges::copy(presented->tag, tag.begin());

    if (!Distributed::VerifyNodeProof(std::span<std::byte const> { *key }, challenge, nodeId, tag))
        return std::unexpected(
            Cc::Refuse(_metrics, RefusedRejected, "the tag does not authenticate under this cluster's key for that id"));

    // **An EMPTY label is legal, and that is not a gap.** The id is a label the MAC covers so it
    // cannot be swapped, not an identity: an id is minted into `--cluster-dir` only by a node
    // that runs consensus, so every keyed worker without one would otherwise be refused the
    // proof it exists for. What carries *this connection proved the key* is
    // `PeerIdentity::provenNodeId` being ENGAGED, which an empty string still is -- so nothing
    // downstream has to read an emptiness two ways.
    //
    // What an empty label costs is stated rather than left to be found: the log line and
    // `--node-status` then name the ADDRESS a key holder proved from, and nothing else.
    _metrics.Increment(IMetricsSink::Counter::NodeProofsAccepted);
    return std::string { nodeId };
}

} // namespace FastCache::Node
