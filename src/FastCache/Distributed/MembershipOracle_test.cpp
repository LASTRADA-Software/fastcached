// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache::Distributed;

TEST_CASE("An empty cluster admits nobody", "[distributed][membership]")
{
    // The direction a mistake has to fail in. A node that has not yet discovered a
    // peer -- or whose discovery is misconfigured, or whose key is wrong -- must not
    // silently become an open scheduler: that failure is invisible from both ends,
    // because the fleet keeps working and simply serves strangers too.
    ClusterMembership const cluster;

    CHECK(cluster.Size() == 0);
    CHECK(cluster.Classify("10.0.0.9") == Membership::Outsider);
    CHECK(cluster.Classify("") == Membership::Outsider);
}

TEST_CASE("A member is admitted by host, whatever port it dials from", "[distributed][membership]")
{
    // The whole reason the constructor takes endpoints and stores hosts. Discovery
    // admits a peer at a (node, endpoint) pair, but a peer *connecting* to the
    // scheduler comes from an ephemeral source port -- so an endpoint-keyed set would
    // refuse every legitimate member while looking entirely correct, and the fleet
    // would silently never distribute anything.
    ClusterMembership const cluster { { "10.0.0.1:7000", "10.0.0.2:7000" } };

    CHECK(cluster.Classify("10.0.0.1") == Membership::Member);
    CHECK(cluster.Classify("10.0.0.2") == Membership::Member);
    CHECK(cluster.Classify("10.0.0.3") == Membership::Outsider);

    // Matched whole, not by prefix. A prefix test would admit `10.0.0.10` on the
    // strength of `10.0.0.1`, which is a different machine.
    CHECK(cluster.Classify("10.0.0.10") == Membership::Outsider);

    // And an endpoint is NOT an identity here. Querying in the vocabulary the set was
    // built from is the mistake the constructor exists to make impossible, so it is
    // pinned: this must not accidentally start working.
    CHECK(cluster.Classify("10.0.0.1:7000") == Membership::Outsider);
}

TEST_CASE("An IPv6 endpoint keeps its address rather than its last colon group",
          "[distributed][membership]")
{
    // `rfind(':')` on `[::1]:7000` splits at the wrong colon and yields a host of
    // `[::1]` or worse -- the exact defect `Core/HostPort` exists to hold in one
    // place. Reaching for it here rather than splitting locally is what keeps this
    // from being a second author of that rule.
    ClusterMembership const cluster { { "[::1]:7000", "[fe80::1]:7000" } };

    CHECK(cluster.Classify("::1") == Membership::Member);
    CHECK(cluster.Classify("fe80::1") == Membership::Member);
    CHECK(cluster.Classify("fe80::2") == Membership::Outsider);
}

TEST_CASE("An endpoint with no port is kept whole", "[distributed][membership]")
{
    // A member the set cannot represent must not silently stop being one. Dropping it
    // would be the same silent-refusal failure the host/endpoint collapse exists to
    // prevent, arriving by a different route.
    ClusterMembership const cluster { { "10.0.0.1" } };

    CHECK(cluster.Size() == 1);
    CHECK(cluster.Classify("10.0.0.1") == Membership::Member);
}

TEST_CASE("Membership can be republished while the scheduler runs", "[distributed][membership]")
{
    // The carve-out to configuration-at-construction, and the reason for it:
    // membership is precisely what changes while this object lives. Rebuilding the
    // oracle per join would mean handing a new one to a running server.
    ClusterMembership cluster { { "10.0.0.1:7000" } };
    REQUIRE(cluster.Classify("10.0.0.1") == Membership::Member);

    cluster.Publish({ "10.0.0.2:7000" });

    // A peer that left is refused from the next request, not from the next restart.
    CHECK(cluster.Classify("10.0.0.1") == Membership::Outsider);
    CHECK(cluster.Classify("10.0.0.2") == Membership::Member);
    CHECK(cluster.Size() == 1);
}

TEST_CASE("An open deployment admits everyone, and says so by name", "[distributed][membership]")
{
    // The right answer for one machine, or a fleet whose reachability is its
    // boundary -- but never a default. "No policy" and "a policy that admits
    // everybody" have to be the same explicit decision, which is why this is a
    // named type somebody constructs rather than an unset field.
    OpenMembership const open;

    CHECK(open.Classify("10.0.0.9:7100") == Membership::Member);
    CHECK(open.Classify("") == Membership::Member);
}

TEST_CASE("A scheduler refuses a non-member through the oracle", "[distributed][membership][scheduler]")
{
    // The two halves joined: the oracle answers who, `SchedulerService` decides
    // what. Asserted together because each is correct in isolation and the wiring
    // between them is what a caller actually depends on.
    ClusterMembership const cluster { { "10.0.0.1:7000" } };
    FastCache::ManualClock clock;
    FastCache::AtomicMetricsSink metrics;
    SchedulerService service { clock, metrics };
    service.SetRole(SchedulerRole::Leader, {});

    auto const ask = [&](std::string_view peer) {
        return service.Lease(
            CallerContext { .membership = cluster.Classify(peer), .peerId = peer },
            FastCache::CompileCacheWire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
    };

    // A member reaches the fleet and is refused only for want of a worker, which is
    // the fleet's own answer rather than the policy's.
    CHECK(ask("10.0.0.1").error == FastCache::CompileCacheWire::ErrorCode::NoWorker);

    // A stranger never gets that far.
    CHECK(ask("10.0.0.9").error == FastCache::CompileCacheWire::ErrorCode::NotAMember);
}
