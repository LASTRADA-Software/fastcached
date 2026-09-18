// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/InMemoryDatagram.hpp>
#include <FastCache/Net/SharedPortDatagram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/CoHostedDatagram.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::CoHostedDatagramSocket;
using FastCache::Testing::RosterPeerKeys;
using FastCache::Testing::ScriptedSecureRandom;
using FastCache::Testing::SharedRoster;
using FastCache::Testing::TestBeaconPort;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace
{

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
    /// Over a socket and a beacon address somebody else chose.
    ///
    /// The door the co-hosted cases take, because what they vary is precisely
    /// the socket: two nodes on one machine listen on the SAME beacon address
    /// and must still be answerable apart.
    /// @param ownSocket Where this node's datagrams come from and go.
    /// @param beaconAddress Where it announces itself.
    /// @param clock Time source.
    /// @param random Where nonces come from.
    /// @param logger Where joins and rejections are reported.
    /// @param id This node's identity.
    /// @param endpoint Where it answers Raft peer traffic.
    /// @param cluster Which fleet it belongs to.
    /// @param roster Whose key every id is, as this node's replicated state says.
    /// @param own The key this node signs its proofs with.
    Node(std::unique_ptr<IDatagramSocket> ownSocket,
         DatagramAddress beaconAddress,
         IClock& clock,
         ISecureRandom& random,
         ILogger& logger,
         std::string id,
         std::string endpoint,
         std::string cluster,
         std::shared_ptr<SharedRoster const> roster,
         Ed25519KeyPair own):
        socket { std::move(ownSocket) },
        directory { clock, cluster, id },
        keys { std::move(own), std::move(roster) },
        service { *socket,
                  clock,
                  random,
                  directory,
                  DiscoveryConfig { .clusterId = std::move(cluster),
                                    .nodeId = std::move(id),
                                    .raftEndpoint = std::move(endpoint),
                                    .beaconAddress = std::move(beaconAddress) },
                  keys,
                  metrics,
                  logger }
    {
    }

    /// One node per address, which is what most of these cases want.
    /// @param bus The segment.
    /// @param clock Time source.
    /// @param random Where nonces come from.
    /// @param logger Where joins and rejections are reported.
    /// @param id This node's identity.
    /// @param endpoint Where it answers Raft peer traffic, and where it listens.
    /// @param cluster Which fleet it belongs to.
    /// @param roster Whose key every id is, as this node's replicated state says.
    /// @param own The key this node signs its proofs with; by default the one `roster`'s
    ///        `SharedRoster::Of` records for @p id.
    Node(DatagramBus& bus,
         IClock& clock,
         ISecureRandom& random,
         ILogger& logger,
         std::string const& id,
         std::string const& endpoint,
         std::string const& cluster,
         std::shared_ptr<SharedRoster const> roster,
         std::optional<Ed25519KeyPair> own = std::nullopt):
        // By reference here and by value one frame down, deliberately.
        // `AtEndpoint(endpoint)` is evaluated in the same argument list as the
        // forwarded `endpoint` and the order of the two is unspecified, so there
        // is nothing here that could be moved from anyway; the copies happen
        // where they can be moved out of.
        Node(bus.Open(AtEndpoint(endpoint)),
             DatagramBus::BroadcastAddress(),
             clock,
             random,
             logger,
             id,
             endpoint,
             cluster,
             std::move(roster),
             own.has_value() ? *std::move(own) : TestKeyPair(id))
    {
    }

    std::unique_ptr<IDatagramSocket> socket;
    PeerDirectory directory;
    AtomicMetricsSink metrics;
    RosterPeerKeys keys;
    DiscoveryService service;
};

/// Beacon, challenge, proof, and what @p listener made of the proof.
///
/// The three legs in the order they happen, each asserted, so a case that fails names the
/// leg that went wrong rather than the verdict at the end.
/// @param listener Who challenges.
/// @param peer Who announces and answers.
/// @return The listener's event for the proof.
DiscoveryEvent Handshake(Node& listener, Node& peer)
{
    REQUIRE(peer.service.SendBeacon());
    REQUIRE(listener.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(peer.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
    REQUIRE(peer.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
    return listener.service.PumpOnce(1ms);
}

/// How many Warn lines @p logger holds that contain @p needle.
/// @param logger What captured them.
/// @param needle What to look for.
/// @return The count.
[[nodiscard]] std::ptrdiff_t Warned(CapturingLogger const& logger, std::string_view needle)
{
    return std::ranges::count_if(logger.Snapshot(), [needle](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Warn && record.message.contains(needle);
    });
}

/// Nonces for a scripted generator: one per value in @p fills, each `NonceBytes` of that
/// value, served in turn and cycling. So `{ 1, 2 }` is two distinct nonces and then the first
/// again -- how many a case can draw before one repeats is the number of values it names.
/// @param fills One byte value per nonce.
/// @return The script.
[[nodiscard]] std::vector<std::byte> NonceScript(std::initializer_list<std::uint8_t> fills)
{
    std::vector<std::byte> script;
    for (auto const fill: fills)
        script.insert(script.end(), NonceBytes, static_cast<std::byte>(fill));
    return script;
}

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

TEST_CASE("Two nodes on one host share a beacon port and still prove their keys", "[cluster][discovery][service]")
{
    // The configuration the whole shared-port shape exists for, and the one that
    // used to fail in silence. A beacon is a broadcast, so every node on a
    // segment binds the SAME port -- including two nodes that happen to be on one
    // machine. Sharing that port is what lets both hear a beacon; it is also what
    // makes a unicast to it arrive at only one of them, and the challenge and the
    // proof are both unicast.
    //
    // Before this, a node answered from the port it shared, so the address a peer
    // replied to named the MACHINE. Every challenge and every proof went to
    // whichever co-hosted node the kernel picked, each one arriving somewhere
    // that had not asked for it, and the pair sat there seen-but-unproved
    // forever with nothing logged.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    NullLogger logger;

    auto const beacon = DatagramBus::BroadcastAddressOn(TestBeaconPort);
    auto const roster = SharedRoster::Of({ "first", "second" });

    Node first { CoHostedDatagramSocket(bus, "10.0.0.1", 40001),
                 beacon,
                 clock,
                 random,
                 logger,
                 "first",
                 "10.0.0.1:7000",
                 "prod",
                 roster,
                 TestKeyPair("first") };
    Node second { CoHostedDatagramSocket(bus, "10.0.0.1", 40002),
                  beacon,
                  clock,
                  random,
                  logger,
                  "second",
                  "10.0.0.1:7001",
                  "prod",
                  roster,
                  TestKeyPair("second") };

    REQUIRE(first.service.SendBeacon());
    REQUIRE(second.service.SendBeacon());

    // Settled by draining both sides in turn rather than by a scripted sequence
    // of steps: a challenge one side issues is an answer the other has to see, so
    // each needs a turn after the other has had one. Four rounds is well over the
    // three legs a handshake takes.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        Drain(first);
        Drain(second);
    }

    auto const atFirst = first.directory.AuthenticatedPeers();
    REQUIRE(atFirst.size() == 1);
    CHECK(atFirst.front().nodeId == "second");
    CHECK(atFirst.front().raftEndpoint == "10.0.0.1:7001");

    auto const atSecond = second.directory.AuthenticatedPeers();
    REQUIRE(atSecond.size() == 1);
    CHECK(atSecond.front().nodeId == "first");
    CHECK(atSecond.front().raftEndpoint == "10.0.0.1:7000");
}

TEST_CASE("A peer that cannot name itself is never challenged", "[cluster][discovery][service]")
{
    // #159, one layer up from `PeerDirectory`. The refusal is worth asserting from
    // here as well as there, because two things follow from it that the directory
    // alone cannot show: no challenge is spent on such a peer -- the nonce and the
    // `_pending` entry are both work an unauthenticated broadcast should not be
    // able to provoke -- and the datagram is REPORTED, which its two siblings
    // (another fleet's beacon, and this node's own) deliberately are not.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "listener" });
    Node listener { bus, clock, random, logger, "listener", "10.0.0.1:7000", "prod", roster };

    // A lone surrogate: valid in shape, refused by every strict decoder, and the
    // sequence a lenient encoder is most likely to emit by accident.
    Node rogue { bus, clock, random, logger, "rogue-\xED\xA0\x80", "10.0.0.2:7000", "prod", roster };

    REQUIRE(rogue.service.SendBeacon());
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);

    CHECK(listener.directory.Size() == 0);

    // Exactly one datagram is waiting for the rogue -- its own beacon, which a
    // broadcast doubles back to its sender -- and a challenge would make it two.
    // That is the half the directory cannot show: a challenge is a nonce and a
    // `_pending` entry, and an unauthenticated broadcast must not provoke either.
    CHECK(Drain(rogue) == 1);

    // Reported by the address it came from, which is the only part of such a
    // beacon that can be printed -- and the part that says which machine to look
    // at.
    auto const reported = [&logger] {
        return std::ranges::count_if(logger.Snapshot(), [](CapturingLogger::Record const& record) {
            return record.level == LogLevel::Warn && record.message.contains("10.0.0.2:7000")
                   && record.message.contains("cannot record");
        });
    };
    CHECK(reported() == 1);

    // And once, however many arrive. This is the half a per-datagram log line gets
    // wrong: one unauthenticated datagram provokes it, so anything on the segment
    // can drive it at line rate without ever holding a key the cluster knows, which is
    // a disk-exhaustion hole reached from outside the fleet.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
    {
        REQUIRE(rogue.service.SendBeacon());
        CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    }
    CHECK(reported() == 1);

    // Until the interval has passed. The fault is a STANDING one -- a peer whose
    // identity is not text stays that way -- so an operator who starts reading the
    // log an hour later must still find it.
    clock.Advance(DiscoveryService::UnnameableReportInterval);
    REQUIRE(rogue.service.SendBeacon());
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    CHECK(reported() == 2);
}

TEST_CASE("A proof is refused before this node logs what it claimed", "[cluster][discovery][service]")
{
    // The mismatch line names the endpoint a PROOF claimed, and a proof is
    // unauthenticated until its signature checks out -- so without this, anything on the
    // segment could write arbitrary bytes into a node's log by sending one beacon
    // and then one lie, holding no key at all.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "listener", "peer" });
    Node listener { bus, clock, random, logger, "listener", "10.0.0.1:7000", "prod", roster };
    Node peer { bus, clock, random, logger, "peer", "10.0.0.2:7000", "prod", roster };

    // Announced properly, so it is recorded and challenged -- which is what makes a
    // proof carrying its id something this node looks at rather than discards.
    REQUIRE(peer.service.SendBeacon());
    REQUIRE(listener.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);

    // And then answers for an endpoint that is not text. The signature is not even filled
    // in: this is refused long before anything is verified.
    REQUIRE(peer.socket
                ->Send(DiscoveryWire::EncodeProof(DiscoveryWire::Proof {
                           .nodeId = "peer", .raftEndpoint = "10.0.0.2:7000\xFF", .publicKey = {}, .signature = {} }),
                       AtEndpoint("10.0.0.1:7000"))
                .has_value());
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);

    CHECK(std::ranges::none_of(logger.Snapshot(),
                               [](CapturingLogger::Record const& record) { return record.message.contains('\xFF'); }));
    CHECK(listener.directory.AuthenticatedPeers().empty());
}

TEST_CASE("A beacon this node cannot draw a challenge for is recorded, not challenged, and said once",
          "[cluster][discovery][service]")
{
    // Withheld rather than issued with a weak nonce (#1527): no challenge goes out, nothing is
    // left pending that a proof could answer, and the Error names this host's generator --
    // once per interval, because a beacon is unauthenticated and anything on the segment can
    // provoke the line.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom denied { ScriptedSecureRandom::DeniedFailure() };
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "listener", "peer" });
    Node listener { bus, clock, denied, logger, "listener", "10.0.0.1:7000", "prod", roster };
    Node peer { bus, clock, random, logger, "peer", "10.0.0.2:7000", "prod", roster };

    REQUIRE(peer.service.SendBeacon());
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeWithheld);
    CHECK(denied.FillCount() == 1);
    CHECK(listener.service.PendingChallenges() == 0);

    // Nothing reached the peer: there was no challenge to answer.
    CHECK(peer.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    CHECK(peer.service.PumpOnce(1ms) == DiscoveryEvent::Nothing);

    // A second beacon inside the interval is withheld too, and not said again.
    REQUIRE(peer.service.SendBeacon());
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeWithheld);

    auto const said = std::ranges::count_if(logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains("cannot draw a nonce")
               && record.message.contains(ScriptedSecureRandom::DeniedFailure().primitive);
    });
    CHECK(said == 1);
    CHECK(listener.directory.AuthenticatedPeers().empty());
}

TEST_CASE("Two nodes discover each other and prove the keys the roster holds for them", "[cluster][discovery][service]")
{
    // The whole feature, end to end, in one process: beacon, challenge, proof,
    // authenticated. What makes this a unit test rather than a fixture is that the
    // segment, the clock and the nonces are all injected.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

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

    // And the key it PROVED, which is what a desire built from this entry carries.
    CHECK(admitted.front().provenKey == TestKeyPair("alice").PublicKey());
}

TEST_CASE("A proof under a key the roster does not hold is reported and never authenticated",
          "[cluster][discovery][service][security]")
{
    // **The acceptance case for #178's discovery half: LAN auto-admission ends.** Under the
    // shared key a proof WAS membership. Now a proof names a key, and an outsider signs
    // perfectly well with one of its own -- so this is a machine that proves POSSESSION and
    // is still not authenticated, because the roster does not hold that key for its id.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 11 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "insider" });
    Node insider { bus, clock, random, logger, "insider", "10.0.0.1:7000", "prod", roster };
    Node outsider { bus, clock, random, logger, "outsider", "10.0.0.2:7000", "prod", roster };

    CHECK(Handshake(insider, outsider) == DiscoveryEvent::PeerUnknownKey);

    // Seen, but never authenticated -- the two facts the directory keeps apart, and the
    // second is what `DiscoveryTier` builds a desire from.
    CHECK(insider.directory.Size() == 1);
    CHECK(insider.directory.AuthenticatedPeers().empty());

    // Counted by name, and apart from a forgery and from a revoked key: three diagnoses.
    CHECK(insider.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) == 1);
    CHECK(insider.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedRevokedKey) == 0);
    CHECK(insider.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 0);

    // Reported with the key WHOLE and the command that would admit it: the key verified, so
    // only its holder could have signed this, and naming it is no oracle.
    auto const key = FormatEd25519PublicKey(TestKeyPair("outsider").PublicKey());
    CHECK(Warned(logger, std::format("--cluster-admit=outsider=10.0.0.2:7000@{}", key)) == 1);
}

TEST_CASE("A KNOWN id proving another key is reported, not authenticated", "[cluster][discovery][service][security]")
{
    // The impostor's shape, which the case above cannot show: the id is one the roster
    // knows, the endpoint is plausible, and the key is the impostor's own. An id is a label;
    // the key the roster holds for it is the credential.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 11 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "insider", "worker-a" });
    Node insider { bus, clock, random, logger, "insider", "10.0.0.1:7000", "prod", roster };
    Node impostor { bus, clock, random, logger, "worker-a", "10.0.0.2:7000", "prod", roster, TestKeyPair("impostor") };

    CHECK(Handshake(insider, impostor) == DiscoveryEvent::PeerUnknownKey);
    CHECK(insider.directory.AuthenticatedPeers().empty());

    // The control, through the same fixture: the genuine holder of that id's key IS
    // authenticated. Without it the refusal above passes under a service refusing everybody.
    Node genuine { bus, clock, random, logger, "worker-a", "10.0.0.3:7000", "prod", roster };
    CHECK(Handshake(insider, genuine) == DiscoveryEvent::PeerAuthenticated);
}

TEST_CASE("A proof under a REVOKED key is recognised as one", "[cluster][discovery][service][security]")
{
    // Revoked is not unknown: the remedies are opposite -- admit it if it belongs, against
    // never admit it again -- so the event, the counter and the log each say which.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 11 }) };
    CapturingLogger logger;

    auto roster = SharedRoster::Of({ "insider", "gone" });
    Node insider { bus, clock, random, logger, "insider", "10.0.0.1:7000", "prod", roster };
    Node gone { bus, clock, random, logger, "gone", "10.0.0.2:7000", "prod", roster };
    REQUIRE(Handshake(insider, gone) == DiscoveryEvent::PeerAuthenticated);

    roster->Revoke("gone");
    CHECK(Handshake(insider, gone) == DiscoveryEvent::PeerRevokedKey);
    CHECK(insider.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedRevokedKey) == 1);
    CHECK(insider.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) == 0);
    CHECK(Warned(logger, "REVOKED") == 1);
    CHECK(Warned(logger, "--cluster-admit") == 0);
}

TEST_CASE("A proof whose signature does not verify is counted as forged and names nothing it claimed",
          "[cluster][discovery][service][security]")
{
    // A signature that fails under the key the proof CARRIES is the one case where the key
    // is as much a claim as the id: anybody could have typed it. So it is counted, and
    // logged by the address it came from and nothing else.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 7 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon());
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    auto const captured = alice.socket->Receive(1ms);
    REQUIRE(captured.has_value());
    auto const challenge = DiscoveryWire::DecodeChallenge(captured->payload);
    REQUIRE(challenge.has_value());

    // Alice's KEY, and a signature made by somebody else.
    auto const pair = TestKeyPair("alice");
    auto proof = DiscoveryWire::Proof {
        .nodeId = "alice", .raftEndpoint = "10.0.0.1:7000", .publicKey = pair.PublicKey(), .signature = {}
    };
    proof.signature = TestKeyPair("mallory").Sign(
        DiscoveryWire::ProofMessage(Unwrap(challenge), proof.nodeId, proof.raftEndpoint, proof.publicKey));
    REQUIRE(alice.socket->Send(DiscoveryWire::EncodeProof(proof), AtEndpoint("10.0.0.2:7000")).has_value());

    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(bob.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 1);
    CHECK(bob.directory.AuthenticatedPeers().empty());
    CHECK(Warned(logger, "not signed by the key it carries") == 1);
    CHECK(Warned(logger, FormatEd25519PublicKey(pair.PublicKey())) == 0);

    // The nonce was spent by the forgery: the real proof arriving after it finds nothing
    // outstanding, which is what keeps a forger from racing the honest answer for free.
    CHECK(bob.service.PendingChallenges() == 0);
}

TEST_CASE("Proofs under keys the roster does not accept are all counted and reported at most once a minute",
          "[cluster][discovery][service]")
{
    // A fresh key costs nothing to make, so anything on the segment can send one proof per
    // beacon. The counter takes every one; the log takes the latest per interval and says how
    // many it stands for, or the line is a disk-exhaustion hole reached from outside the fleet.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2, 3 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "insider" });
    Node insider { bus, clock, random, logger, "insider", "10.0.0.1:7000", "prod", roster };
    Node outsider { bus, clock, random, logger, "outsider", "10.0.0.2:7000", "prod", roster };

    CHECK(Handshake(insider, outsider) == DiscoveryEvent::PeerUnknownKey);
    CHECK(Handshake(insider, outsider) == DiscoveryEvent::PeerUnknownKey);
    CHECK(Handshake(insider, outsider) == DiscoveryEvent::PeerUnknownKey);
    CHECK(insider.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) == 3);
    CHECK(Warned(logger, "outsider at 10.0.0.2:7000") == 1);

    clock.Advance(DiscoveryService::UnacceptedKeyReportInterval);
    CHECK(Handshake(insider, outsider) == DiscoveryEvent::PeerUnknownKey);
    CHECK(Warned(logger, "outsider at 10.0.0.2:7000") == 2);
    CHECK(Warned(logger, "3 such proof(s) since the last report") == 1);
}

TEST_CASE("A proof nobody asked for is refused", "[cluster][discovery][service]")
{
    // A proof is only ever an answer to a challenge this node issued. An
    // unsolicited one carries a nonce nobody here chose, so accepting it would
    // make the nonce -- and therefore the replay protection -- pointless.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 5 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "ghost" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    auto intruder = bus.Open(AtEndpoint("10.0.0.9:7000"));

    DiscoveryWire::Challenge const invented { .clusterId = "prod", .nonce = {} };
    auto const ghost = TestKeyPair("ghost");
    auto const signature = ghost.Sign(DiscoveryWire::ProofMessage(invented, "ghost", "10.0.0.9:7000", ghost.PublicKey()));
    REQUIRE(intruder
                ->Send(DiscoveryWire::EncodeProof({ .nodeId = "ghost",
                                                    .raftEndpoint = "10.0.0.9:7000",
                                                    .publicKey = ghost.PublicKey(),
                                                    .signature = signature }),
                       AtEndpoint("10.0.0.1:7000"))
                .has_value());

    // Even holding a key the roster knows, a proof against a self-chosen nonce is refused:
    // alice never challenged "ghost".
    CHECK(alice.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(alice.directory.AuthenticatedPeers().empty());
}

TEST_CASE("A challenge is spent once", "[cluster][discovery][service]")
{
    // A nonce that could answer twice is a nonce that can be replayed: an
    // observer who captured one valid proof could re-send it later and be
    // re-authenticated without ever holding the key.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 7 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon());
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);

    // Capture the proof before Bob consumes it, then deliver it twice.
    auto const captured = alice.socket->Receive(1ms);
    REQUIRE(captured.has_value());
    auto const proofDatagram = DiscoveryWire::DecodeChallenge(captured->payload);
    REQUIRE(proofDatagram.has_value());

    auto const pair = TestKeyPair("alice");
    auto const signature =
        pair.Sign(DiscoveryWire::ProofMessage(Unwrap(proofDatagram), "alice", "10.0.0.1:7000", pair.PublicKey()));
    auto const proof = DiscoveryWire::EncodeProof(
        { .nodeId = "alice", .raftEndpoint = "10.0.0.1:7000", .publicKey = pair.PublicKey(), .signature = signature });

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
    ScriptedSecureRandom random { NonceScript({ 1 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "prod-a", "staging-a" });
    Node prod { bus, clock, random, logger, "prod-a", "10.0.0.1:7000", "prod", roster };
    Node staging { bus, clock, random, logger, "staging-a", "10.0.0.2:7000", "staging", roster };

    REQUIRE(prod.service.SendBeacon());

    CHECK(staging.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    CHECK(staging.directory.Size() == 0);

    // Same roster, different cluster: routing, not authentication. The cluster id is what
    // keeps two fleets apart even when one roster knows both.
    CHECK(staging.service.PendingChallenges() == 0);
}

TEST_CASE("Discovery survives a lost beacon", "[cluster][discovery][service]")
{
    // These are broadcasts and loss is expected. What must not happen is a peer
    // being forgotten because one datagram went missing -- which is why the
    // directory's expiry is generous relative to the beacon interval.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 3 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

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
    // a memory-exhaustion hole reachable without holding any key.
    DatagramBus bus;
    ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2, 3 }) };
    NullLogger logger;

    Node watcher { bus, clock, random, logger, "watcher", "10.0.0.1:7000", "prod", SharedRoster::Of({ "watcher" }) };
    auto noisy = bus.Open(AtEndpoint("10.0.0.9:7000"));

    auto const beacon =
        DiscoveryWire::EncodeBeacon({ .clusterId = "prod", .nodeId = "noisy", .raftEndpoint = "10.0.0.9:7000" });

    // Repeated beacons from one source replace that source's entry rather than
    // adding to it.
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 5))
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
