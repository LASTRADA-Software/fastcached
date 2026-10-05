// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/PeerDirectory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

#include <core/platform/Clock.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using namespace std::chrono_literals;

namespace
{
/// The key a proof verified under. Any 32 bytes: the directory records it and verifies nothing.
/// @return The key.
[[nodiscard]] Ed25519PublicKey ProvenKey()
{
    auto key = Ed25519PublicKey {};
    key.fill(std::byte { 0x42 });
    return key;
}

/// What a beacon from node @p nodeId of fleet @p clusterId says, answering Raft at @p endpoint.
/// @param clusterId The fleet it claims.
/// @param nodeId Who sent it.
/// @param endpoint Where it says it answers; empty for a learner, which has nowhere.
/// @return The summary; every other field defaulted.
[[nodiscard]] CompileCacheWire::FleetSummary Beacon(std::string_view clusterId,
                                                    std::string_view nodeId,
                                                    std::string_view endpoint)
{
    return CompileCacheWire::FleetSummary { .clusterId = std::string { clusterId },
                                            .nodeId = std::string { nodeId },
                                            .raftEndpoint = std::string { endpoint } };
}
} // namespace

TEST_CASE("PeerDirectory records a peer and forgets it when its beacons stop", "[cluster][discovery]")
{
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster, 90s };

    CHECK(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Recorded);
    REQUIRE(directory.Size() == 1);

    // Still inside the window: a beacon may be lost without costing a peer its
    // place, which is the whole reason the expiry is generous relative to the
    // beacon interval.
    clock.advance(89s);
    CHECK(directory.ExpireStale() == 0);
    CHECK(directory.Size() == 1);

    clock.advance(2s);
    CHECK(directory.ExpireStale() == 1);
    CHECK(directory.Size() == 0);
}

TEST_CASE("PeerDirectory records another cluster apart and ignores its own beacons", "[cluster][discovery]")
{
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };

    // Another fleet on the segment is recorded APART: as a fleet this node may challenge and
    // report to formation, never as a peer anything could desire.
    CHECK(directory.NoteBeacon(Beacon("staging", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Foreign);
    CHECK(directory.ForeignFleets() == 1);
    CHECK(directory.Peers().empty());

    // Another fleet's node carrying THIS node's id is that fleet's node: an id is operator-typed,
    // and two offices may both have a `build1`. Only this cluster's own id is this node.
    CHECK(directory.NoteBeacon(Beacon("staging", "self", "10.0.0.8:6677")) == BeaconOutcome::Foreign);

    // A node's own beacon comes back to it on a broadcast address. Recording it
    // would make a lone node believe it has a peer, and propose a membership
    // change to admit itself.
    CHECK(directory.NoteBeacon(Beacon("prod", "self", "10.0.0.1:6677")) == BeaconOutcome::Self);

    // Nobody to name in a membership entry.
    CHECK(directory.NoteBeacon(Beacon("prod", "", "10.0.0.5:6677")) == BeaconOutcome::Unnameable);

    CHECK(directory.Size() == 0);
}

TEST_CASE("PeerDirectory records a learner, which has no consensus endpoint", "[cluster][discovery][formation]")
{
    // A learner dials in and answers nothing, so it announces no Raft endpoint -- and it is
    // still a peer this node can name, and so challenge. What is desired from it is the
    // discovery tier's question, which skips a peer with nothing to dial.
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };

    CHECK(directory.NoteBeacon(Beacon("prod", "learner", "")) == BeaconOutcome::Recorded);
    REQUIRE(directory.Peers().size() == 1);
    CHECK(directory.Peers().front().raftEndpoint.empty());
}

TEST_CASE("PeerDirectory holds at most MaxForeignFleets other fleets, displacing the oldest that proved nothing",
          "[cluster][discovery][formation]")
{
    // [RF-5] A beacon is unauthenticated, so the table of other fleets is one anything on the
    // segment can grow by inventing cluster ids. Bounded; at the bound a newcomer displaces the
    // OLDEST fleet that has proven nothing -- by when its slot was taken, so re-sending does not
    // keep a spoofed id young -- and a fleet that answered a challenge keeps its slot.
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster, 90s };

    for (auto const index: std::views::iota(std::size_t { 0 }, MaxForeignFleets))
    {
        REQUIRE(directory.NoteBeacon(Beacon(std::format("c-{:02}", index), "n", "10.0.0.5:6677")) == BeaconOutcome::Foreign);
        clock.advance(1s);
    }
    CHECK(directory.ForeignFleets() == MaxForeignFleets);

    // The oldest, c-00, proves its summary; c-01 is re-sent and so heard most recently of all.
    REQUIRE(directory.MarkForeignProven("c-00"));
    CHECK(directory.NoteBeacon(Beacon("c-01", "n", "10.0.0.5:6677")) == BeaconOutcome::Foreign);

    // A newcomer displaces c-01 -- the oldest slot that proved nothing, however recently heard --
    // and never c-00, which proved.
    CHECK(directory.NoteBeacon(Beacon("c-new", "n", "10.0.0.5:6677")) == BeaconOutcome::ForeignDisplacing);
    CHECK(directory.ForeignFleets() == MaxForeignFleets);
    CHECK(directory.NoteBeacon(Beacon("c-00", "n", "10.0.0.5:6677")) == BeaconOutcome::Foreign);
    CHECK(directory.NoteBeacon(Beacon("c-01", "n", "10.0.0.5:6677")) == BeaconOutcome::ForeignDisplacing);

    // A fleet already held is refreshed, not displacing anything: the bound is on DISTINCT fleets.
    CHECK(directory.NoteBeacon(Beacon("c-new", "n2", "10.0.0.6:6677")) == BeaconOutcome::Foreign);
}

TEST_CASE("PeerDirectory drops a new fleet only when every slot has proven itself", "[cluster][discovery][formation]")
{
    // Holding a slot against a newcomer costs an answered challenge per slot: a real address and a
    // real key, never a spoofed datagram.
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster, 90s };

    for (auto const index: std::views::iota(std::size_t { 0 }, MaxForeignFleets))
    {
        auto const cluster = std::format("c-{:02}", index);
        REQUIRE(directory.NoteBeacon(Beacon(cluster, "n", "10.0.0.5:6677")) == BeaconOutcome::Foreign);
        REQUIRE(directory.MarkForeignProven(cluster));
    }
    CHECK(directory.NoteBeacon(Beacon("c-new", "n", "10.0.0.5:6677")) == BeaconOutcome::ForeignTableFull);
    CHECK_FALSE(directory.MarkForeignProven("c-new"));

    // Expiry frees a proven slot on the peers' own clock, as it does any other.
    clock.advance(PeerDirectory::DefaultExpiry);
    CHECK(directory.ExpireStale() == MaxForeignFleets);
    CHECK(directory.NoteBeacon(Beacon("c-new", "n", "10.0.0.5:6677")) == BeaconOutcome::Foreign);
}

TEST_CASE("PeerDirectory bounds this cluster's unrostered peers and never displaces a rostered one",
          "[cluster][discovery][formation]")
{
    // This cluster's id is in every beacon it sends, so its peer table is one anything on the
    // segment can grow by inventing node ids. A peer the roster holds a key for is bounded by the
    // roster; every other shares `MaxUnrosteredPeers`, the least recently heard displaced first.
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    auto const members = Testing::SharedRoster::Of({ "worker-r" });
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), members };
    PeerDirectory directory { clock, self, roster, 90s };

    REQUIRE(directory.NoteBeacon(Beacon("prod", "worker-r", "10.0.0.2:6677")) == BeaconOutcome::Recorded);
    for (auto const index: std::views::iota(0, 5000))
    {
        clock.advance(1ms);
        (void) directory.NoteBeacon(Beacon("prod", std::format("n-{}", index), "10.0.0.66:6677"));
    }
    CHECK(directory.UnrosteredPeers() == MaxUnrosteredPeers);
    CHECK(directory.Size() == MaxUnrosteredPeers + 1);
    CHECK(std::ranges::contains(directory.Peers(), std::string { "worker-r" }, &KnownPeer::nodeId));

    // The newest are the ones kept: the flood displaced its own oldest.
    CHECK(std::ranges::contains(directory.Peers(), std::string { "n-4999" }, &KnownPeer::nodeId));
    CHECK_FALSE(std::ranges::contains(directory.Peers(), std::string { "n-0" }, &KnownPeer::nodeId));
}

TEST_CASE("PeerDirectory remembers only a peer it could name", "[cluster][discovery]")
{
    // The regression for #159, at the door it enters by -- `BeaconOutcome` carries
    // why it is this door and not the proposer. One fixture for the rule and its two
    // edges, because that is what they are.
    core::platform::ManualClock clock;

    SECTION("a peer whose claim is not text")
    {
        Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
        Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
        PeerDirectory directory { clock, self, roster };

        // Both halves are asked, which is what this pins. WHICH sequences are invalid
        // is `Core/Utf8`'s question and is asserted there, over the whole taxonomy --
        // overlong forms, lone surrogates and the ceiling included.
        CHECK(directory.NoteBeacon(Beacon("prod", "worker-\x80", "10.0.0.5:6677")) == BeaconOutcome::Unnameable);
        CHECK(directory.NoteBeacon(Beacon("prod", "worker-b", "10.0.0.5:6677\xE2\x82")) == BeaconOutcome::Unnameable);

        // Nothing was remembered, so nothing can be published, desired or proposed --
        // and no challenge is worth the datagram either.
        CHECK(directory.Size() == 0);
    }

    SECTION("a peer named in multi-byte UTF-8, which is text")
    {
        // Encoding, not ASCII. Hex escapes rather than the characters themselves,
        // which is this tree's idiom for the same reason it is anywhere: a narrow
        // literal's meaning otherwise depends on the compiler's source charset.
        Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
        Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
        PeerDirectory directory { clock, self, roster };

        CHECK(directory.NoteBeacon(Beacon("prod", "arbeiter-\xC3\xA9\xE2\x82\xAC", "b\xC3\xBCro.example:6677"))
              == BeaconOutcome::Recorded);
        REQUIRE(directory.Peers().size() == 1);
        CHECK(directory.Peers().front().nodeId == "arbeiter-\xC3\xA9\xE2\x82\xAC");
    }

    SECTION("a cluster id, which is compared and never recorded as a member")
    {
        // The deliberate hole in the rule, pinned so nobody closes it. A cluster id
        // never reaches replicated state -- it is matched byte for byte, and another
        // fleet's is kept only as a key of the foreign table, which nothing renders -- so
        // subjecting it to the same filter would buy nothing and would cost
        // a fleet named in some other encoding every peer it has, silently, because
        // both ends would agree to ignore each other.
        Testing::ScriptedSummarySource self { Beacon("prod-\x80", "self", "10.0.0.1:6677") };
        Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
        PeerDirectory directory { clock, self, roster };

        CHECK(directory.NoteBeacon(Beacon("prod-\x80", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Recorded);
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
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };

    REQUIRE(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Recorded);
    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.5:6677", ProvenKey()));
    REQUIRE(directory.AuthenticatedPeers().size() == 1);

    CHECK(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.9:6677\xFF")) == BeaconOutcome::Unnameable);

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
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };

    REQUIRE(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Recorded);
    REQUIRE(directory.Peers().size() == 1);
    CHECK_FALSE(directory.Peers().front().authenticated);
    CHECK_FALSE(directory.Peers().front().provenKey.has_value());
    CHECK(directory.AuthenticatedPeers().empty());

    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.5:6677", ProvenKey()));
    REQUIRE(directory.AuthenticatedPeers().size() == 1);

    // And WHICH key it proved, which is what lets a reader re-ask the roster later rather
    // than trusting a proof from whenever it was taken (#178). Absent until proved.
    CHECK(directory.AuthenticatedPeers().front().provenKey == ProvenKey());
}

TEST_CASE("PeerDirectory will not authenticate an endpoint nobody proved", "[cluster][discovery]")
{
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };
    REQUIRE(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Recorded);

    // A proof covers a (node, endpoint) PAIR, because both are inside the signature.
    // Accepting one against a different endpoint would let a beacon sent between
    // the challenge and the proof redirect an authenticated peer to an address
    // its holder never proved.
    CHECK_FALSE(directory.MarkAuthenticated("worker-a", "10.0.0.9:6677", ProvenKey()));
    CHECK(directory.AuthenticatedPeers().empty());

    // And an id nobody has beaconed for is not a peer at all.
    CHECK_FALSE(directory.MarkAuthenticated("worker-z", "10.0.0.5:6677", ProvenKey()));
}

TEST_CASE("PeerDirectory drops authentication when a peer moves", "[cluster][discovery]")
{
    // The mirror image of the case above, and the one that would be easy to get
    // wrong by treating the authenticated bit as a property of the NODE. It is a
    // property of the node at an endpoint: carrying it across a move would admit
    // an address nobody proved, which is exactly what putting the endpoint inside
    // the signature exists to prevent.
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };

    REQUIRE(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Recorded);
    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.5:6677", ProvenKey()));
    REQUIRE(directory.AuthenticatedPeers().size() == 1);

    REQUIRE(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.9:6677")) == BeaconOutcome::Recorded);
    CHECK(directory.AuthenticatedPeers().empty());
    REQUIRE(directory.Peers().size() == 1);
    CHECK(directory.Peers().front().raftEndpoint == "10.0.0.9:6677");

    // The proven key goes with the bit: a key proved at one address says nothing about who
    // answers at the next.
    CHECK_FALSE(directory.Peers().front().provenKey.has_value());

    // A repeated beacon for the SAME endpoint must not drop it, or a peer would
    // lose its place every beacon interval and the cluster would never settle.
    REQUIRE(directory.MarkAuthenticated("worker-a", "10.0.0.9:6677", ProvenKey()));
    REQUIRE(directory.NoteBeacon(Beacon("prod", "worker-a", "10.0.0.9:6677")) == BeaconOutcome::Recorded);
    CHECK(directory.AuthenticatedPeers().size() == 1);
}

TEST_CASE("PeerDirectory answers in a stable order", "[cluster][discovery]")
{
    // The backing container is unordered, and its iteration order varies between
    // runs and standard libraries. A caller proposing a membership change from an
    // unordered snapshot would produce a different proposal on each node.
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("prod", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };

    for (auto const* const id: { "worker-c", "worker-a", "worker-b" })
        REQUIRE(directory.NoteBeacon(Beacon("prod", id, "10.0.0.1:6677")) == BeaconOutcome::Recorded);

    auto const peers = directory.Peers();
    REQUIRE(peers.size() == 3);
    CHECK(peers[0].nodeId == "worker-a");
    CHECK(peers[1].nodeId == "worker-b");
    CHECK(peers[2].nodeId == "worker-c");
}

TEST_CASE("PeerDirectory follows this node's cluster as its summary changes", "[cluster][discovery][formation]")
{
    // One source for this node's cluster, read live: a node that dissolves or adopts a fleet is
    // judged against the cluster it is in NOW, never the one it was built in.
    core::platform::ManualClock clock;
    Testing::ScriptedSummarySource self { Beacon("c-old", "self", "10.0.0.1:6677") };
    Testing::RosterPeerKeys const roster { Testing::TestKeyPair("self"), Testing::SharedRoster::Of({}) };
    PeerDirectory directory { clock, self, roster };
    CHECK(directory.NoteBeacon(Beacon("c-new", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Foreign);

    self.Set(Beacon("c-new", "self", "10.0.0.1:6677"));
    CHECK(directory.NoteBeacon(Beacon("c-new", "worker-a", "10.0.0.5:6677")) == BeaconOutcome::Recorded);
    CHECK(directory.NoteBeacon(Beacon("c-new", "self", "10.0.0.1:6677")) == BeaconOutcome::Self);
    CHECK(directory.NoteBeacon(Beacon("c-old", "self", "10.0.0.1:6677")) == BeaconOutcome::Foreign);
}
