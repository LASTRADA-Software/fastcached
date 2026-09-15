// SPDX-License-Identifier: Apache-2.0
#include "FleetReadGate.hpp"
#include "LiveStatsResponder.hpp"
#include "MembershipGate.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Whether a subject is the fleet map: streamed only by the leader, and only to a holder of the
    /// dashboard credential -- or, with no token file, to this machine. The fleet map is behind that
    /// credential on `/fleet`, and a follower's registry is a fraction presented as the whole.
    struct LiveSubjectGate
    {
        Wire::LiveSubject subject; ///< Which subject.
        bool fleetGates;           ///< Whether the fleet's two extra gates apply.
    };

    /// One row per `LiveSubjectTable` row, in its order.
    constexpr std::array<LiveSubjectGate, Wire::LiveSubjectTable.size()> LiveSubjectGates { {
        { .subject = Wire::LiveSubject::Cache, .fleetGates = false },
        { .subject = Wire::LiveSubject::Node, .fleetGates = false },
        { .subject = Wire::LiveSubject::Fleet, .fleetGates = true },
    } };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, LiveSubjectGates.size()),
                                      [](std::size_t index) {
                                          return LiveSubjectGates.at(index).subject
                                                 == Wire::LiveSubjectTable.at(index).subject;
                                      }),
                  "LiveSubjectGates must follow LiveSubjectTable row for row");

    /// A peer refused at subscribe for not being a member.
    constexpr Cc::SurfaceRefusal RefusedNotAMember { .code = Wire::ErrorCode::NotAMember,
                                                     .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember };

    /// A running stream whose peer stopped being a member. The same code as the refusal above and
    /// a different event, so a different counter: one is a stranger turned away, the other a
    /// removal acting on a live connection.
    constexpr Cc::SurfaceRefusal Revoked { .code = Wire::ErrorCode::NotAMember,
                                           .counter = IMetricsSink::Counter::LiveSubscriptionsRevoked };

    /// A fleet subscription without the dashboard credential.
    constexpr Cc::SurfaceRefusal RefusedUnauthenticated {
        .code = Wire::ErrorCode::Unauthenticated,
        .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedUnauthenticated,
    };

    /// A fleet stream whose node stopped leading.
    constexpr Cc::SurfaceRefusal EndedNotLeader { .code = Wire::ErrorCode::NotLeader,
                                                  .counter = IMetricsSink::Counter::LiveSubscriptionsEndedNotLeader };

    /// A fleet subscription at a follower, before it streams.
    constexpr Cc::UncountedRefusal NotLeaderAtSubscribe {
        .code = Wire::ErrorCode::NotLeader,
        .rationale = "a redirect rather than an event: a dashboard pointed at any member is told where the leader is "
                     "and follows it, once per dashboard opened, exactly as the scheduler's own NotLeader is uncounted",
    };

    /// A fleet subscription at a node that runs no scheduler.
    constexpr Cc::UncountedRefusal FleetNotServedHere {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "a misdirection a healthy fleet produces whenever a dashboard is pointed at a worker, answered "
                     "with where to go instead; DispatchNotPermitted and not UnimplementedVerb because the fleet is "
                     "served elsewhere, not unimplemented",
    };

    /// A SUBSCRIBE reached through `Answer`, which cannot carry a stream.
    constexpr Cc::UncountedRefusal NotThroughAnswer {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "unreachable through the endpoint, which asks StreamFor before it would call Answer; a caller "
                     "that cannot stream has no subscription to count",
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
                // Counted: SUBSCRIBE's `OpTable` row bounds it to `MaxControlPayload`, so a header
                // declaring more came from no client of this tree at any version.
                return { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedPayloadTooLarge, .rationale = {} };
            case Wire::PrePayloadDecision::UnknownOpcode:
                return { .counter = std::nullopt,
                         .rationale = "MergedResponder routes only the Live family here, and an opcode with no OpTable "
                                      "row belongs to no family, so it is answered at the door" };
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
          // Counted, for `NodeStatusResponder`'s reason: a dashboard that cannot subscribe is an
          // operator losing the view at the moment the listener is busiest.
          .policy = { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedEndpointBusy, .rationale = {} } },
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

LiveStatsResponder::LiveStatsResponder(ILiveStatsSources const& sources,
                                       Distributed::IMembershipOracle const& membership,
                                       AdminCredential dashboard,
                                       IReactor& reactor,
                                       IMetricsSink& metrics) noexcept:
    _sources { sources },
    _membership { membership },
    _dashboard { std::move(dashboard) },
    _reactor { reactor },
    _metrics { metrics },
    _stream { sources, metrics }
{
}

Task<FrameReply> LiveStatsResponder::Answer(std::span<std::byte const> frame, std::string peer)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    auto const opRaw = header.has_value() ? header->opRaw : std::uint8_t { 0xFF };
    if (auto refusal = RefusePeer(peer, opRaw); refusal.has_value())
        co_return *std::move(refusal);
    co_return Cc::RefuseWithoutCounter(NotThroughAnswer,
                                       "SUBSCRIBE is answered as a stream, which this caller cannot carry");
}

std::optional<std::vector<std::byte>> LiveStatsResponder::RefusePeer(std::string_view peer, std::uint8_t /*opRaw*/) const
{
    return RefuseWatcher(peer);
}

std::optional<std::vector<std::byte>> LiveStatsResponder::RefuseWatcher(std::string_view peer) const
{
    return RefuseUnlessMember(
        _membership, _metrics, peer, RefusedNotAMember, "this node streams its live stats to fleet members only");
}

std::vector<std::byte> LiveStatsResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                        std::uint8_t /*opRaw*/,
                                                        std::string_view detail) const
{
    return AnswerRefusal(_metrics, Wire::ErrorCodeFor(decision), PrePayloadPolicy(decision), detail);
}

std::vector<std::byte> LiveStatsResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                std::uint8_t /*opRaw*/,
                                                                std::string_view detail) const
{
    auto const& row = EndpointRefusals.at(static_cast<std::size_t>(refusal));
    return AnswerRefusal(_metrics, ErrorCodeFor(refusal), row.policy, detail);
}

IFrameStream* LiveStatsResponder::StreamFor(std::uint8_t opRaw) noexcept
{
    return opRaw == static_cast<std::uint8_t>(Wire::Op::Subscribe) ? this : nullptr;
}

std::optional<std::vector<std::byte>> LiveStatsResponder::Admit(Wire::SubscribeRequest const& request,
                                                                std::string_view peer) const
{
    if (!LiveSubjectGates.at(static_cast<std::size_t>(request.subject)).fleetGates)
        return std::nullopt;

    // The decision `FleetText` answers from too; what is this surface's is which counter each
    // refusal moves.
    auto const verdict = DecideFleetRead(_sources.Leadership(), _dashboard, request.dashboardToken, peer);
    switch (verdict.decision)
    {
        case FleetReadDecision::Admitted:
            return std::nullopt;
        case FleetReadDecision::NoScheduler:
            return Cc::RefuseWithoutCounter(FleetNotServedHere, verdict.detail);
        case FleetReadDecision::NotLeader:
            // The message IS the leader's endpoint, or empty during an election: a client parses it.
            return Cc::RefuseWithoutCounter(NotLeaderAtSubscribe, verdict.detail);
        case FleetReadDecision::Unauthenticated:
            break;
    }
    // `Unauthenticated`, and anything a switch cannot name: a gate fails closed.
    return Cc::Refuse(_metrics, RefusedUnauthenticated, verdict.detail);
}

std::optional<std::vector<std::byte>> LiveStatsResponder::Recheck(Wire::LiveSubject subject, std::string_view peer) const
{
    // Re-asked every tick of the bound oracle, which is the whole defence against removal failing
    // open.
    // Through the same gate as the door, so a stream ENDS for the reason the door would
    // have refused it -- and a forget names itself rather than arriving as the generic
    // revocation, which is what an operator watching a subscriber drop needs to read.
    if (auto refusal = RefuseUnlessMember(_membership, _metrics, peer, Revoked, "this peer is no longer a fleet member");
        refusal.has_value())
        return refusal;

    if (LiveSubjectGates.at(static_cast<std::size_t>(subject)).fleetGates)
        if (auto const leadership = _sources.Leadership(); !leadership.has_value() || !leadership->leads)
            return Cc::Refuse(
                _metrics, EndedNotLeader, leadership.has_value() ? leadership->leaderEndpoint : std::string {});

    return std::nullopt;
}

Task<std::vector<std::byte>> LiveStatsResponder::Serve(std::span<std::byte const> frame, std::string peer, IPushSink* sink)
{
    co_return co_await _stream.Serve(frame, std::move(peer), sink, this, &_reactor);
}

} // namespace FastCache::Node
