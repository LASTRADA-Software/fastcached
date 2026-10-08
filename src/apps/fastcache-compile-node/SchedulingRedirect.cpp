// SPDX-License-Identifier: Apache-2.0
#include "Responders.hpp"
#include "SchedulingRedirect.hpp"

#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A caller the node's oracle does not admit.
    ///
    /// The scheduler's own answer, byte for byte: `SchedulerService::RefuseUnlessMember` refuses
    /// `NotAMember` with no words, and the same caller must not learn from the reply which of the
    /// two kinds of node it reached.
    constexpr Cc::UncountedRefusal NotAMember {
        .code = Wire::ErrorCode::NotAMember,
        .rationale = "the scheduler answers this refusal uncounted (SchedulerService::UncountedRefusals): a policy "
                     "answer, which beside the capacity refusals would put noise into the numbers a fleet is sized "
                     "from; this node answers for the scheduler and keeps its accounting",
    };

    /// A scheduling verb from a member: go to the leader.
    constexpr Cc::UncountedRefusal NotLeader {
        .code = Wire::ErrorCode::NotLeader,
        .rationale = "ordinary traffic, not an event: every launcher pointed at this node is redirected once per "
                     "lease, which is what SchedulerService::UncountedRefusals decided for a follower's NotLeader",
    };

    /// A RELEASE: the lease was granted by whoever answered the LEASE, and that was never this node.
    constexpr Cc::UncountedRefusal ReleaseNotGrantedHere {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "only a broken client reaches it -- a launcher releases a lease to the scheduler that granted "
                     "it, and this node grants none -- and that client is told so in the refusal; a series of it "
                     "would describe one client's bug, not this fleet",
    };

    /// What `ReleaseNotGrantedHere` says to the client.
    constexpr std::string_view ReleaseNotGrantedHereWhy =
        "this node runs no scheduler and granted no lease; release a lease to the scheduler that granted it";

    /// A verb outside the scheduler family, reaching `Answer` directly.
    constexpr Cc::UncountedRefusal NotThisComponentsVerb {
        .code = Wire::UnimplementedVerb,
        .rationale = "a healthy answer, not an event: MergedResponder routes only the Scheduler family here, so only "
                     "a direct caller reaches this, and a client steps over it",
    };

    /// Why a pre-payload refusal moves nothing here.
    constexpr std::string_view ShapeRefusalRationale =
        "a size or opcode refusal says the peer is confused about the framing, not about the fleet";

    /// A scheduling verb this node answers by NAME rather than by redirect.
    struct NamedRefusal
    {
        Wire::Op op;                 ///< The verb.
        Cc::UncountedRefusal answer; ///< What it is told.
        std::string_view detail;     ///< The words that ride with it.
    };

    /// The scheduling verbs a redirect would send to the wrong place. Every other one is sent to
    /// the leader.
    ///
    /// RELEASE alone: a follower's own scheduler serves it unleadered (`GateScope::Settlement`),
    /// since it settles a lease in the table of whoever minted it -- and a node with no scheduler
    /// minted nothing, so the leader is not the answer either.
    constexpr std::array NamedRefusals {
        NamedRefusal { .op = Wire::Op::Release, .answer = ReleaseNotGrantedHere, .detail = ReleaseNotGrantedHereWhy },
    };

    static_assert(std::ranges::all_of(NamedRefusals,
                                      [](NamedRefusal const& row) {
                                          auto const* const descriptor = Wire::FindOp(static_cast<std::uint8_t>(row.op));
                                          return descriptor != nullptr && descriptor->family == Wire::VerbFamily::Scheduler;
                                      }),
                  "a named refusal for a verb outside the scheduler family is dead: Decide never reaches it");

    // `Detail::SchedulerEndpointRefusals` is borrowed with its rationales and WITHOUT a metrics
    // sink, which is sound only while every row is uncounted. A row that gains a counter must fail
    // here, where somebody decides whether this surface counts it too.
    static_assert(std::ranges::none_of(Detail::SchedulerEndpointRefusals,
                                       [](Detail::SchedulerEndpointRefusal const& row) { return row.answer.has_value(); }),
                  "a scheduler endpoint refusal gained a counter; decide whether the redirect surface counts it");
} // namespace

void KnownSchedulingLeader::Publish(std::string_view endpoint)
{
    std::scoped_lock const guard { _lock };
    _endpoint = endpoint;
}

std::string KnownSchedulingLeader::LeaderSchedulingEndpoint() const
{
    std::scoped_lock const guard { _lock };
    return _endpoint;
}

std::string_view SchedulingEndpointToPublish(std::string_view leaderEndpoint,
                                             std::optional<core::platform::SteadyTimePoint::duration> silentFor,
                                             core::platform::SteadyTimePoint::duration bound) noexcept
{
    // Strictly past the bound, as the roster's `Isolated` reads it, so the condition and the redirect
    // change their answer at the same reading.
    if (silentFor.has_value() && *silentFor > bound)
        return {};
    return leaderEndpoint;
}

SchedulingLeaderPublisher::SchedulingLeaderPublisher(KnownSchedulingLeader& holder) noexcept:
    _holder { holder }
{
}

void SchedulingLeaderPublisher::LeaderChanged(std::string_view leaderEndpoint)
{
    std::scoped_lock const guard { _lock };
    _leaderEndpoint = leaderEndpoint;
    RepublishLocked();
}

void SchedulingLeaderPublisher::LeaderContact(Distributed::LeaderReading const& reading)
{
    std::scoped_lock const guard { _lock };
    _silentFor = reading.silentFor;
    RepublishLocked();
}

void SchedulingLeaderPublisher::RepublishLocked()
{
    // Published under this lock, so two observers racing cannot leave the holder with the answer of
    // the one that decided first.
    _holder.Publish(SchedulingEndpointToPublish(_leaderEndpoint, _silentFor, Distributed::LeaderSilenceBound));
}

SchedulingRedirectResponder::SchedulingRedirectResponder(Distributed::IMembershipOracle const& membership,
                                                         ISchedulingLeaderSource const& leader) noexcept:
    _membership { membership },
    _leader { leader }
{
}

std::optional<std::vector<std::byte>> SchedulingRedirectResponder::Decide(PeerIdentity const& peer, std::uint8_t opRaw) const
{
    // Admission BEFORE the redirect: a caller nothing admits must not learn where the leader is.
    // Folded exactly as `SchedulerResponder::Context` folds it, and refused on the same rule
    // `SchedulerService::RefuseUnlessMember` applies -- anything but `Member` is refused.
    if (Distributed::CallerContextOf(_membership, peer).membership != Distributed::Membership::Member)
        return Cc::RefuseWithoutCounter(NotAMember);

    auto const* const descriptor = Wire::FindOp(opRaw);
    if (descriptor == nullptr || descriptor->family != Wire::VerbFamily::Scheduler)
        return std::nullopt;

    if (auto const* const row = core::findOrNull(NamedRefusals, descriptor->code, &NamedRefusal::op); row != nullptr)
        return Cc::RefuseWithoutCounter(row->answer, row->detail);

    // Possibly empty: no leader is known. Sent anyway rather than refused another way, because a
    // `NotLeader` naming nobody is what a follower says during an election, and every client
    // already reads it as *no leader*: the reply then carries the code's default words, which
    // `LeaderRedirectTarget` does not take for an endpoint.
    return Cc::RefuseWithoutCounter(NotLeader, _leader.LeaderSchedulingEndpoint());
}

core::async::Task<FrameReply> SchedulingRedirectResponder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Not this protocol at all; empty is CLOSE, as every surface answers such a header.
        co_return std::vector<std::byte> {};

    if (auto refusal = Decide(peer, header->opRaw); refusal.has_value())
        co_return *std::move(refusal);
    co_return Cc::RefuseWithoutCounter(NotThisComponentsVerb, "this node serves no component for that verb");
}

std::optional<std::vector<std::byte>> SchedulingRedirectResponder::RefusePeer(PeerIdentity const& peer,
                                                                              std::uint8_t opRaw) const
{
    return Decide(peer, opRaw);
}

std::vector<std::byte> SchedulingRedirectResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                                 std::uint8_t /*opRaw*/,
                                                                 std::string_view detail) const
{
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCodeFor(decision),
                                      .rationale = decision == Wire::PrePayloadDecision::Unauthenticated
                                                       ? NodeChecksNoPasswordRationale
                                                       : ShapeRefusalRationale },
                                    detail);
}

std::vector<std::byte> SchedulingRedirectResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                         std::uint8_t /*opRaw*/,
                                                                         std::string_view detail) const
{
    auto const& row = Detail::SchedulerEndpointRefusals[static_cast<std::size_t>(refusal)];
    return Cc::RefuseWithoutCounter({ .code = ErrorCodeFor(refusal), .rationale = row.rationale }, detail);
}

} // namespace FastCache::Node
