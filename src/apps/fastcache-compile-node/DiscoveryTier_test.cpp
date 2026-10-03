// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "DiscoveryTier.hpp"
#include "NodeConditions.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/net/testing/InMemoryDatagram.hpp>
#include <core/platform/Clock.hpp>
#include <tests/CoHostedDatagram.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/NodeConditionFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/RefusingDatagram.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using FastCache::Testing::CoHostedDatagramSocket;
using FastCache::Testing::RosterPeerKeys;
using FastCache::Testing::SharedRoster;
using FastCache::Testing::TestBeaconPort;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;

namespace
{
/// Where one node beacons, and how often.
/// @param beaconAddress Where this node announces itself.
/// @return The configuration.
[[nodiscard]] Cluster::DiscoveryConfig ConfigFor(core::net::DatagramAddress beaconAddress)
{
    return Cluster::DiscoveryConfig { .beaconDestinations =
                                          std::make_shared<Cluster::FixedBeaconDestination const>(std::move(beaconAddress)),
                                      .beaconInterval = 15s,
                                      .challengeLifetime = 30s };
}

/// What one node of fleet @p cluster says about itself: an established member answering Raft at
/// `<id>.local:6675`, or nowhere when @p endpoint says so.
/// @param nodeId Its identity.
/// @param cluster Its fleet.
/// @param endpoint Where it answers Raft; by default `<id>.local:6675`.
/// @return The summary.
[[nodiscard]] FleetSummary SummaryFor(std::string const& nodeId,
                                      std::string const& cluster = "fleet",
                                      std::optional<std::string> const& endpoint = std::nullopt)
{
    return FleetSummary { .clusterId = cluster,
                          .state = FleetState::Established,
                          .nodeId = nodeId,
                          .raftEndpoint = endpoint.value_or(nodeId + ".local:6675") };
}

/// One node's tier, plus what its observers were told.
///
/// Named `Peer` rather than `Node`, which is the namespace this whole file is
/// `using` -- a struct by that name makes every later mention of it ambiguous.
struct Peer
{
    NullLogger logger;
    std::vector<Cluster::DesiredMember> seen;
    AtomicMetricsSink metrics;
    core::platform::SteadyClock clock;
    SystemSecureRandom random;
    Testing::ScriptedSummarySource self;
    NodeConditions conditions;
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets fleets;
    RosterPeerKeys keys;
    std::unique_ptr<DiscoveryTier> tier;

    /// One node per machine, which is the ordinary deployment.
    /// @param bus The segment.
    /// @param nodeId This node's identity; its key is `TestKeyPair(nodeId)`.
    /// @param roster Whose key every id is, as this node's replicated state says.
    Peer(core::net::testing::DatagramBus& bus, std::string const& nodeId, std::shared_ptr<SharedRoster const> roster):
        Peer(bus, SummaryFor(nodeId), std::move(roster))
    {
    }

    /// One node per machine, saying @p summary about itself.
    /// @param bus The segment.
    /// @param summary What it says; its `nodeId` names its key, `TestKeyPair(nodeId)`.
    /// @param roster Whose key every id is, as this node's replicated state says.
    Peer(core::net::testing::DatagramBus& bus, FleetSummary const& summary, std::shared_ptr<SharedRoster const> roster):
        Peer(bus.open(core::net::DatagramAddress { .host = summary.nodeId, .port = TestBeaconPort }),
             core::net::testing::DatagramBus::broadcastAddress(),
             summary,
             std::move(roster))
    {
    }

    /// Over a socket and a beacon address somebody else chose.
    /// @param socket Where this node's datagrams come from and go.
    /// @param beaconAddress Where it announces itself.
    /// @param summary What it says; its `nodeId` names its key, `TestKeyPair(nodeId)`.
    /// @param roster Whose key every id is, as this node's replicated state says.
    Peer(std::unique_ptr<core::net::IDatagramSocket> socket,
         core::net::DatagramAddress beaconAddress,
         FleetSummary const& summary,
         std::shared_ptr<SharedRoster const> roster):
        self { summary },
        keys { TestKeyPair(summary.nodeId), std::move(roster) },
        tier { DiscoveryTier::Over(DiscoveryTier::Parts {
            .socket = std::move(socket),
            .clock = clock,
            .random = random,
            .config = ConfigFor(std::move(beaconAddress)),
            .keys = keys,
            .self = self,
            .conditions = conditions,
            .evidence = evidence,
            .fleets = fleets,
            .onPeers = [this](std::span<Cluster::DesiredMember const> peers) { seen.assign(peers.begin(), peers.end()); },
            .metrics = metrics,
            .logger = logger }) }
    {
    }
};

/// Step both sides @p rounds times.
/// @param a One side.
/// @param b The other.
/// @param rounds How many turns each gets.
void Settle(Peer& a, Peer& b, int rounds);

/// A step short enough that a whole handshake costs milliseconds.
///
/// A timeout rather than a sleep: the bus delivers into an inbox synchronously, so
/// every assertion below is decided by the ORDER of the steps and not by how long
/// any of them waits. What the timeout bounds is only the last step of each side,
/// where there is nothing left to read.
constexpr auto Step = 5ms;

void Settle(Peer& a, Peer& b, int rounds)
{
    for ([[maybe_unused]] auto const round: std::views::iota(0, rounds))
    {
        CHECK(a.tier->Step(Step));
        CHECK(b.tier->Step(Step));
    }
}
} // namespace

TEST_CASE("A discovery bind failure names the reply socket by its port kind", "[node][discovery]")
{
    // Both sockets named, since either can be the one that failed; the reply socket spelled as the
    // worksheet spells it, never read back from the flag.
    auto const beacon = SurfaceEndpoint { .host = "0.0.0.0", .port = 6681, .role = "beacon" };

    auto const kernelChosen = DiscoveryBindFailure(
        beacon,
        SurfaceEndpoint { .host = "0.0.0.0", .port = 0, .portKind = SurfacePortKind::KernelChosen, .role = "reply" },
        "address in use");
    CHECK(kernelChosen
          == "cannot bind the UDP sockets discovery needs: 0.0.0.0:6681 to listen on, and 0.0.0.0:* to answer on, "
             "port chosen by the kernel at bind (address in use)");

    auto const pinned = DiscoveryBindFailure(
        beacon,
        SurfaceEndpoint { .host = "0.0.0.0", .port = 6682, .portKind = SurfacePortKind::Fixed, .role = "reply" },
        "address in use");
    CHECK(pinned
          == "cannot bind the UDP sockets discovery needs: 0.0.0.0:6681 to listen on, and 0.0.0.0:6682 to answer on "
             "(address in use)");
}

TEST_CASE("Two nodes the roster knows find and prove each other", "[node][discovery]")
{
    // The whole handshake, driven by hand: no threads, no sleeps, and a failure
    // names the step it happened at rather than timing out.
    core::net::testing::DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer first { bus, "n1", roster };
    Peer second { bus, "n2", roster };

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
    // endpoint. Discovery proved where `n2` answers CONSENSUS -- that is what the
    // signature covered -- and knows nothing about the port clients speak to, so saying
    // `""` would clear whatever `n2` had announced about itself.
    REQUIRE(first.seen.size() == 1);
    CHECK(first.seen.front().id == "n2");
    CHECK(first.seen.front().raftEndpoint == "n2.local:6675");
    CHECK_FALSE(first.seen.front().schedulerEndpoint.has_value());

    // And no opinion about the KEY either, although one was just proved (#178): it is the
    // key the roster already holds, and a desire outlives the moment it was stated, so a
    // key named here would be proposed straight back over an operator who re-keyed `n2`.
    CHECK_FALSE(first.seen.front().publicKey.has_value());
}

TEST_CASE("A node that cannot name itself is never desired", "[node][discovery]")
{
    // #159 at the far end of the path it travels: what this tier hands its observer
    // becomes `ConsensusTier::Desire`, then a `MembershipProposals` entry, then a
    // `ClusterMember` in replicated state.
    //
    // Stopped at the directory rather than filtered here, so that no proposal is
    // ever GENERATED. One a leader would refuse every pass, forever, is the shape
    // that stalls a cluster: `Reconcile` abandons the pass at the first refusal and
    // never reaches `ReconcileQuorum`.
    core::net::testing::DatagramBus bus;
    // A truncated three-byte sequence -- the shape is right and the bytes stop
    // early -- which reaches the endpoint too, since a node's endpoint is derived
    // from its id here exactly as a bootstrap member's derives from the roster that
    // names it.
    auto const namelessId = std::string { "n2-\xE2\x82" };
    auto const roster = SharedRoster::Of({ "n1", namelessId });
    Peer good { bus, "n1", roster };
    Peer nameless { bus, namelessId, roster };

    // Four rounds is well over the three legs a handshake takes, so this fails as
    // "it was admitted" rather than as "it had not finished yet".
    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        CHECK(good.tier->Step(Step));
        CHECK(nameless.tier->Step(Step));
    }

    // Never seen, so never proved, so never desired. The key was one the roster holds and
    // made no difference: this is a refusal about what the peer CLAIMS rather than about
    // what it holds.
    CHECK(good.tier->AuthenticatedCount() == 0);
    CHECK(good.seen.empty());

    // And the other way round, which is what makes this a one-sided refusal rather
    // than a segment that stops working: the refusal is about what a peer CLAIMS,
    // so the node nobody can name still sees, proves and desires everybody else.
    CHECK(nameless.tier->AuthenticatedCount() == 1);
    REQUIRE(nameless.seen.size() == 1);
    CHECK(nameless.seen.front().id == "n1");
}

TEST_CASE("A beacon from an UNKNOWN key is reported and never desired", "[node][discovery][security]")
{
    // **The acceptance case for #178's discovery half.** Under the shared key a proof WAS
    // membership, so any machine holding the file was desired -- and a desired machine the
    // reconciler would admit. Now a machine proves possession of a key of its own, and
    // proving it perfectly is not enough: the roster does not hold that key for `n2`, so
    // `n2` is seen, counted, reported, and never handed to `Desire`.
    core::net::testing::DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1" });
    Peer honest { bus, "n1", roster };
    Peer stranger { bus, "n2", roster };

    Settle(honest, stranger, 6);

    CHECK(honest.tier->AuthenticatedCount() == 0);
    CHECK(honest.seen.empty());
    CHECK(honest.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) >= 1);
}

TEST_CASE("The same stranger IS desired once the roster holds its key", "[node][discovery][security]")
{
    // The control for the case above, through the same arrangement with ONE fact changed:
    // the roster now holds `n2`'s key. Without it the refusal above passes under a tier
    // that desires nobody at all -- which would also end auto-admission, and would also
    // break every discovered address change.
    core::net::testing::DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1" });
    roster->Admit("n2", TestKeyPair("n2").PublicKey());
    Peer honest { bus, "n1", roster };
    Peer stranger { bus, "n2", roster };

    Settle(honest, stranger, 6);

    CHECK(honest.tier->AuthenticatedCount() == 1);
    REQUIRE(honest.seen.size() == 1);
    CHECK(honest.seen.front().id == "n2");
    CHECK(honest.metrics.Read(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey) == 0);
}

TEST_CASE("A peer whose proven key the roster has since revoked is not desired again", "[node][discovery][security]")
{
    // The authenticated set is re-published whenever ANY peer proves itself, and the
    // directory remembers a proof from whenever it was taken. So what is published is
    // re-asked of the roster at the moment it is published: `n2` proved its key, the key
    // was then revoked, and `n3` proving ITS key must not carry `n2` along with it.
    core::net::testing::DatagramBus bus;
    auto roster = SharedRoster::Of({ "n1", "n2", "n3" });
    Peer first { bus, "n1", roster };
    Peer second { bus, "n2", roster };

    Settle(first, second, 4);
    REQUIRE(first.seen.size() == 1);
    REQUIRE(first.seen.front().id == "n2");

    roster->Revoke("n2");

    Peer third { bus, "n3", roster };
    Settle(first, third, 4);

    REQUIRE_FALSE(first.seen.empty());
    CHECK(std::ranges::any_of(first.seen, [](Cluster::DesiredMember const& m) { return m.id == "n3"; }));
    CHECK(std::ranges::none_of(first.seen, [](Cluster::DesiredMember const& m) { return m.id == "n2"; }));
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
    core::net::testing::DatagramBus bus;
    auto const beacon = core::net::testing::DatagramBus::broadcastAddressOn(TestBeaconPort);

    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer first { CoHostedDatagramSocket(bus, "host", 40001), beacon, SummaryFor("n1"), roster };
    Peer second { CoHostedDatagramSocket(bus, "host", 40002), beacon, SummaryFor("n2"), roster };

    // Each announces itself, then reads the other's beacon and challenges it, then
    // answers the challenge it was given, then checks the proof it was sent. The
    // extra rounds are slack: a shared-port socket looks at one of its halves and
    // waits on the other, so a datagram can need one more step to be reached.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 8))
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

TEST_CASE("A peer discovery proves is recorded as a learner", "[node][discovery][learner]")
{
    // #1535, from the proof to the command a leader proposes: what this tier hands its
    // observer is what `ConsensusTier::Desire` stores and `MembershipProposals` decides
    // from. `n1` bootstrapped alone and `n2` has never been recorded or counted, so the
    // proof buys `n2` a learner's record -- replicated to, counted by nothing -- and a
    // vote stays the operator's to give.
    //
    // Since #178 the proof counts only because `n2`'s KEY is one `n1` already holds -- here
    // the roster knows it, as a bootstrap roster naming `n2` with its key makes it known
    // before the state records any `n2`. A key nobody holds is reported and never desired
    // (the UNKNOWN-key case above), so this path records a learner and admits nobody new.
    core::net::testing::DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer first { bus, "n1", roster };
    Peer second { bus, "n2", roster };

    for ([[maybe_unused]] auto const round: std::views::iota(0, 4))
    {
        CHECK(first.tier->Step(Step));
        CHECK(second.tier->Step(Step));
    }
    REQUIRE(first.seen.size() == 1);

    auto const plan = Cluster::MembershipProposals(
        Cluster::ClusterState {}, Consensus::Configuration { .voters = { "n1" }, .learners = {} }, first.seen);

    REQUIRE(plan.proposals.size() == 1);
    CHECK(plan.proposals.front()
          == Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                .key = "n2",
                                .value = "n2.local:6675",
                                .schedulerEndpoint = {},
                                .publicKey = std::nullopt,
                                .role = std::nullopt });
    CHECK(plan.forgotten.empty());
}

TEST_CASE("Two established fleets on one segment prove each other, desire nothing, and raise foreign-fleet-visible",
          "[node][discovery][formation][conditions]")
{
    // A cluster id is routing rather than authentication, and this is what it buys: each side
    // challenges the other and hands what it proved to formation, and a roster that knew both
    // would still admit neither -- the other fleet's proof never reaches it.
    core::net::testing::DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1", "n2" });
    Peer ours { bus, "n1", roster };
    Peer theirs { bus, SummaryFor("n2", "somebody-elses"), roster };
    // Not clear before discovery has listened: nothing has been heard to say "nobody" with.
    CHECK(ours.conditions.StateOf(NodeCondition::ForeignFleetVisible) == CompileCacheWire::ConditionState::NotEvaluated);

    Settle(ours, theirs, 6);

    CHECK(ours.tier->AuthenticatedCount() == 0);
    CHECK(theirs.tier->AuthenticatedCount() == 0);
    CHECK(ours.seen.empty());
    CHECK(theirs.seen.empty());

    // Through the tier's watch and on to the fleet observer, both.
    REQUIRE_FALSE(ours.fleets.proven.empty());
    CHECK(ours.fleets.proven.front().Summary().clusterId == "somebody-elses");
    CHECK(ours.conditions.StateOf(NodeCondition::ForeignFleetVisible) == CompileCacheWire::ConditionState::Raised);
    CHECK(theirs.conditions.StateOf(NodeCondition::ForeignFleetVisible) == CompileCacheWire::ConditionState::Raised);
}

TEST_CASE("A learner that proves its key is never desired, for it has nowhere to be dialled",
          "[node][discovery][formation][learner]")
{
    // A learner dials in and answers no consensus, so its beacon names no Raft endpoint. It
    // proves its key like any member -- and is left out of what is desired, since a desire at no
    // address would ask the leader to record a member nobody can reach.
    core::net::testing::DatagramBus bus;
    auto const roster = SharedRoster::Of({ "n1", "learner" });
    Peer voter { bus, "n1", roster };
    Peer learner { bus, SummaryFor("learner", "fleet", std::string {}), roster };

    Settle(voter, learner, 6);

    CHECK(voter.tier->AuthenticatedCount() == 1);
    CHECK(voter.seen.empty());

    // The control: a learner's view of the voter, which has an endpoint, IS published.
    REQUIRE(learner.seen.size() == 1);
    CHECK(learner.seen.front().id == "n1");
}

TEST_CASE("Discovery on a consensus port bound to loopback stands down when defaulted and is refused when typed",
          "[node][discovery][formation][defaults]")
{
    // A loopback consensus port is a cluster that never leaves this machine, which the startup
    // rules accept. Its beacon would name 127.0.0.1 and send every peer to dial ITSELF, so a
    // defaulted discovery stands down -- saying so, and answering foreign-fleet-visible as not
    // evaluated rather than leaving it undecided -- and a typed one is refused by name.
    NullLogger logger;
    AtomicMetricsSink metrics;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    Testing::ScratchDirectory const scratch { "discovery-loopback-consensus" };

    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.raftListen = std::format("127.0.0.1:{}", port);
    cfg.raftSelf = "127.0.0.1";
    cfg.clusterDir = scratch / "state";
    REQUIRE_FALSE(cfg.discoveryAddress.empty());
    REQUIRE_FALSE(cfg.discoveryAddressExplicit);

    auto started = ConsensusTier::Start(
        cfg,
        {},
        TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        core::platform::defaultSystemWallClock(),
        {},
        metrics,
        logger,
        nullptr,
        FormationHooks {});
    REQUIRE(started.has_value());
    auto const consensus = std::move(*started);

    NodeConditions conditions;
    // The one summary `main` hands both discovery and the FLEET-SUMMARY responder.
    FixedFleetSummary const answered { AnsweredFleetSummary(cfg) };
    auto const defaulted =
        StartDiscoveryOrExplain(cfg, consensus, answered, conditions, metrics, logger, DiscoveryFormation {});
    REQUIRE(defaulted.has_value());
    CHECK(*defaulted == nullptr);
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == CompileCacheWire::ConditionState::NotEvaluated);

    // It asks the summary it was HANDED -- the one the FLEET-SUMMARY responder signs -- and derives
    // none of its own: handed one naming another loopback endpoint, that is the endpoint it names.
    NodeConditions namedConditions;
    FixedFleetSummary const named { ConfiguredFleetSummary(cfg, "localhost:7777") };
    REQUIRE(
        StartDiscoveryOrExplain(cfg, consensus, named, namedConditions, metrics, logger, DiscoveryFormation {}).has_value());
    CHECK(Testing::DetailOf(namedConditions, NodeCondition::ForeignFleetVisible).contains("localhost:7777"));

    auto typed = cfg;
    typed.discoveryAddressExplicit = true;
    NodeConditions typedConditions;
    auto const refused =
        StartDiscoveryOrExplain(typed, consensus, answered, typedConditions, metrics, logger, DiscoveryFormation {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason == DiscoveryAnnouncesOnlyThisMachineRefusal);

    // And the startup table refuses the typed one first, where an install is judged too; the
    // defaulted one it accepts, since standing down is the answer for it.
    CHECK(StartupPolicyRejection(typed) == std::optional { std::string { DiscoveryAnnouncesOnlyThisMachineRefusal } });
    CHECK(StartupPolicyRejection(cfg) != std::optional { std::string { DiscoveryAnnouncesOnlyThisMachineRefusal } });
}

TEST_CASE("A node announces the state its recorded mode announces", "[node][discovery][formation]")
{
    // What discovery says about this node until the formation controller says it: the record's
    // mode decides the state, through the mode table's own column.
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeId = "n1";
    cfg.clusterId = "c-1";
    CHECK(ConfiguredFleetSummary(cfg, "n1.example:6680").state == FleetState::Solitary);
    CHECK(ConfiguredFleetSummary(cfg, "n1.example:6680").raftEndpoint == "n1.example:6680");
    CHECK(ConfiguredFleetSummary(cfg, "n1.example:6680").nodeId == "n1");
    CHECK(ConfiguredFleetSummary(cfg, "n1.example:6680").clusterId == "c-1");

    REQUIRE(cfg.formation.has_value());
    auto voter = Unwrap(cfg.formation);
    voter.mode = Cluster::NodeMode::Voter;
    cfg.formation = std::move(voter);
    CHECK(ConfiguredFleetSummary(cfg, "n1.example:6680").state == FleetState::Established);

    // A pending record points before the controller exists to say where: at nothing, which nobody
    // follows, rather than at a leader of its own a peer would take for the fleet to join.
    auto pending = Unwrap(cfg.formation);
    pending.mode = Cluster::NodeMode::Pending;
    cfg.formation = std::move(pending);
    CHECK(ConfiguredFleetSummary(cfg, "n1.example:6680").state == FleetState::Pending);
    CHECK(ConfiguredFleetSummary(cfg, "n1.example:6680").leaderNodeEndpoint.empty());
}

TEST_CASE("Discovery defaulted on a node running no consensus starts nothing, and typed is refused",
          "[node][discovery][formation][defaults]")
{
    // Discovery is on by default and runs beside consensus only, so a worker that turns
    // consensus off with `--listen-raft=` turns discovery off with it -- it must still start.
    // A `--discovery` the operator TYPED on such a node is the silent no-op the startup table
    // refuses, and this tier refuses it too rather than ignore it.
    AtomicMetricsSink metrics;
    NullLogger logger;
    std::unique_ptr<ConsensusTier> const none;

    NodeConfig worker;
    worker.raftListen.clear();
    REQUIRE_FALSE(worker.discoveryAddress.empty());
    NodeConditions conditions;
    FixedFleetSummary const answered { AnsweredFleetSummary(worker) };
    auto const defaulted =
        StartDiscoveryOrExplain(worker, none, answered, conditions, metrics, logger, DiscoveryFormation {});
    REQUIRE(defaulted.has_value());
    CHECK(*defaulted == nullptr);

    // A node with no consensus leaves the row to its scope, which `Settle` answers.
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == CompileCacheWire::ConditionState::Undecided);

    auto typed = worker;
    typed.discoveryAddressExplicit = true;
    auto const refused = StartDiscoveryOrExplain(typed, none, answered, conditions, metrics, logger, DiscoveryFormation {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason.starts_with("--discovery needs --listen-raft"));
}

TEST_CASE("A named --discovery is beaconed exactly, and an unnamed one on every link's directed broadcast",
          "[node][discovery][beacon]")
{
    // Decided by PROVENANCE: an operator who types the default's own spelling asked for the limited
    // broadcast, so the one row that separates provenance from comparing against the default is
    // `--discovery=255.255.255.255:6681`. Parsed the way production parses it, so the explicit bit is
    // the parser's, not the case's.
    core::platform::ManualClock clock;
    CapturingLogger logger;
    auto const machine = Testing::ScriptedInterfaceAddresses { { Ipv4InterfaceAddress { .interfaceName = "Ethernet",
                                                                                        .address = { 192, 168, 86, 24 },
                                                                                        .prefixLength = 24,
                                                                                        .up = true,
                                                                                        .loopback = false,
                                                                                        .broadcastLink = true } } };
    auto const parsed = [](char const* flag) {
        auto cfg = NodeConfig {};
        auto const args = std::array<char const*, 1> { flag };
        REQUIRE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg).has_value());
        return cfg;
    };
    auto const hostsFor = [&](NodeConfig const& cfg, std::uint16_t port) {
        auto const destinations = BeaconDestinationsFor(cfg, port, machine, clock, logger);
        REQUIRE(destinations.has_value());
        auto hosts = std::vector<std::string> {};
        for (auto const& destination: Unwrap(destinations)->Destinations())
        {
            CHECK(destination.port == port);
            hosts.push_back(destination.host);
        }
        return hosts;
    };

    SECTION("unnamed: every up interface's directed broadcast")
    {
        auto const cfg = NodeConfig {};
        REQUIRE_FALSE(cfg.discoveryAddressExplicit);
        CHECK(hostsFor(cfg, 6681) == std::vector<std::string> { "192.168.86.255" });
    }
    SECTION("named, as the default's own spelling: the limited broadcast, exactly")
    {
        auto const cfg = parsed("--discovery=255.255.255.255:6681");
        REQUIRE(cfg.discoveryAddressExplicit);
        CHECK(hostsFor(cfg, 6681) == std::vector<std::string> { "255.255.255.255" });
    }
    SECTION("named, anything else: exactly that, whatever the interfaces say")
    {
        auto const cfg = parsed("--discovery=10.9.255.255:7000");
        CHECK(hostsFor(cfg, 7000) == std::vector<std::string> { "10.9.255.255" });
    }
    SECTION("named, and not an address: refused by name")
    {
        auto cfg = NodeConfig {};
        cfg.discoveryAddress = "7000";
        cfg.discoveryAddressExplicit = true;
        auto const refused = BeaconDestinationsFor(cfg, 7000, machine, clock, logger);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("--discovery=7000 is not <address>:<port>"));
    }
}

namespace
{
/// How far apart a beating tier's beacons are.
constexpr auto BeatInterval = 15s;

/// One tier on a manual clock with a captured log, so a case can count the lines a run of beats
/// leaves behind: the question every "said once per change" rule is asked by.
struct BeatingTier
{
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    core::platform::ManualClock clock;
    SystemSecureRandom random;
    Testing::ScriptedSummarySource self;
    NodeConditions conditions;
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets fleets;
    RosterPeerKeys keys;
    Testing::ScriptedInterfaceAddresses interfaces;
    std::unique_ptr<DiscoveryTier> tier;

    /// @param socket Where its datagrams go.
    /// @param summary What it says about itself.
    /// @param machine Its interfaces; the beacons go to their directed broadcasts unless @p fixed.
    /// @param fixed Where the beacons go instead, as a named `--discovery` would say.
    BeatingTier(std::unique_ptr<core::net::IDatagramSocket> socket,
                FleetSummary const& summary,
                std::vector<Ipv4InterfaceAddress> machine,
                std::optional<core::net::DatagramAddress> fixed = std::nullopt):
        self { summary },
        keys { TestKeyPair(summary.nodeId), SharedRoster::Of({ summary.nodeId }) },
        interfaces { std::move(machine) }
    {
        auto destinations = fixed.has_value() ? std::shared_ptr<Cluster::IBeaconDestinations const> { std::make_shared<
                                                    Cluster::FixedBeaconDestination const>(*std::move(fixed)) }
                                              : std::shared_ptr<Cluster::IBeaconDestinations const> {
                                                    std::make_shared<Cluster::DirectedBroadcastDestinations const>(
                                                        interfaces, clock, TestBeaconPort, logger)
                                                };
        tier = DiscoveryTier::Over(
            DiscoveryTier::Parts { .socket = std::move(socket),
                                   .clock = clock,
                                   .random = random,
                                   .config = Cluster::DiscoveryConfig { .beaconDestinations = std::move(destinations),
                                                                        .beaconInterval = BeatInterval,
                                                                        .challengeLifetime = 30s },
                                   .keys = keys,
                                   .self = self,
                                   .conditions = conditions,
                                   .evidence = evidence,
                                   .fleets = fleets,
                                   .onPeers = [](std::span<Cluster::DesiredMember const>) {},
                                   .metrics = metrics,
                                   .logger = logger });
    }

    /// Run @p count beats, one beacon interval apart, the first at the current instant.
    /// @param count How many.
    void Beat(int count)
    {
        for ([[maybe_unused]] auto const beat: std::views::iota(0, count))
        {
            CHECK(tier->Step(1ms));
            clock.advance(BeatInterval);
        }
    }

    /// @param needle What a line must contain; empty counts every warning.
    /// @return How many warnings contain @p needle.
    [[nodiscard]] std::size_t Warnings(std::string_view needle = {}) const
    {
        return Lines(LogLevel::Warn, needle);
    }

    /// @param level Which level to count.
    /// @param needle What a line must contain; empty counts every line of @p level.
    /// @return How many lines of @p level contain @p needle.
    [[nodiscard]] std::size_t Lines(LogLevel level, std::string_view needle) const
    {
        auto const records = logger.Snapshot();
        return static_cast<std::size_t>(
            std::ranges::count_if(records, [level, needle](CapturingLogger::Record const& record) {
                return record.level == level && record.message.contains(needle);
            }));
    }
};
} // namespace

TEST_CASE("A machine with no link to beacon on says so once, never once a beat", "[node][discovery][beacon]")
{
    // The reviewer's VPN-only machine: one point-to-point /32 and nothing else, measured at a warning
    // per beat claiming a refusal that never happened. Eight beats leave exactly one warning, the one
    // naming the interface and the remedy -- and no refusal at all, since nothing was ever sent.
    core::net::testing::DatagramBus bus;
    BeatingTier node { bus.open(core::net::DatagramAddress { .host = "vpn-only", .port = TestBeaconPort }),
                       SummaryFor("vpn-only"),
                       { Ipv4InterfaceAddress { .interfaceName = "VPN",
                                                .address = { 10, 99, 0, 2 },
                                                .prefixLength = 32,
                                                .up = true,
                                                .loopback = false,
                                                .broadcastLink = true } } };
    node.Beat(8);
    CHECK(node.Warnings() == 1);
    CHECK(node.Warnings("no interface to beacon on") == 1);
    CHECK(node.Warnings("refused") == 0);
    CHECK(node.Warnings("could not be sent") == 0);
}

TEST_CASE("A withheld announcement is said at its own interval, never once a beat", "[node][discovery][beacon]")
{
    // The older half of the same defect: a summary naming only this machine is withheld, said by the
    // service at most once a minute, and the tier added its own unthrottled line at every beat.
    core::net::testing::DatagramBus bus;
    BeatingTier node { bus.open(core::net::DatagramAddress { .host = "local", .port = TestBeaconPort }),
                       SummaryFor("local", "fleet", "127.0.0.1:6675"),
                       {},
                       core::net::testing::DatagramBus::broadcastAddress() };
    node.Beat(4); // 0s to 45s: inside one withheld-report interval
    CHECK(node.Warnings() == 1);
    CHECK(node.Warnings("withheld this node's announcement") == 1);
}

TEST_CASE("A beacon refused on one link while another takes it is said once per change, naming the link",
          "[node][discovery][beacon]")
{
    // The machine this default exists for, the other way round: the LAN's broadcast refused while the
    // VPN adapter's is taken. Nothing else would say so -- the destinations line lists both, and the
    // beacon counts as sent. Said once while the same link refuses, however the rest changes; and its
    // end said once, when every destination takes it again.
    core::net::testing::DatagramBus bus;
    auto const at = [](std::string name, std::array<std::uint8_t, 4> octets) {
        return Ipv4InterfaceAddress { .interfaceName = std::move(name),
                                      .address = octets,
                                      .prefixLength = 24,
                                      .up = true,
                                      .loopback = false,
                                      .broadcastLink = true };
    };
    auto const lan = at("Ethernet", { 192, 168, 86, 24 });
    auto const vpn = at("vEthernet (VPN)", { 172, 31, 255, 2 });
    auto const wifi = at("Wi-Fi", { 10, 20, 0, 5 });
    auto refusing = std::make_unique<Testing::RefusingDatagramSocket>(
        bus.open(core::net::DatagramAddress { .host = "dual", .port = TestBeaconPort }),
        std::vector<std::string> { "192.168.86.255" });
    auto const& refused = *refusing;
    BeatingTier node { std::move(refusing), SummaryFor("dual"), { lan, vpn } };

    node.Beat(4);
    CHECK(refused.Refused() == 4);
    CHECK(node.Warnings() == 1);
    CHECK(node.Warnings(std::format("refused this node's beacon at 192.168.86.255:{}: permission denied", TestBeaconPort))
          == 1);
    CHECK(node.Warnings(std::format("while 172.31.255.255:{} took it", TestBeaconPort)) == 1);
    CHECK(node.Warnings("at every destination") == 0);

    // Another link that takes it changes nothing about which one refuses: no second line.
    node.interfaces.Publish({ lan, vpn, wifi });
    node.Beat(4);
    CHECK(node.Warnings() == 1);

    // Every link left takes it: the end is said once, after a whole interval with no refusal --
    // the refresh that drops the LAN lands inside these beats, so eight rather than four.
    node.interfaces.Publish({ vpn, wifi });
    node.Beat(8);
    CHECK(node.Warnings() == 1);
    CHECK(node.Lines(LogLevel::Info, "every destination takes this node's beacon again") == 1);
}

namespace
{
/// A LAN and a VPN adapter, each a /24 with a broadcast, as the reviewer's machine had.
[[nodiscard]] std::vector<Ipv4InterfaceAddress> LanAndVpn()
{
    auto const at = [](std::string name, std::array<std::uint8_t, 4> octets) {
        return Ipv4InterfaceAddress { .interfaceName = std::move(name),
                                      .address = octets,
                                      .prefixLength = 24,
                                      .up = true,
                                      .loopback = false,
                                      .broadcastLink = true };
    };
    return { at("Ethernet", { 192, 168, 86, 24 }), at("vEthernet (VPN)", { 172, 31, 255, 2 }) };
}

/// Forty beats: ten minutes at the default interval.
constexpr auto FlapBeats = 40;
/// The most lines of one kind a throttle of `RefusedBeaconReportInterval` allows over `FlapBeats`.
constexpr auto FlapLineBound =
    static_cast<std::size_t>((FlapBeats * BeatInterval) / DiscoveryTier::RefusedBeaconReportInterval) + 1;
} // namespace

TEST_CASE("A link that refuses every other beacon is said once, and its end only after a quiet interval",
          "[node][discovery][beacon]")
{
    // The reviewer's probe, committed: the LAN refuses every other send while the VPN takes every
    // one. Said at the first clean beat, each recovery cleared the refusing set, so the next refusal
    // was a change again -- a Warn and an Info every two beats, 2880 of each a day. The recovery now
    // waits for an interval with no refusal, so the flap is ONE change, said once, and nothing ends
    // until it does.
    core::net::testing::DatagramBus bus;
    auto refusing = std::make_unique<Testing::RefusingDatagramSocket>(
        bus.open(core::net::DatagramAddress { .host = "flapping-lan", .port = TestBeaconPort }),
        std::vector<std::string> { "192.168.86.255" });
    auto& socket = *refusing;
    BeatingTier node { std::move(refusing), SummaryFor("flapping-lan"), LanAndVpn() };

    for (auto const beat: std::views::iota(0, FlapBeats))
    {
        if (beat % 2 == 0)
            socket.Refuse({ "192.168.86.255" });
        else
            socket.RefuseNothing();
        node.Beat(1);
    }
    CHECK(socket.Refused() == static_cast<std::size_t>(FlapBeats / 2));
    CHECK(node.Warnings() == 1);
    CHECK(node.Warnings("while 172.31.255.255") == 1);
    CHECK(node.Warnings() <= FlapLineBound);
    CHECK(node.Lines(LogLevel::Info, "takes this node's beacon again") == 0);

    // The flap stops: one interval with no refusal, and its end is said once.
    socket.RefuseNothing();
    node.Beat(6);
    CHECK(node.Warnings() == 1);
    CHECK(node.Lines(LogLevel::Info, "takes this node's beacon again") == 1);
}

TEST_CASE("A link that flaps between refusing on one link and everywhere is bounded by the interval",
          "[node][discovery][beacon]")
{
    // The other flap direction: the LAN always refuses and the VPN refuses every other beacon, so the
    // outcome alternates between refused everywhere and partly refused. The everywhere line is its
    // own throttle, one a minute; the partial one's set of refusing links is the LAN every time it is
    // asked, one change, so one line -- and never a line per beat, however the two interleave.
    core::net::testing::DatagramBus bus;
    auto refusing = std::make_unique<Testing::RefusingDatagramSocket>(
        bus.open(core::net::DatagramAddress { .host = "flapping-vpn", .port = TestBeaconPort }),
        std::vector<std::string> { "192.168.86.255", "172.31.255.255" });
    auto& socket = *refusing;
    BeatingTier node { std::move(refusing), SummaryFor("flapping-vpn"), LanAndVpn() };
    static_assert(DiscoveryTier::RefusedBeaconReportInterval == 4 * BeatInterval,
                  "the everywhere count below assumes a line every fourth beat");

    for (auto const beat: std::views::iota(0, FlapBeats))
    {
        if (beat % 2 == 0)
            socket.Refuse({ "192.168.86.255", "172.31.255.255" });
        else
            socket.Refuse({ "192.168.86.255" });
        node.Beat(1);
    }
    // Refused everywhere on the even beats, 0s to 570s: a line at 0s, 60s, ... 540s.
    CHECK(node.Warnings("at every destination") == static_cast<std::size_t>(FlapBeats / 4));
    CHECK(node.Warnings("while 172.31.255.255") == 1);
    CHECK(node.Warnings("at every destination") <= FlapLineBound);
    CHECK(node.Warnings("while 172.31.255.255") <= FlapLineBound);
    CHECK(node.Lines(LogLevel::Info, "takes this node's beacon again") == 0);
}

TEST_CASE("Links that keep changing are said at most once an interval, with how many changes a line stands for",
          "[node][discovery][beacon]")
{
    // The partial line's own throttle, which neither flap above reaches once the recovery waits:
    // WHICH link refuses changes at every beat -- the LAN, then the VPN, then the LAN -- while the
    // other always takes it. One line a minute, the second counting the changes it stands for.
    core::net::testing::DatagramBus bus;
    auto refusing = std::make_unique<Testing::RefusingDatagramSocket>(
        bus.open(core::net::DatagramAddress { .host = "alternating", .port = TestBeaconPort }),
        std::vector<std::string> { "192.168.86.255" });
    auto& socket = *refusing;
    BeatingTier node { std::move(refusing), SummaryFor("alternating"), LanAndVpn() };

    for (auto const beat: std::views::iota(0, 5))
    {
        socket.Refuse({ beat % 2 == 0 ? "192.168.86.255" : "172.31.255.255" });
        node.Beat(1);
    }
    // 0s says the first; 15s, 30s and 45s are counted; 60s is due and stands for those and itself.
    CHECK(node.Warnings() == 2);
    CHECK(node.Warnings("(1 change(s) of which links refuse since the last such line") == 1);
    CHECK(node.Warnings("(4 change(s) of which links refuse since the last such line") == 1);
}

TEST_CASE("A beacon refused everywhere is said at most once an interval, with how many it stands for",
          "[node][discovery][beacon]")
{
    // The one outcome the tier reports, and still throttled: a refusal that is not transient
    // repeats at every beat. Four beats inside the interval leave one line; the fifth, at the
    // interval, is the second line and counts the three it stood in for plus itself.
    core::net::testing::DatagramBus bus;
    auto refusing = std::make_unique<Testing::RefusingDatagramSocket>(
        bus.open(core::net::DatagramAddress { .host = "walled", .port = TestBeaconPort }), std::vector<std::string> {});
    auto const& refused = *refusing;
    BeatingTier node { std::move(refusing), SummaryFor("walled"), {}, core::net::testing::DatagramBus::broadcastAddress() };
    static_assert(DiscoveryTier::RefusedBeaconReportInterval == 4 * BeatInterval,
                  "the beat counts below assume the fifth beat lands exactly on the interval");

    node.Beat(4);
    CHECK(refused.Refused() == 4);
    CHECK(node.Warnings() == 1);
    CHECK(node.Warnings("refused this node's beacon at every destination (1 refused since") == 1);
    // With the cause: what the stack answered, at the destination it answered it for.
    CHECK(node.Warnings(std::format("s): {}: permission denied (refused by the test's local stack)",
                                    FormatHostPort(core::net::testing::DatagramBus::broadcastAddress().host,
                                                   core::net::testing::DatagramBus::broadcastAddress().port)))
          == 1);

    node.Beat(1);
    CHECK(refused.Refused() == 5);
    CHECK(node.Warnings() == 2);
    CHECK(node.Warnings("(4 refused since the last such line") == 1);
}

TEST_CASE("The discovery tier refuses a node without consensus in the startup table's words", "[node][discovery]")
{
    // The belt and the braces said different things: the table named `--listen-raft`, the tier
    // `--node-id` -- a flag that has not turned consensus on since #1022, which an operator reading
    // the tier's sentence would add to no effect.
    // TYPED, on a node that turned consensus off: a defaulted `--discovery` there starts nothing.
    NodeConfig cfg;
    cfg.raftListen.clear();
    cfg.discoveryAddress = "255.255.255.255:6681";
    cfg.discoveryAddressExplicit = true;
    AtomicMetricsSink metrics;
    NullLogger logger;
    std::unique_ptr<ConsensusTier> const none;
    NodeConditions conditions;
    FixedFleetSummary const answered { AnsweredFleetSummary(cfg) };

    auto const started = StartDiscoveryOrExplain(cfg, none, answered, conditions, metrics, logger, DiscoveryFormation {});
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().reason == DiscoveryNeedsConsensusRefusal);
    CHECK(started.error().cause == NodeRefusalCause::EarlierRule);
    auto const table = StartupPolicyRejection(cfg);
    REQUIRE(table.has_value());
    CHECK(Unwrap(table) == started.error().reason);
}
