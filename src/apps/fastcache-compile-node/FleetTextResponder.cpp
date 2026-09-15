// SPDX-License-Identifier: Apache-2.0
#include "FleetReadGate.hpp"
#include "FleetTextResponder.hpp"
#include "MembershipGate.hpp"

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

    /// A reader refused for not being a member.
    constexpr Cc::SurfaceRefusal RefusedNotAMember { .code = Wire::ErrorCode::NotAMember,
                                                     .counter = IMetricsSink::Counter::FleetTextRequestsRefusedNotAMember };

    /// A reader without the dashboard credential.
    constexpr Cc::SurfaceRefusal RefusedUnauthenticated {
        .code = Wire::ErrorCode::Unauthenticated,
        .counter = IMetricsSink::Counter::FleetTextRequestsRefusedUnauthenticated,
    };

    /// A request whose fields did not decode.
    constexpr Cc::SurfaceRefusal RefusedMalformed { .code = Wire::ErrorCode::MalformedFrame,
                                                    .counter = IMetricsSink::Counter::FleetTextRequestsRefusedMalformed };

    /// A read at a follower.
    constexpr Cc::UncountedRefusal NotLeaderHere {
        .code = Wire::ErrorCode::NotLeader,
        .rationale = "a redirect rather than an event: a reader pointed at any member is told where the leader is and "
                     "follows it, exactly as the fleet subscription's own NotLeader is uncounted",
    };

    /// A read at a node that has no fleet.
    constexpr Cc::UncountedRefusal FleetNotServedHere {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "a misdirection a healthy fleet produces whenever a reader is pointed at a worker, answered with "
                     "where to go instead; DispatchNotPermitted and not UnimplementedVerb because the fleet is served "
                     "elsewhere, not unimplemented",
    };

    /// A section or a range this build does not serve.
    constexpr Cc::UncountedRefusal UnknownSelector {
        .code = Wire::ErrorCode::UnknownFleetSelector,
        .rationale = "a typo or a client of another build, seen by whoever typed it in the keys the refusal lists; a "
                     "rise is nothing an operator acts on",
    };

    /// A verb routed here that this component does not own.
    constexpr Cc::UncountedRefusal NotThisComponentsVerb {
        .code = Wire::UnimplementedVerb,
        .rationale = "a healthy answer, not an event: MergedResponder routes only the Fleet family here, so only a "
                     "direct caller reaches this, and a client steps over it",
    };

    /// What this surface does about one refusal it may be asked to answer.
    ///
    /// Exactly one of the two is set; see `NodeStatusResponder.cpp`'s identical record for why a
    /// counter and a rationale are the two claims.
    struct RefusalPolicy
    {
        std::optional<IMetricsSink::Counter> counter; ///< What rises, or nothing.
        std::string_view rationale;                   ///< Why nothing rises. Empty exactly when `counter` is set.
    };

    /// Answer a refusal the way its row decided.
    [[nodiscard]] std::vector<std::byte> AnswerRefusal(IMetricsSink& metrics,
                                                       Wire::ErrorCode code,
                                                       RefusalPolicy const& policy,
                                                       std::string_view detail)
    {
        if (policy.counter.has_value())
            return Cc::Refuse(metrics, { .code = code, .counter = *policy.counter }, detail);
        return Cc::RefuseWithoutCounter({ .code = code, .rationale = policy.rationale }, detail);
    }

    /// What this surface does about each pre-payload decision.
    [[nodiscard]] constexpr RefusalPolicy PrePayloadPolicy(Wire::PrePayloadDecision decision) noexcept
    {
        switch (decision)
        {
            case Wire::PrePayloadDecision::PayloadTooLarge:
                // Counted: FLEET-TEXT's `OpTable` row bounds it to `MaxControlPayload`, and two keys
                // and a token come nowhere near, so a header declaring more came from no client of
                // this tree at any version.
                return { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedPayloadTooLarge, .rationale = {} };
            case Wire::PrePayloadDecision::UnknownOpcode:
                return { .counter = std::nullopt,
                         .rationale = "MergedResponder routes only the Fleet family here, and an opcode with no "
                                      "OpTable row belongs to no family, so it is answered at the door" };
            case Wire::PrePayloadDecision::Unauthenticated:
                return { .counter = std::nullopt,
                         .rationale =
                             "AuthRequired() is false here by decision -- the listener's credential is the "
                             "scheduler's -- and DecidePrePayload yields this only for a surface that requires one" };
            case Wire::PrePayloadDecision::Serve:
                break;
        }
        return { .counter = std::nullopt, .rationale = "Serve is not a refusal and the endpoint never asks about it" };
    }

    static_assert(std::ranges::all_of(std::array { Wire::PrePayloadDecision::Serve,
                                                   Wire::PrePayloadDecision::UnknownOpcode,
                                                   Wire::PrePayloadDecision::PayloadTooLarge,
                                                   Wire::PrePayloadDecision::Unauthenticated },
                                      [](Wire::PrePayloadDecision decision) {
                                          auto const policy = PrePayloadPolicy(decision);
                                          return Cc::StatesOneRefusalClaim(policy.counter.has_value(), policy.rationale);
                                      }),
                  "every pre-payload arm must state either a counter or a rationale, and not both");

    /// One row of `EndpointRefusals`.
    struct EndpointRefusalRow
    {
        EndpointRefusal refusal; ///< Which endpoint decision this describes.
        RefusalPolicy policy;    ///< What this surface does about it.
    };

    /// Why neither credential arm counts here.
    constexpr std::string_view CredentialIsTheSchedulersRationale =
        "AUTH is the Session family, which MergedResponder routes to the scheduler; no credential outcome is ever "
        "decided against this surface";

    /// What this surface does about each endpoint-decided refusal.
    constexpr EnumTable<EndpointRefusal, EndpointRefusalRow> EndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          // Counted, for `NodeStatusResponder`'s reason: an operator asking for the fleet is
          // turned away at the moment the leader is busiest.
          .policy = { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedEndpointBusy, .rationale = {} } },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
        { .refusal = EndpointRefusal::CredentialRejected,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .policy = { .counter = std::nullopt, .rationale = AnswerDeadlineIsTheEndpointsRationale } },
    } };

    static_assert(RowsInEnumeratorOrder(EndpointRefusals, &EndpointRefusalRow::refusal),
                  "EndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    static_assert(Cc::RowsStateOneRefusalClaim(EndpointRefusals,
                                               [](EndpointRefusalRow const& row) {
                                                   return Cc::RefusalClaim { .counted = row.policy.counter.has_value(),
                                                                             .rationale = row.policy.rationale };
                                               }),
                  "every endpoint refusal row must state either a counter or a rationale, and not both");

} // namespace

FleetTextResponder::FleetTextResponder(ILiveStatsSources const& sources,
                                       Distributed::IMembershipOracle const& membership,
                                       AdminCredential dashboard,
                                       IMetricsSink& metrics) noexcept:
    _sources { sources },
    _membership { membership },
    _dashboard { std::move(dashboard) },
    _metrics { metrics }
{
}

Task<FrameReply> FleetTextResponder::Answer(std::span<std::byte const> frame, std::string peer)
{
    // The verb is read back out of the frame rather than taken on the endpoint's word, for
    // `NodeStatusResponder::Answer`'s reason: `Answer` is reachable directly.
    auto const header = Wire::DecodeRequestHeader(frame);
    auto const opRaw = header.has_value() ? header->opRaw : std::uint8_t { 0xFF };

    // Before the payload matters: a caller this node will not answer must not make it render.
    if (auto refusal = RefusePeer(peer, opRaw); refusal.has_value())
        co_return *std::move(refusal);

    if (!header.has_value())
        // Not this protocol at all; empty is CLOSE.
        co_return std::vector<std::byte> {};

    if (header->opRaw != static_cast<std::uint8_t>(Wire::Op::FleetText))
        co_return Cc::RefuseWithoutCounter(NotThisComponentsVerb, "this node serves no component for that verb");

    auto const request = Wire::DecodeFleetTextRequest(frame.subspan(Wire::RequestHeaderSize));
    if (!request.has_value())
        co_return Cc::Refuse(_metrics, RefusedMalformed, "a fleet-text request is a section, a range and a token");

    co_return Read(*request, peer);
}

std::vector<std::byte> FleetTextResponder::Read(Wire::FleetTextRequest const& request, std::string_view peer) const
{
    // The decision the fleet subscription answers from too; what is this surface's is which
    // counter each refusal moves.
    auto const verdict = DecideFleetRead(_sources.Leadership(), _dashboard, request.dashboardToken, peer);
    switch (verdict.decision)
    {
        case FleetReadDecision::Admitted:
            return Render(request);
        case FleetReadDecision::NoScheduler:
            return Cc::RefuseWithoutCounter(FleetNotServedHere, verdict.detail);
        case FleetReadDecision::NotLeader:
            // The message IS the leader's endpoint, or empty during an election: a client parses it.
            return Cc::RefuseWithoutCounter(NotLeaderHere, verdict.detail);
        case FleetReadDecision::Unauthenticated:
            break;
    }
    // `Unauthenticated`, and anything a switch cannot name: a gate fails closed.
    return Cc::Refuse(_metrics, RefusedUnauthenticated, verdict.detail);
}

std::vector<std::byte> FleetTextResponder::Render(Wire::FleetTextRequest const& request) const
{
    auto document = _sources.FleetText(request.section, request.range);
    if (!document.has_value())
    {
        if (document.error().refusal == FleetTextRefusal::UnknownSelector)
            return Cc::RefuseWithoutCounter(UnknownSelector, document.error().detail);
        // No fleet after all: the scheduler's sources were detached between the decision and the
        // read, which is this process stopping.
        return Cc::RefuseWithoutCounter(FleetNotServedHere, document.error().detail);
    }

    // Judged again by the snapshot the body was rendered from: leadership read by the gate a moment
    // earlier is not evidence about a document rendered after an election.
    if (!document->leads)
        return Cc::RefuseWithoutCounter(NotLeaderHere, document->leaderEndpoint);

    return Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes(document->body));
}

std::optional<std::vector<std::byte>> FleetTextResponder::RefusePeer(std::string_view peer, std::uint8_t /*opRaw*/) const
{
    return RefuseUnlessMember(
        _membership, _metrics, peer, RefusedNotAMember, "this node serves the fleet to fleet members only");
}

std::vector<std::byte> FleetTextResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                        std::uint8_t /*opRaw*/,
                                                        std::string_view detail) const
{
    return AnswerRefusal(_metrics, Wire::ErrorCodeFor(decision), PrePayloadPolicy(decision), detail);
}

std::vector<std::byte> FleetTextResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                std::uint8_t /*opRaw*/,
                                                                std::string_view detail) const
{
    auto const& row = EndpointRefusals.at(static_cast<std::size_t>(refusal));
    return AnswerRefusal(_metrics, ErrorCodeFor(refusal), row.policy, detail);
}

} // namespace FastCache::Node
