// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace FastCache;
using namespace FastCache::Cluster;
using namespace std::chrono_literals;

TEST_CASE("PeerDirectory records a peer and forgets it when its beacons stop", "[cluster][discovery]")
{
    ManualClock clock;
    PeerDirectory directory { clock, "prod", "self", 90s };

    CHECK(directory.NoteBeacon("prod", "worker-a", "10.0.0.5:6677") == BeaconOutcome::Recorded);
    REQUIRE(directory.Size() == 1);

    // Still inside the window: a beacon may be lost without costing a peer its
    // place, which is the whole reason the expiry is generous relative to the
    // beacon interval.
    clock.Advance(89s);
    CHECK(directory.ExpireStale() == 0);
    CHECK(directory.Size() == 1);

    clock.Advance(2s);
    CHECK(directory.ExpireStale() == 1);
    CHECK(directory.Size() == 0);
}

TEST_CASE("PeerDirectory ignores another cluster and its own beacons", "[cluster][discovery]")
{
    ManualClock clock;
    PeerDirectory directory { clock, "prod", "self" };

    // Two unrelated fleets on one segment must not disturb each other.
    CHECK(directory.NoteBeacon("staging", "worker-a", "10.0.0.5:6677") == BeaconOutcome::OtherCluster);

    // A node's own beacon comes back to it on a broadcast address. Recording it
    // would make a lone node believe it has a peer, and propose a membership
    // change to admit itself.
    CHECK(directory.NoteBeacon("prod", "self", "10.0.0.1:6677") == BeaconOutcome::Self);

    // Nothing to reach or to name in a membership entry.
    CHECK(directory.NoteBeacon("prod", "", "10.0.0.5:6677") == BeaconOutcome::Unnameable);
    CHECK(directory.NoteBeacon("prod", "worker-a", "") == BeaconOutcome::Unnameable);

    CHECK(directory.Size() == 0);
}

TEST_CASE("PeerDirectory remembers only a peer it could name", "[cluster][discovery]")
{
    // The regression for #159, at the door it enters by -- `BeaconOutcome` carries
    // why it is this door and not the proposer. One fixture for the rule and its two
    // edges, because that is what they are.
    ManualClock clock;

    SECTION("a peer whose claim is not text")
    {
        PeerDirectory directory { clock, "prod", "self" };

        // Both halves are asked, which is what this pins. WHICH sequences are invalid
        // is `Core/Utf8`'s question and is asserted there, over the whole taxonomy --
        // overlong forms, lone surrogates and the ceiling included.
        CHECK(directory.NoteBeacon("prod", "worker-\x80", "10.0.0.5:6677") == BeaconOutcome::Unnameable);
        CHECK(directory.NoteBeacon("prod", "worker-b", "10.0.0.5:6677\xE2\x82") == BeaconOutcome::Unnameable);

        // Nothing was remembered, so nothing can be published, desired or proposed --
        // and no challenge is worth the datagram either.
        CHECK(directory.Size() == 0);
    }

    SECTION("a peer named in multi-byte UTF-8, which is text")
    {
        // Encoding, not ASCII. Hex escapes rather than the characters themselves,
        // which is this tree's idiom for the same reason it is anywhere: a narrow
        // literal's meaning otherwise depends on the compiler's source charset.
        PeerDirectory directory { clock, "prod", "self" };

        CHECK(directory.NoteBeacon("prod", "arbeiter-\xC3\xA9\xE2\x82\xAC", "b\xC3\xBCro.example:6677")
              == BeaconOutcome::Recorded);
        REQUIRE(directory.Peers().size() == 1);
        CHECK(directory.Peers().front().nodeId == "arbeiter-\xC3\xA9\xE2\x82\xAC");
    }

    SECTION("a cluster id, which is compared and never recorded")
    {
        // The deliberate hole in the rule, pinned so nobody closes it. A cluster id
        // never reaches replicated state -- it is matched byte for byte and thrown
        // away -- so subjecting it to the same filter would buy nothing and would cost
        // a fleet named in some other encoding every peer it has, silently, because
        // both ends would agree to ignore each other.
        PeerDirectory directory { clock, "prod-\x80", "self" };

        CHECK(directory.NoteBeacon("prod-\x80", "worker-a", "10.0.0.5:6677") == BeaconOutcome::Recorded);
        CHECK(directory.Size() == 1);
    }
}

TEST_CASE("PeerDirectory keeps a peer it can name when the next beacon is one it cannot", "[cluster][discovery]")
{
    // A refused beacon changes nothing, which matters because the endpoint path it
    // does not reach is destructive: a peer that advertises a DIFFERENT endpoint
    // loses its authenticated bit. Letting an unrecordable claim take that path
    // would let anything on the segment un-admit a proved peer by beaconing
    // garbage in its name once per interval.
    ManualClock clock;
    PeerDirectory directory { clock, "prod", "self" };

    REQUIRE(directory.NoteBeacon("prod", "worker-a", "10.0.0.5:6677") == BeaconOutcome::Recorded);
    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.5:6677"));
    REQUIRE(directory.AuthenticatedPeers().size() == 1);

    CHECK(directory.NoteBeacon("prod", "worker-a", "10.0.0.9:6677\xFF") == BeaconOutcome::Unnameable);

    REQUIRE(directory.Peers().size() == 1);
    CHECK(directory.Peers().front().raftEndpoint == "10.0.0.5:6677");
    CHECK(directory.AuthenticatedPeers().size() == 1);
}

TEST_CASE("PeerDirectory keeps 'seen' and 'proved' apart", "[cluster][discovery]")
{
    // The security property of this whole layer. A beacon is unauthenticated by
    // construction -- anybody on the segment can send one -- and a node that is
    // admitted gets compile jobs and returns objects cached fleet-wide. So being
    // seen must never imply being trusted.
    ManualClock clock;
    PeerDirectory directory { clock, "prod", "self" };

    REQUIRE(directory.NoteBeacon("prod", "worker-a", "10.0.0.5:6677") == BeaconOutcome::Recorded);
    REQUIRE(directory.Peers().size() == 1);
    CHECK_FALSE(directory.Peers().front().authenticated);
    CHECK(directory.AuthenticatedPeers().empty());

    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.5:6677"));
    CHECK(directory.AuthenticatedPeers().size() == 1);
}

TEST_CASE("PeerDirectory will not authenticate an endpoint nobody proved", "[cluster][discovery]")
{
    ManualClock clock;
    PeerDirectory directory { clock, "prod", "self" };
    REQUIRE(directory.NoteBeacon("prod", "worker-a", "10.0.0.5:6677") == BeaconOutcome::Recorded);

    // A proof covers a (node, endpoint) PAIR, because both are inside the MAC.
    // Accepting one against a different endpoint would let a beacon sent between
    // the challenge and the proof redirect an authenticated peer to an address
    // its holder never proved.
    CHECK_FALSE(directory.MarkAuthenticated("worker-a", "10.0.0.9:6677"));
    CHECK(directory.AuthenticatedPeers().empty());

    // And an id nobody has beaconed for is not a peer at all.
    CHECK_FALSE(directory.MarkAuthenticated("worker-z", "10.0.0.5:6677"));
}

TEST_CASE("PeerDirectory drops authentication when a peer moves", "[cluster][discovery]")
{
    // The mirror image of the case above, and the one that would be easy to get
    // wrong by treating the authenticated bit as a property of the NODE. It is a
    // property of the node at an endpoint: carrying it across a move would admit
    // an address nobody proved, which is exactly what putting the endpoint inside
    // the MAC exists to prevent.
    ManualClock clock;
    PeerDirectory directory { clock, "prod", "self" };

    REQUIRE(directory.NoteBeacon("prod", "worker-a", "10.0.0.5:6677") == BeaconOutcome::Recorded);
    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.5:6677"));
    REQUIRE(directory.AuthenticatedPeers().size() == 1);

    REQUIRE(directory.NoteBeacon("prod", "worker-a", "10.0.0.9:6677") == BeaconOutcome::Recorded);
    CHECK(directory.AuthenticatedPeers().empty());
    REQUIRE(directory.Peers().size() == 1);
    CHECK(directory.Peers().front().raftEndpoint == "10.0.0.9:6677");

    // A repeated beacon for the SAME endpoint must not drop it, or a peer would
    // lose its place every beacon interval and the cluster would never settle.
    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.9:6677"));
    REQUIRE(directory.NoteBeacon("prod", "worker-a", "10.0.0.9:6677") == BeaconOutcome::Recorded);
    CHECK(directory.AuthenticatedPeers().size() == 1);
}

TEST_CASE("PeerDirectory answers in a stable order", "[cluster][discovery]")
{
    // The backing container is unordered, and its iteration order varies between
    // runs and standard libraries. A caller proposing a membership change from an
    // unordered snapshot would produce a different proposal on each node.
    ManualClock clock;
    PeerDirectory directory { clock, "prod", "self" };

    for (auto const* const id: { "worker-c", "worker-a", "worker-b" })
        REQUIRE(directory.NoteBeacon("prod", id, "10.0.0.1:6677") == BeaconOutcome::Recorded);

    auto const peers = directory.Peers();
    REQUIRE(peers.size() == 3);
    CHECK(peers[0].nodeId == "worker-a");
    CHECK(peers[1].nodeId == "worker-b");
    CHECK(peers[2].nodeId == "worker-c");
}
