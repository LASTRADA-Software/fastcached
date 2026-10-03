// SPDX-License-Identifier: Apache-2.0
#include "FleetSummaryResponder.hpp"

#include <FastCache/Cluster/FleetSummarySignature.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A question whose nonce is not exactly one `NodeChallengeBytes` wide.
    constexpr Cc::UncountedRefusal RefusedMalformed {
        .code = Wire::ErrorCode::MalformedFrame,
        .rationale = "the verb is answered to strangers by design and costs this node one signature at most; a "
                     "malformed nonce is a client of another build or somebody typing bytes, which the asker sees in "
                     "the refusal, and a series of it would say nothing an operator of THIS fleet acts on",
    };

    /// A node that belongs to no cluster, or will not offer its own.
    constexpr Cc::UncountedRefusal NoFleetToOffer {
        .code = Wire::ErrorCode::NoCluster,
        .rationale = "a fact about this node's configuration rather than an event: a node running no consensus, or "
                     "one whose consensus address reaches only itself, answers every asker this way for as long as it "
                     "runs, and the asker is told which in the refusal",
    };

    /// A verb routed here that this component does not own.
    constexpr Cc::UncountedRefusal NotThisComponentsVerb {
        .code = Wire::UnimplementedVerb,
        .rationale = "a healthy answer, not an event: MergedResponder routes only the Formation family here, so only a "
                     "direct caller reaches this, and a client steps over it",
    };

    /// Why a pre-payload refusal moves nothing here.
    constexpr std::string_view ShapeRefusalRationale =
        "a size or opcode refusal says the asker is confused about the framing; the verb's own OpTable ceiling is one "
        "nonce, so a larger header came from no client of this tree, and the asker reads the refusal itself";

    /// One row of `EndpointRefusals`.
    struct EndpointRefusalRow
    {
        EndpointRefusal refusal;    ///< Which endpoint decision this describes.
        std::string_view rationale; ///< Why nothing is counted.
    };

    /// What this surface does about each endpoint-decided refusal: none is counted.
    constexpr EnumTable<EndpointRefusal, EndpointRefusalRow> EndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          .rationale = "the byte budget says this surface is momentarily full; a joiner's probe sees it and asks the "
                       "next seed, and nothing about the fleet has happened" },
        { .refusal = EndpointRefusal::CredentialMalformed, .rationale = CredentialIsTheSessionsRationale },
        { .refusal = EndpointRefusal::CredentialRejected, .rationale = CredentialIsTheSessionsRationale },
        { .refusal = EndpointRefusal::AnswerDeadline, .rationale = AnswerDeadlineIsTheEndpointsRationale },
        { .refusal = EndpointRefusal::NodeProofUnchallenged, .rationale = NodeProofIsTheProversRationale },
    } };

    static_assert(RowsInEnumeratorOrder(EndpointRefusals, &EndpointRefusalRow::refusal),
                  "EndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    static_assert(std::ranges::none_of(EndpointRefusals,
                                       [](EndpointRefusalRow const& row) { return row.rationale.empty(); }),
                  "every uncounted endpoint refusal must say why it is uncounted");

} // namespace

FleetSummaryResponder::FleetSummaryResponder(Cluster::IFleetSummarySource const& self,
                                             Ed25519KeyPair const& identity) noexcept:
    _self { self },
    _identity { identity }
{
}

core::async::Task<FrameReply> FleetSummaryResponder::Answer(std::span<std::byte const> frame, PeerIdentity /*peer*/)
{
    // The peer is not consulted, and the parameter is unnamed to say so: `RefusePeer` admits
    // everybody here by decision.
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Not this protocol at all; empty is CLOSE, as every surface answers such a header.
        co_return std::vector<std::byte> {};

    if (header->opRaw != static_cast<std::uint8_t>(Wire::Op::FleetSummary))
        co_return Cc::RefuseWithoutCounter(NotThisComponentsVerb, "this node serves no component for that verb");

    // One question per connection, then close, as the header promises: a signature is what a
    // stranger's question costs this node, so each one also costs the stranger a handshake and a slot
    // of the port's pool, rather than a pipelined frame on a connection it already holds.
    auto reply = FrameReply { AnswerOnce(frame) };
    reply.endsConnection = true;
    co_return reply;
}

std::vector<std::byte> FleetSummaryResponder::AnswerOnce(std::span<std::byte const> frame) const
{
    auto const nonce = Wire::DecodeFleetSummaryRequestPayload(frame.subspan(Wire::RequestHeaderSize));
    if (!nonce.has_value())
        return Cc::RefuseWithoutCounter(RefusedMalformed, "a fleet-summary question is one 32-byte nonce");
    return Sign(*nonce);
}

std::vector<std::byte> FleetSummaryResponder::Sign(std::span<std::byte const> nonce) const
{
    // Cut to what a reply carries, the list up to `MaxFleetSummaryReplyMembers`, with `memberTotal`
    // saying when it was cut: a seed's answer, which the node that dialled it reads its split evidence
    // over as it reads a beacon's.
    auto summary = Wire::WithMembersAtMost(_self.Current(), Wire::MaxFleetSummaryReplyMembers);
    if (summary.clusterId.empty())
        return Cc::RefuseWithoutCounter(NoFleetToOffer, "this node runs no consensus, so it belongs to no fleet");

    // Never signed: an endpoint every peer resolves to itself would send the asker to dial ITSELF.
    // Discovery holds the same rule at its own door; this is the second door a summary leaves by.
    if (Cluster::AnnouncesOnlyThisMachine(summary))
        return Cc::RefuseWithoutCounter(NoFleetToOffer,
                                        "this node's consensus address reaches only this machine, so it offers its "
                                        "fleet to no other");

    auto reply = Wire::FleetSummaryReply { .summary = std::move(summary) };
    reply.publicKey = _identity.PublicKey();
    reply.signature = SignLabelled(_identity, Cluster::FleetSummaryMessage(nonce, reply.summary, reply.publicKey));
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeFleetSummaryReply(reply));
}

std::optional<std::vector<std::byte>> FleetSummaryResponder::RefusePeer(PeerIdentity const& /*peer*/,
                                                                        std::uint8_t /*opRaw*/) const
{
    // Nobody, and both parameters are unnamed to say that is a decision: the asker is on no list,
    // which is the question it is asking about. The signature stands in place of a list.
    return std::nullopt;
}

std::vector<std::byte> FleetSummaryResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                           std::uint8_t /*opRaw*/,
                                                           std::string_view detail) const
{
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCodeFor(decision), .rationale = ShapeRefusalRationale }, detail);
}

std::vector<std::byte> FleetSummaryResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                   std::uint8_t /*opRaw*/,
                                                                   std::string_view detail) const
{
    auto const& row = EndpointRefusals[static_cast<std::size_t>(refusal)];
    return Cc::RefuseWithoutCounter({ .code = ErrorCodeFor(refusal), .rationale = row.rationale }, detail);
}

} // namespace FastCache::Node
