// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/net/SharedPortDatagram.hpp>
#include <core/net/testing/InMemoryDatagram.hpp>
#include <core/platform/Clock.hpp>
#include <tests/CoHostedDatagram.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/RefusingDatagram.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using FastCache::Testing::CoHostedDatagramSocket;
using FastCache::Testing::RecordingFleets;
using FastCache::Testing::RefusingDatagramSocket;
using FastCache::Testing::RosterPeerKeys;
using FastCache::Testing::ScriptedSecureRandom;
using FastCache::Testing::ScriptedSummarySource;
using FastCache::Testing::SharedRoster;
using FastCache::Testing::TestBeaconPort;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace
{

/// The bus address a `host:port` endpoint names -- see `core::net::DatagramAddress` for why
/// the two halves travel apart below this layer.
///
/// Named for what it takes, because `InMemoryDatagram_test`'s `AtHost` takes a
/// bare host and supplies a port: both end up in one test binary, and two
/// same-named helpers whose contracts differ by an invisible `:7000` is how a
/// case comes to address nowhere at all.
/// @param endpoint `host:port` text.
/// @return The two halves apart.
[[nodiscard]] core::net::DatagramAddress AtEndpoint(std::string_view endpoint)
{
    // Asserted rather than defaulted. An endpoint this cannot split would
    // otherwise become `{"", 0}`, which is a perfectly valid bus address that
    // simply matches no inbox -- so every datagram aimed at it would be
    // discarded exactly as UDP discards one addressed to nobody, and the case
    // would fail as "the peer never answered" instead of "the test said the
    // wrong thing".
    auto const parsed = ParseEndpoint(endpoint, "");
    REQUIRE(parsed.has_value());
    return core::net::DatagramAddress { .host = Unwrap(parsed).first, .port = Unwrap(parsed).second };
}

/// What a solitary node in @p cluster says about itself, as a service's own beacon and proof say it.
/// @param cluster Which fleet it is in.
/// @param nodeId Who is speaking.
/// @param endpoint Where it answers Raft.
/// @return The summary.
[[nodiscard]] CompileCacheWire::FleetSummary Claiming(std::string_view cluster,
                                                      std::string_view nodeId,
                                                      std::string_view endpoint)
{
    return CompileCacheWire::FleetSummary { .clusterId = std::string { cluster },
                                            .state = CompileCacheWire::FleetState::Solitary,
                                            .createdAtUnixSeconds = 0,
                                            .leaderId = {},
                                            .leaderNodeEndpoint = {},
                                            .nodeId = std::string { nodeId },
                                            .raftEndpoint = std::string { endpoint } };
}

/// One node on the segment: its socket, what it says about itself, its directory and service.
///
/// Bundled because a discovery node is exactly these over one clock, and a test that wired
/// them by hand at every case would spend more lines on setup than on the property being
/// asserted.
struct Node
{
    /// Over a socket, beacon destinations and a key somebody else chose: every other constructor
    /// ends here.
    /// @param ownSocket Where this node's datagrams come from and go.
    /// @param destinations Where it announces itself.
    /// @param clock Time source.
    /// @param random Where nonces come from.
    /// @param logger Where joins and rejections are reported.
    /// @param summary What this node says about itself.
    /// @param ownKeys Its key, and the roster that says whose key every other id is.
    Node(std::unique_ptr<core::net::IDatagramSocket> ownSocket,
         std::shared_ptr<IBeaconDestinations const> destinations,
         core::platform::IClock& clock,
         ISecureRandom& random,
         ILogger& logger,
         FleetSummary summary,
         std::unique_ptr<Consensus::IRaftPeerKeys const> ownKeys):
        socket { std::move(ownSocket) },
        self { std::move(summary) },
        keys { std::move(ownKeys) },
        directory { clock, self, *keys },
        service { *socket, clock,  random, directory, DiscoveryConfig { .beaconDestinations = std::move(destinations) },
                  self,    fleets, *keys,  metrics,   logger }
    {
    }

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
    Node(std::unique_ptr<core::net::IDatagramSocket> ownSocket,
         core::net::DatagramAddress beaconAddress,
         core::platform::IClock& clock,
         ISecureRandom& random,
         ILogger& logger,
         std::string const& id,
         std::string const& endpoint,
         std::string const& cluster,
         std::shared_ptr<SharedRoster const> roster,
         Ed25519KeyPair own):
        Node(std::move(ownSocket),
             std::make_shared<FixedBeaconDestination const>(std::move(beaconAddress)),
             clock,
             random,
             logger,
             Claiming(cluster, id, endpoint),
             std::make_unique<RosterPeerKeys>(std::move(own), std::move(roster)))
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
    Node(core::net::testing::DatagramBus& bus,
         core::platform::IClock& clock,
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
        Node(bus.open(AtEndpoint(endpoint)),
             core::net::testing::DatagramBus::broadcastAddress(),
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

    std::unique_ptr<core::net::IDatagramSocket> socket;
    ScriptedSummarySource self;
    RecordingFleets fleets;
    std::unique_ptr<Consensus::IRaftPeerKeys const> keys;
    PeerDirectory directory;
    AtomicMetricsSink metrics;
    DiscoveryService service;
};

/// A key that is not the one it signs with: what a liar presents.
///
/// Carries one machine's public key and signs with another's, so a proof it sends names a key
/// whose holder never signed it -- a forgery, whatever the roster holds.
class LyingPeerKeys final: public Consensus::IRaftPeerKeys
{
  public:
    /// @param carried The key the proof names.
    /// @param signer The key that actually signs.
    LyingPeerKeys(Ed25519PublicKey const& carried, Ed25519KeyPair signer):
        _carried { carried },
        _signer { std::move(signer) }
    {
    }

    [[nodiscard]] Ed25519PublicKey OwnPublicKey() const override
    {
        return _carried;
    }

    [[nodiscard]] Ed25519Signature SignAsSelf(LabelledMessage const& message) const override
    {
        return SignLabelled(_signer, message);
    }

    [[nodiscard]] Consensus::PeerKeys KeysOf(Consensus::NodeId const& /*peer*/) const override
    {
        return {};
    }

  private:
    Ed25519PublicKey _carried;
    Ed25519KeyPair _signer;
};

/// Something on the segment that sends whatever bytes a case hands it, holding no key.
struct RawSender
{
    std::unique_ptr<core::net::IDatagramSocket> socket; ///< Where its datagrams come from.

    /// Broadcast @p datagram to the segment.
    /// @param datagram The bytes.
    void Send(std::vector<std::byte> const& datagram) const
    {
        REQUIRE(socket->send(datagram, core::net::testing::DatagramBus::broadcastAddress()).has_value());
    }
};

/// A segment of fleets: one bus, one clock, real nonces, and a roster PER CLUSTER, so a node of
/// one fleet holds no key of another's -- which is what makes "never asked of the roster"
/// observable rather than assumed.
struct Fixture
{
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    std::map<std::string, std::shared_ptr<SharedRoster>> rosters;
    std::deque<std::unique_ptr<Node>> nodes;
    int nextAddress { 1 };

    /// A node of fleet @p cluster in @p state, its key admitted to that fleet's roster.
    /// @param cluster Its fleet.
    /// @param id Its node id.
    /// @param endpoint Where it says it answers Raft; empty for a learner.
    /// @param state What its summary claims.
    /// @param signer The key it signs with; by default its own.
    /// @param carryKeyOf Whose public key its proofs carry; by default its own.
    /// @return The node, owned by the fixture.
    Node& NodeIn(std::string const& cluster,
                 std::string const& id,
                 std::string const& endpoint,
                 FleetState state,
                 std::optional<Ed25519KeyPair> signer = std::nullopt,
                 std::optional<std::string> const& carryKeyOf = std::nullopt)
    {
        auto& roster = rosters[cluster];
        if (roster == nullptr)
            roster = std::make_shared<SharedRoster>();
        roster->Admit(id, TestKeyPair(id).PublicKey());

        auto summary = Claiming(cluster, id, endpoint);
        summary.state = state;

        // Its own address on the bus, apart from the endpoint it claims: a learner claims none.
        auto const at = core::net::DatagramAddress { .host = std::format("10.0.1.{}", nextAddress++), .port = 6681 };
        auto keys = std::unique_ptr<Consensus::IRaftPeerKeys const> {};
        if (signer.has_value() || carryKeyOf.has_value())
            keys = std::make_unique<LyingPeerKeys>(TestKeyPair(carryKeyOf.value_or(id)).PublicKey(),
                                                   signer.has_value() ? *std::move(signer) : TestKeyPair(id));
        else
            keys = std::make_unique<RosterPeerKeys>(TestKeyPair(id), roster);

        nodes.push_back(std::make_unique<Node>(
            bus.open(at),
            std::make_shared<FixedBeaconDestination const>(core::net::testing::DatagramBus::broadcastAddress()),
            clock,
            random,
            logger,
            std::move(summary),
            std::move(keys)));
        return *nodes.back();
    }

    /// A sender with no key and no service, at @p endpoint.
    /// @param endpoint Where it sends from.
    /// @return The sender.
    [[nodiscard]] RawSender Sender(std::string_view endpoint)
    {
        return RawSender { .socket = bus.open(AtEndpoint(endpoint)) };
    }
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
    REQUIRE(peer.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
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

/// The next datagram of @p kind waiting at @p socket, stepping over any other -- a sender's own
/// broadcast beacon doubles back to it.
/// @param socket Whose inbox to read.
/// @param kind What to look for.
/// @return The datagram, or nullopt when none of that kind is waiting.
[[nodiscard]] std::optional<std::vector<std::byte>> NextOf(core::net::IDatagramSocket& socket, DiscoveryWire::Kind kind)
{
    auto received = socket.receive(1ms);
    while (received.has_value())
    {
        if (DiscoveryWire::ClassifyDatagram(received->payload) == std::optional { kind })
            return received->payload;
        received = socket.receive(1ms);
    }
    return std::nullopt;
}

/// The challenge waiting at @p socket, decoded.
/// @param socket Whose inbox to read.
/// @return The challenge.
[[nodiscard]] DiscoveryWire::Challenge NextChallenge(core::net::IDatagramSocket& socket)
{
    auto const datagram = NextOf(socket, DiscoveryWire::Kind::Challenge);
    REQUIRE(datagram.has_value());
    auto challenge = DiscoveryWire::DecodeChallenge(Unwrap(datagram));
    REQUIRE(challenge.has_value());
    return Unwrap(challenge);
}

/// @p pair's proof of @p summary, answering @p challenge and echoing its nonce.
/// @param pair Who signs, and whose key the proof carries.
/// @param challenge What was asked.
/// @param summary What it says.
/// @return The proof, encoded.
[[nodiscard]] std::vector<std::byte> ProofDatagram(Ed25519KeyPair const& pair,
                                                   DiscoveryWire::Challenge const& challenge,
                                                   FleetSummary const& summary)
{
    return DiscoveryWire::EncodeProof(
        { .summary = summary,
          .answers = challenge.nonce,
          .publicKey = pair.PublicKey(),
          .signature = SignLabelled(pair, DiscoveryWire::ProofMessage(challenge, summary, pair.PublicKey())) });
}

/// What an attacker at a real address holds after beaconing @p count invented ids at @p target:
/// one live cookie each, and the summary it was issued for.
/// @param attacker Who beacons, at its own address, and reads the challenges.
/// @param target The node that challenges.
/// @param count How many cookies.
/// @return The summaries beaconed and the challenges they were sent.
[[nodiscard]] std::vector<std::pair<FleetSummary, DiscoveryWire::Challenge>> CollectCookies(RawSender const& attacker,
                                                                                            Node& target,
                                                                                            std::size_t count)
{
    auto held = std::vector<std::pair<FleetSummary, DiscoveryWire::Challenge>> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
    {
        auto const summary = FleetSummary { .clusterId = "c-evil",
                                            .state = FleetState::Solitary,
                                            .nodeId = std::format("n-e-{}", index),
                                            .raftEndpoint = "10.0.0.66:6680" };
        attacker.Send(DiscoveryWire::EncodeBeacon(DiscoveryWire::Beacon { .summary = summary }));
        REQUIRE(target.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
        held.emplace_back(summary, NextChallenge(*attacker.socket));
    }
    return held;
}

/// A proof of @p summary answering @p challenge, carrying @p carried's key and signed by nobody.
/// @param summary What it claims.
/// @param challenge The challenge it names.
/// @param carried The key it carries.
/// @return The proof, encoded.
[[nodiscard]] std::vector<std::byte> ForgedProof(FleetSummary const& summary,
                                                 DiscoveryWire::Challenge const& challenge,
                                                 Ed25519KeyPair const& carried)
{
    return DiscoveryWire::EncodeProof(
        { .summary = summary, .answers = challenge.nonce, .publicKey = carried.PublicKey(), .signature = {} });
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
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    NullLogger logger;

    auto const beacon = core::net::testing::DatagramBus::broadcastAddressOn(TestBeaconPort);
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

    REQUIRE(first.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(second.service.SendBeacon().outcome == BeaconSendOutcome::Sent);

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
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "listener" });
    Node listener { bus, clock, random, logger, "listener", "10.0.0.1:7000", "prod", roster };

    // A lone surrogate: valid in shape, refused by every strict decoder, and the
    // sequence a lenient encoder is most likely to emit by accident.
    Node rogue { bus, clock, random, logger, "rogue-\xED\xA0\x80", "10.0.0.2:7000", "prod", roster };

    REQUIRE(rogue.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
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
        REQUIRE(rogue.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
        CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    }
    CHECK(reported() == 1);

    // Until the interval has passed. The fault is a STANDING one -- a peer whose
    // identity is not text stays that way -- so an operator who starts reading the
    // log an hour later must still find it.
    clock.advance(DiscoveryService::UnnameableReportInterval);
    REQUIRE(rogue.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    CHECK(reported() == 2);
}

TEST_CASE("A proof is refused before this node logs what it claimed", "[cluster][discovery][service]")
{
    // A proof is unauthenticated until its cookie and its signature check out, so the line a
    // refusal writes names the ADDRESS it came from and nothing it claimed -- or anything on the
    // segment could write arbitrary bytes into a node's log by sending one beacon and then one lie,
    // holding no key at all.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "listener", "peer" });
    Node listener { bus, clock, random, logger, "listener", "10.0.0.1:7000", "prod", roster };
    Node peer { bus, clock, random, logger, "peer", "10.0.0.2:7000", "prod", roster };

    // Announced properly, so it is recorded and challenged -- which is what makes a
    // proof carrying its id something this node looks at rather than discards.
    REQUIRE(peer.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(listener.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);

    // And then answers the real cookie for an endpoint that is not text. The signature is not even
    // filled in: the cookie names another endpoint, so this is refused before anything is verified.
    // The byte sits in the HOST, which the summary's dial rule does not read: past the port it would
    // make the proof undecodable, dropped before the refusal this case is about.
    REQUIRE(peer.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
    auto const challenge = NextChallenge(*peer.socket);
    REQUIRE(peer.socket
                ->send(DiscoveryWire::EncodeProof(
                           DiscoveryWire::Proof { .summary = Claiming("prod", "peer", "10.0.0.2\xFF:7000"),
                                                  .answers = challenge.nonce,
                                                  .publicKey = {},
                                                  .signature = {} }),
                       AtEndpoint("10.0.0.1:7000"))
                .has_value());
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(listener.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 0);

    CHECK(std::ranges::none_of(logger.Snapshot(),
                               [](CapturingLogger::Record const& record) { return record.message.contains('\xFF'); }));
    CHECK(listener.directory.AuthenticatedPeers().empty());
}

TEST_CASE("A beacon this node cannot draw a challenge for is recorded, not challenged, and said once",
          "[cluster][discovery][service]")
{
    // Withheld rather than issued under a weak key (#1527): no challenge goes out, so nothing a
    // proof could answer exists, and the Error names this host's generator -- once per interval,
    // because a beacon is unauthenticated and anything on the segment can provoke the line.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom denied { ScriptedSecureRandom::DeniedFailure() };
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "listener", "peer" });
    Node listener { bus, clock, denied, logger, "listener", "10.0.0.1:7000", "prod", roster };
    Node peer { bus, clock, random, logger, "peer", "10.0.0.2:7000", "prod", roster };

    REQUIRE(peer.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeWithheld);
    CHECK(denied.FillCount() == 1);

    // Nothing reached the peer: there was no challenge to answer.
    CHECK(peer.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    CHECK(peer.service.PumpOnce(1ms) == DiscoveryEvent::Nothing);

    // A second beacon inside the interval is withheld too, and not said again.
    REQUIRE(peer.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(listener.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeWithheld);

    auto const said = std::ranges::count_if(logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains("cannot draw the key")
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
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);

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
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
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
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
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

TEST_CASE("Every authenticated reply is handed on, this node's own fleet's included, and nothing else",
          "[cluster][discovery][service]")
{
    // What lets "no other fleet is visible" be a FINDING: the watch hears this fleet's replies through
    // the one seam it hears other fleets' through. Only authenticated ones -- a known id proving
    // another key is not a reply anybody should count.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 11 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "insider", "worker-a" });
    Node insider { bus, clock, random, logger, "insider", "10.0.0.1:7000", "prod", roster };
    Node impostor { bus, clock, random, logger, "worker-a", "10.0.0.2:7000", "prod", roster, TestKeyPair("impostor") };
    REQUIRE(Handshake(insider, impostor) == DiscoveryEvent::PeerUnknownKey);
    CHECK(insider.fleets.proven.empty());

    Node genuine { bus, clock, random, logger, "worker-a", "10.0.0.3:7000", "prod", roster };
    REQUIRE(Handshake(insider, genuine) == DiscoveryEvent::PeerAuthenticated);
    REQUIRE(insider.fleets.proven.size() == 1);
    CHECK(insider.fleets.proven[0].Summary().clusterId == "prod");
    CHECK(insider.fleets.proven[0].Key() == TestKeyPair("worker-a").PublicKey());
}

TEST_CASE("A proof under a REVOKED key is recognised as one", "[cluster][discovery][service][security]")
{
    // Revoked is not unknown: the remedies are opposite -- admit it if it belongs, against
    // never admit it again -- so the event, the counter and the log each say which.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
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
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 7 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    auto const challenge = NextChallenge(*alice.socket);

    // The real cookie and alice's KEY, and a signature made by somebody else.
    auto const pair = TestKeyPair("alice");
    auto const claim = Claiming("prod", "alice", "10.0.0.1:7000");
    auto proof = DiscoveryWire::Proof {
        .summary = claim, .answers = challenge.nonce, .publicKey = pair.PublicKey(), .signature = {}
    };
    proof.signature =
        SignLabelled(TestKeyPair("mallory"), DiscoveryWire::ProofMessage(challenge, proof.summary, proof.publicKey));
    REQUIRE(alice.socket->send(DiscoveryWire::EncodeProof(proof), AtEndpoint("10.0.0.2:7000")).has_value());

    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(bob.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 1);
    CHECK(bob.directory.AuthenticatedPeers().empty());
    CHECK(Warned(logger, "not signed by the key it carries") == 1);
    CHECK(Warned(logger, FormatEd25519PublicKey(pair.PublicKey())) == 0);

    // The forgery spent nothing: the honest answer arriving after it is taken. A forgery that
    // spent the challenge would let anybody who saw it go out destroy its answer.
    REQUIRE(alice.socket->send(ProofDatagram(pair, challenge, claim), AtEndpoint("10.0.0.2:7000")).has_value());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);
}

TEST_CASE("A proof under the all-zero key with the all-zero signature is forged, and no admission is suggested for it",
          "[cluster][discovery][service][security]")
{
    // The small-order forgery, end to end on the wire and with a real challenge: nobody signed
    // anything, and before the seam refused a small-order key this "verified" for every nonce.
    // Then it reached the roster question as an UNKNOWN key and was reported with the
    // `--cluster-admit` that would admit it -- a command naming a key anybody can prove, which the
    // report's own premise ("only its holder could have signed this") made look safe to paste.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 7 }) };
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    auto const challenge = NextChallenge(*alice.socket);

    // Answering for alice, at the endpoint that was challenged -- the only answer bob will judge --
    // echoing the nonce anybody on the segment saw, with a key and a signature that are not.
    auto const proof = DiscoveryWire::Proof {
        .summary = Claiming("prod", "alice", "10.0.0.1:7000"), .answers = challenge.nonce, .publicKey = {}, .signature = {}
    };
    REQUIRE(alice.socket->send(DiscoveryWire::EncodeProof(proof), AtEndpoint("10.0.0.2:7000")).has_value());

    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(bob.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 1);
    CHECK(bob.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) == 0);
    CHECK(bob.directory.AuthenticatedPeers().empty());
    CHECK(Warned(logger, "--cluster-admit") == 0);
    CHECK(Warned(logger, FormatEd25519PublicKey(Ed25519PublicKey {})) == 0);
}

TEST_CASE("Proofs under keys the roster does not accept are all counted and reported at most once a minute",
          "[cluster][discovery][service]")
{
    // A fresh key costs nothing to make, so anything on the segment can send one proof per
    // beacon. The counter takes every one; the log takes the latest per interval and says how
    // many it stands for, or the line is a disk-exhaustion hole reached from outside the fleet.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
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

    clock.advance(DiscoveryService::RejectedProofReportInterval);
    CHECK(Handshake(insider, outsider) == DiscoveryEvent::PeerUnknownKey);
    CHECK(Warned(logger, "outsider at 10.0.0.2:7000") == 2);
    CHECK(Warned(logger, "3 such proof(s) since the last report") == 1);
}

TEST_CASE("A proof for another endpoint than the one challenged answers nothing, and is said once a minute",
          "[cluster][discovery][service][security]")
{
    // The reviewer's log-flood probe, kept. A keyless attacker opens a challenge with one spoofed
    // beacon and answers its REAL cookie naming another endpoint. The cookie binds the endpoint, so
    // that answers no challenge this node issued -- refused before any signature is checked -- and
    // the line it provokes names the source alone and is throttled like every rejected proof, or
    // each pair of datagrams writes a line.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "office" });
    Node office { bus, clock, random, logger, "office", "10.0.0.1:7000", "prod", roster };
    auto const attacker = bus.open(AtEndpoint("10.0.0.66:6681"));

    auto const beacon = DiscoveryWire::EncodeBeacon({ .summary = Claiming("c-evil", "n-evil", "10.0.0.66:6680") });
    auto const round = [&] {
        REQUIRE(attacker->send(beacon, core::net::testing::DatagramBus::broadcastAddress()).has_value());
        REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
        auto const challenge = NextChallenge(*attacker);
        REQUIRE(attacker
                    ->send(DiscoveryWire::EncodeProof({ .summary = Claiming("c-evil", "n-evil", "10.0.0.99:6680"),
                                                        .answers = challenge.nonce,
                                                        .publicKey = {},
                                                        .signature = {} }),
                           AtEndpoint("10.0.0.1:7000"))
                    .has_value());
        REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    };

    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 50))
        round();
    CHECK(Warned(logger, "answers no challenge this node issued") == 1);
    CHECK(Warned(logger, "10.0.0.99") == 0);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 0);

    // A minute on, the next is said, with how many it stands for.
    clock.advance(DiscoveryService::RejectedProofReportInterval);
    round();
    CHECK(Warned(logger, "answers no challenge this node issued") == 2);
    CHECK(Warned(logger, "50 such proof(s) since the last report") == 1);
}

TEST_CASE("Forged proofs are all counted and said at most once a minute", "[cluster][discovery][service][security]")
{
    // A forgery costs its sender one spoofed beacon to aim, so its line is throttled like every
    // rejected proof; the counter takes every one. A real key and a signature it never made: an
    // all-zero key is a different subject -- the seam refuses a small-order key before the curve is
    // asked (`Ed25519Verify`), which `FleetSummarySignature_test` pins.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "office" });
    Node office { bus, clock, random, logger, "office", "10.0.0.1:7000", "prod", roster };
    auto const attacker = bus.open(AtEndpoint("10.0.0.66:6681"));

    auto const beacon = DiscoveryWire::EncodeBeacon({ .summary = Claiming("c-evil", "n-evil", "10.0.0.66:6680") });
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 20))
    {
        // Spaced by the attacker's own check refill, so each forgery IS checked: the subject here is
        // the counter and the throttle, and the check budget has cases of its own.
        clock.advance(BudgetOf(DiscoveryWork::ProofCheck).sourceRefillEvery);
        REQUIRE(attacker->send(beacon, core::net::testing::DatagramBus::broadcastAddress()).has_value());
        REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
        // The real cookie, so what is judged is the signature.
        auto const challenge = NextChallenge(*attacker);
        auto const forged = DiscoveryWire::EncodeProof({ .summary = Claiming("c-evil", "n-evil", "10.0.0.66:6680"),
                                                         .answers = challenge.nonce,
                                                         .publicKey = TestKeyPair("mallory").PublicKey(),
                                                         .signature = {} });
        REQUIRE(attacker->send(forged, AtEndpoint("10.0.0.1:7000")).has_value());
        REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    }
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 20);
    CHECK(Warned(logger, "not signed by the key it carries") == 1);
}

TEST_CASE("A discovery node never replies with more than the datagram that provoked it",
          "[cluster][discovery][service][security]")
{
    // The reviewer's reflection probe, kept and turned round. A reply goes to whatever address its
    // request came FROM, which the sender typed, so a reply larger than its request is an amplifier
    // aimed at a third party. The challenge a beacon provokes is exactly the beacon's size; a
    // challenge too small for this node's proof is not answered, and counted; one padded to this
    // node's beacon, as every challenger of this build pads it, is answered with a proof no larger.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "office" });
    Node office { bus, clock, random, logger, "office", "10.0.0.1:7000", "prod", roster };
    auto const attacker = bus.open(AtEndpoint("10.0.0.66:6681"));

    // What the attacker's socket holds next of @p kind, stepping over its own broadcast beacon.
    auto const nextOf = [&attacker](DiscoveryWire::Kind kind) -> std::optional<std::size_t> {
        auto received = attacker->receive(1ms);
        while (received.has_value())
        {
            if (DiscoveryWire::ClassifyDatagram(received->payload) == std::optional { kind })
                return received->payload.size();
            received = attacker->receive(1ms);
        }
        return std::nullopt;
    };

    auto const beacon = DiscoveryWire::EncodeBeacon({ .summary = Claiming("c-evil", "n-evil", "10.0.0.66:6680") });
    REQUIRE(attacker->send(beacon, core::net::testing::DatagramBus::broadcastAddress()).has_value());
    REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    auto const challenge = nextOf(DiscoveryWire::Kind::Challenge);
    REQUIRE(challenge.has_value());
    CHECK(Unwrap(challenge) == beacon.size());

    // An unpadded challenge: smaller than this node's proof, so nothing goes back.
    auto const nonce = Nonce {};
    auto const unpadded = DiscoveryWire::Frame(DiscoveryWire::Kind::Challenge,
                                               WireFields::Encode({ WireFields::AsBytes(std::string_view { "x" }),
                                                                    std::span<std::byte const> { nonce },
                                                                    std::span<std::byte const> {} }));
    REQUIRE(attacker->send(unpadded, AtEndpoint("10.0.0.1:7000")).has_value());
    CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::ReplyWithheld);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryRepliesWithheld) == 1);
    CHECK_FALSE(nextOf(DiscoveryWire::Kind::Proof).has_value());

    // Padded to this node's beacon, as a challenger of this build pads it: answered, no larger.
    auto const ownBeacon = DiscoveryWire::EncodeBeacon({ .summary = office.self.Current() });
    auto const padded = DiscoveryWire::EncodeChallenge({ .clusterId = "x", .nonce = {} }, ownBeacon.size());
    REQUIRE(padded.has_value());
    REQUIRE(attacker->send(Unwrap(padded), AtEndpoint("10.0.0.1:7000")).has_value());
    CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
    auto const proof = nextOf(DiscoveryWire::Kind::Proof);
    REQUIRE(proof.has_value());
    CHECK(Unwrap(proof) <= Unwrap(padded).size());
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryRepliesWithheld) == 1);
}

TEST_CASE("A beacon and the proof after it carry the same member list, cut to what a datagram holds",
          "[cluster][discovery][service][formation]")
{
    // The node's summary names more members than a datagram carries. Both announcements leave by one
    // door, which cuts the list there -- the first ids kept, the total untouched -- so the beacon a
    // peer challenged and the proof it gets back say the same thing, and neither is refused.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "office" });
    Node office { bus, clock, random, logger, "office", "10.0.0.1:7000", "prod", roster };
    auto summary = office.self.Current();
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 20 }))
        summary.members.push_back(std::format("n-{}", index));
    summary.memberTotal = 20;
    office.self.Set(summary);
    auto const listener = bus.open(AtEndpoint("10.0.0.66:6681"));

    // What the listener's socket holds next of @p kind, stepping over anything else.
    auto const nextOf = [&listener](DiscoveryWire::Kind kind) -> std::optional<std::vector<std::byte>> {
        auto received = listener->receive(1ms);
        while (received.has_value())
        {
            if (DiscoveryWire::ClassifyDatagram(received->payload) == std::optional { kind })
                return received->payload;
            received = listener->receive(1ms);
        }
        return std::nullopt;
    };

    REQUIRE(office.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    auto const beaconBytes = nextOf(DiscoveryWire::Kind::Beacon);
    REQUIRE(beaconBytes.has_value());
    auto const beacon = DiscoveryWire::DecodeBeacon(Unwrap(beaconBytes));
    REQUIRE(beacon.has_value());
    auto const expected = std::vector<std::string>(summary.members.begin(),
                                                   summary.members.begin() + CompileCacheWire::MaxFleetSummaryMembers);
    CHECK(Unwrap(beacon).summary.members == expected);
    CHECK(Unwrap(beacon).summary.memberTotal == 20);

    auto const challenge = DiscoveryWire::EncodeChallenge({ .clusterId = "x", .nonce = {} }, Unwrap(beaconBytes).size());
    REQUIRE(challenge.has_value());
    REQUIRE(listener->send(Unwrap(challenge), AtEndpoint("10.0.0.1:7000")).has_value());
    REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
    REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
    auto const proofBytes = nextOf(DiscoveryWire::Kind::Proof);
    REQUIRE(proofBytes.has_value());
    auto const proof = DiscoveryWire::DecodeProof(Unwrap(proofBytes));
    REQUIRE(proof.has_value());
    CHECK(Unwrap(proof).summary == Unwrap(beacon).summary);
}

TEST_CASE("A beacon too small for this node's challenge is not challenged, and is counted",
          "[cluster][discovery][service][security]")
{
    // Padding only grows a challenge, so a beacon smaller than this node's challenge could only be
    // answered by an amplifier. A beacon of this build is as large as its sender's proof, which is
    // larger than any challenge; a hand-made one shorn of its padding is not challenged.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "office" });
    auto const longCluster = std::string(64, 'c');
    Node office { bus, clock, random, logger, "office", "10.0.0.1:7000", longCluster, roster };
    auto const attacker = bus.open(AtEndpoint("10.0.0.66:6681"));

    auto const summary = CompileCacheWire::EncodeFleetSummaryFields(Claiming("c", "n", "10.0.0.66:6680"));
    auto const shorn =
        DiscoveryWire::Frame(DiscoveryWire::Kind::Beacon,
                             WireFields::Encode({ std::span<std::byte const> { summary }, std::span<std::byte const> {} }));
    REQUIRE(attacker->send(shorn, core::net::testing::DatagramBus::broadcastAddress()).has_value());
    CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::ReplyWithheld);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryRepliesWithheld) == 1);
    CHECK_FALSE(NextOf(*attacker, DiscoveryWire::Kind::Challenge).has_value());

    // The control: the same summary in a beacon of this build is challenged.
    REQUIRE(attacker
                ->send(DiscoveryWire::EncodeBeacon({ .summary = Claiming("c", "n", "10.0.0.66:6680") }),
                       core::net::testing::DatagramBus::broadcastAddress())
                .has_value());
    CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    CHECK(NextOf(*attacker, DiscoveryWire::Kind::Challenge).has_value());
}

TEST_CASE("Answers to challenges are rate-limited, and every one withheld is counted",
          "[cluster][discovery][service][security]")
{
    // Every answer is a signature, and goes to an address the challenger typed: unlimited, it is a
    // signing oracle and a reflector anybody on the segment can aim. So the answers every source
    // shares come out of one burst and then one more per refill interval on the injected clock, and
    // every challenge past that is counted. Asked from enough sources, each inside its own burst,
    // that what refuses is the SHARED budget -- the bound a flood varying its address meets.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "office" });
    Node office { bus, clock, random, logger, "office", "10.0.0.1:7000", "prod", roster };
    auto const challenge = Unwrap(DiscoveryWire::EncodeChallenge(
        { .clusterId = "x", .nonce = {} }, DiscoveryWire::EncodeBeacon({ .summary = office.self.Current() }).size()));

    auto constexpr Answers = BudgetOf(DiscoveryWork::Answer);
    auto constexpr Past = std::size_t { 3 };
    auto constexpr Sources = (Answers.burst + Past + Answers.sourceBurst - 1) / Answers.sourceBurst;
    auto sources = std::vector<std::unique_ptr<core::net::IDatagramSocket>> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, Sources + 1))
        sources.push_back(bus.open(AtEndpoint(std::format("10.0.2.{}:6681", index + 1))));

    auto answered = std::size_t { 0 };
    auto withheld = std::size_t { 0 };
    for (auto const attempt: std::views::iota(std::size_t { 0 }, Answers.burst + Past))
    {
        // Each source asks no more than its own burst, so no refusal below is a source's.
        auto const& source = sources[attempt / Answers.sourceBurst];
        REQUIRE(source->send(challenge, AtEndpoint("10.0.0.1:7000")).has_value());
        auto const event = office.service.PumpOnce(1ms);
        answered += event == DiscoveryEvent::ChallengeAnswered ? 1 : 0;
        withheld += event == DiscoveryEvent::ReplyWithheld ? 1 : 0;
    }
    CHECK(answered == Answers.burst);
    CHECK(withheld == Past);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryRepliesWithheld) == Past);

    // One interval on, exactly one more, to a source that has spent nothing of its own.
    clock.advance(Answers.refillEvery);
    REQUIRE(sources.back()->send(challenge, AtEndpoint("10.0.0.1:7000")).has_value());
    CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
    REQUIRE(sources.back()->send(challenge, AtEndpoint("10.0.0.1:7000")).has_value());
    CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::ReplyWithheld);
}

TEST_CASE("One host flooding challenges never spends the answer this cluster's own peer is owed",
          "[cluster][discovery][service][security]")
{
    // The reviewer's probe P6, kept. A stranger at one real address spends a whole round's global
    // burst on challenges before this node's peer asks; one budget shared by everybody left the
    // peer unanswered in every round (0 of 4). The stranger's host has a budget of its own, spent
    // first, so the peer is answered from what the stranger could not reach. The arm without the
    // stranger is the control, so the flooded arm's verdict is the flood's.
    for (auto const withStrangers: { false, true })
    {
        INFO("strangers: " << withStrangers);
        Fixture fix;
        auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
        auto& peer = fix.NodeIn("c-office", "n-peer", "10.0.0.2:6680", FleetState::Established);
        auto const flood = fix.Sender("10.0.0.66:6681");
        auto const target = office.socket->boundAddress();
        auto const size = DiscoveryWire::ProofDatagramSize(office.self.Current());
        auto answeredPeer = 0;
        for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
        {
            for ([[maybe_unused]] auto const index:
                 std::views::iota(std::size_t { 0 }, withStrangers ? BudgetOf(DiscoveryWork::Answer).burst : 0))
            {
                auto const challenge = DiscoveryWire::EncodeChallenge({ .clusterId = "c-x", .nonce = {} }, size);
                REQUIRE(challenge.has_value());
                REQUIRE(flood.socket->send(Unwrap(challenge), target).has_value());
                (void) office.service.PumpOnce(1ms);
            }
            REQUIRE(office.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
            REQUIRE(peer.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
            REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
            if (office.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered)
                ++answeredPeer;
            Drain(peer);
            fix.clock.advance(15s);
            office.service.Maintain();
            peer.service.Maintain();
        }
        CHECK(answeredPeer == 4);
    }
}

TEST_CASE("Forged proofs from one host are checked only up to that host's share", "[cluster][discovery][service][security]")
{
    // A forgery spends no challenge, so one beacon from a real address buys a live cookie a forged
    // proof can name, each costing this node a signature check. One forgery per cookie here, so
    // what bounds the checks is the host's own check budget: its burst, and the rest counted and
    // said, never checked. And the flood leaves everybody else's budget alone.
    Fixture fix;
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    auto& peer = fix.NodeIn("c-office", "n-peer", "10.0.0.2:6680", FleetState::Established);
    auto const attacker = fix.Sender("10.0.0.66:6681");
    auto constexpr Checks = BudgetOf(DiscoveryWork::ProofCheck);
    auto constexpr Past = std::size_t { 5 };

    auto const held = CollectCookies(attacker, office, Checks.sourceBurst + Past);
    auto checked = std::size_t { 0 };
    auto unchecked = std::size_t { 0 };
    for (auto const& [summary, challenge]: held)
    {
        REQUIRE(attacker.socket->send(ForgedProof(summary, challenge, TestKeyPair("mallory")), office.socket->boundAddress())
                    .has_value());
        auto const event = office.service.PumpOnce(1ms);
        checked += event == DiscoveryEvent::ProofRejected ? 1 : 0;
        unchecked += event == DiscoveryEvent::ProofUnchecked ? 1 : 0;
    }
    CHECK(checked == Checks.sourceBurst);
    CHECK(unchecked == Past);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == Checks.sourceBurst);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofChecksWithheld) == Past);
    CHECK(Warned(fix.logger, "check budget is spent") == 1);

    // Another host's proof is checked from the budget the flood could not reach. The peer heard the
    // attacker's broadcast beacons too; it reads those first.
    Drain(peer);
    CHECK(Handshake(office, peer) == DiscoveryEvent::PeerAuthenticated);
}

TEST_CASE("Forged proofs from many hosts are checked only up to the shared budget",
          "[cluster][discovery][service][security]")
{
    // A cookie binds the host it was sent to, so a flood answers only from addresses it OWNS, each
    // collecting its own cookies there -- and each with a fresh bucket of its own. One forgery per
    // cookie, so what bounds the checks is the budget every proof shares, and one refill interval
    // buys exactly one more.
    Fixture fix;
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    auto constexpr Checks = BudgetOf(DiscoveryWork::ProofCheck);
    auto constexpr Past = std::size_t { 3 };
    auto constexpr Sources = (Checks.burst + Past + Checks.sourceBurst - 1) / Checks.sourceBurst;
    auto sources = std::vector<RawSender> {};
    auto held = std::vector<std::vector<std::pair<FleetSummary, DiscoveryWire::Challenge>>> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, Sources + 1))
    {
        sources.push_back(fix.Sender(std::format("10.0.3.{}:6681", index + 1)));
        held.push_back(CollectCookies(sources.back(), office, Checks.sourceBurst));
    }
    auto const forgedFrom = [&](std::size_t source, std::size_t cookie) {
        auto const& [summary, challenge] = held[source][cookie];
        REQUIRE(sources[source]
                    .socket->send(ForgedProof(summary, challenge, TestKeyPair("mallory")), office.socket->boundAddress())
                    .has_value());
        return office.service.PumpOnce(1ms);
    };

    auto checked = std::size_t { 0 };
    auto unchecked = std::size_t { 0 };
    for (auto const attempt: std::views::iota(std::size_t { 0 }, Checks.burst + Past))
    {
        // Each source sends no more than its own burst, so no refusal below is a source's.
        auto const event = forgedFrom(attempt / Checks.sourceBurst, attempt % Checks.sourceBurst);
        checked += event == DiscoveryEvent::ProofRejected ? 1 : 0;
        unchecked += event == DiscoveryEvent::ProofUnchecked ? 1 : 0;
    }
    CHECK(checked == Checks.burst);
    CHECK(unchecked == Past);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == Checks.burst);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofChecksWithheld) == Past);

    // One interval on, exactly one more, from a source that has spent nothing of its own.
    fix.clock.advance(Checks.refillEvery);
    CHECK(forgedFrom(Sources, 0) == DiscoveryEvent::ProofRejected);
    CHECK(forgedFrom(Sources, 1) == DiscoveryEvent::ProofUnchecked);
}

TEST_CASE("Forged proofs against one live challenge buy only its own few checks, and only from where it went",
          "[cluster][discovery][service][security]")
{
    // One cookie. From any host but the one it was sent to it names no cookie this node made --
    // refused before the budget, so not one check. From its own host, each forgery inside that
    // host's budget, the cookie itself is what bounds them: after `MaxForgeriesPerChallenge`
    // failed checks it is exhausted -- refused unchecked, counted, and said -- so no cookie is the
    // key to unlimited signature checks.
    Fixture fix;
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    auto const attacker = fix.Sender("10.0.0.66:6681");
    auto constexpr Checks = BudgetOf(DiscoveryWork::ProofCheck);
    auto constexpr Forgeries = std::size_t { 20 };
    auto const held = CollectCookies(attacker, office, 1);
    auto const& [summary, challenge] = held.front();

    for (auto const index: std::views::iota(std::size_t { 0 }, Forgeries))
    {
        auto const elsewhere = fix.Sender(std::format("10.0.4.{}:6681", index + 1));
        REQUIRE(
            elsewhere.socket->send(ForgedProof(summary, challenge, TestKeyPair("mallory")), office.socket->boundAddress())
                .has_value());
        CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    }
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 0);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofChecksWithheld) == 0);

    auto checked = std::size_t { 0 };
    auto exhausted = std::size_t { 0 };
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, Forgeries))
    {
        // Paced to the host's own refill, so no refusal below is its budget's.
        fix.clock.advance(Checks.sourceRefillEvery);
        REQUIRE(attacker.socket->send(ForgedProof(summary, challenge, TestKeyPair("mallory")), office.socket->boundAddress())
                    .has_value());
        auto const event = office.service.PumpOnce(1ms);
        checked += event == DiscoveryEvent::ProofRejected ? 1 : 0;
        exhausted += event == DiscoveryEvent::ProofExhausted ? 1 : 0;
    }
    CHECK(checked == MaxForgeriesPerChallenge);
    CHECK(exhausted == Forgeries - MaxForgeriesPerChallenge);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == MaxForgeriesPerChallenge);
    CHECK(office.metrics.Read(IMetricsSink::Counter::DiscoveryProofChecksWithheld) == Forgeries - MaxForgeriesPerChallenge);
    CHECK(Warned(fix.logger, "already failed") == 1);
}

TEST_CASE("Forged proofs over real cookies from spoofed hosts spend no check an honest proof needs",
          "[cluster][discovery][formation][security]")
{
    // The reviewer's probe P9, kept. An attacker collects real cookies at its own address -- more
    // than the shared budget's worth of forgeries -- and, when it hears the lab's beacon, answers
    // each twice from spoofed hosts, each inside its own share. Before the cookie bound the host it
    // was sent to, every one reached the budget: 128 were checked, the lab's proof was refused
    // unchecked, and the fleet went unproven. Bound to the host, each fails the cookie, before the
    // budget. The arm without the flood is the control.
    auto constexpr Checks = BudgetOf(DiscoveryWork::ProofCheck);
    auto constexpr Cookies = (Checks.burst / MaxForgeriesPerChallenge) + 8;
    auto constexpr Spoofed = (Checks.burst / Checks.sourceBurst) + 4;
    for (auto const withFlood: { false, true })
    {
        INFO("flood: " << withFlood);
        Fixture fix;
        auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
        auto& lab = fix.NodeIn("c-lab", "n-lab", "10.0.0.3:6680", FleetState::Established);
        auto const attacker = fix.Sender("10.0.0.66:6681");
        auto const held = CollectCookies(attacker, laptop, withFlood ? Cookies : 0);
        auto spoofed = std::vector<RawSender> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, withFlood ? Spoofed : 0))
            spoofed.push_back(fix.Sender(std::format("10.8.0.{}:6681", index + 1)));

        // The lab beacons and is challenged.
        Drain(lab);
        REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
        REQUIRE(laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);

        auto sent = std::size_t { 0 };
        auto rejected = std::size_t { 0 };
        for (auto const& [summary, challenge]: held)
            for ([[maybe_unused]] auto const attempt: std::views::iota(std::size_t { 0 }, MaxForgeriesPerChallenge))
            {
                auto const& from = spoofed[sent % spoofed.size()];
                REQUIRE(
                    from.socket->send(ForgedProof(summary, challenge, TestKeyPair("mallory")), laptop.socket->boundAddress())
                        .has_value());
                rejected += laptop.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected ? 1 : 0;
                ++sent;
            }
        CHECK(sent == (withFlood ? Cookies * MaxForgeriesPerChallenge : 0));
        CHECK(rejected == sent);
        CHECK(laptop.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 0);
        CHECK(laptop.metrics.Read(IMetricsSink::Counter::DiscoveryProofChecksWithheld) == 0);

        // The lab answers, and is proven.
        REQUIRE(lab.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
        REQUIRE(lab.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
        CHECK(laptop.service.PumpOnce(1ms) == DiscoveryEvent::ForeignFleetProven);
    }
}

TEST_CASE("A burst of verified proofs never fills the record an honest proof needs",
          "[cluster][discovery][formation][security]")
{
    // The reviewer's probe P8, kept, and driven THROUGH the check budget. An attacker at addresses it
    // owns collects a thousand cookies, one beacon each, eight at each address; when it hears the
    // lab's beacon it answers every one, validly, under a key of its own, from the address each was
    // sent to, each inside its share and paced to the shared refill. A record of 1024 per epoch was full before the lab's
    // answer arrived, and the lab was refused as expired; a state for every issued cookie never is. The arm without the
    // burst is the control.
    auto constexpr Burst = std::size_t { 1024 };
    auto constexpr Checks = BudgetOf(DiscoveryWork::ProofCheck);
    for (auto const withBurst: { false, true })
    {
        INFO("burst: " << withBurst);
        Fixture fix;
        auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
        auto& lab = fix.NodeIn("c-lab", "n-lab", "10.0.0.3:6680", FleetState::Established);
        auto const evil = TestKeyPair("mallory");
        auto sources = std::vector<RawSender> {};
        auto held = std::vector<std::pair<FleetSummary, DiscoveryWire::Challenge>> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, withBurst ? Burst / Checks.sourceBurst : 0))
        {
            sources.push_back(fix.Sender(std::format("10.1.{}.{}:6681", index / 200, (index % 200) + 1)));
            std::ranges::move(CollectCookies(sources.back(), laptop, Checks.sourceBurst), std::back_inserter(held));
        }

        // The lab beacons and is challenged, in the same epoch.
        Drain(lab);
        REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
        REQUIRE(laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);

        auto verified = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, held.size()))
        {
            auto const& [summary, challenge] = held[index];
            auto proof =
                DiscoveryWire::Proof { .summary = summary, .answers = challenge.nonce, .publicKey = evil.PublicKey() };
            proof.signature = SignLabelled(evil, DiscoveryWire::ProofMessage(challenge, proof.summary, proof.publicKey));
            fix.clock.advance(Checks.refillEvery);
            REQUIRE(sources[index / Checks.sourceBurst]
                        .socket->send(DiscoveryWire::EncodeProof(proof), laptop.socket->boundAddress())
                        .has_value());
            verified += laptop.service.PumpOnce(1ms) == DiscoveryEvent::ForeignFleetProven ? 1 : 0;
        }
        CHECK(verified == held.size());

        // The lab answers.
        REQUIRE(lab.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
        REQUIRE(lab.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
        CHECK(laptop.service.PumpOnce(1ms) == DiscoveryEvent::ForeignFleetProven);
    }
}

namespace
{
/// Beacon destinations a case names outright.
class ListedDestinations final: public IBeaconDestinations
{
  public:
    /// @param destinations Where every beacon goes.
    explicit ListedDestinations(std::vector<core::net::DatagramAddress> destinations):
        _destinations { std::move(destinations) }
    {
    }

    [[nodiscard]] std::vector<core::net::DatagramAddress> Destinations() const override
    {
        return _destinations;
    }

  private:
    std::vector<core::net::DatagramAddress> _destinations;
};
} // namespace

TEST_CASE("A beacon goes to every destination named now, one datagram each, and nowhere else",
          "[cluster][discovery][service]")
{
    // The default names one directed broadcast per link, and each link hears the beacon once.
    // Asserted at the receiving ends: two named addresses hear exactly one each, and a bystander
    // at an address nobody named hears nothing. The empty list is the control that the service
    // sends where it is told and nowhere by itself: no beacon, and it says so as `NoDestination` --
    // never as a refusal, which nothing produced.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "announcer" });
    auto first = bus.open(AtEndpoint("10.0.2.1:6681"));
    auto second = bus.open(AtEndpoint("10.0.3.1:6681"));
    auto bystander = bus.open(AtEndpoint("10.0.4.1:6681"));
    auto const summary = Claiming("prod", "announcer", "10.0.1.1:7000");

    for (auto const named: { true, false })
    {
        INFO("destinations named: " << named);
        auto destinations = std::vector<core::net::DatagramAddress> {};
        if (named)
            destinations = { { .host = "10.0.2.1", .port = 6681 }, { .host = "10.0.3.1", .port = 6681 } };
        Node announcer { bus.open(AtEndpoint(named ? "10.0.1.1:6681" : "10.0.1.2:6681")),
                         std::make_shared<ListedDestinations const>(std::move(destinations)),
                         clock,
                         random,
                         logger,
                         summary,
                         std::make_unique<RosterPeerKeys>(TestKeyPair("announcer"), roster) };
        CHECK(announcer.service.SendBeacon().outcome
              == (named ? BeaconSendOutcome::Sent : BeaconSendOutcome::NoDestination));
        CHECK(NextOf(*first, DiscoveryWire::Kind::Beacon).has_value() == named);
        CHECK(NextOf(*second, DiscoveryWire::Kind::Beacon).has_value() == named);
        CHECK_FALSE(NextOf(*first, DiscoveryWire::Kind::Beacon).has_value());
        CHECK_FALSE(NextOf(*second, DiscoveryWire::Kind::Beacon).has_value());
        CHECK_FALSE(NextOf(*bystander, DiscoveryWire::Kind::Beacon).has_value());
    }
}

TEST_CASE("A beacon the stack refuses everywhere is AllRefused, and one refused on some links is PartlyRefused",
          "[cluster][discovery][service]")
{
    // The one outcome a caller reports, told apart from the two that send nothing: a refusal has
    // to have HAPPENED -- the socket counts it -- and a link that refuses does not keep the beacon
    // from the link that takes it. Whichever link that is: refusing the FIRST alone would let a loop
    // in which the last destination decides pass, and that loop calls a beacon the LAN took, on a
    // machine whose VPN refused it, refused everywhere.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;
    auto const roster = SharedRoster::Of({ "announcer" });
    auto first = bus.open(AtEndpoint("10.0.2.1:6681"));
    auto second = bus.open(AtEndpoint("10.0.3.1:6681"));
    auto const both = std::vector<core::net::DatagramAddress> { { .host = "10.0.2.1", .port = 6681 },
                                                                { .host = "10.0.3.1", .port = 6681 } };

    struct Row
    {
        std::string_view what;
        std::vector<std::string> refusedHosts; ///< Empty refuses every destination.
        bool firstHears;
        bool secondHears;
    };
    auto const rows = std::array {
        Row { .what = "the first refused", .refusedHosts = { "10.0.2.1" }, .firstHears = false, .secondHears = true },
        Row { .what = "the last refused", .refusedHosts = { "10.0.3.1" }, .firstHears = true, .secondHears = false },
        Row { .what = "refused everywhere", .refusedHosts = {}, .firstHears = false, .secondHears = false },
    };
    for (auto const& row: rows)
    {
        INFO(row.what);
        auto const everywhere = row.refusedHosts.empty();
        auto refusing = std::make_unique<RefusingDatagramSocket>(bus.open(AtEndpoint("10.0.1.1:6681")), row.refusedHosts);
        auto const& refused = *refusing;
        Node announcer { std::move(refusing),
                         std::make_shared<ListedDestinations const>(both),
                         clock,
                         random,
                         logger,
                         Claiming("prod", "announcer", "10.0.1.1:7000"),
                         std::make_unique<RosterPeerKeys>(TestKeyPair("announcer"), roster) };
        auto const report = announcer.service.SendBeacon();
        CHECK(report.outcome == (everywhere ? BeaconSendOutcome::AllRefused : BeaconSendOutcome::PartlyRefused));
        // Where it was taken, too: the other half of what a partial refusal is said with.
        CHECK(report.accepted.size() == 2U - refused.Refused());
        CHECK(refused.Refused() == (everywhere ? 2U : 1U));
        // Every refusal comes back with where and why, so the line that reports it can say so.
        CHECK(report.refused.size() == refused.Refused());
        for (auto const& refusal: report.refused)
        {
            CHECK((everywhere || refusal.destination.host == row.refusedHosts.front()));
            CHECK(refusal.error.code == core::net::NetErrorCode::PermissionDenied);
        }
        CHECK(DescribeRefusals(report.refused).contains("permission denied (refused by the test's local stack)"));
        CHECK(NextOf(*first, DiscoveryWire::Kind::Beacon).has_value() == row.firstHears);
        CHECK(NextOf(*second, DiscoveryWire::Kind::Beacon).has_value() == row.secondHears);
    }
}

TEST_CASE("A proof nobody asked for is refused", "[cluster][discovery][service]")
{
    // A proof is only ever an answer to a challenge this node issued. An
    // unsolicited one carries a nonce nobody here chose, so accepting it would
    // make the nonce -- and therefore the replay protection -- pointless.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 5 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "ghost" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    auto intruder = bus.open(AtEndpoint("10.0.0.9:7000"));

    DiscoveryWire::Challenge const invented { .clusterId = "prod", .nonce = {} };
    auto const ghost = TestKeyPair("ghost");
    auto const claim = Claiming("prod", "ghost", "10.0.0.9:7000");
    auto const signature = SignLabelled(ghost, DiscoveryWire::ProofMessage(invented, claim, ghost.PublicKey()));
    REQUIRE(
        intruder
            ->send(
                DiscoveryWire::EncodeProof(
                    { .summary = claim, .answers = invented.nonce, .publicKey = ghost.PublicKey(), .signature = signature }),
                AtEndpoint("10.0.0.1:7000"))
            .has_value());

    // Even holding a key the roster knows, a proof against a self-chosen nonce is refused:
    // alice never challenged "ghost".
    CHECK(alice.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(alice.directory.AuthenticatedPeers().empty());
}

TEST_CASE("A challenge is spent once", "[cluster][discovery][service]")
{
    // A cookie that could answer twice is one that can be replayed: an observer who captured one
    // valid proof could re-send it inside its window and be re-authenticated without ever holding
    // the key. Spent once VERIFIED, so the replay is named for what it is.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 7 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);

    // Capture the challenge before Alice's service answers it, then deliver one proof twice.
    auto const proof =
        ProofDatagram(TestKeyPair("alice"), NextChallenge(*alice.socket), Claiming("prod", "alice", "10.0.0.1:7000"));

    REQUIRE(alice.socket->send(proof, AtEndpoint("10.0.0.2:7000")).has_value());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);
    CHECK(bob.service.SpentChallenges() == 1);

    // The replay answers a spent challenge and is refused as one.
    REQUIRE(alice.socket->send(proof, AtEndpoint("10.0.0.2:7000")).has_value());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::ProofReplayed);
    CHECK(bob.service.SpentChallenges() == 1);
}

TEST_CASE("An answer after its challenge's window is refused as expired", "[cluster][discovery][service]")
{
    // A challenge is answerable for a bounded time -- its only job is to bound how long a nonce is
    // worth capturing -- so a proof that arrives a lifetime late is refused, and named apart from a
    // forgery: an honest slow peer looks exactly like it, and the next beacon asks again.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 7, 8 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);

    clock.advance(DiscoveryConfig {}.challengeLifetime);
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::ProofExpired);
    CHECK(bob.directory.AuthenticatedPeers().empty());
    CHECK(bob.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 0);

    // The control: the same exchange answered inside the window is taken.
    CHECK(Handshake(bob, alice) == DiscoveryEvent::PeerAuthenticated);
}

TEST_CASE("A cookie this node did not make answers nothing, however well it is signed",
          "[cluster][discovery][service][security]")
{
    // The roster holds the prover's key and the signature is its own, over the cookie it sends --
    // but one byte of that cookie is not what this node made for it, so it answers no challenge
    // here. Refused at the MAC, before any signature is checked: not a forgery, and not counted as one.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    CapturingLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    REQUIRE(alice.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    auto const challenge = NextChallenge(*alice.socket);
    auto const claim = Claiming("prod", "alice", "10.0.0.1:7000");

    auto forged = challenge;
    forged.nonce.back() ^= std::byte { 0x01 };
    REQUIRE(alice.socket->send(ProofDatagram(TestKeyPair("alice"), forged, claim), AtEndpoint("10.0.0.2:7000")).has_value());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::ProofRejected);
    CHECK(bob.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 0);
    CHECK(Warned(logger, "answers no challenge this node issued") == 1);
    CHECK(bob.directory.AuthenticatedPeers().empty());

    // The control: the cookie as it was made, under the same key, is taken.
    REQUIRE(
        alice.socket->send(ProofDatagram(TestKeyPair("alice"), challenge, claim), AtEndpoint("10.0.0.2:7000")).has_value());
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);
}

TEST_CASE("A node proves a beacon from another fleet and hands the proven summary on", "[cluster][discovery][formation]")
{
    // A solitary machine has to SEE a fleet to yield to it, so a beacon of another cluster is
    // challenged, and the challenge is answered whichever cluster asked: what the answer signs
    // is the summary the beacon already shouted, so it announces nothing a listener lacked.
    Fixture fix;
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);

    CHECK(Handshake(laptop, office) == DiscoveryEvent::ForeignFleetProven);
    REQUIRE(laptop.fleets.proven.size() == 1);
    CHECK(laptop.fleets.proven[0].Summary() == office.self.Current());
    CHECK(laptop.fleets.proven[0].Key() == TestKeyPair("n-office").PublicKey());
    CHECK(laptop.fleets.proven[0].Origin() == FleetOrigin::Beacon);

    // Never desired: the roster was not asked -- it holds no c-office key, so asking it would
    // have been `PeerUnknownKey` and counted -- and nothing was authenticated for membership.
    CHECK(laptop.directory.AuthenticatedPeers().empty());
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) == 0);
    CHECK(laptop.service.SpentChallenges() == 1);
}

TEST_CASE("A forged beacon from another fleet is ignored and counted as forged", "[cluster][discovery][formation]")
{
    // Judged by its signature alone, and the signature still comes first: a proof that carries
    // n-office's key signed by somebody else hands nothing to formation.
    Fixture fix;
    auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
    auto& liar = fix.NodeIn("c-office",
                            "n-office",
                            "10.0.0.1:6680",
                            FleetState::Established,
                            TestKeyPair("mallory"),
                            std::string { "n-office" });

    CHECK(Handshake(laptop, liar) == DiscoveryEvent::ProofRejected);
    CHECK(laptop.fleets.proven.empty());
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedForged) == 1);
}

TEST_CASE("A flood of beacons from forged ids keeps every discovery table bounded", "[cluster][discovery][formation]")
{
    // [RF-5] A beacon is unauthenticated, and challenging another fleet's widens what one forged
    // datagram can provoke. So the other fleets and this cluster's unrostered peers are bounded,
    // every overflow is COUNTED -- and a challenge is held by nobody, so the flood leaves nothing
    // outstanding at all: only a VERIFIED proof is remembered.
    Fixture fix;
    auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
    auto const flood = fix.Sender("10.0.0.66:6681");
    for (auto const index: std::views::iota(0, 500))
    {
        // Every other datagram claims THIS cluster: its id is in every beacon the laptop sends.
        auto const cluster = index % 2 == 0 ? std::string { "c-laptop" } : std::format("c-{}", index % 40);
        flood.Send(DiscoveryWire::EncodeBeacon(
            DiscoveryWire::Beacon { .summary = FleetSummary { .clusterId = cluster,
                                                              .state = FleetState::Solitary,
                                                              .nodeId = std::format("n-{}", index),
                                                              .raftEndpoint = "10.0.0.66:6680" } }));
        (void) laptop.service.PumpOnce(1ms);
    }
    CHECK(laptop.service.SpentChallenges() == 0);
    CHECK(laptop.directory.ForeignFleets() == MaxForeignFleets);
    CHECK(laptop.directory.Size() == MaxUnrosteredPeers);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::DiscoveryBeaconsOverBound) > 0);
}

TEST_CASE("Spoofed ids of a fleet's own cluster never displace the challenge that fleet is answering",
          "[cluster][discovery][formation][security]")
{
    // The reviewer's probe P5, kept. An attacker that heard the lab's broadcast beacon sends at once
    // four invented ids claiming the lab's cluster: one hop and no signature, against the lab's two
    // hops and one. A table of outstanding challenges, bounded per other cluster, gave each of them
    // the lab's slot and the lab proved nothing in four rounds; with no table there is no slot. The
    // arm without spoofs is the control, so the spoofed arm's verdict is the spoofs'.
    for (auto const withSpoofs: { false, true })
    {
        INFO("spoofs: " << withSpoofs);
        Fixture fix;
        auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
        auto& lab = fix.NodeIn("c-lab", "n-lab", "10.0.0.3:6680", FleetState::Established);
        auto const flood = fix.Sender("10.0.0.66:6681");
        constexpr auto SpoofsPerRound = 4;
        auto proven = 0;
        for (auto const round: std::views::iota(0, 4))
        {
            // The lab hears the spoofs too (they are broadcast); it reads what it has not yet.
            Drain(lab);
            REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
            REQUIRE(laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
            fix.clock.advance(1ms); // the spoofs are strictly newer: no tie decides this
            for (auto const index: std::views::iota(0, withSpoofs ? SpoofsPerRound : 0))
            {
                flood.Send(DiscoveryWire::EncodeBeacon(
                    DiscoveryWire::Beacon { .summary = FleetSummary { .clusterId = "c-lab",
                                                                      .state = FleetState::Established,
                                                                      .nodeId = std::format("n-spoof-{}-{}", round, index),
                                                                      .raftEndpoint = "10.0.0.66:6680" } }));
                REQUIRE(laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
            }
            REQUIRE(lab.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
            REQUIRE(lab.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
            if (laptop.service.PumpOnce(1ms) == DiscoveryEvent::ForeignFleetProven)
                ++proven;
            fix.clock.advance(15s);
            laptop.service.Maintain();
            lab.service.Maintain();
        }
        CHECK(proven == 4);
    }
}

TEST_CASE("A flood of other fleets' beacons never keeps this cluster's rostered peer from a challenge",
          "[cluster][discovery][formation]")
{
    // The reviewer's starvation probe, kept. 64 spoofed other-fleet ids re-sent every 20 s --
    // 3.2 datagrams a second, inside the challenge lifetime -- against a peer the roster holds a
    // key for, which beacons once a round. No table holds a challenge, so no flood takes its place.
    Fixture fix;
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    auto& peer = fix.NodeIn("c-office", "n-peer", "10.0.0.2:6680", FleetState::Established);
    auto const flood = fix.Sender("10.0.0.66:6681");
    auto constexpr SpoofedClusters = 4;
    auto constexpr SpoofedIds = 64;
    auto const spoofAll = [&] {
        for (auto const index: std::views::iota(0, SpoofedIds))
        {
            flood.Send(DiscoveryWire::EncodeBeacon(DiscoveryWire::Beacon {
                .summary = FleetSummary { .clusterId = std::format("c-spoof-{}", index % SpoofedClusters),
                                          .state = FleetState::Solitary,
                                          .nodeId = std::format("n-{}", index),
                                          .raftEndpoint = "10.0.0.66:6680" } }));
            (void) office.service.PumpOnce(1ms);
        }
    };

    auto peerChallenged = 0;
    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        spoofAll();
        REQUIRE(peer.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
        if (office.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen)
            ++peerChallenged;
        fix.clock.advance(20s);
        office.service.Maintain();
    }
    CHECK(peerChallenged == 4);
    CHECK(office.service.SpentChallenges() == 0);
}

TEST_CASE("A rostered peer's challenge survives a flood that arrives inside its round trip",
          "[cluster][discovery][formation]")
{
    // A table of outstanding challenges displaced its oldest entry at a bound -- so a flood fast
    // enough to cycle it between a challenge and its answer displaced a real peer's. There is no
    // such table: the flood below is twice what the old bound held, and the answer is still taken.
    Fixture fix;
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    auto& peer = fix.NodeIn("c-office", "n-peer", "10.0.0.2:6680", FleetState::Established);
    auto const flood = fix.Sender("10.0.0.66:6681");

    REQUIRE(peer.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);

    // A moment later, so the peer's is strictly the OLDEST challenge: the first a pool would give up.
    fix.clock.advance(1ms);
    auto constexpr FloodedIds = 128;
    for (auto const index: std::views::iota(0, FloodedIds))
    {
        flood.Send(DiscoveryWire::EncodeBeacon(
            DiscoveryWire::Beacon { .summary = FleetSummary { .clusterId = "c-office",
                                                              .state = FleetState::Established,
                                                              .nodeId = std::format("n-{}", index),
                                                              .raftEndpoint = "10.0.0.66:6680" } }));
        (void) office.service.PumpOnce(1ms);
    }

    Drain(peer);
    CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);
}

TEST_CASE("Sixteen spoofed fleets never keep a real fleet from being proven", "[cluster][discovery][formation]")
{
    // The reviewer's second probe, kept. Sixteen invented cluster ids re-sent once a minute fill
    // the table of other fleets; a real fleet arriving after them displaces the oldest that has
    // proven nothing, is challenged, answers, and holds its slot from then on.
    Fixture fix;
    auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
    auto& lab = fix.NodeIn("c-lab", "n-lab", "10.0.0.3:6680", FleetState::Established);
    auto const flood = fix.Sender("10.0.0.66:6681");
    auto labChallenged = 0;
    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        for (auto const index: std::views::iota(0, static_cast<int>(MaxForeignFleets)))
        {
            flood.Send(DiscoveryWire::EncodeBeacon(
                DiscoveryWire::Beacon { .summary = FleetSummary { .clusterId = std::format("c-spoof-{}", index),
                                                                  .state = FleetState::Solitary,
                                                                  .nodeId = "n-x",
                                                                  .raftEndpoint = "10.0.0.66:6680" } }));
            (void) laptop.service.PumpOnce(1ms);
        }
        REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
        if (laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen)
            ++labChallenged;
        fix.clock.advance(60s);
        laptop.service.Maintain();
    }
    CHECK(labChallenged == 4);

    // And once it answers, its slot is its own: a round of sixteen more invented fleets displaces
    // other invented fleets, never the one that proved. The rounds above left the lab four stale
    // challenges it never read, and their answers name cookies whose window has closed: both
    // inboxes are emptied first, so the exchange below is one fresh question and its answer.
    Drain(lab);
    Drain(laptop);
    REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    REQUIRE(laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    Drain(lab);
    REQUIRE(laptop.service.PumpOnce(1ms) == DiscoveryEvent::ForeignFleetProven);
    for (auto const index: std::views::iota(100, 100 + static_cast<int>(MaxForeignFleets)))
    {
        fix.clock.advance(1s);
        flood.Send(DiscoveryWire::EncodeBeacon(
            DiscoveryWire::Beacon { .summary = FleetSummary { .clusterId = std::format("c-spoof-{}", index),
                                                              .state = FleetState::Solitary,
                                                              .nodeId = "n-x",
                                                              .raftEndpoint = "10.0.0.66:6680" } }));
        (void) laptop.service.PumpOnce(1ms);
    }
    REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(laptop.service.PumpOnce(1ms) != DiscoveryEvent::Ignored); // still remembered, so challenged again
    CHECK(laptop.directory.ForeignFleets() == MaxForeignFleets);
}

TEST_CASE("Sixteen fleets proven under fresh keys hold every foreign slot, and that is said once a minute",
          "[cluster][discovery][formation][security]")
{
    // The reviewer's probe P7, kept: the residual inside trust-on-first-use. Every slot held by a
    // fleet that PROVED itself -- a key and an answering host each -- so a real fleet arriving now
    // is not recorded. Not a spoofed datagram's doing, so unlike every other bound it is said, by
    // the address the beacon came from and nothing it claimed, and throttled.
    Fixture fix;
    auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
    auto& lab = fix.NodeIn("c-lab", "n-lab", "10.0.0.3:6680", FleetState::Established);
    auto const labSource = FormatHostPort(lab.socket->boundAddress().host, lab.socket->boundAddress().port);
    auto const said = [&fix] {
        return Warned(fix.logger, "has proven itself");
    };

    // The control first: with room in the table, the lab's beacon is recorded, challenged and unsaid.
    REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    CHECK(said() == 0);
    // Then it is forgotten -- its beacons stop for longer than the directory remembers one.
    Drain(lab);
    Drain(laptop);
    fix.clock.advance(10min);
    laptop.service.Maintain();
    REQUIRE(laptop.directory.ForeignFleets() == 0);

    for (auto const index: std::views::iota(0, static_cast<int>(MaxForeignFleets)))
    {
        auto& squat = fix.NodeIn(std::format("c-squat-{}", index),
                                 std::format("n-squat-{}", index),
                                 std::format("10.0.2.{}:6680", index + 1),
                                 FleetState::Established);
        REQUIRE(Handshake(laptop, squat) == DiscoveryEvent::ForeignFleetProven);
    }
    REQUIRE(laptop.directory.ForeignFleets() == MaxForeignFleets);
    auto const overBound = laptop.metrics.Read(IMetricsSink::Counter::DiscoveryBeaconsOverBound);

    for ([[maybe_unused]] auto const beacon: std::views::iota(0, 3))
    {
        REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
        CHECK(laptop.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    }
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::DiscoveryBeaconsOverBound) == overBound + 3);
    CHECK(said() == 1);
    CHECK(Warned(fix.logger, labSource) == 1);
    CHECK(Warned(fix.logger, "c-lab") == 0);

    // A minute on, the next is said, with how many it stands for.
    fix.clock.advance(DiscoveryService::ForeignTableFullReportInterval);
    REQUIRE(lab.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(laptop.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
    CHECK(said() == 2);
    CHECK(Warned(fix.logger, "3 such beacon(s) since the last report") == 1);
}

TEST_CASE("A learner's beacon with no raft endpoint is recorded and never desired", "[cluster][discovery][formation]")
{
    // A learner dials in and answers nothing, so its beacon names no Raft endpoint. It is still
    // a machine the roster knows, and proves its key like any member; what is DESIRED from the
    // directory is the discovery tier's question, which skips a peer with nothing to dial.
    Fixture fix;
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    auto& learner = fix.NodeIn("c-office", "n-learner", "", FleetState::Established);

    CHECK(Handshake(office, learner) == DiscoveryEvent::PeerAuthenticated);
    auto const proved = office.directory.AuthenticatedPeers();
    REQUIRE(proved.size() == 1);
    CHECK(proved.front().nodeId == "n-learner");
    CHECK(proved.front().raftEndpoint.empty());
}

TEST_CASE("A node never announces an endpoint that reaches only the machine dialling it", "[cluster][discovery][formation]")
{
    // Every peer resolves `localhost` and `127.0.0.1` to ITSELF, so a beacon naming one would send
    // each of them to dial itself, confidently, with no error at either end. Withheld at the one
    // door every announcement passes -- the beacon and the answer to a challenge alike -- so no
    // configuration can put such an endpoint on the wire.
    for (auto const* const endpoint: { "127.0.0.1:6680", "localhost:6680", "[::1]:6680" })
    {
        INFO(endpoint);
        Fixture fix;
        auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
        auto& local = fix.NodeIn("c-local", "n-local", endpoint, FleetState::Solitary);

        CHECK(local.service.SendBeacon().outcome == BeaconSendOutcome::Withheld);
        CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::Nothing);

        // Challenged by a peer that heard it some other way, it answers nothing either.
        REQUIRE(office.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
        REQUIRE(local.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
        REQUIRE(office.service.PumpOnce(1ms) == DiscoveryEvent::Ignored); // its own beacon
        CHECK(office.service.PumpOnce(1ms) == DiscoveryEvent::ChallengeAnswered);
        CHECK(local.service.PumpOnce(1ms) == DiscoveryEvent::ForeignFleetProven);

        // Padded well past any proof here, so what withholds the answer is the endpoint and not the size.
        auto const direct =
            local.socket->send(Unwrap(DiscoveryWire::EncodeChallenge({ .clusterId = "c-office", .nonce = {} }, 512)),
                               local.socket->boundAddress());
        REQUIRE(direct.has_value());
        CHECK(local.service.PumpOnce(1ms) == DiscoveryEvent::Ignored);
        CHECK(local.service.PumpOnce(1ms) == DiscoveryEvent::Nothing);

        // The control: the same node naming an address other machines reach is announced.
        local.self.Set(Claiming("c-local", "n-local", "10.0.0.7:6680"));
        CHECK(local.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    }
}

TEST_CASE("Discovery survives a lost beacon", "[cluster][discovery][service]")
{
    // These are broadcasts and loss is expected. What must not happen is a peer
    // being forgotten because one datagram went missing -- which is why the
    // directory's expiry is generous relative to the beacon interval.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 3 }) };
    NullLogger logger;

    auto const roster = SharedRoster::Of({ "alice", "bob" });
    Node alice { bus, clock, random, logger, "alice", "10.0.0.1:7000", "prod", roster };
    Node bob { bus, clock, random, logger, "bob", "10.0.0.2:7000", "prod", roster };

    REQUIRE(bus.dropNext(AtEndpoint("10.0.0.2:7000"), 1) == 1);

    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::Nothing); // lost
    Drain(alice);

    // The next beacon gets through and the handshake completes.
    REQUIRE(alice.service.SendBeacon().outcome == BeaconSendOutcome::Sent);
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    Drain(alice);
    CHECK(bob.service.PumpOnce(1ms) == DiscoveryEvent::PeerAuthenticated);
    CHECK(bob.directory.AuthenticatedPeers().size() == 1);
}

TEST_CASE("A flood of beacons from one source holds nothing", "[cluster][discovery][service]")
{
    // A beacon is unauthenticated, so anything on the segment can provoke a challenge. No table
    // holds one, so repeating a beacon grows nothing and nothing has to expire.
    core::net::testing::DatagramBus bus;
    core::platform::ManualClock clock;
    ScriptedSecureRandom random { NonceScript({ 1, 2, 3 }) };
    NullLogger logger;

    Node watcher { bus, clock, random, logger, "watcher", "10.0.0.1:7000", "prod", SharedRoster::Of({ "watcher" }) };
    auto noisy = bus.open(AtEndpoint("10.0.0.9:7000"));

    auto const beacon = DiscoveryWire::EncodeBeacon({ .summary = Claiming("prod", "noisy", "10.0.0.9:7000") });
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 5))
    {
        REQUIRE(noisy->send(beacon, AtEndpoint("10.0.0.1:7000")).has_value());
        REQUIRE(watcher.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    }
    CHECK(watcher.service.SpentChallenges() == 0);

    // Each was a challenge of its own -- five cookies, none of them the same -- and one key drew them.
    auto cookies = std::vector<Nonce> {};
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 5))
        cookies.push_back(NextChallenge(*noisy).nonce);
    std::ranges::sort(cookies);
    CHECK(std::ranges::adjacent_find(cookies) == cookies.end());
    CHECK(random.FillCount() == 1);
}

TEST_CASE("The directory, the challenges and the judgement of a proof all follow this node's cluster as it changes",
          "[cluster][discovery][formation]")
{
    // One source for this node's cluster, read live by all three (a dissolve or an adoption changes
    // it): nothing may keep the cluster this node was built in.
    Fixture fix;
    auto& laptop = fix.NodeIn("c-laptop", "n-laptop", "10.0.0.9:6680", FleetState::Solitary);
    auto& office = fix.NodeIn("c-office", "n-office", "10.0.0.1:6680", FleetState::Established);
    REQUIRE(Handshake(laptop, office) == DiscoveryEvent::ForeignFleetProven);
    REQUIRE(laptop.directory.Size() == 0);

    // The laptop joins the office's cluster.
    auto joined = Claiming("c-office", "n-laptop", "10.0.0.9:6680");
    joined.state = FleetState::Established;
    laptop.self.Set(joined);

    // The directory: the office is now this cluster's peer.
    // The judgement of the proof: this cluster's path, which asks the roster -- the laptop's holds
    // no key for n-office yet, so the answer is an unknown key rather than another fleet.
    CHECK(Handshake(laptop, office) == DiscoveryEvent::PeerUnknownKey);
    CHECK(laptop.directory.Size() == 1);
    CHECK(laptop.fleets.proven.size() == 1);

    // The challenge: it names the cluster the laptop is in now.
    auto const asker = fix.Sender("10.0.0.66:6681");
    asker.Send(DiscoveryWire::EncodeBeacon(
        { .summary = FleetSummary { .clusterId = "c-office", .nodeId = "n-other", .raftEndpoint = "10.0.0.66:6680" } }));
    REQUIRE(laptop.service.PumpOnce(1ms) == DiscoveryEvent::PeerSeen);
    // Past its own beacon, which the broadcast doubles back to it.
    auto decoded = std::optional<DiscoveryWire::Challenge> {};
    auto received = asker.socket->receive(1ms);
    while (received.has_value() && !decoded.has_value())
    {
        decoded = DiscoveryWire::DecodeChallenge(received->payload);
        received = asker.socket->receive(1ms);
    }
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).clusterId == "c-office");
}
