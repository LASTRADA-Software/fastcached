// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "ConsensusTier.hpp"
#include "EnrollmentWindow.hpp"
#include "NodeConditions.hpp"
#include "NodeMembership.hpp"
#include "NodeStatusText.hpp"
#include "SchedulerTier.hpp"
#include "WorkerTierTestFixture.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/net/BlockingSocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SkewedMetricsSink.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
namespace Wire = CompileCacheWire;

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
    .worker = true, .scheduler = true, .adminSurface = true, .enrollment = true, .consensus = true
};

/// No component present but the process itself.
constexpr PresentComponents NoComponent {
    .worker = false, .scheduler = false, .adminSurface = false, .enrollment = false, .consensus = false
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
    SECTION("nothing runs but the process, and the process row was never evaluated")
    {
        NodeConditions conditions;
        auto const undecided = conditions.Settle(NoComponent);
        CHECK(undecided == std::vector { NodeCondition::CounterTableSkew });
        CHECK(conditions.StateOf(NodeCondition::CounterTableSkew) == Wire::ConditionState::Undecided);

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
        conditions.Clear(NodeCondition::CounterTableSkew);
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

    // The process scope.
    EvaluateProcessConditions(conditions, metrics);

    // The consensus scope: the membership a consensus node builds, with the registry.
    NodeConfig clustered;
    clustered.fleetMembers = { "10.0.0.7:6674" };
    NodeMembership membership { clustered, membershipLog, &conditions };

    // The scheduler scope: a scheduler, signing with its identity key (#178). It answers no row
    // of its own since unsigned grants went; it is started so a row joining its scope later is
    // asked here.
    NodeConfig scheduling;
    scheduling.serveScheduler = true;
    scheduling.nodeId = "n1";
    core::platform::ManualClock schedulerClock;
    core::platform::ManualWallClock wallClock;
    auto scheduler = SchedulerTier::Start(
        scheduling, membership.Oracle(), schedulerClock, wallClock, metrics, logger, FastCache::Testing::TestKeyPair("n1"));
    REQUIRE(scheduler.has_value());

    // The worker scope.
    auto const workerTier = worker.Start();
    REQUIRE(workerTier.has_value());
    REQUIRE(*workerTier != nullptr);

    // The enrollment scope.
    core::platform::ManualClock windowClock;
    EnrollmentWindow window { windowClock, &conditions };

    // The consensus scope's own tier (#1552): one voter over a state directory of its own,
    // started the way `main` starts it and with the registry, because it answers
    // `unreadable-leader-snapshot` as its driver starts.
    FastCache::Testing::ScratchDirectory consensusState { "conditions-consensus" };
    auto const raftPort = FreePort();
    NodeConfig clusteredNode;
    clusteredNode.nodeId = "n1";
    clusteredNode.raftListen = std::format("127.0.0.1:{}", raftPort);
    clusteredNode.raftPeers = { Unwrap(Cluster::ParseMemberSpec(std::format("n1=127.0.0.1:{}", raftPort))) };
    clusteredNode.clusterDir = consensusState / "state";
    auto const consensus = ConsensusTier::Start(
        clusteredNode,
        {},
        FastCache::Testing::TestKeyPair("n1"),
        [](Distributed::SchedulerRole, std::string_view, std::uint64_t) {},
        [](Cluster::ClusterState const&) {},
        core::platform::defaultSystemWallClock(),
        {},
        metrics,
        logger,
        &conditions);
    REQUIRE(consensus.has_value());

    // The admin surface.
    NodeConfig admin;
    admin.adminListen = std::format("127.0.0.1:{}", FreePort());
    auto const host = MakeSystemHostFacts();
    auto surface = StartAdminSurfaceOrExplain(
        admin, *host, metrics, QuietSnapshot(), std::nullopt, nullptr, AdminCredential {}, logger, conditions);
    REQUIRE(surface.has_value());
    REQUIRE(surface->endpoint != nullptr);

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
