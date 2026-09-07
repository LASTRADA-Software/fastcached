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

TEST_CASE("DescribeAnnounceRound: a steady heartbeat round does not claim a registration", "[node][scheduler]")
{
    using namespace FastCache::Node;

    // The defect itself (#999): six heartbeats reported "6 of 6 toolchain(s)
    // registered", every interval, forever. Asserted on the WORDING and not only the
    // level, because that is what was wrong -- a test checking the level alone passes
    // with the sentence still collapsed.
    auto const steady = DescribeAnnounceRound(6, 0, 6, false);
    CHECK(steady.level == FastCache::LogLevel::Trace);
    CHECK(!steady.message.contains("registered,"));
    CHECK(steady.message.contains("still registered"));
    CHECK(steady.message.contains("6"));
}

TEST_CASE("DescribeAnnounceRound: a real registration is an event and says so", "[node][scheduler]")
{
    using namespace FastCache::Node;

    auto const fresh = DescribeAnnounceRound(0, 6, 6, false);
    CHECK(fresh.level == FastCache::LogLevel::Info);

    // The ACCEPTED form is a fixture contract: `E2eRegisteredMarker` is
    // "1 of 1 toolchain(s) registered" and distinguishes an accepted worker from one
    // the scheduler turned away (#445). Pinned here as bytes, because rewording it
    // breaks three e2e fixtures by TIMEOUT rather than by a failed assertion.
    CHECK(DescribeAnnounceRound(0, 1, 1, false).message == "1 of 1 toolchain(s) registered");
    CHECK(fresh.message.contains("6 of 6 toolchain(s) registered"));

    // And it must be distinguishable from the steady round, which is the whole point.
    CHECK(fresh.message != DescribeAnnounceRound(6, 0, 6, false).message);
    CHECK(fresh.level != DescribeAnnounceRound(6, 0, 6, false).level);
}

TEST_CASE("DescribeAnnounceRound: a heartbeat that fell through to a register is its own outcome", "[node][scheduler]")
{
    using namespace FastCache::Node;

    // The case worth catching: the scheduler forgot this worker and it re-announced.
    // Fully accepted, so a count-based check calls it healthy -- and it is an event.
    auto const mixed = DescribeAnnounceRound(4, 2, 6, false);
    CHECK(mixed.level == FastCache::LogLevel::Info);
    CHECK(mixed.message.contains("2 of 6 toolchain(s) re-registered"));

    // Three outcomes, three sentences. None may collide with another.
    auto const steady = DescribeAnnounceRound(6, 0, 6, false);
    auto const fresh = DescribeAnnounceRound(0, 6, 6, false);
    CHECK(mixed.message != steady.message);
    CHECK(mixed.message != fresh.message);
}

TEST_CASE("DescribeAnnounceRound: a shortfall is Warn, unless a leader was named", "[node][scheduler]")
{
    using namespace FastCache::Node;

    // Preserved from the code this replaced: reporting a shortfall at Warn on every
    // election trains an operator to ignore the line that matters, because the caller
    // follows the redirect inside this same round.
    CHECK(DescribeAnnounceRound(0, 0, 1, false).level == FastCache::LogLevel::Warn);
    CHECK(DescribeAnnounceRound(0, 0, 1, true).level == FastCache::LogLevel::Debug);

    // A shortfall still says which half happened, so "the scheduler refused two" and
    // "two never got a heartbeat in" are not one sentence.
    // The SHORTFALL form is the other half of that contract -- the self-test stages
    // "0 of 1 toolchain(s) registered" as the line a turned-away worker writes, so it
    // is pinned as bytes too.
    CHECK(DescribeAnnounceRound(0, 0, 1, false).message == "0 of 1 toolchain(s) registered");
    CHECK(DescribeAnnounceRound(3, 1, 6, false).message == "4 of 6 toolchain(s) registered");
}
