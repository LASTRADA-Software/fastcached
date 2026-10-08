// SPDX-License-Identifier: Apache-2.0
#include "Responders.hpp"
#include "SchedulingRedirect.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LeaderRedirect.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/platform/Clock.hpp>
#include <tests/CounterMovement.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/VerbFamilies.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ErrorOf;
using FastCache::Testing::PayloadOf;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The leader's scheduling endpoint every redirect case names.
constexpr std::string_view LeaderEndpoint = "office-a.example.com:6674";

/// A launcher on this machine: admitted by `LoopbackMembership`.
[[nodiscard]] PeerIdentity ThisMachine()
{
    return PeerIdentity { .host = "127.0.0.1" };
}

/// A machine no route admits: not this one, on no list, holding no key.
[[nodiscard]] PeerIdentity Outsider()
{
    return PeerIdentity { .host = "203.0.113.9" };
}

/// A leader source that answers a fixed endpoint.
class StatedLeader final: public ISchedulingLeaderSource
{
  public:
    /// @param endpoint What every read answers; empty for *no leader known*.
    explicit StatedLeader(std::string endpoint):
        _endpoint { std::move(endpoint) }
    {
    }

    /// @copydoc ISchedulingLeaderSource::LeaderSchedulingEndpoint
    [[nodiscard]] std::string LeaderSchedulingEndpoint() const override
    {
        return _endpoint;
    }

  private:
    std::string _endpoint;
};

/// A header-only request for @p op: this responder reads no payload.
/// @param op The verb.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> FrameOf(Wire::Op op)
{
    return Wire::Detail::EncodeRequest(Wire::CurrentVersion, op, {});
}

/// What the responder answers @p op from @p peer, on BOTH paths.
struct BothAnswers
{
    std::vector<std::byte> refusePeer; ///< What `RefusePeer` answered at the header.
    std::vector<std::byte> answer;     ///< What `Answer` answered on the whole frame.
};

/// Ask @p responder both ways.
/// @param responder Who answers.
/// @param op The verb.
/// @param peer Who asks.
/// @return Both answers; `RefusePeer`'s is required to be a refusal.
[[nodiscard]] BothAnswers AskBoth(SchedulingRedirectResponder& responder, Wire::Op op, PeerIdentity const& peer)
{
    auto refused = responder.RefusePeer(peer, static_cast<std::uint8_t>(op));
    REQUIRE(refused.has_value());
    auto answered = core::async::syncRun(responder.Answer(FrameOf(op), peer)).bytes;
    return BothAnswers { .refusePeer = Unwrap(refused), .answer = std::move(answered) };
}

/// The words an error reply carries.
/// @param reply The reply.
/// @return Its message.
[[nodiscard]] std::string MessageOf(std::vector<std::byte> const& reply)
{
    auto const decoded = Wire::DecodeErrorPayload(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    return std::string { Unwrap(decoded).second };
}

} // namespace

TEST_CASE("A learner redirects a member's LEASE to the leader's scheduling endpoint", "[node][scheduling-redirect]")
{
    Distributed::LoopbackMembership membership;
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder responder { membership, leader };

    auto const [refusePeer, answer] = AskBoth(responder, Wire::Op::Lease, ThisMachine());
    for (auto const& reply: { refusePeer, answer })
    {
        CHECK(ErrorOf(reply) == std::optional { Wire::ErrorCode::NotLeader });
        CHECK(ErrorOf(reply) != std::optional { Wire::UnimplementedVerb });
        CHECK(MessageOf(reply) == LeaderEndpoint);
        // What the launcher asks of the reply: the redirect is one it FOLLOWS.
        auto const message = MessageOf(reply);
        CHECK(LeaderRedirectTarget(Wire::ErrorCode::NotLeader, message) == std::optional { LeaderEndpoint });
    }
    CHECK(refusePeer == answer);
}

TEST_CASE("A learner that knows no leader says NotLeader naming nobody, which redirects nowhere",
          "[node][scheduling-redirect]")
{
    Distributed::LoopbackMembership membership;
    StatedLeader leader { std::string {} };
    SchedulingRedirectResponder responder { membership, leader };

    auto const [refusePeer, answer] = AskBoth(responder, Wire::Op::Lease, ThisMachine());
    for (auto const& reply: { refusePeer, answer })
    {
        CHECK(ErrorOf(reply) == std::optional { Wire::ErrorCode::NotLeader });
        CHECK(ErrorOf(reply) != std::optional { Wire::UnimplementedVerb });
        // No endpoint: the wire's default words for the code, exactly what a follower says during an
        // election -- and nothing a launcher can mistake for somewhere to go.
        CHECK(reply == Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader));
        CHECK_FALSE(LeaderRedirectTarget(Wire::ErrorCode::NotLeader, MessageOf(reply)).has_value());
    }
    CHECK(refusePeer == answer);
}

TEST_CASE("A learner refuses an outsider NotAMember and never tells it where the leader is", "[node][scheduling-redirect]")
{
    Distributed::LoopbackMembership membership;
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder responder { membership, leader };

    // The control: the same verb from this machine IS redirected, so the outsider's answer below is
    // the admission's doing and not a leader the responder failed to read.
    CHECK(ErrorOf(AskBoth(responder, Wire::Op::Lease, ThisMachine()).answer)
          == std::optional { Wire::ErrorCode::NotLeader });

    auto const verbs = Testing::OpsOfFamily(Wire::VerbFamily::Scheduler);
    // Asked of the table rather than assumed: a sweep over nothing would pass.
    REQUIRE(std::ranges::contains(verbs, Wire::Op::Lease, &Wire::OpDescriptor::code));
    for (auto const& row: verbs)
    {
        INFO("verb " << row.name);
        auto const [refusePeer, answer] = AskBoth(responder, row.code, Outsider());
        for (auto const& reply: { refusePeer, answer })
        {
            CHECK(ErrorOf(reply) == std::optional { Wire::ErrorCode::NotAMember });
            auto const endpoint = std::as_bytes(std::span { LeaderEndpoint });
            CHECK(std::ranges::search(reply, endpoint).empty());
        }
        CHECK(refusePeer == answer);
    }
}

TEST_CASE("A learner answers every scheduling verb: RELEASE by name, every other by redirect", "[node][scheduling-redirect]")
{
    Distributed::LoopbackMembership membership;
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder responder { membership, leader };

    auto const verbs = Testing::OpsOfFamily(Wire::VerbFamily::Scheduler);
    // Asked of the table rather than assumed: a sweep over nothing would pass.
    REQUIRE(std::ranges::any_of(verbs, [](Wire::OpDescriptor const& row) { return row.code == Wire::Op::Release; }));
    REQUIRE(std::ranges::any_of(verbs, [](Wire::OpDescriptor const& row) { return row.code == Wire::Op::Lease; }));

    for (auto const& row: verbs)
    {
        INFO("verb " << row.name);
        auto const expected =
            row.code == Wire::Op::Release ? Wire::ErrorCode::DispatchNotPermitted : Wire::ErrorCode::NotLeader;
        auto const [refusePeer, answer] = AskBoth(responder, row.code, ThisMachine());
        for (auto const& reply: { refusePeer, answer })
        {
            CHECK(ErrorOf(reply) == std::optional { expected });
            CHECK(ErrorOf(reply) != std::optional { Wire::UnimplementedVerb });
            if (expected == Wire::ErrorCode::NotLeader)
                CHECK(MessageOf(reply) == LeaderEndpoint);
            else
                CHECK_FALSE(MessageOf(reply).empty());
        }
        CHECK(refusePeer == answer);
    }
}

TEST_CASE("A learner's redirect answers the same bytes a follower scheduler does", "[node][scheduling-redirect]")
{
    // The learner speaks for a scheduler it does not run, so it must be indistinguishable from one
    // that is not leading: the same refusal of an outsider, and the same redirect of a member.
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };
    service.SetRole(Distributed::SchedulerRole::Follower, LeaderEndpoint, 1);

    Distributed::LoopbackMembership membership;
    SchedulerResponder follower { protocol, membership, metrics };
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder learner { membership, leader };

    auto const lease = Wire::EncodeLease({ .fingerprint = "fp", .key = "key", .acceptedCodecs = {} });
    auto const followerAnswer = core::async::syncRun(follower.Answer(lease, ThisMachine())).bytes;
    REQUIRE(ErrorOf(followerAnswer) == std::optional { Wire::ErrorCode::NotLeader });
    CHECK(core::async::syncRun(learner.Answer(lease, ThisMachine())).bytes == followerAnswer);

    auto const leaseOp = static_cast<std::uint8_t>(Wire::Op::Lease);
    auto const followerRefusal = follower.RefusePeer(Outsider(), leaseOp);
    REQUIRE(followerRefusal.has_value());
    CHECK(learner.RefusePeer(Outsider(), leaseOp) == followerRefusal);
}

TEST_CASE("A learner's redirect moves no counter", "[node][scheduling-redirect]")
{
    AtomicMetricsSink metrics;
    Distributed::LoopbackMembership membership;
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder responder { membership, leader };

    auto const before = Testing::CounterReadingsOf(metrics);
    // A reading taken over a sink that moves nothing cannot be told from one that was never read,
    // so prove the instrument first: an increment IS seen, and then undone from the baseline.
    metrics.Increment(IMetricsSink::Counter::DispatchFramesRefusedNotPermitted);
    REQUIRE(Testing::CountersMoved(before, metrics)
            == std::format("{} +1\n", Testing::CounterName(IMetricsSink::Counter::DispatchFramesRefusedNotPermitted)));
    auto const baseline = Testing::CounterReadingsOf(metrics);

    auto const verbs = Testing::OpsOfFamily(Wire::VerbFamily::Scheduler);
    // Asked of the table rather than assumed: a sweep over nothing would pass.
    REQUIRE(std::ranges::contains(verbs, Wire::Op::Lease, &Wire::OpDescriptor::code));
    for (auto const& row: verbs)
        for (auto const& peer: { ThisMachine(), Outsider() })
            static_cast<void>(AskBoth(responder, row.code, peer));
    auto const moved = Testing::CountersMoved(baseline, metrics);
    INFO("counters moved: " << moved);
    CHECK(moved.empty());
}

TEST_CASE("KnownSchedulingLeader answers what was last published, and nothing before that", "[node][scheduling-redirect]")
{
    KnownSchedulingLeader leader;
    CHECK(leader.LeaderSchedulingEndpoint().empty());

    leader.Publish(LeaderEndpoint);
    CHECK(leader.LeaderSchedulingEndpoint() == LeaderEndpoint);

    leader.Publish("office-b.example.com:6674");
    CHECK(leader.LeaderSchedulingEndpoint() == "office-b.example.com:6674");

    leader.Publish({});
    CHECK(leader.LeaderSchedulingEndpoint().empty());
}

TEST_CASE("A leader silent past the bound it is judged by is named to nobody", "[node][scheduling-redirect]")
{
    // R1 of #1639, as amended: the redirect names a leader only while it is inside the bound it is
    // handed. Asked at the bound's edges, on both sides, so an off-by-one in either direction is red.
    // The bound here is arbitrary on purpose: which bound production hands is the observer's case.
    using Duration = core::platform::SteadyTimePoint::duration;
    constexpr auto Bound = Duration { std::chrono::milliseconds { 300 } };
    constexpr auto Tick = Duration { 1 };

    SECTION("within the bound: the endpoint")
    {
        CHECK(SchedulingEndpointToPublish(LeaderEndpoint, Duration::zero(), Bound) == LeaderEndpoint);
        CHECK(SchedulingEndpointToPublish(LeaderEndpoint, Bound, Bound) == LeaderEndpoint);
    }

    SECTION("past the bound: empty")
    {
        CHECK(SchedulingEndpointToPublish(LeaderEndpoint, Bound + Tick, Bound).empty());
        CHECK(SchedulingEndpointToPublish(LeaderEndpoint, Bound * 10, Bound).empty());
    }

    SECTION("no leader: empty, whatever the silence")
    {
        CHECK(SchedulingEndpointToPublish({}, Duration::zero(), Bound).empty());
        CHECK(SchedulingEndpointToPublish({}, std::nullopt, Bound).empty());
    }

    SECTION("no pass yet: not silence, so the endpoint")
    {
        CHECK(SchedulingEndpointToPublish(LeaderEndpoint, std::nullopt, Bound) == LeaderEndpoint);
    }
}

TEST_CASE("The publisher folds who leads and how long it has been silent into one published endpoint",
          "[node][scheduling-redirect]")
{
    // Two observers feed it; each one's half alone moves the holder, and the later half never
    // overrules the earlier one's fact -- a silent leader stays unnamed when the role observer
    // repeats it, and a named one is named again on the first contact. The bound travels WITH each
    // reading, so the publisher judges by the one it was last handed and holds no number of its own.
    using Duration = core::platform::SteadyTimePoint::duration;
    constexpr auto Bound = Duration { std::chrono::milliseconds { 300 } };
    KnownSchedulingLeader holder;
    SchedulingLeaderPublisher publisher { holder };
    REQUIRE(holder.LeaderSchedulingEndpoint().empty());

    publisher.LeaderChanged(LeaderEndpoint);
    CHECK(holder.LeaderSchedulingEndpoint() == LeaderEndpoint);

    // AT the bound: still named.
    publisher.LeaderContact(Distributed::LeaderReading { .leader = "n1", .silentFor = Bound }, Bound);
    CHECK(holder.LeaderSchedulingEndpoint() == LeaderEndpoint);

    // Past it: nobody.
    publisher.LeaderContact(Distributed::LeaderReading { .leader = "n1", .silentFor = Bound + Duration { 1 } }, Bound);
    CHECK(holder.LeaderSchedulingEndpoint().empty());

    // The role observer repeating the endpoint does not revive a leader the pass found silent.
    publisher.LeaderChanged(LeaderEndpoint);
    CHECK(holder.LeaderSchedulingEndpoint().empty());

    // The same silence judged by a LONGER bound it is now handed is inside it: the bound is the
    // reading's, never a value the publisher kept from an earlier one.
    publisher.LeaderContact(Distributed::LeaderReading { .leader = "n1", .silentFor = Bound + Duration { 1 } }, Bound * 2);
    CHECK(holder.LeaderSchedulingEndpoint() == LeaderEndpoint);

    publisher.LeaderContact(Distributed::LeaderReading { .leader = "n1", .silentFor = Duration::zero() }, Bound);
    CHECK(holder.LeaderSchedulingEndpoint() == LeaderEndpoint);

    // An election: nobody leads, and nobody is named however recently the last leader spoke.
    publisher.LeaderChanged({});
    CHECK(holder.LeaderSchedulingEndpoint().empty());
}
