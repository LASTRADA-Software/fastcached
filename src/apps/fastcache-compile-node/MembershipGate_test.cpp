// SPDX-License-Identifier: Apache-2.0
#include "MembershipGate.hpp"

#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include <tests/MembershipFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::FixedMembership;
using FastCache::Testing::Unwrap;

namespace
{

namespace Wire = CompileCacheWire;

/// The row one surface answers a host nobody listed with. Any of the six would do:
/// what is under test is which of the two rows the gate picks, not this one's contents.
constexpr Cc::SurfaceRefusal Stranger {
    .code = Wire::ErrorCode::NotAMember,
    .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember,
};

// Every case here names `Loopback` and holds it fixed. The gate's subject is WHETHER a
// caller is refused, not by whom -- the real routes are asserted where they live -- so the
// route is a constant of the arrangement rather than a variable of it. It is still spelled at
// each site: a shared fake defaulting it would make the statement disappear, and `DecidedBy`
// keeps `Outsider` unattributed even here, so no gate case can be the thing that establishes
// the opposite convention (#1497).
constexpr std::string_view StrangerWhy = "this node reports its identity and counters to fleet members only";

/// The code and the sentence a refusal carries.
///
/// The payload and not the header: `ReplyHeader` carries the status alone, so a case
/// reading only that cannot tell two refusals apart at all.
/// @param refusal An encoded error reply.
/// @return Its code and message.
[[nodiscard]] std::pair<Wire::ErrorCode, std::string_view> RefusalOf(std::span<std::byte const> refusal)
{
    auto const header = Wire::DecodeReplyHeader(refusal);
    REQUIRE(header.has_value());
    REQUIRE(Unwrap(header).status == Wire::Status::Error);

    auto const payload = Wire::DecodeErrorPayload(refusal.subspan(Wire::ReplyHeaderSize));
    REQUIRE(payload.has_value());
    return Unwrap(payload);
}

} // namespace

TEST_CASE("A member is admitted and nothing is counted", "[node][membership][forget]")
{
    FixedMembership const oracle { Distributed::Membership::Member, Distributed::MembershipParticipant::OpenPolicy };
    AtomicMetricsSink metrics;

    CHECK_FALSE(RefuseUnlessMember(oracle, metrics, PeerIdentity { .host = "10.0.0.7" }, Stranger, StrangerWhy).has_value());

    // The control every counter assertion below needs: a gate that counted an ADMITTED
    // caller would make both of the cases that follow pass for the wrong reason.
    CHECK(metrics.Read(Stranger.counter) == 0);
    CHECK(metrics.Read(KeyForgotten.counter) == 0);
}

TEST_CASE("A revoked key is refused apart from a stranger, counted as the removed machine", "[node][membership][forget]")
{
    // **The counter that does NOT move is the assertion.** A gate that had lost its
    // forgotten arm still refuses this caller, still answers `NotAMember` on the wire,
    // and still reads as correct from the client's end -- what it loses is the
    // diagnosis, and the only thing that can see the difference is which series rose.
    // A machine is forgotten by its key, so the key roster is the one author of `Forgotten`: the
    // production fold over a roster that revoked `gone`, asked by a connection that proved that key.
    // Never a fixed `Forgotten` answered by host -- no route production has forgets an address.
    Testing::RosterFold const fold { {}, { "gone" } };
    AtomicMetricsSink metrics;

    auto const refusal = RefuseUnlessMember(fold.admitted,
                                            metrics,
                                            PeerIdentity { .host = "10.0.0.7", .proven = Testing::IdentityOf("gone") },
                                            Stranger,
                                            StrangerWhy);
    REQUIRE(refusal.has_value());

    CHECK(metrics.Read(KeyForgotten.counter) == 1);
    CHECK(KeyForgotten.counter == IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked);
    CHECK(metrics.Read(Stranger.counter) == 0);

    // One code for both causes, because a client does the same thing with either: the
    // launcher steps over the refusal and compiles locally. Pinned here because it is
    // the half a reader assumes must differ when the counter does.
    CHECK(RefusalOf(Unwrap(refusal)).first == Wire::ErrorCode::NotAMember);
}

TEST_CASE("A host nobody listed is refused as the surface's own stranger", "[node][membership][forget]")
{
    // The mirror, and the reason the case above cannot stand alone: a gate that counted
    // EVERY refusal as a forget would pass it and lose the distinction the other way.
    FixedMembership const oracle { Distributed::Membership::Outsider, Distributed::MembershipParticipant::Loopback };
    AtomicMetricsSink metrics;

    auto const refusal = RefuseUnlessMember(oracle, metrics, PeerIdentity { .host = "10.0.0.7" }, Stranger, StrangerWhy);
    REQUIRE(refusal.has_value());

    CHECK(metrics.Read(Stranger.counter) == 1);
    CHECK(metrics.Read(KeyForgotten.counter) == 0);
}

TEST_CASE("A revoked key is told what happened and what brings the machine back", "[node][membership][forget]")
{
    // The remedy text, which is the part of a guard nothing tests and the only part most
    // people read. It must say that a decision was made -- not that the caller is
    // unknown, which is what the stranger sentence says and what the old one-armed gate
    // told a decommissioned machine -- and that the way back is a NEW key.
    //
    // Told to a connection that PROVED the revoked key: the machine itself, holding it.
    Testing::RosterFold const fold { {}, { "gone" } };
    AtomicMetricsSink metrics;

    auto const refusal = RefuseUnlessMember(fold.admitted,
                                            metrics,
                                            PeerIdentity { .host = "10.0.0.7", .proven = Testing::IdentityOf("gone") },
                                            Stranger,
                                            StrangerWhy);
    REQUIRE(refusal.has_value());

    auto const [code, detail] = RefusalOf(Unwrap(refusal));
    CHECK(code == Wire::ErrorCode::NotAMember);
    CHECK(detail.contains("revoked"));
    CHECK(detail.contains("forgotten"));
    CHECK(detail.contains("admitted again under a new key"));

    // And the sentence a stranger would have got is NOT what it was told, which is the
    // half that fails when a gate loses its forgotten arm while still refusing.
    CHECK_FALSE(detail.contains(StrangerWhy));
    CHECK(metrics.Read(KeyForgotten.counter) == 1);
}

TEST_CASE("A revoked key only a ticket showed is refused as the forgotten machine and told what a stranger is",
          "[node][membership][forget][ticket]")
{
    // A ticket is bytes anybody may have captured, so its holder is not told the machine was
    // forgotten -- the fact AUTH's own refusal declines to say. It is refused exactly as firmly
    // and counted on the forgotten machine's row; only the WORDS are a stranger's. Both ways a
    // ticket shows a revoked key: evidence recorded when AUTH refused it, and a ticket AUTH
    // accepted whose key was revoked while the connection stayed open.
    Testing::RosterFold const fold { {}, { "gone" } };
    auto const byTicket = std::to_array<PeerIdentity>({
        PeerIdentity { .host = "10.0.0.7", .revokedMachine = RevokedKeyEvidence { Testing::IdentityOf("gone") } },
        PeerIdentity { .host = "10.0.0.7", .authenticatedMachine = Testing::IdentityOf("gone") },
    });
    for (auto const& peer: byTicket)
    {
        AtomicMetricsSink metrics;
        auto const refusal = RefuseUnlessMember(fold.admitted, metrics, peer, Stranger, StrangerWhy);
        REQUIRE(refusal.has_value());

        auto const [code, detail] = RefusalOf(Unwrap(refusal));
        CHECK(code == Wire::ErrorCode::NotAMember);
        CHECK(detail == StrangerWhy);
        CHECK(metrics.Read(KeyForgotten.counter) == 1);
        CHECK(metrics.Read(Stranger.counter) == 0);
    }

    // And a connection that proved the key AND showed it in a ticket is the machine itself: told.
    AtomicMetricsSink metrics;
    auto const both =
        RefuseUnlessMember(fold.admitted,
                           metrics,
                           PeerIdentity { .host = "10.0.0.7",
                                          .proven = Testing::IdentityOf("gone"),
                                          .revokedMachine = RevokedKeyEvidence { Testing::IdentityOf("gone") } },
                           Stranger,
                           StrangerWhy);
    REQUIRE(both.has_value());
    CHECK(RefusalOf(Unwrap(both)).second == KeyForgottenWhy);
}
