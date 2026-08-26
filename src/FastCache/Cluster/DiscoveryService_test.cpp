// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryDatagram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace
{
/// A key, as the config wants it.
[[nodiscard]] std::vector<std::byte> Key(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (auto const ch: text)
        out.push_back(static_cast<std::byte>(ch));
    return out;
}

/// The bus address a `host:port` endpoint names -- see `DatagramAddress` for why
/// the two halves travel apart below this layer.
///
/// Named for what it takes, because `InMemoryDatagram_test`'s `AtHost` takes a
/// bare host and supplies a port: both end up in one test binary, and two
/// same-named helpers whose contracts differ by an invisible `:7000` is how a
/// case comes to address nowhere at all.
/// @param endpoint `host:port` text.
/// @return The two halves apart.
[[nodiscard]] DatagramAddress AtEndpoint(std::string_view endpoint)
{
    // Asserted rather than defaulted. An endpoint this cannot split would
    // otherwise become `{"", 0}`, which is a perfectly valid bus address that
    // simply matches no inbox -- so every datagram aimed at it would be
    // discarded exactly as UDP discards one addressed to nobody, and the case
    // would fail as "the peer never answered" instead of "the test said the
    // wrong thing".
    auto const parsed = ParseEndpoint(endpoint, "");
    REQUIRE(parsed.has_value());
    return DatagramAddress { .host = Unwrap(parsed).first, .port = Unwrap(parsed).second };
}

/// One node on the segment: its socket, directory and service.
///
/// Bundled because a discovery node is exactly these three over one clock,
/// and a test that wired them by hand at every case would spend more lines on
/// setup than on the property being asserted.
struct Node
{
    Node(DatagramBus& bus,
         IClock& clock,
         IRandomSource& random,
         ILogger& logger,
         std::string id,
         std::string endpoint,
         std::string cluster,
         std::string const& key):
        socket { bus.Open(AtEndpoint(endpoint)) },
        directory { clock, cluster, id },
        service { *socket,
                  clock,
                  random,
                  directory,
                  DiscoveryConfig { .clusterId = std::move(cluster),
                                    .nodeId = std::move(id),
                                    .raftEndpoint = std::move(endpoint),
                                    .beaconAddress = DatagramBus::BroadcastAddress(),
                                    .presharedKey = Key(key) },
                  logger }
    {
    }

    std::unique_ptr<IDatagramSocket> socket;
    PeerDirectory directory;
    DiscoveryService service;
};

/// Drain everything waiting for @p node, so a test can settle the segment.
/// @param node Whose inbox to drain.
/// @return How many datagrams were handled.
std::size_t Drain(Node& node)
{
    std::size_t handled = 0;
    while (node.service.PumpOnce(1ms) != DiscoveryEvent::Nothing)
        ++handled;
    return handled;
}
} // namespace

TEST_CASE("Two nodes discover each other and prove the key", "[cluster][discovery][service]")
{
    // The whole feature, end to end, in one process: beacon, challenge, proof,
    // admitted. What makes this a unit test rather than a fixture is that the
    // segment, the clock and the nonces are all injected.
    DatagramBus bus;
    ManualClock clock;
    ScriptedRandomSource random { { 1, 2, 3, 4, 5, 6, 7, 8 } };
    NullLogger logger;

    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", "secret" };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", "secret" };

    REQUIRE(alice.service.SendBeacon());

    // Bob sees the beacon and challenges. Alice sees her own and ignores it.
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    CHECK(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);

    // Alice answers the challenge; Bob checks the proof.
    CHECK(alice.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);

    auto const admitted = bob.directory.AuthenticatedPeers();
    REQUIRE(admitted.size() == 1);
    CHECK(admitted.front().nodeId == "alice");
    CHECK(admitted.front().raftEndpoint == "10.0.0.1:7000");

    // And Bob's own directory carries the endpoint, which is the entire point:
    // RaftMembership names a member by id and carries no address, so a node the
    // cluster agrees to admit is unreachable until discovery supplies one.
    CHECK_FALSE(admitted.front().raftEndpoint.empty());
}

TEST_CASE("A node with the wrong key is never admitted", "[cluster][discovery][service]")
{
    // The reason the handshake exists. An admitted node is assigned compile jobs
    // and returns objects cached fleet-wide, so this is the boundary between a
    // shared cache and object injection into everybody's build.
    DatagramBus bus;
    ManualClock clock;
    ScriptedRandomSource random { { 11, 22, 33, 44 } };
    NullLogger logger;

    Node insider { bus, clock, random, logger, "insider", "10.0.0.1:7000", "prod", "secret" };
    Node outsider { bus, clock, random, logger, "outsider", "10.0.0.2:7000", "prod", "not-the-secret" };

    REQUIRE(outsider.service.SendBeacon());

    CHECK(insider.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    CHECK(outsider.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
    CHECK(outsider.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
    CHECK(insider.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);

    // Seen, but never admitted -- the two facts the directory keeps apart.
    CHECK(insider.directory.Size() == 1);
    CHECK(insider.directory.AuthenticatedPeers().empty());
}

TEST_CASE("A proof nobody asked for is refused", "[cluster][discovery][service]")
{
    // A proof is only ever an answer to a challenge this node issued. An
    // unsolicited one carries a nonce nobody here chose, so accepting it would
    // make the nonce -- and therefore the replay protection -- pointless.
    DatagramBus bus;
    ManualClock clock;
    ScriptedRandomSource random { { 5 } };
    NullLogger logger;

    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", "secret" };
    auto intruder = bus.Open(AtEndpoint("10.0.0.9:7000"));

    DiscoveryWire::Challenge const invented { .clusterId = "prod", .nonce = {} };
    auto const tag = DiscoveryWire::ExpectedProofTag(Key("secret"), invented, "ghost", "10.0.0.9:7000");
    REQUIRE(intruder
                ->Send(DiscoveryWire::EncodeProof({ .nodeId = "ghost", .raftEndpoint = "10.0.0.9:7000", .tag = tag }),
                       AtEndpoint("10.0.0.1:7000"))
                .has_value());

    // Even holding the real key, a proof against a self-chosen nonce is refused:
    // alice never challenged "ghost".
    CHECK(alice.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(alice.directory.AuthenticatedPeers().empty());
}

TEST_CASE("A challenge is spent once", "[cluster][discovery][service]")
{
    // A nonce that could answer twice is a nonce that can be replayed: an
    // observer who captured one valid proof could re-send it later and be
    // re-admitted without ever holding the key.
    DatagramBus bus;
    ManualClock clock;
    ScriptedRandomSource random { { 7, 8, 9, 10 } };
    NullLogger logger;

    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", "secret" };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", "secret" };

    REQUIRE(alice.service.SendBeacon());
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);

    // Capture the proof before Bob consumes it, then deliver it twice.
    auto const captured = alice.socket->Receive(1ms);
    REQUIRE(captured.has_value());
    auto const proofDatagram = DiscoveryWire::DecodeChallenge(captured->payload);
    REQUIRE(proofDatagram.has_value());

    auto const tag = DiscoveryWire::ExpectedProofTag(Key("secret"), Unwrap(proofDatagram), "alice", "10.0.0.1:7000");
    auto const proof = DiscoveryWire::EncodeProof({ .nodeId = "alice", .raftEndpoint = "10.0.0.1:7000", .tag = tag });

    REQUIRE(alice.socket->Send(proof, AtEndpoint("10.0.0.2:7000")).has_value());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);
    CHECK(bob.service.PendingChallenges() == 0);

    // The replay finds no outstanding challenge and is refused.
    REQUIRE(alice.socket->Send(proof, AtEndpoint("10.0.0.2:7000")).has_value());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
}

TEST_CASE("Two fleets on one segment ignore each other", "[cluster][discovery][service]")
{
    DatagramBus bus;
    ManualClock clock;
    ScriptedRandomSource random { { 1, 2 } };
    NullLogger logger;

    Node prod { bus, clock, random, logger, "prod-a", "10.0.0.1:7000", "prod", "secret" };
    Node staging { bus, clock, random, logger, "staging-a", "10.0.0.2:7000", "staging", "secret" };

    REQUIRE(prod.service.SendBeacon());

    CHECK(staging.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    CHECK(staging.directory.Size() == 0);

    // Same key, different cluster: routing, not authentication. Sharing a key
    // across fleets is a bad idea, but the cluster id is what keeps them apart
    // even when somebody does it.
    CHECK(staging.service.PendingChallenges() == 0);
}

TEST_CASE("Discovery survives a lost beacon", "[cluster][discovery][service]")
{
    // These are broadcasts and loss is expected. What must not happen is a peer
    // being forgotten because one datagram went missing -- which is why the
    // directory's expiry is generous relative to the beacon interval.
    DatagramBus bus;
    ManualClock clock;
    ScriptedRandomSource random { { 3, 4, 5, 6 } };
    NullLogger logger;

    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", "secret" };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", "secret" };

    REQUIRE(bus.DropNext(AtEndpoint("10.0.0.2:7000"), 1) == 1);

    REQUIRE(alice.service.SendBeacon());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::Nothing); // lost
    Drain(alice);

    // The next beacon gets through and the handshake completes.
    REQUIRE(alice.service.SendBeacon());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    Drain(alice);
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);
    CHECK(bob.directory.AuthenticatedPeers().size() == 1);
}

TEST_CASE("A challenge expires rather than accumulating", "[cluster][discovery][service]")
{
    // A beacon is unauthenticated, so anything on the segment can provoke a
    // challenge. One entry per node and a lifetime is what keeps that from being
    // a memory-exhaustion hole reachable without holding the key.
    DatagramBus bus;
    ManualClock clock;
    ScriptedRandomSource random { { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 } };
    NullLogger logger;

    Node watcher { bus, clock, random, logger, "watcher", "10.0.0.1:7000", "prod", "secret" };
    auto noisy = bus.Open(AtEndpoint("10.0.0.9:7000"));

    auto const beacon =
        DiscoveryWire::EncodeBeacon({ .clusterId = "prod", .nodeId = "noisy", .raftEndpoint = "10.0.0.9:7000" });

    // Repeated beacons from one source replace that source's entry rather than
    // adding to it.
    for (auto attempt = 0; attempt < 5; ++attempt)
    {
        REQUIRE(noisy->Send(beacon, AtEndpoint("10.0.0.1:7000")).has_value());
        REQUIRE(watcher.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    }
    CHECK(watcher.service.PendingChallenges() == 1);

    // And an unanswered challenge does not live forever.
    clock.Advance(31s);
    watcher.service.Maintain();
    CHECK(watcher.service.PendingChallenges() == 0);
}
