// SPDX-License-Identifier: Apache-2.0
#include "Responders.hpp"
#include "SchedulingRedirect.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LeaderRedirect.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
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

/// Where a launcher goes after @p reply: the code and words DECODED from it, through the launcher's
/// own predicate -- never a code the case states beside the reply.
/// @param reply The reply; the answer views into it.
/// @return The endpoint it redirects to, or nullopt when it redirects nowhere.
[[nodiscard]] std::optional<std::string_view> RedirectOf(std::vector<std::byte> const& reply)
{
    auto const decoded = Wire::DecodeErrorPayload(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    auto const& [code, message] = Unwrap(decoded);
    return LeaderRedirectTarget(code, message);
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
        CHECK(RedirectOf(reply) == std::optional { LeaderEndpoint });
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
        CHECK_FALSE(RedirectOf(reply).has_value());
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
            {
                // RELEASE's own sentence, never merely words: a refusal with none carries the code's
                // default ones, so a non-empty check would pass a row that had lost its detail.
                auto const message = MessageOf(reply);
                CHECK(message.contains("granted no lease"));
                CHECK(message.contains("release a lease to the scheduler that granted it"));
            }
        }
        CHECK(refusePeer == answer);
    }
}

TEST_CASE("A learner's redirect answers the same bytes a follower scheduler does", "[node][scheduling-redirect]")
{
    // The learner speaks for a scheduler it does not run, so for a LEASE it must be indistinguishable
    // from one that is not leading: the same refusal of an outsider, and the same redirect of a
    // member. Not for every verb: a follower refuses an unproven member's `ProvenNodeOnly` verb
    // (`REGISTER`) `NodeIdentityRequired` at its door, while the learner checks no identity and
    // answers it `NotLeader` -- the leader it names asks that question on the redirected request.
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

TEST_CASE("A learner's surface limits are the scheduler surface's", "[node][scheduling-redirect]")
{
    // The redirect restates the scheduler surface's caps rather than sharing them, so a client is
    // held to the same ceilings whichever of the two it reaches. Pinned against the responder that
    // states them, so changing one without the other is red here.
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };
    Distributed::LoopbackMembership membership;
    SchedulerResponder const scheduler { protocol, membership, metrics };
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder const learner { membership, leader };

    CHECK(learner.MaxRequestBytes() == scheduler.MaxRequestBytes());
    CHECK(learner.MaxOpenConnections() == scheduler.MaxOpenConnections());
    CHECK(learner.MaxInFlightBytes() == scheduler.MaxInFlightBytes());
}

TEST_CASE("A learner refuses a caller presenting a revoked key and never tells it where the leader is",
          "[node][scheduling-redirect][forget]")
{
    // REMOVAL, the direction an admission path fails OPEN in. A machine is forgotten by its KEY, so
    // the oracle is the production fold over a roster that revoked `gone`, and the key is PRESENTED
    // on the connection -- proved, or shown in a ticket -- never a host list labelled `Forgotten`.
    // From loopback too: a revoked key is `Forgotten` from every address, and loopback is the route
    // that would otherwise admit a caller showing nothing.
    Testing::RosterFold const fold { { "pc-07" }, { "gone" } };
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder responder { fold.admitted, leader };

    constexpr std::string_view RemoteHost = "198.51.100.12";
    // The control: the same address proving a key the roster holds IS redirected, so a refusal below
    // is the revocation's doing and not the address's.
    CHECK(ErrorOf(AskBoth(responder,
                          Wire::Op::Lease,
                          PeerIdentity { .host = std::string { RemoteHost }, .proven = Testing::IdentityOf("pc-07") })
                      .answer)
          == std::optional { Wire::ErrorCode::NotLeader });

    auto const revoked = std::to_array<PeerIdentity>({
        PeerIdentity { .host = std::string { RemoteHost }, .proven = Testing::IdentityOf("gone") },
        PeerIdentity { .host = std::string { RemoteHost }, .authenticatedMachine = Testing::IdentityOf("gone") },
        PeerIdentity { .host = "127.0.0.1", .proven = Testing::IdentityOf("gone") },
    });
    for (auto const& peer: revoked)
    {
        // Through the production fold, so the case says which verdict it is about.
        REQUIRE(Distributed::CallerContextOf(fold.admitted, peer).membership == Distributed::Membership::Forgotten);
        INFO("caller " << peer.host << (peer.proven.has_value() ? " proving" : " showing a ticket for") << " gone");
        auto const [refusePeer, answer] = AskBoth(responder, Wire::Op::Lease, peer);
        for (auto const& reply: { refusePeer, answer })
        {
            CHECK(ErrorOf(reply) == std::optional { Wire::ErrorCode::NotAMember });
            auto const endpoint = std::as_bytes(std::span { LeaderEndpoint });
            CHECK(std::ranges::search(reply, endpoint).empty());
        }
        CHECK(refusePeer == answer);
    }
}

TEST_CASE("A learner redirects a machine on another address that presents a key the fleet holds",
          "[node][scheduling-redirect]")
{
    // A launcher on ANOTHER machine pointed at this node: admitted by its key from wherever it
    // dials, as every surface admits it, and then sent to the leader like any member. Not loopback
    // and on no list, so only the key route can admit it -- an admission that asked the address
    // would refuse it.
    Testing::RosterFold const fold { { "pc-07" } };
    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder responder { fold.admitted, leader };

    constexpr std::string_view RemoteHost = "198.51.100.12";
    REQUIRE_FALSE(IsLoopbackHost(RemoteHost));

    // The control: the same address showing nothing is refused and told nothing, so the redirect
    // below is the key's doing.
    auto const [strangerRefused, strangerAnswered] =
        AskBoth(responder, Wire::Op::Lease, PeerIdentity { .host = std::string { RemoteHost } });
    CHECK(ErrorOf(strangerAnswered) == std::optional { Wire::ErrorCode::NotAMember });
    CHECK(std::ranges::search(strangerAnswered, std::as_bytes(std::span { LeaderEndpoint })).empty());
    CHECK(strangerRefused == strangerAnswered);

    auto const holders = std::to_array<PeerIdentity>({
        PeerIdentity { .host = std::string { RemoteHost }, .proven = Testing::IdentityOf("pc-07") },
        PeerIdentity { .host = std::string { RemoteHost }, .authenticatedMachine = Testing::IdentityOf("pc-07") },
    });
    for (auto const& peer: holders)
    {
        REQUIRE(Distributed::CallerContextOf(fold.admitted, peer).membership == Distributed::Membership::Member);
        INFO("caller " << (peer.proven.has_value() ? "proving" : "showing a ticket for") << " pc-07");
        auto const [refusePeer, answer] = AskBoth(responder, Wire::Op::Lease, peer);
        for (auto const& reply: { refusePeer, answer })
        {
            CHECK(ErrorOf(reply) == std::optional { Wire::ErrorCode::NotLeader });
            CHECK(MessageOf(reply) == LeaderEndpoint);
        }
        CHECK(refusePeer == answer);
    }
}

TEST_CASE("A learner sends --cluster-status to the leader, which admits the machine by the ticket it presents there",
          "[node][scheduling-redirect][ticket]")
{
    // The operator's one-shot verb from a learner PC with no `--scheduler`: it asks this machine's own
    // node, which answers `NotLeader` naming the leader; the client follows it and presents a ticket
    // minted for THAT endpoint (`ClusterAdminCli_test`'s redirect case pins the client half). What
    // is left is the leader's half: a remote caller showing a verified ticket for a key the roster
    // holds is admitted to `ClusterStatus`, which asks no proven identity of its caller.
    Testing::RosterFold const fold { { "pc-07" } };

    StatedLeader leader { std::string { LeaderEndpoint } };
    SchedulingRedirectResponder learner { fold.admitted, leader };
    auto const [refusePeer, answer] = AskBoth(learner, Wire::Op::ClusterStatus, ThisMachine());
    for (auto const& reply: { refusePeer, answer })
    {
        CHECK(RedirectOf(reply) == std::optional { LeaderEndpoint });
    }

    AtomicMetricsSink metrics;
    CapturingLogger logger;
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };
    service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    SchedulerResponder const leading { protocol, fold.admitted, metrics };

    auto const clusterStatus = static_cast<std::uint8_t>(Wire::Op::ClusterStatus);
    constexpr std::string_view LearnerHost = "198.51.100.12";
    // The control: the learner PC's address showing nothing is refused, so the admission below is
    // the ticket's.
    auto const stranger = leading.RefusePeer(PeerIdentity { .host = std::string { LearnerHost } }, clusterStatus);
    REQUIRE(stranger.has_value());
    CHECK(ErrorOf(Unwrap(stranger)) == std::optional { Wire::ErrorCode::NotAMember });

    auto const ticketed = leading.RefusePeer(
        PeerIdentity { .host = std::string { LearnerHost }, .authenticatedMachine = Testing::IdentityOf("pc-07") },
        clusterStatus);
    INFO("refused: " << (ticketed.has_value() ? MessageOf(Unwrap(ticketed)) : std::string { "(admitted)" }));
    CHECK_FALSE(ticketed.has_value());
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
    // #1639: the redirect names a leader only while its silence is inside the bound it is handed,
    // the tier's Raft election timeout rather than the hour-long `consensus-leader-silent` bound.
    // Asked at the bound's edges, on both sides, so an off-by-one in either direction is red.
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
