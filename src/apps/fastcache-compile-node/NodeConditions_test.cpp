// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "ConsensusTier.hpp"
#include "DiscoveryTier.hpp"
#include "EnrollmentWindow.hpp"
#include "FormationLoop.hpp"
#include "FormationRuntime.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "NodeMembership.hpp"
#include "NodeProofClient.hpp"
#include "NodeRoster.hpp"
#include "NodeStateFiles.hpp"
#include "NodeStatusText.hpp"
#include "SchedulerReachability.hpp"
#include "SchedulerTier.hpp"
#include "SharedCacheResponder.hpp"
#include "SharedCacheSession.hpp"
#include "SharedCacheUpstream.hpp"
#include "WorkerTierTestFixture.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <latch>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/ManualClockWait.hpp>
#include <tests/NodeFormationControllerFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ReactorHomeFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/SkewedMetricsSink.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
namespace Wire = CompileCacheWire;

/// Where a tier under test says its `0xFC` port answers: what its founding record states. One object
/// for the whole binary, so it outlives every tier a case starts.
/// @return The source.
[[nodiscard]] FastCache::Cc::IAdvertisedEndpointSource const& TestAdvertised()
{
    static FastCache::Node::AnnouncedEndpoint const advertised { "127.0.0.1:6674" };
    return advertised;
}

/// `NodeMembership` reports an unreadable `fleet-open` row here; no case asserts on it.
NullLogger membershipLog;

/// The row @p id names in @p rows.
/// @param rows A snapshot.
/// @param id The row's id.
/// @return The row, or nullptr.
[[nodiscard]] Wire::NodeConditionFields const* RowNamed(std::vector<Wire::NodeConditionFields> const& rows,
                                                        std::string_view id)
{
    auto const found = std::ranges::find(rows, id, &Wire::NodeConditionFields::id);
    return found == rows.end() ? nullptr : &*found;
}

/// Every component present: what `main` passes on a node that runs all of them.
constexpr PresentComponents EveryComponent {
    .worker = true, .scheduler = true, .adminSurface = true, .enrollment = true, .consensus = true, .announces = true
};

/// No component present but the process itself.
constexpr PresentComponents NoComponent {
    .worker = false, .scheduler = false, .adminSurface = false, .enrollment = false, .consensus = false, .announces = false
};

/// A port nothing is bound to, from a probe released at once: `Start` refuses port 0.
/// @return The port.
[[nodiscard]] std::uint16_t FreePort()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();
    return port;
}

/// A snapshot with no cache and a small host, for an admin surface nothing scrapes here.
/// @return The provider.
[[nodiscard]] AdminHttpServer::SnapshotProvider QuietSnapshot()
{
    return [] {
        return MetricsSnapshot { .storage = std::nullopt,
                                 .storageTiers = {},
                                 .host = std::nullopt,
                                 .hostLoad = std::nullopt,
                                 .upstreamConfigured = std::nullopt,
                                 .consensus = std::nullopt,
                                 .uptime = {} };
    };
}

} // namespace

TEST_CASE("Every condition row travels, in table order, whatever its state", "[node][conditions]")
{
    // #1364. The list is the table walked, never a list of what happened to be raised: a list of
    // raised rows cannot tell *checked and benign* from *not evaluated here*, and an empty one
    // cannot tell *nothing raised* from *this build has no conditions*.
    NodeConditions conditions;
    auto const rows = conditions.Snapshot();
    REQUIRE(rows.size() == NodeConditionTable.size());
    for (auto const& row: NodeConditionTable)
    {
        INFO("condition " << row.id);
        auto const* const sent = RowNamed(rows, row.id);
        REQUIRE(sent != nullptr);
        CHECK(sent->persistence == Wire::ConditionName(row.persistence));
        CHECK(sent->severity == Wire::ConditionName(row.severity));
        CHECK(sent->remedy == row.remedy);
        // Nothing has evaluated it, and it says so rather than reading as clear.
        CHECK(sent->state == Wire::ConditionName(Wire::ConditionState::Undecided));
    }
}

TEST_CASE("A live condition clears and raises again; a latched one stays raised", "[node][conditions]")
{
    // The distinction the whole design turns on: a LIVE row clearing is progress an operator can
    // watch, a LATCHED row is fixed for the life of the process. WHAT DISTINGUISHES: the same three
    // calls give the two rows different histories. (A latched clear after a raise is a programmer
    // error and asserted; it is not driven here, because an assert cannot be a passing case.)
    NodeConditions conditions;

    conditions.Raise(NodeCondition::EnrollmentWindowOpen, "open");
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Raised);
    conditions.Clear(NodeCondition::EnrollmentWindowOpen);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);
    conditions.Raise(NodeCondition::EnrollmentWindowOpen, "open again");
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Raised);

    // A latched row may be raised again -- a second observation of a fixed fact -- and says the
    // newer thing; it is never cleared by it.
    conditions.Raise(NodeCondition::ScratchRootUnmappable, "first");
    conditions.Raise(NodeCondition::ScratchRootUnmappable, "second");
    auto const rows = conditions.Snapshot();
    auto const* const latched = RowNamed(rows, RowFor(NodeCondition::ScratchRootUnmappable).id);
    REQUIRE(latched != nullptr);
    CHECK(latched->state == "raised");
    CHECK(latched->detail == "second");
    CHECK(latched->persistence == "latched");
    auto const* const live = RowNamed(rows, RowFor(NodeCondition::EnrollmentWindowOpen).id);
    REQUIRE(live != nullptr);
    CHECK(live->persistence == "live");
}

TEST_CASE("Settle answers a component this node does not run, and names what nothing answered", "[node][conditions]")
{
    // `NotEvaluated` comes from the SCOPE's row, never from each place that decided not to build a
    // component remembering to say so -- and a row whose component RUNS and never said anything is
    // returned, and stays `undecided` rather than being dressed as a neighbour.
    // Every process-scope row, in table order: derived, so a row added to the scope is asked here
    // without this list being remembered.
    auto processRows = std::vector<NodeCondition> {};
    for (auto const& row: NodeConditionTable)
        if (row.scope == ConditionScope::Process)
            processRows.push_back(row.condition);
    REQUIRE(processRows.size() >= 2); // counter-table-skew and surface-not-accepting at least

    SECTION("nothing runs but the process, and the process rows were never evaluated")
    {
        NodeConditions conditions;
        auto const undecided = conditions.Settle(NoComponent);
        CHECK(undecided == processRows);
        for (auto const condition: processRows)
            CHECK(conditions.StateOf(condition) == Wire::ConditionState::Undecided);

        auto const rows = conditions.Snapshot();
        for (auto const& row: NodeConditionTable)
        {
            if (row.scope == ConditionScope::Process)
                continue;
            INFO("condition " << row.id);
            auto const* const sent = RowNamed(rows, row.id);
            REQUIRE(sent != nullptr);
            CHECK(sent->state == "not-evaluated");
            // The reason is the scope's own words, so the reader learns WHICH component is missing.
            CHECK(sent->detail == ConditionScopeTable[static_cast<std::size_t>(row.scope)].notEvaluated);
        }
    }

    SECTION("every component runs and none evaluated its rows")
    {
        NodeConditions conditions;
        for (auto const condition: processRows)
            conditions.Clear(condition);
        auto const undecided = conditions.Settle(EveryComponent);
        // Every scoped row, in table order: the wiring defect named row by row.
        auto expected = std::vector<NodeCondition> {};
        for (auto const& row: NodeConditionTable)
            if (row.scope != ConditionScope::Process)
                expected.push_back(row.condition);
        CHECK(undecided == expected);
    }
}

TEST_CASE("A condition's detail is made text and kept inside its ceiling", "[node][conditions]")
{
    // A scratch path a host spelled in another encoding would otherwise make the leader refuse this
    // machine's whole announcement, which drops it from the fleet over the report of what is wrong.
    NodeConditions conditions;

    SECTION("a byte belonging to no UTF-8 sequence is written, not dropped")
    {
        conditions.Raise(NodeCondition::ScratchRootUnmappable,
                         std::string_view { "/tmp/a\xFF"
                                            "b" });
        auto const rows = conditions.Snapshot();
        auto const* const row = RowNamed(rows, RowFor(NodeCondition::ScratchRootUnmappable).id);
        REQUIRE(row != nullptr);
        CHECK(row->detail == "/tmp/a\\xFFb");
    }

    SECTION("an over-long detail is cut on a code point and says it was cut")
    {
        // Two-byte code points throughout, so a cut on a byte count lands mid-sequence half the time.
        auto long_ = std::string {};
        while (long_.size() < Wire::MaxConditionDetailBytes * 2)
            long_ += "\xC3\xA9";
        conditions.Raise(NodeCondition::ScratchRootUnmappable, long_);
        auto const rows = conditions.Snapshot();
        auto const* const row = RowNamed(rows, RowFor(NodeCondition::ScratchRootUnmappable).id);
        REQUIRE(row != nullptr);
        CHECK(row->detail.size() <= Wire::MaxConditionDetailBytes);
        CHECK(row->detail.ends_with("..."));
        CHECK(IsValidUtf8(row->detail));
    }
}

TEST_CASE("A list detail counts what does not fit rather than cutting a name", "[node][conditions]")
{
    auto hosts = std::vector<std::string> {};
    for (auto const index: std::views::iota(0, 200))
        hosts.push_back(std::format("10.0.{}.{}", index / 250, index % 250));
    auto const detail = ListDetail("names:", hosts);
    CHECK(detail.size() <= Wire::MaxConditionDetailBytes);
    CHECK(detail.starts_with("names: 10.0.0.0, 10.0.0.1"));
    // The tail says how many were left out, and the arithmetic holds: named plus counted is all.
    auto const tail = detail.rfind(" and ");
    REQUIRE(tail != std::string::npos);
    auto const named = static_cast<std::size_t>(std::ranges::count(detail.substr(0, tail), ',')) + 1;
    CHECK(detail.substr(tail) == std::format(" and {} more", hosts.size() - named));
}

TEST_CASE("The process scope asks the scrape's own question about a skewed build", "[node][conditions]")
{
    // #1362's constraint: the startup report is derived from the COUNTED set, never the exported one,
    // so it cannot disagree with the `# SKEW` lines a scrape writes.
    SECTION("a consistent build is checked and benign")
    {
        AtomicMetricsSink metrics;
        NodeConditions conditions;
        EvaluateProcessConditions(conditions, metrics);
        CHECK(conditions.StateOf(NodeCondition::CounterTableSkew) == Wire::ConditionState::Clear);
    }

    SECTION("a sink with no slot for a row raises it, naming the series")
    {
        auto const missing = IMetricsSink::Counter::ConnectionsAdmissionRejected;
        FastCache::Testing::SkewedMetricsSink metrics { missing };
        NodeConditions conditions;
        EvaluateProcessConditions(conditions, metrics);
        CHECK(conditions.StateOf(NodeCondition::CounterTableSkew) == Wire::ConditionState::Raised);
        auto const rows = conditions.Snapshot();
        auto const* const row = RowNamed(rows, RowFor(NodeCondition::CounterTableSkew).id);
        REQUIRE(row != nullptr);
        auto const* const descriptor = DescriptorOf(missing);
        REQUIRE(descriptor != nullptr);
        CHECK(row->detail.contains(descriptor->prometheusName));
    }
}

TEST_CASE("Every condition row is evaluated on a fully configured node", "[node][conditions][wiring]")
{
    // #1364's wiring clause. Each component this node can run is started the way `main` starts it,
    // against ONE registry, and then the table is walked: no row may be left `undecided`. A row added
    // to the table without an evaluator in the component its scope names fails HERE, by name, rather
    // than shipping as a row every surface reports as nobody's decision.
    //
    // The same startup also runs `Settle` the way `main` does, so a component that forgot its row and
    // one that answered it are told apart by the list `Settle` returns as well as by the states.
    //
    // ONE registry for every component, and it is the worker fixture's, because that fixture is how a
    // worker tier is started for a test: the other components borrow it exactly as `main`'s borrow
    // the one it declares.
    WorkerTierTesting::WorkerTierFixture worker;
    auto& conditions = worker.conditions;
    NullLogger logger;
    AtomicMetricsSink metrics;

    // The process scope: the catalogue, the host name this node is dialled at, and how its state
    // directory replaces a file -- evaluated where `main` evaluates them, the second over the
    // configuration it runs and the third over a state directory, through this machine's files.
    EvaluateProcessConditions(conditions, metrics);
    EvaluateHostNameCondition(conditions, Testing::FirstStart(NodeConfig {}));
    FastCache::Testing::ScratchDirectory const replaceState { "conditions-replace-route" };
    Consensus::SystemDurableFiles replaceFiles;
    Platform::SystemReplacingRename const replaceRename;
    REQUIRE(ReportReplaceRoute(replaceState.Path(), replaceFiles, replaceRename, logger, metrics, conditions).has_value());

    // The consensus scope: the membership a consensus node builds.
    auto clustered = Testing::FirstStart(NodeConfig {});
    NodeMembership membership { clustered, membershipLog };

    // And the consensus scope's prover, built as `main` builds it on a consensus node that announces
    // (`AwaitsItsOwnRecord`) -- over that node's roster and its own membership -- which answers
    // `own-record-awaited`.
    NodeConfig proving;
    proving.raftListen = "127.0.0.1:6680";
    // Its roster answers no condition here: the consensus scope's own roster, below, is the one
    // built as `main` builds it.
    core::platform::ManualClock proverRosterClock;
    auto const proverRoster = NodeRoster::Build(proving, proverRosterClock, nullptr);
    REQUIRE(proverRoster.has_value());
    auto const proverKey = FastCache::Testing::TestKeyPair("n1");
    FastCache::Testing::ScriptedSecureRandom proofRandom;
    NodeProofClient const prover { "n1", proverKey, *Unwrap(proverRoster), &membership, &conditions, proofRandom };

    // The scheduler scope: a scheduler, signing with its identity key (#178), started the way `main`
    // starts it -- with the registry -- because it answers its fleet-wide rows as it starts.
    auto scheduling = Testing::FirstStart(NodeConfig {});
    scheduling.nodeId = "n1";
    core::platform::ManualClock schedulerClock;
    core::platform::ManualWallClock wallClock;
    auto scheduler = SchedulerTier::Start(scheduling,
                                          membership.Oracle(),
                                          schedulerClock,
                                          wallClock,
                                          metrics,
                                          logger,
                                          FastCache::Testing::TestKeyPair("n1"),
                                          conditions,
                                          SchedulerConditionInterval);
    REQUIRE(scheduler.has_value());

    // The worker scope.
    auto const workerTier = worker.Start();
    REQUIRE(workerTier.has_value());
    REQUIRE(*workerTier != nullptr);

    // The enrollment scope.
    core::platform::ManualClock windowClock;
    EnrollmentWindow window { windowClock, &conditions, &metrics, wallClock };

    // The consensus scope's own tier (#1552): one voter over a state directory of its own,
    // started the way `main` starts it and with the registry, because it answers
    // `unreadable-leader-snapshot` as its driver starts.
    FastCache::Testing::ScratchDirectory consensusState { "conditions-consensus" };
    // Held and handed to the tier, so the peer port is not free between choosing and serving it.
    auto raftHeld = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(raftHeld);
    REQUIRE(raftHeld->IsBound());
    auto const raftPort = raftHeld->boundPort();
    auto clusteredNode = Testing::FirstStart(NodeConfig {});
    clusteredNode.nodeId = "n1";
    clusteredNode.raftListen = std::format("127.0.0.1:{}", raftPort);
    clusteredNode.raftSelf = "127.0.0.1";
    clusteredNode.clusterDir = consensusState / "state";
    clusteredNode.identityPublicKey = FastCache::Testing::TestKeyPair("n1").PublicKey(); // what a formation states
    // Its lease roster, built as `main` builds it: it answers `consensus-leader-silent`, from the
    // start and at every consensus pass.
    core::platform::SteadyClock const rosterClock;
    auto const roster = NodeRoster::Build(clusteredNode, rosterClock, &conditions);
    REQUIRE(roster.has_value());
    auto const consensus = ConsensusTier::Start(
        clusteredNode,
        TestAdvertised(),
        FastCache::Testing::TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [&roster](Cluster::ClusterState const& state) { (*roster)->Applied(state); },
        [&roster](Distributed::LeaderReading const& reading) { (*roster)->ConsensusPass(reading); },
        metrics,
        logger,
        &conditions,
        FormationHooks {},
        std::move(raftHeld));
    REQUIRE(consensus.has_value());

    // Discovery, started beside consensus the way `main` starts it: it answers
    // `foreign-fleet-visible` -- here as not evaluated, since this consensus port is loopback and
    // is never announced, which is a component answering its row rather than one forgetting it.
    FixedFleetSummary const answered { AnsweredFleetSummary(clusteredNode) };
    auto const discovery =
        StartDiscoveryOrExplain(clusteredNode, *consensus, answered, conditions, metrics, logger, DiscoveryFormation {});
    REQUIRE(discovery.has_value());

    // The consensus scope's shared cache, built the way `main` builds it on a consensus node: it
    // answers `shared-cache-unavailable` from the start, before any state has named this machine.
    core::platform::ManualClock sharedCacheClock;
    SharedCacheService sharedCache { clusteredNode, membership.Oracle(), sharedCacheClock,   metrics,
                                     logger,        &conditions,         ReconcileOn::Caller };

    // And its reading side, the upstream a node the setting does not name reads through: it answers
    // `shared-cache-unproven` from its construction, since nothing has failed to reach one yet.
    auto const readerKey = FastCache::Testing::TestKeyPair("n1");
    SystemSecureRandom readerRandom;
    NamedMachineTrust const readerTrust { "n1", readerKey.PublicKey() };
    NodeProofClient const reader { "n1", readerKey, readerTrust, nullptr, nullptr, readerRandom };
    core::net::BlockingConnector readerConnector;
    SharedCacheDirectory sharedDirectory { "n1", {} };
    SharedCacheDialer sharedDialer { reader, readerConnector, nullptr, metrics, SharedCacheDialPolicy {} };
    core::platform::ManualClock sharedSessionClock;
    Testing::UnturnedLoopHome sharedSessionHome;
    SharedSessionPool sharedSessions { sharedDialer, sharedSessionClock, sharedSessionHome };
    SharedCacheUpstream const sharedUpstream { sharedDirectory, sharedSessions,          nullptr, metrics, &conditions,
                                               logger,          SharedCacheDialPolicy {} };

    // The formation, built the way `main` builds it over the consensus node: it answers
    // `formation-move-refused` as its controller starts.
    FastCache::Testing::InMemoryFormationStore formationStore;
    Cluster::FleetEndpointsFile formationEndpoints { consensusState.Path() };
    ReformRequest reform;
    core::platform::ManualWallClock formationWall;
    FastCache::Testing::ScriptedSecureRandom formationRandom { FastCache::Testing::ScriptedSecureRandom::Ascending(64) };
    FastCache::Testing::UnreachableDialer formationDialer;
    FastCache::Testing::ScriptedSrvResolver formationSrv;
    FastCache::Testing::ManualClockWait formationWait;
    auto const formationBody =
        FormationBody { .record = FastCache::Testing::Minted("c-n1", 500),
                        .durables = FormationDurables { formationStore,
                                                        formationEndpoints,
                                                        reform,
                                                        FormationRuntimeParts { .wall = formationWall,
                                                                                .random = formationRandom,
                                                                                .dialer = formationDialer,
                                                                                .srv = formationSrv,
                                                                                .wait = formationWait } },
                        .reloader = nullptr };
    auto const formationKey = std::optional { FastCache::Testing::TestKeyPair("n1") }; // what its Enroll is signed with
    auto const formation = MakeFormationRuntime(
        clusteredNode, formationKey, formationBody, TestAdvertised(), nullptr, metrics, logger, &conditions);
    REQUIRE(formation.has_value());
    REQUIRE(*formation != nullptr);

    // The admin surface.
    auto admin = Testing::FirstStart(NodeConfig {});
    admin.adminListen = std::format("127.0.0.1:{}", FreePort());
    auto const host = MakeSystemHostFacts();
    // The node's accept-loop registry, watched the way `main` watches it: the process-scope row
    // `surface-not-accepting` is answered by `WatchAcceptLoops`, and the admin surface reads it.
    core::net::AcceptLoopHealth acceptLoops;
    WatchAcceptLoops(acceptLoops, conditions);
    auto surface = StartAdminSurfaceOrExplain(
        admin, *host, metrics, QuietSnapshot(), std::nullopt, nullptr, AdminCredential {}, logger, conditions, acceptLoops);
    REQUIRE(surface.has_value());
    REQUIRE(surface->endpoint != nullptr);

    // The announce scope: the one tracker both announce loops share, with the registry, as `main` builds it.
    core::platform::ManualClock reachabilityClock;
    SchedulerReachability reachability { reachabilityClock, &conditions };

    CHECK(conditions.Settle(EveryComponent).empty());
    for (auto const& row: NodeConditionTable)
    {
        INFO("condition " << row.id << ", scope " << static_cast<int>(row.scope));
        CHECK(conditions.StateOf(row.condition) != Wire::ConditionState::Undecided);
    }
}

TEST_CASE("Every surface renders every row the table holds, walking what arrived", "[node][conditions][renderers]")
{
    // #1364: no renderer restates the list. WHAT DISTINGUISHES: the rows here are the node's REAL
    // table, every one raised, so a renderer that carried its own list of ids -- or filtered to the
    // ones it knew -- misses whichever row joined the table after it was written. The ids asserted
    // are read off the table, never written out beside it.
    NodeConditions conditions;
    for (auto const& row: NodeConditionTable)
        conditions.Raise(row.condition, std::format("observed for {}", row.id));
    auto const rows = conditions.Snapshot();

    // `node`'s record line, and the mentions the `node` panel draws from.
    auto const line = Unwrap(Cli::DescribeConditions(rows));
    auto const mentions = Unwrap(Cli::ConditionsAskingForAttention(rows));
    CHECK(mentions.size() == NodeConditionTable.size());

    // The leader's three documents, from one machine that announced this list.
    Distributed::FleetSnapshot snapshot;
    snapshot.role = Distributed::SchedulerRole::Leader;
    snapshot.nodes = { Distributed::NodeReport { .endpoint = "10.0.0.2:6674",
                                                 .fingerprints = {},
                                                 .capacity = {},
                                                 .load = {},
                                                 .registeredSlots = std::nullopt,
                                                 .fleetJobsInFlight = 0,
                                                 .heartbeatAge = {},
                                                 .version = {},
                                                 .displayName = {},
                                                 .conditions = rows } };
    auto const json = Distributed::RenderFleetJson(snapshot, Distributed::FleetHistoryView {});
    auto const text =
        Distributed::RenderFleetText(snapshot, Distributed::FleetHistoryView {}, Distributed::FleetSection::Conditions);
    auto const page = Distributed::RenderFleetHtml(snapshot, Distributed::FleetHistoryView {}, 0);

    for (auto const& row: NodeConditionTable)
    {
        INFO("condition " << row.id);
        CHECK(line.contains(row.id));
        CHECK(std::ranges::any_of(mentions, [&row](Cli::ConditionMention const& m) { return m.id == row.id; }));
        CHECK(json.contains(row.id));
        CHECK(text.contains(row.id));
        CHECK(page.contains(row.id));
        // The remedy is text the NODE sent, so it reaches the documents as written.
        CHECK(text.contains(row.remedy));
    }
}

TEST_CASE("A surface whose accept loop stops raises surface-not-accepting naming it", "[node][conditions][accept-loop]")
{
    // What an operator reads over the fleet page or `node-conditions` when a port listens and
    // refuses -- the state the installed node sat in for nine hours with nothing saying so.
    NodeConditions conditions;
    core::net::AcceptLoopHealth acceptLoops;
    WatchAcceptLoops(acceptLoops, conditions);
    CHECK(conditions.StateOf(NodeCondition::SurfaceNotAccepting) == Wire::ConditionState::Clear);

    acceptLoops.record(core::net::AcceptLoopEvent {
        .surface = "node", .line = "bad handle (AcceptEx)", .error = {}, .kind = core::net::AcceptLoopEventKind::GaveUp });
    acceptLoops.record(core::net::AcceptLoopEvent {
        .surface = "raft", .line = "cancelled (accept)", .error = {}, .kind = core::net::AcceptLoopEventKind::GaveUp });

    CHECK(conditions.StateOf(NodeCondition::SurfaceNotAccepting) == Wire::ConditionState::Raised);
    auto const rows = conditions.Snapshot();
    auto const* const row = RowNamed(rows, RowFor(NodeCondition::SurfaceNotAccepting).id);
    REQUIRE(row != nullptr);
    CHECK(row->detail.contains("node (bad handle (AcceptEx))"));
    CHECK(row->detail.contains("raft (cancelled (accept))"));
    // A loop that ended does not start again, so the row cannot clear while this process runs.
    CHECK(row->persistence == "latched");
    // And a surface that gave up is not a degraded one.
    CHECK(conditions.StateOf(NodeCondition::SurfaceAcceptDegraded) == Wire::ConditionState::Clear);
}

TEST_CASE("A degraded accept loop raises surface-accept-degraded until it recovers", "[node][conditions][accept-loop]")
{
    // WHAT DISTINGUISHES: a degraded surface is LIVE -- it clears when an accept succeeds, which a
    // stopped one never does -- so the two must be separate rows, and a recovery must clear this one
    // while leaving a stopped surface's row raised.
    NodeConditions conditions;
    core::net::AcceptLoopHealth acceptLoops;
    WatchAcceptLoops(acceptLoops, conditions);
    auto const report = [&acceptLoops](std::string surface, core::net::AcceptLoopEventKind kind) {
        acceptLoops.record(core::net::AcceptLoopEvent {
            .surface = std::move(surface), .line = "32 accepts in a row failed", .error = {}, .kind = kind });
    };

    report("node", core::net::AcceptLoopEventKind::Degraded);
    CHECK(conditions.StateOf(NodeCondition::SurfaceAcceptDegraded) == Wire::ConditionState::Raised);
    CHECK(conditions.StateOf(NodeCondition::SurfaceNotAccepting) == Wire::ConditionState::Clear);
    auto const rows = conditions.Snapshot();
    auto const* const row = RowNamed(rows, RowFor(NodeCondition::SurfaceAcceptDegraded).id);
    REQUIRE(row != nullptr);
    CHECK(row->detail.contains("node (32 accepts in a row failed)"));
    CHECK(row->persistence == "live");

    report("raft", core::net::AcceptLoopEventKind::GaveUp);
    report("node", core::net::AcceptLoopEventKind::Recovered);
    CHECK(conditions.StateOf(NodeCondition::SurfaceAcceptDegraded) == Wire::ConditionState::Clear);
    CHECK(conditions.StateOf(NodeCondition::SurfaceNotAccepting) == Wire::ConditionState::Raised);

    // A degraded surface its owner shut down leaves nothing degraded either.
    report("admin", core::net::AcceptLoopEventKind::Degraded);
    REQUIRE(conditions.StateOf(NodeCondition::SurfaceAcceptDegraded) == Wire::ConditionState::Raised);
    report("admin", core::net::AcceptLoopEventKind::Stopped);
    CHECK(conditions.StateOf(NodeCondition::SurfaceAcceptDegraded) == Wire::ConditionState::Clear);
}

TEST_CASE("surface-accept-degraded stays raised while any surface is degraded, and clears with the last",
          "[node][conditions][accept-loop]")
{
    // WHAT DISTINGUISHES: two surfaces degraded at once. A watcher that cleared on ANY Recovered, or
    // that read only the event it was handed, passes every one-surface case and fails here.
    NodeConditions conditions;
    core::net::AcceptLoopHealth acceptLoops;
    WatchAcceptLoops(acceptLoops, conditions);
    auto const report = [&acceptLoops](std::string surface, core::net::AcceptLoopEventKind kind) {
        acceptLoops.record(core::net::AcceptLoopEvent {
            .surface = std::move(surface), .line = "32 accepts in a row failed", .error = {}, .kind = kind });
    };
    auto const detail = [&conditions] {
        auto const rows = conditions.Snapshot();
        auto const* const row = RowNamed(rows, RowFor(NodeCondition::SurfaceAcceptDegraded).id);
        REQUIRE(row != nullptr);
        return row->detail;
    };

    report("node", core::net::AcceptLoopEventKind::Degraded);
    report("raft", core::net::AcceptLoopEventKind::Degraded);
    report("node", core::net::AcceptLoopEventKind::Recovered);
    CHECK(conditions.StateOf(NodeCondition::SurfaceAcceptDegraded) == Wire::ConditionState::Raised);
    CHECK(detail().contains("raft ("));
    CHECK_FALSE(detail().contains("node ("));

    report("raft", core::net::AcceptLoopEventKind::Recovered);
    CHECK(conditions.StateOf(NodeCondition::SurfaceAcceptDegraded) == Wire::ConditionState::Clear);
}

namespace
{
/// How many rounds the two-loop case runs: each is one chance at the race, at its last events.
constexpr auto AcceptLoopRaceRounds = 400;
/// How many events each loop records per round.
constexpr auto AcceptLoopRaceFlips = 25;
} // namespace

TEST_CASE("Two loops reporting at once leave surface-accept-degraded as the registry finally reads",
          "[node][conditions][accept-loop][concurrency]")
{
    // The registry calls its listeners outside its lock, from each loop's own thread. Two loops flip
    // their surfaces between Degraded and Recovered at once; whatever order their listeners ran in,
    // the row must end where the registry ends. Many short rounds rather than one long one: the
    // race can only bite at a round's LAST events, so each round is one more chance at it.
    auto wrong = std::vector<std::string> {};
    for (auto const round: std::views::iota(0, AcceptLoopRaceRounds))
    {
        NodeConditions conditions;
        core::net::AcceptLoopHealth acceptLoops;
        WatchAcceptLoops(acceptLoops, conditions);
        // Each loop's last event alternates by round, so every ending -- both degraded, one, none --
        // is the answer some rounds must reach.
        auto const flip = [&acceptLoops](std::string const& surface, bool endsDegraded, std::latch& start) {
            start.arrive_and_wait();
            for (auto const index: std::views::iota(0, AcceptLoopRaceFlips))
            {
                auto const degraded = (index % 2 == 0) == endsDegraded;
                acceptLoops.record(core::net::AcceptLoopEvent { .surface = surface,
                                                                .line = "failed accepts",
                                                                .error = {},
                                                                .kind = degraded
                                                                            ? core::net::AcceptLoopEventKind::Degraded
                                                                            : core::net::AcceptLoopEventKind::Recovered });
            }
        };
        std::latch start { 2 };
        {
            std::jthread const a { flip, std::string { "node" }, round % 2 == 1, std::ref(start) };
            std::jthread const b { flip, std::string { "raft" }, (round / 2) % 2 == 1, std::ref(start) };
        }
        auto const anyDegraded = std::ranges::any_of(acceptLoops.snapshot(), [](auto const& surface) {
            return surface.kind == core::net::AcceptLoopEventKind::Degraded;
        });
        auto const state = conditions.StateOf(NodeCondition::SurfaceAcceptDegraded);
        if (state != (anyDegraded ? Wire::ConditionState::Raised : Wire::ConditionState::Clear))
            wrong.push_back(std::format("round {}: the registry {} a degraded surface, the row reads {}",
                                        round,
                                        anyDegraded ? "holds" : "holds no",
                                        static_cast<int>(state)));
    }
    auto shown = std::string {};
    for (auto const& line: wrong | std::views::take(5))
        shown += line + "; ";
    INFO("rounds that ended wrong: " << wrong.size() << " of " << AcceptLoopRaceRounds << ", the first: " << shown);
    CHECK(wrong.empty());
}

TEST_CASE("Every flag a condition's remedy names is one this node parses", "[node][conditions]")
{
    // A remedy is the part of a condition an operator ACTS on, and nothing else reads it: the one
    // `shared-cache-unproven` carried sent them to `--node-status`, which no binary parses -- a node's
    // status is `fastcache-cli node`. So every `--flag` a remedy names is a spelling of
    // `NodeOptions()`, the table this binary parses its command line through.
    std::vector<std::string_view> spellings;
    for (auto const& option: NodeOptions())
    {
        spellings.push_back(option.primary);
        if (!option.alias.empty())
            spellings.push_back(option.alias);
    }
    static std::regex const flag { "--[a-z][a-z0-9-]*" };
    std::vector<std::string> named;
    std::vector<std::string> unknown;
    for (auto const& row: NodeConditionTable)
    {
        auto const remedy = std::string { row.remedy };
        for (auto const& match:
             std::ranges::subrange(std::sregex_iterator { remedy.begin(), remedy.end(), flag }, std::sregex_iterator {}))
        {
            named.push_back(match.str());
            if (!std::ranges::contains(spellings, std::string_view { named.back() }))
                unknown.push_back(std::format("{} names {}", row.id, named.back()));
        }
    }

    // The control: the scan found the flags remedies are known to name, so an empty `unknown` is a
    // verdict and not a regex that matched nothing.
    CHECK(std::ranges::contains(named, std::string { "--cluster-status" }));
    CHECK(std::ranges::contains(named, std::string { "--cluster-forget" }));
    for (auto const& line: unknown)
        UNSCOPED_INFO(line);
    CHECK(unknown.empty());
    CHECK(std::string_view { NodeConditionTable[static_cast<std::size_t>(NodeCondition::SharedCacheUnproven)].remedy }
              .contains("fastcache-cli node"));
}
