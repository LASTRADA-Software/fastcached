// SPDX-License-Identifier: Apache-2.0
#include "SchedulerLink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
constexpr std::string_view Configured = "scheduler.example:6676";
constexpr std::string_view Leader = "10.0.0.7:6676";
constexpr std::string_view Other = "10.0.0.9:6676";
} // namespace

TEST_CASE("A node starts at the endpoint it was configured with", "[node][schedulerlink]")
{
    SchedulerLink link { std::string { Configured } };
    link.BeginRound();

    CHECK(link.Target() == Configured);
    CHECK_FALSE(link.Following());
}

TEST_CASE("A NotLeader redirect moves this round's target", "[node][schedulerlink]")
{
    // The whole of #237's worker half: before this, the refusal was logged and the
    // next dial went back to the demoted scheduler, so a worker announced itself to
    // a node that refuses `Register` and expired out of the real leader's registry.
    SchedulerLink link { std::string { Configured } };
    link.BeginRound();

    REQUIRE(link.Redirect(std::string { Leader }));
    CHECK(link.Target() == Leader);
}

TEST_CASE("The redirect chain is bounded, so two schedulers naming each other cost one round", "[node][schedulerlink]")
{
    // Not this node's chain to trust. A partition healing, or a stale `_knownLeader`
    // on either side, can have two schedulers name each other indefinitely.
    SchedulerLink link { std::string { Configured } };
    link.BeginRound();

    for (int hop = 0; hop < MaxAnnounceRedirects; ++hop)
        CHECK(link.Redirect(std::string { Leader }));

    // Spent: the caller gives up until the next round rather than looping.
    CHECK_FALSE(link.Redirect(std::string { Other }));
}

TEST_CASE("The budget is per round, not per process", "[node][schedulerlink]")
{
    // A fleet that re-elects once an hour should spend one redirect an hour. A
    // lifetime ceiling would follow redirects for a while and then silently stop,
    // which is the same outage as never following one -- arriving later.
    SchedulerLink link { std::string { Configured } };

    link.BeginRound();
    for (int hop = 0; hop < MaxAnnounceRedirects; ++hop)
        REQUIRE(link.Redirect(std::string { Leader }));
    REQUIRE_FALSE(link.Redirect(std::string { Other }));

    link.BeginRound();
    CHECK(link.Redirect(std::string { Leader }));
}

TEST_CASE("A leader is remembered only once a round has been accepted there", "[node][schedulerlink]")
{
    // An endpoint some scheduler NAMED is a lead; an endpoint that took this node's
    // registration is a leader. Committing on the name alone would let one bad
    // redirect become the endpoint every future round starts at.
    SchedulerLink link { std::string { Configured } };

    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    CHECK_FALSE(link.Following()); // followed, not yet committed

    link.Accepted();
    CHECK(link.Following());

    // And the next round opens there, which is what stops a steady-state fleet
    // paying a redirect on every single heartbeat.
    link.BeginRound();
    CHECK(link.Target() == Leader);
}

TEST_CASE("A redirect that is followed and then refused does not become the next round's start", "[node][schedulerlink]")
{
    // The endpoint answered -- it was reachable -- but refused for its own reasons:
    // not a member, a fingerprint it will not take. That is not a leader, and
    // starting there every round would pin this node to it.
    SchedulerLink link { std::string { Configured } };

    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    CHECK(link.Lost() == std::optional { std::string { Configured } });

    link.BeginRound();
    CHECK(link.Target() == Configured);
    CHECK_FALSE(link.Following());
}

TEST_CASE("A remembered leader that stops answering falls back inside the same round", "[node][schedulerlink]")
{
    // Not a heartbeat interval later. This machine is absent from the fleet for as
    // long as this takes, and the configured endpoint is the one still standing
    // after an election the remembered leader lost.
    SchedulerLink link { std::string { Configured } };
    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    link.Accepted();
    REQUIRE(link.Following());

    link.BeginRound();
    REQUIRE(link.Target() == Leader);

    auto const fallback = link.Lost();
    REQUIRE(fallback.has_value());
    CHECK(Unwrap(fallback) == Configured);
    CHECK(link.Target() == Configured);
    CHECK_FALSE(link.Following());
}

TEST_CASE("Losing the configured endpoint offers nothing further, rather than spinning", "[node][schedulerlink]")
{
    // There is nowhere further back to fall. Returning the same endpoint again
    // would have the caller redial it inside one round forever.
    SchedulerLink link { std::string { Configured } };
    link.BeginRound();

    CHECK_FALSE(link.Lost().has_value());
    CHECK(link.Target() == Configured);
}

TEST_CASE("Being accepted back at the configured endpoint forgets the remembered leader", "[node][schedulerlink]")
{
    // A fleet that re-elects back to the original scheduler must stop reporting
    // that it is following one. Storing the configured endpoint as a `_learned`
    // equal to the default would behave identically and mislead every diagnostic.
    SchedulerLink link { std::string { Configured } };
    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    link.Accepted();
    REQUIRE(link.Following());

    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Configured }));
    link.Accepted();

    CHECK_FALSE(link.Following());
    link.BeginRound();
    CHECK(link.Target() == Configured);
}
