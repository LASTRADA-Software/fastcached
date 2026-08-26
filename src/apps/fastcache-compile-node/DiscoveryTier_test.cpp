// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryTier.hpp"

#include <FastCache/Net/InMemoryDatagram.hpp>
#include <FastCache/Net/SharedPortDatagram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;

namespace
{
/// A key every node in a test cluster shares.
[[nodiscard]] std::vector<std::byte> TestKey()
{
    // Constructed rather than typed as a hex run, for the reason the RFC 4231
    // vectors next door are: a hand-written literal of this length is one nobody
    // recounts, and a miscounted key accuses the code that reads it.
    auto bytes = std::views::iota(0, 32) | std::views::transform([](int value) { return static_cast<std::byte>(value); });
    return std::vector<std::byte> { bytes.begin(), bytes.end() };
}

/// The port every node on a segment listens for beacons on.
constexpr std::uint16_t BeaconPort = 6677;

/// What one node announces about itself.
/// @param nodeId Its identity.
/// @param key The cluster key it holds.
/// @param beaconAddress Where this node announces itself.
/// @return The configuration.
[[nodiscard]] Cluster::DiscoveryConfig ConfigFor(std::string const& nodeId,
                                                 std::vector<std::byte> key,
                                                 DatagramAddress beaconAddress)
{
    return Cluster::DiscoveryConfig { .clusterId = "fleet",
                                      .nodeId = nodeId,
                                      .raftEndpoint = nodeId + ".local:6675",
                                      .beaconAddress = std::move(beaconAddress),
                                      .presharedKey = std::move(key),
                                      .beaconInterval = 15s,
                                      .challengeLifetime = 30s };
}

/// One node's tier, plus what its observer was told.
///
/// Named `Peer` rather than `Node`, which is the namespace this whole file is
/// `using` -- a struct by that name makes every later mention of it ambiguous.
struct Peer
{
    NullLogger logger;
    std::vector<Cluster::DesiredMember> seen;
    std::unique_ptr<DiscoveryTier> tier;

    /// One node per machine, which is the ordinary deployment.
    /// @param bus The segment.
    /// @param nodeId This node's identity.
    /// @param key The cluster key it holds.
    Peer(DatagramBus& bus, std::string const& nodeId, std::vector<std::byte> key):
        Peer(bus.Open(DatagramAddress { .host = nodeId, .port = BeaconPort }),
             DatagramBus::BroadcastAddress(),
             nodeId,
             std::move(key))
    {
    }

    /// Over a socket and a beacon address somebody else chose.
    /// @param socket Where this node's datagrams come from and go.
    /// @param beaconAddress Where it announces itself.
    /// @param nodeId This node's identity.
    /// @param key The cluster key it holds.
    Peer(std::unique_ptr<IDatagramSocket> socket,
         DatagramAddress beaconAddress,
         std::string const& nodeId,
         std::vector<std::byte> key):
        tier { DiscoveryTier::Over(
            std::move(socket),
            ConfigFor(nodeId, std::move(key), std::move(beaconAddress)),
            [this](std::span<Cluster::DesiredMember const> peers) { seen.assign(peers.begin(), peers.end()); },
            logger) }
    {
    }
};

/// A node sharing a host, and therefore a beacon port, with another.
///
/// Listens where every node on the segment does and answers from an address of
/// its own -- what `DiscoveryTier::Start` builds over real sockets, and the whole
/// of issue #126. See `Net/SharedPortDatagram`.
/// @param bus The segment.
/// @param host The machine; co-hosted nodes pass the same one.
/// @param ownPort The port only this node holds.
/// @return The pair, as one socket.
[[nodiscard]] std::unique_ptr<IDatagramSocket> CoHostedSocket(DatagramBus& bus,
                                                              std::string const& host,
                                                              std::uint16_t ownPort)
{
    return AnswerFromOwnAddress(bus.Open(DatagramAddress { .host = host, .port = BeaconPort }),
                                bus.Open(DatagramAddress { .host = host, .port = ownPort }));
}

/// A step short enough that a whole handshake costs milliseconds.
///
/// A timeout rather than a sleep: the bus delivers into an inbox synchronously, so
/// every assertion below is decided by the ORDER of the steps and not by how long
/// any of them waits. What the timeout bounds is only the last step of each side,
/// where there is nothing left to read.
constexpr auto Step = 5ms;
} // namespace

TEST_CASE("Two nodes holding one key find and prove each other", "[node][discovery]")
{
    // The whole handshake, driven by hand: no threads, no sleeps, and a failure
    // names the step it happened at rather than timing out.
    DatagramBus bus;
    Peer first { bus, "n1", TestKey() };
    Peer second { bus, "n2", TestKey() };

    // Each announces itself; the bus doubles a broadcast back to its sender, exactly
    // as a real one does, which is the case `PeerDirectory` must ignore.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    // Each reads the other's beacon and challenges it.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    // Each answers the challenge it was given.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    // And each checks the proof it was sent.
    CHECK(first.tier->Step(Step));
    CHECK(second.tier->Step(Step));

    REQUIRE(first.tier->AuthenticatedCount() == 1);
    REQUIRE(second.tier->AuthenticatedCount() == 1);

    // What reaches the observer is a DESIRE with no opinion about the scheduler
    // endpoint. Discovery proved where `n2` answers CONSENSUS -- that is what the MAC
    // covered -- and knows nothing about the port clients speak to, so saying `""`
    // would clear whatever `n2` had announced about itself.
    REQUIRE(first.seen.size() == 1);
    CHECK(first.seen.front().id == "n2");
    CHECK(first.seen.front().raftEndpoint == "n2.local:6675");
    CHECK_FALSE(first.seen.front().schedulerEndpoint.has_value());
}

TEST_CASE("A node holding the wrong key is never admitted", "[node][discovery]")
{
    // The refusal that matters, because an admitted node is assigned compile jobs and
    // returns objects cached fleet-wide. It still SEES the beacon -- a beacon is
    // unauthenticated by construction -- and is simply never marked as having proved
    // anything.
    DatagramBus bus;
    Peer honest { bus, "n1", TestKey() };

    auto wrong = TestKey();
    wrong.front() = std::byte { 0xFF };
    Peer stranger { bus, "n2", std::move(wrong) };

    for (auto round = 0; round < 6; ++round)
    {
        CHECK(honest.tier->Step(Step));
        CHECK(stranger.tier->Step(Step));
    }

    CHECK(honest.tier->AuthenticatedCount() == 0);
    CHECK(stranger.tier->AuthenticatedCount() == 0);
    CHECK(honest.seen.empty());
}

TEST_CASE("Two nodes on one host find and prove each other", "[node][discovery]")
{
    // Issue #126, at the tier that owns the sockets. Both nodes listen on the one
    // beacon port -- they have to, a beacon being a broadcast -- so both hear each
    // other. What used to fail is everything after that: a unicast to a shared
    // port reaches only one of the sockets on it, and the challenge and the proof
    // are both unicast to wherever the last datagram came from, so each landed on
    // whichever co-hosted node the kernel picked. Nothing logged; the pair simply
    // stayed seen-and-unproved.
    //
    // Driven step by step rather than by a settle loop, because each step here is
    // one leg of the handshake and a failure should name the leg.
    DatagramBus bus;
    auto const beacon = DatagramBus::BroadcastAddressOn(BeaconPort);

    Peer first { CoHostedSocket(bus, "host", 40001), beacon, "n1", TestKey() };
    Peer second { CoHostedSocket(bus, "host", 40002), beacon, "n2", TestKey() };

    // Each announces itself, then reads the other's beacon and challenges it, then
    // answers the challenge it was given, then checks the proof it was sent. The
    // extra rounds are slack: a shared-port socket looks at one of its halves and
    // waits on the other, so a datagram can need one more step to be reached.
    for (auto round = 0; round < 8; ++round)
    {
        CHECK(first.tier->Step(Step));
        CHECK(second.tier->Step(Step));
    }

    REQUIRE(first.tier->AuthenticatedCount() == 1);
    REQUIRE(second.tier->AuthenticatedCount() == 1);

    // And each learned where the OTHER answers Raft, not where it does -- the
    // failure a co-hosted pair would show if the two ever crossed.
    REQUIRE(first.seen.size() == 1);
    CHECK(first.seen.front().id == "n2");
    CHECK(first.seen.front().raftEndpoint == "n2.local:6675");

    REQUIRE(second.seen.size() == 1);
    CHECK(second.seen.front().id == "n1");
    CHECK(second.seen.front().raftEndpoint == "n1.local:6675");
}

TEST_CASE("Two fleets on one segment ignore each other", "[node][discovery]")
{
    // A cluster id is routing rather than authentication, and this is what it buys:
    // the challenge is never even issued, so a shared key would not help. Checked
    // before a challenge goes out AND before one is answered.
    DatagramBus bus;
    Peer ours { bus, "n1", TestKey() };

    NullLogger otherLogger;
    auto otherConfig = ConfigFor("n2", TestKey(), DatagramBus::BroadcastAddress());
    otherConfig.clusterId = "somebody-elses";
    auto const theirs = DiscoveryTier::Over(
        bus.Open(DatagramAddress { .host = "n2", .port = BeaconPort }), std::move(otherConfig), {}, otherLogger);

    for (auto round = 0; round < 6; ++round)
    {
        CHECK(ours.tier->Step(Step));
        CHECK(theirs->Step(Step));
    }

    CHECK(ours.tier->AuthenticatedCount() == 0);
    CHECK(theirs->AuthenticatedCount() == 0);
}

TEST_CASE("A key file is read, trimmed, and refused when it is too short", "[node][discovery]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcache-discovery-key-test" };
    auto const& dir = scratch.Path();

    SECTION("a generated key, with the newline an editor left on it")
    {
        // The overwhelmingly common way to produce one of these ends the file with a
        // newline, and a key one byte different from its peers' fails to authenticate
        // with a message about a bad proof rather than about a newline.
        auto const path = dir / "good";
        {
            std::ofstream out { path, std::ios::binary };
            out << "0123456789abcdefghij\n";
        }

        auto const key = ReadClusterKey(path);
        REQUIRE(key.has_value());
        CHECK(key->size() == 20);
    }

    SECTION("a key short enough to guess is refused, and the message says how to make one")
    {
        auto const path = dir / "short";
        {
            std::ofstream out { path, std::ios::binary };
            out << "hunter2\n";
        }

        auto const key = ReadClusterKey(path);
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().contains("urandom"));
    }

    SECTION("a file that is not there is a refusal rather than an empty key")
    {
        // An empty key would authenticate every node on the segment against every
        // other, which is the one failure this whole layer exists to prevent.
        CHECK_FALSE(ReadClusterKey(dir / "absent").has_value());
    }

    std::filesystem::remove_all(dir);
}
