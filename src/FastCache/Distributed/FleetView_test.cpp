// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Distributed/FleetChart.hpp>
#include <FastCache/Distributed/FleetText.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/NodeLoadTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using namespace FastCache::Distributed::Testing;
using FastCache::Testing::Unwrap;

namespace
{

/// A cluster that reports whatever a case put in it.
///
/// The whole `IClusterAdmin` surface, so a fleet report can be exercised without
/// Raft, two threads and a durable log behind it -- the seam that interface exists
/// for.
class FakeCluster final: public IClusterAdmin
{
  public:
    explicit FakeCluster(Cluster::ClusterState state):
        _state { std::move(state) }
    {
    }

    [[nodiscard]] Cluster::ClusterState ClusterState() const override
    {
        return _state;
    }

    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& /*command*/) override
    {
        return {};
    }

  private:
    Cluster::ClusterState _state;
};

/// A machine, spelled once so a case varies only what it is about.
[[nodiscard]] NodeReport Machine(std::string endpoint, std::uint32_t cores)
{
    return NodeReport { .endpoint = std::move(endpoint),
                        .fingerprints = { "gcc-13-abcdef" },
                        .capacity = NodeCapacity { .logicalCores = cores, .totalMemoryBytes = 64ULL << 30 },
                        .load = Busy(1),
                        .registeredSlots = 8,
                        .fleetJobsInFlight = 1,
                        .heartbeatAge = std::chrono::milliseconds { 250 } };
}

/// A snapshot that leads, so the interesting rendering paths are reachable.
[[nodiscard]] FleetSnapshot LeadingSnapshot()
{
    FleetSnapshot snapshot;
    snapshot.role = SchedulerRole::Leader;
    snapshot.nodes = { Machine("10.0.0.2:7100", 32) };
    snapshot.leases = { 100, 7, 5, 3, 2 };
    snapshot.liveLeases = 4;
    snapshot.registrations = 9;
    return snapshot;
}

constexpr auto MemoryIndex = static_cast<std::size_t>(StorageTier::Memory);
constexpr auto DiskIndex = static_cast<std::size_t>(StorageTier::Disk);

/// One KPI tile's sub-line, as the page actually renders it.
///
/// Spelled as markup rather than as words, because the words are not unique:
/// `LeaseOutcomeTable`'s no-capacity meaning carries "this fleet's own work" too,
/// and a bare `contains` for that phrase matched it from three sections away —
/// asserting the tile said something it had stopped saying. The apostrophe arrives
/// escaped for the same reason it does everywhere else on this page.
/// @param text The sub-line, already in its escaped spelling.
/// @return The markup to search for.
[[nodiscard]] std::string KpiSub(std::string_view text)
{
    return std::format(R"(<span class="kpi-sub">{}</span>)", text);
}

/// A history nobody has sampled yet, for a case that is not about the charts.
///
/// Empty rather than fabricated: most cases here are about the snapshot, and a
/// literal series in each would be four lines of noise proving nothing.
[[nodiscard]] FleetHistoryView NoHistory()
{
    return FleetHistoryView {};
}

} // namespace

TEST_CASE("Every fleet column reaches both the page and the JSON", "[distributed][fleetview]")
{
    // Asserted over the TABLES rather than against a list of expected columns
    // written out beside them -- that list is the thing that goes stale, and it
    // would be maintained by the same person who forgot the renderer. This is the
    // same rule `MetricsCatalog`'s test follows, one axis over.
    auto snapshot = LeadingSnapshot();
    snapshot.cluster =
        Cluster::ClusterState { .members = { Cluster::ClusterMember {
                                    .id = "n1", .raftEndpoint = "10.0.0.2:6675", .schedulerEndpoint = "10.0.0.2:6676" } },
                                .settings = {} };
    snapshot.workers = { WorkerReport { .info = WorkerInfo { .id = "w1",
                                                             .fingerprint = "gcc-13-abcdef",
                                                             .endpoint = "10.0.0.2:7100",
                                                             .slots = 8,
                                                             .inFlight = 1,
                                                             .capacity = snapshot.nodes[0].capacity,
                                                             .load = Busy(1),
                                                             .codecs = {} },
                                        .heartbeatAge = std::chrono::milliseconds { 30 } } };

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 10);
    auto const json = RenderFleetJson(snapshot);

    // Column names are one spelling serving both consumers, so a header and a key
    // cannot drift apart.
    for (auto const& name: { "id", "raft-endpoint", "scheduler-endpoint" })
    {
        INFO("member column " << name);
        CHECK(html.contains(std::string { ">" } + name + "</th>"));
        CHECK(json.contains(std::string { "\"" } + name + "\":"));
    }
    for (auto const& name: { "endpoint",
                             "toolchains",
                             "cores",
                             "memory",
                             "class",
                             "cpu-busy",
                             "memory-available",
                             "scratch-free",
                             "cache-hit-rate",
                             "heartbeat-age" })
    {
        INFO("node column " << name);
        CHECK(html.contains(std::string { ">" } + name + "</th>"));
        CHECK(json.contains(std::string { "\"" } + name + "\":"));
    }
    for (auto const& name: { "slots", "in-flight", "available", "limited-by", "toolchain", "compiler", "heartbeat-age" })
    {
        INFO("worker column " << name);
        CHECK(html.contains(std::string { ">" } + name + "</th>"));
        CHECK(json.contains(std::string { "\"" } + name + "\":"));
    }
    // And every lease outcome, by the key its row carries.
    for (auto const& row: LeaseOutcomeTable)
    {
        INFO("lease outcome " << row.key);
        CHECK(json.contains(std::string { "\"" } + std::string { row.key } + "\":"));
        CHECK(html.contains(std::string { row.label }));
    }
}

TEST_CASE("A number nobody reported renders as an absence, never as a zero", "[distributed][fleetview]")
{
    // The failure this rule exists for: `0` says a machine has no cores and a cache
    // is standing empty, which are claims, while absent says nobody asked or nobody
    // answered. A dashboard is the consumer most likely to flatten them.
    auto snapshot = LeadingSnapshot();
    snapshot.nodes[0].capacity.logicalCores = 0; // "did not say" in NodeCapacity
    snapshot.nodes[0].capacity.totalMemoryBytes = 0;
    snapshot.nodes[0].load = Busy(1); // no CPU, no memory, no scratch reading

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    auto const json = RenderFleetJson(snapshot);

    CHECK(json.contains(R"("cores":null)"));
    CHECK(json.contains(R"("memory":null)"));
    CHECK(json.contains(R"("cpu-busy":null)"));
    CHECK(json.contains(R"("scratch-free":null)"));
    // Never the zero that would read as a fact.
    CHECK_FALSE(json.contains(R"("cores":0)"));
    CHECK_FALSE(json.contains(R"("cpu-busy":0)"));
    // The page uses the dash `--cluster-status` already spells absence with.
    CHECK(html.contains("&ndash;"));
}

TEST_CASE("A cache serving no reads has no hit rate rather than a rate of zero", "[distributed][fleetview]")
{
    // One level in from `hits` being optional: a cache nobody has read yet has no
    // hit rate, which is a different claim from one that misses everything.
    auto snapshot = LeadingSnapshot();
    snapshot.nodes[0].load.cache.hits = 0;
    snapshot.nodes[0].load.cache.misses = 0;
    CHECK(RenderFleetJson(snapshot).contains(R"("cache-hit-rate":null)"));

    snapshot.nodes[0].load.cache.hits = 750;
    snapshot.nodes[0].load.cache.misses = 250;
    CHECK(RenderFleetJson(snapshot).contains(R"("cache-hit-rate":750)"));
}

TEST_CASE("A tier's key index is reported as the RAM it is", "[distributed][fleetview][storage-tier]")
{
    // #175, and the half the issue exists to produce. A disk tier's budget is bytes
    // on a filesystem, so a memory ceiling binding on a disk-cache node used to show
    // `Memory` with nothing anywhere saying the cache's own key index was what
    // consumed it. The column is the attribution.
    auto snapshot = LeadingSnapshot();
    snapshot.nodes[0].load.cache.tiers[DiskIndex] =
        CacheTierUsage { .itemCount = 4'000, .bytesUsed = 8ULL << 30, .evictions = 0, .indexBytes = 96ULL << 20 };
    snapshot.nodes[0].capacity.cache.tierBytesLimit[DiskIndex] = 32ULL << 30;
    snapshot.tiersPresent[DiskIndex] = true;

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    auto const json = RenderFleetJson(snapshot);

    // One spelling serving as header and JSON key, like every other column.
    CHECK(html.contains("disk-index-ram"));
    CHECK(json.contains(R"("disk-index-ram":100663296)"));

    // And it is NOT the budget beside it: 96 MiB of RAM against 32 GiB of disk. A
    // renderer that folded the two into one figure would be adding two units, which
    // is the mistake the help text on this column exists to prevent.
    CHECK(json.contains(R"("disk-budget":34359738368)"));
}

TEST_CASE("A tier no member runs contributes no column at all", "[distributed][fleetview][storage-tier]")
{
    // A table cannot omit one cell the way a scrape omits a line, so this is where
    // "absent is not zero" lands at column granularity. The tier names come from
    // `StorageTierTable`, never a hand-written list.
    auto snapshot = LeadingSnapshot();
    snapshot.nodes[0].load.cache.tiers[MemoryIndex] =
        CacheTierUsage { .itemCount = 900, .bytesUsed = 100ULL << 20, .evictions = 3 };
    snapshot.nodes[0].capacity.cache.tierBytesLimit[MemoryIndex] = 256ULL << 20;
    snapshot.tiersPresent[MemoryIndex] = true;
    snapshot.tiersPresent[DiskIndex] = false;

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    auto const json = RenderFleetJson(snapshot);

    CHECK(html.contains("memory-items"));
    CHECK(json.contains(R"("memory-items":900)"));
    // A memory-only node has no disk tier to be empty, so there is nothing to show
    // -- not a column of zeroes.
    CHECK_FALSE(html.contains("disk-items"));
    CHECK_FALSE(json.contains("disk-items"));
}

TEST_CASE("An absent tier budget and an unbounded one are different claims", "[distributed][fleetview][storage-tier]")
{
    // `NodeCacheCapacity` keeps these apart deliberately: absent means the node
    // runs no tier of that kind, and zero means a tier configured with no ceiling.
    // Flattened, a dashboard renders both as the same thing.
    auto snapshot = LeadingSnapshot();
    snapshot.tiersPresent[MemoryIndex] = true;
    snapshot.nodes[0].capacity.cache.tierBytesLimit[MemoryIndex] = 0; // unbounded
    snapshot.nodes[0].load.cache.tiers[MemoryIndex] = CacheTierUsage {};

    CHECK(RenderFleetJson(snapshot).contains(R"("memory-budget":"unbounded")"));
    CHECK(RenderFleetHtml(snapshot, NoHistory(), 0).contains("unbounded"));

    snapshot.nodes[0].capacity.cache.tierBytesLimit[MemoryIndex] = std::nullopt;
    snapshot.nodes[0].load.cache.tiers[MemoryIndex] = std::nullopt;
    snapshot.tiersPresent[MemoryIndex] = true; // some other node has one
    CHECK(RenderFleetJson(snapshot).contains(R"("memory-budget":null)"));
}

TEST_CASE("A node serving two toolchains is one machine row and two worker rows", "[distributed][fleetview]")
{
    // The double count the whole grain exists to prevent: the machines table is
    // what a fleet total is computed over, and the workers table is where a lease
    // is decided.
    auto snapshot = LeadingSnapshot();
    snapshot.nodes[0].fingerprints = { "clang-20-aaaa", "gcc-13-abcdef" };
    snapshot.workers = {
        WorkerReport { .info = WorkerInfo { .id = "w1",
                                            .fingerprint = "gcc-13-abcdef",
                                            .endpoint = "10.0.0.2:7100",
                                            .slots = 8,
                                            .inFlight = 1,
                                            .capacity = snapshot.nodes[0].capacity,
                                            .load = Busy(1),
                                            .codecs = {} },
                       .heartbeatAge = std::chrono::milliseconds { 10 } },
        WorkerReport { .info = WorkerInfo { .id = "w2",
                                            .fingerprint = "clang-20-aaaa",
                                            .endpoint = "10.0.0.2:7100",
                                            .slots = 8,
                                            .inFlight = 2,
                                            .capacity = snapshot.nodes[0].capacity,
                                            .load = Busy(2),
                                            .codecs = {} },
                       .heartbeatAge = std::chrono::milliseconds { 20 } },
    };

    auto const json = RenderFleetJson(snapshot);
    // Two toolchains named on one machine row.
    CHECK(json.contains(R"("toolchains":2)"));
    // And both worker ids present, because a lease is per toolchain.
    CHECK(json.contains(R"("id":"w1")"));
    CHECK(json.contains(R"("id":"w2")"));
}

TEST_CASE("A worker held back names the limit that did it", "[distributed][fleetview][slotlimit]")
{
    // The three limits have opposite fixes, so the page says which one applied
    // rather than only that the number is low.
    auto snapshot = LeadingSnapshot();
    snapshot.workers = { WorkerReport {
        .info = WorkerInfo { .id = "w1",
                             .fingerprint = "gcc-13-abcdef",
                             .endpoint = "10.0.0.2:7100",
                             .slots = 16,
                             .inFlight = 1,
                             .capacity = NodeCapacity { .logicalCores = 32, .totalMemoryBytes = 64ULL << 30 },
                             .load = WithScratch(1, 384ULL << 20), // 3 jobs + the 1 running
                             .codecs = {} },
        .heartbeatAge = std::chrono::milliseconds { 5 } } };

    auto const json = RenderFleetJson(snapshot);
    CHECK(json.contains(R"("available":4)"));
    CHECK(json.contains(R"("limited-by":"scratch")"));
}

TEST_CASE("A follower answers with a page naming the leader, and never a redirect", "[distributed][fleetview]")
{
    // The recorded defect this must not repeat: a follower once redirected clients
    // to the leader's CONSENSUS port, which speaks nothing a client understands. A
    // dashboard address is local configuration on each node and nothing replicates
    // it, so any URL built here would be a guess.
    FleetSnapshot snapshot;
    snapshot.role = SchedulerRole::Follower;
    snapshot.leaderEndpoint = "10.0.0.9:6676";

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 10);
    CHECK(html.starts_with("<!doctype html>"));
    CHECK(html.contains("10.0.0.9:6676"));
    CHECK(html.contains("follower"));
    // No link and no redirect: naming a port the browser cannot use is the failure.
    CHECK_FALSE(html.contains("<a href"));
    CHECK_FALSE(html.contains("Location:"));
    // And it says what the address it printed actually is.
    CHECK(html.contains("scheduler"));

    CHECK(RenderFleetJson(snapshot).contains(R"("role":"follower")"));
    CHECK(RenderFleetJson(snapshot).contains(R"("leader":"10.0.0.9:6676")"));
}

TEST_CASE("An election in progress names nobody rather than guessing", "[distributed][fleetview]")
{
    // `Undecided` is a different fact from `Follower`: there is no leader to name,
    // which is the same distinction `NotLeader` draws with an empty message.
    FleetSnapshot snapshot;
    snapshot.role = SchedulerRole::Undecided;

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(html.contains("election"));
    CHECK(RenderFleetJson(snapshot).contains(R"("leader":null)"));
}

TEST_CASE("A node running no cluster reports no members rather than an empty cluster", "[distributed][fleetview]")
{
    // Absent and empty are different claims: a node started without `--node-id`
    // leads itself and has no replicated state for anybody to read.
    auto snapshot = LeadingSnapshot();
    CHECK_FALSE(snapshot.cluster.has_value());
    CHECK(RenderFleetJson(snapshot).contains(R"("members":null)"));
    CHECK(RenderFleetHtml(snapshot, NoHistory(), 0).contains("runs no cluster"));

    snapshot.cluster = Cluster::ClusterState {};
    CHECK(RenderFleetJson(snapshot).contains(R"("members":[])"));
}

TEST_CASE("A hostile fingerprint cannot escape the page or the document", "[distributed][fleetview][security]")
{
    // Every value here came off a wire: a fingerprint and an endpoint are whatever
    // a peer sent, and this page is opened in a browser.
    auto snapshot = LeadingSnapshot();
    snapshot.workers = { WorkerReport { .info = WorkerInfo { .id = R"(<script>alert(1)</script>)",
                                                             .fingerprint = R"(a"b&c<d)",
                                                             .endpoint = "10.0.0.2:7100",
                                                             .slots = 1,
                                                             .inFlight = 0,
                                                             .capacity = {},
                                                             .load = Busy(0),
                                                             .codecs = {} },
                                        .heartbeatAge = std::chrono::milliseconds { 0 } } };

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK_FALSE(html.contains("<script>alert(1)</script>"));
    CHECK(html.contains("&lt;script&gt;"));
    CHECK(html.contains("&quot;"));

    auto const json = RenderFleetJson(snapshot);
    CHECK(json.contains(R"(\"b&c<d)"));
}

TEST_CASE("The page carries no script and refreshes itself without one", "[distributed][fleetview]")
{
    // No CDN, no bundled framework and no script: a new dependency is forbidden,
    // and a page that fetched one would not render on the air-gapped network a
    // build fleet usually lives on. A meta refresh is a poll interval with no
    // JavaScript at all.
    auto const html = RenderFleetHtml(LeadingSnapshot(), NoHistory(), 10);
    CHECK(html.starts_with("<!doctype html>"));
    CHECK_FALSE(html.contains("<script"));
    CHECK_FALSE(html.contains("http://"));
    CHECK_FALSE(html.contains("https://"));
    CHECK(html.contains(R"(<meta http-equiv="refresh" content="10">)"));

    // Zero means the operator asked for no refresh, and then there is no tag.
    CHECK_FALSE(RenderFleetHtml(LeadingSnapshot(), NoHistory(), 0).contains("http-equiv=\"refresh\""));
}

TEST_CASE("Each lease outcome carries its own number", "[distributed][fleetview]")
{
    // Distinct values per outcome, so a row rendering its neighbour's count fails
    // rather than passing on equal numbers -- and because summing this split is the
    // mistake the whole table exists to prevent.
    auto snapshot = LeadingSnapshot();
    snapshot.leases = { 100, 7, 5, 3, 2 };

    auto const json = RenderFleetJson(snapshot);
    CHECK(json.contains(R"("granted":100)"));
    CHECK(json.contains(R"("no-worker":7)"));
    CHECK(json.contains(R"("no-capacity":5)"));
    CHECK(json.contains(R"("withdrawn":3)"));
    CHECK(json.contains(R"("duplicate":2)"));
    // And the page says out loud that they must not be added together.
    CHECK(RenderFleetHtml(snapshot, NoHistory(), 0).contains("Do not add these together"));
}

TEST_CASE("Collecting a fleet reads the registry per machine and the counters as they stand", "[distributed][fleetview]")
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    SchedulerService scheduler { clock, metrics };
    scheduler.SetRole(SchedulerRole::Leader, {});

    CallerContext const member { .membership = Membership::Member, .peerId = "peer" };
    auto announce =
        WorkerRegistration { .fingerprint = "gcc-13-abcdef", .endpoint = "10.0.0.2:7100", .slots = 4, .codecs = {} };
    CHECK(scheduler.Register(member, announce).status == CompileCacheWire::Status::Ok);
    announce.fingerprint = "clang-20-aaaa";
    CHECK(scheduler.Register(member, announce).status == CompileCacheWire::Status::Ok);

    Cluster::ClusterState state;
    state.members.push_back(Cluster::ClusterMember { .id = "n1", .raftEndpoint = "10.0.0.2:6675", .schedulerEndpoint = {} });
    FakeCluster cluster { state };

    auto const snapshot = CollectFleet(FleetSources { .scheduler = &scheduler, .cluster = &cluster, .metrics = &metrics });

    CHECK(LeadsTheFleet(snapshot));
    // Two registry entries, one machine -- the grain that keeps a fleet total honest.
    CHECK(snapshot.workers.size() == 2);
    REQUIRE(snapshot.nodes.size() == 1);
    CHECK(snapshot.nodes[0].endpoint == "10.0.0.2:7100");
    REQUIRE(snapshot.cluster.has_value());
    CHECK(Unwrap(snapshot.cluster).members.size() == 1);
    // Read off the sink rather than recomputed here: /metrics stays the source of
    // truth for anything alertable.
    REQUIRE(snapshot.leases.size() == LeaseOutcomeTable.size());
    CHECK(snapshot.registrations == 2);
}

TEST_CASE("Collecting a fleet without a cluster is a snapshot, not a crash", "[distributed][fleetview]")
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    SchedulerService scheduler { clock, metrics };

    auto const snapshot = CollectFleet(FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics });
    CHECK_FALSE(snapshot.cluster.has_value());
    CHECK(snapshot.nodes.empty());
}

TEST_CASE("Fleet capacity is summed per machine and splits busy from withheld", "[distributed][fleetview]")
{
    FleetSnapshot snapshot;
    snapshot.role = SchedulerRole::Leader;
    // Two machines, and one of them has a ceiling below what it registered: a
    // host whose CPU is doing somebody else's work.
    auto busy = Machine("10.0.0.1:7100", 16);
    busy.registeredSlots = 16;
    busy.fleetJobsInFlight = 4;
    busy.load.inFlight = 4;
    busy.load.cpuBusyPermille = 900; // most of the machine is not ours
    snapshot.nodes.push_back(busy);

    auto idle = Machine("10.0.0.2:7100", 8);
    idle.registeredSlots = 8;
    idle.fleetJobsInFlight = 0;
    idle.load = NodeLoad {};
    snapshot.nodes.push_back(idle);

    auto const totals = TotalsFor(snapshot);

    CHECK(totals.registered == 24);
    CHECK(totals.inFlight == 4);

    // Named rather than left to the identity below, which is the whole reason
    // this case did not catch a defect it covers. `withheld` is derived by
    // subtraction, so `inFlight + free + withheld == registered` holds for ANY
    // `free` the sum produces -- it asserted the arithmetic was self-consistent,
    // not that it was right.
    //
    // The busy machine: 16 registered, 900 permille of 16 cores is 14 busy, less
    // our own 4 leaves 10 taken by somebody else, so it supports 6 jobs in total
    // -- 4 of which are already running. Two are free and ten are withheld.
    // The idle machine reports nothing, so all 8 of its slots are free.
    CHECK(totals.free == 10);
    CHECK(totals.withheld == 10);
    CHECK(totals.inFlight + totals.free + totals.withheld == totals.registered);
}

TEST_CASE("A fleet nothing was ever dispatched to says so, rather than reading as idle",
          "[distributed][fleetview][capacity]")
{
    // The reported defect. Dispatch is opt-in -- a client asks for a lease only
    // when FASTCACHE_SCHEDULER names a scheduler -- so a node deployed as a shared
    // CACHE registers its slots, grants nothing, and renders `0 compiling` forever
    // while the machine around it compiles flat out. The old panel then read the
    // host CPU, found none of it accounted for by work this fleet had handed out,
    // and told the operator their own build belonged to somebody else.
    auto snapshot = LeadingSnapshot();
    snapshot.leases = { 0, 0, 0, 0, 0 }; // registered, and never asked
    snapshot.liveLeases = 0;             // nor is anything outstanding
    auto busy = Machine("10.0.0.1:7100", 16);
    busy.registeredSlots = 16;
    busy.fleetJobsInFlight = 0;
    busy.load = WithCpu(0, 900); // the operator's own compiles, not the fleet's
    snapshot.nodes = { busy };

    auto const totals = TotalsFor(snapshot);
    REQUIRE(totals.registered == 16);
    REQUIRE(totals.inFlight == 0);
    REQUIRE(totals.withheld > 0); // the misattribution the note used to make

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(html.contains("Nothing has been dispatched to this fleet"));
    CHECK(html.contains("FASTCACHE_SCHEDULER"));
    // And it must REPLACE the reading it explains, not sit beside it: two notes
    // disagreeing about whose work is loading the machine is worse than one wrong.
    CHECK_FALSE(html.contains("Read the hatching first"));
    // The tile the operator actually stares at names which zero this is.
    CHECK(html.contains(KpiSub("nothing dispatched yet")));
    CHECK_FALSE(html.contains(KpiSub("this fleet&#39;s own work")));
}

TEST_CASE("A fleet that has dispatched keeps the withheld reading", "[distributed][fleetview][capacity]")
{
    // The other direction, so the case above cannot pass by suppressing the note
    // for everyone. Once a lease has been granted, host CPU this fleet cannot
    // account for genuinely is somebody else's, and that sentence is the one an
    // operator has to act on.
    auto snapshot = LeadingSnapshot(); // leases = { 100, ... }: it has dispatched
    auto busy = Machine("10.0.0.1:7100", 16);
    busy.registeredSlots = 16;
    busy.fleetJobsInFlight = 4;
    busy.load = WithCpu(4, 900);
    snapshot.nodes = { busy };

    REQUIRE(TotalsFor(snapshot).withheld > 0);

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(html.contains("Read the hatching first"));
    CHECK_FALSE(html.contains("Nothing has been dispatched"));
    CHECK(html.contains(KpiSub("this fleet&#39;s own work")));
    CHECK_FALSE(html.contains(KpiSub("nothing dispatched yet")));
}

TEST_CASE("A scheduler that has just taken over does not call a working fleet unused", "[distributed][fleetview][capacity]")
{
    // `DispatchLeasesGranted` counts what THIS process granted since it started,
    // and leadership moves. A scheduler that has just won an election has granted
    // nothing, while heartbeats have already told it what the machines are running
    // -- so the counter alone would print "no compile has been handed to any of
    // them" on a page whose own bar shows four running, and would displace the
    // withheld reading that is the actually useful one.
    auto snapshot = LeadingSnapshot();
    snapshot.leases = { 0, 0, 0, 0, 0 }; // this leader has granted nothing yet
    snapshot.liveLeases = 0;             // and holds no lease of its own
    auto busy = Machine("10.0.0.1:7100", 16);
    busy.registeredSlots = 16;
    busy.fleetJobsInFlight = 4; // but the fleet is demonstrably working
    busy.load = WithCpu(4, 900);
    snapshot.nodes = { busy };

    REQUIRE(TotalsFor(snapshot).inFlight == 4);

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK_FALSE(html.contains("Nothing has been dispatched"));
    CHECK(html.contains("Read the hatching first"));
    CHECK(html.contains(KpiSub("this fleet&#39;s own work")));
}

TEST_CASE("A lease outstanding is dispatch, even before the count catches up", "[distributed][fleetview][capacity]")
{
    // The other half of the same guard, and the one a failover hits first: a lease
    // has been granted and the job has not started, so nothing is in flight yet.
    // `liveLeases` is the only thing that knows, and without it this page would
    // announce an unused fleet in the gap between a grant and its job.
    auto snapshot = LeadingSnapshot();
    snapshot.leases = { 0, 0, 0, 0, 0 };
    snapshot.liveLeases = 2;
    auto idle = Machine("10.0.0.1:7100", 16);
    idle.registeredSlots = 16;
    idle.fleetJobsInFlight = 0;
    idle.load = NodeLoad {};
    snapshot.nodes = { idle };

    CHECK_FALSE(RenderFleetHtml(snapshot, NoHistory(), 0).contains("Nothing has been dispatched"));
}

TEST_CASE("A snapshot carrying no lease figures claims nothing about dispatch", "[distributed][fleetview][capacity]")
{
    // Absent is not zero, at the one place on this page where reading it as zero
    // would print the strongest sentence there is. `CollectFleet` always fills
    // every row of `LeaseOutcomeTable`; a snapshot that carries none is a caller
    // who never said, and it must fall through to the ordinary readings.
    auto snapshot = LeadingSnapshot();
    snapshot.leases.clear();
    auto idle = Machine("10.0.0.1:7100", 16);
    idle.registeredSlots = 16;
    idle.fleetJobsInFlight = 0;
    idle.load = NodeLoad {};
    snapshot.nodes = { idle };

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK_FALSE(html.contains("Nothing has been dispatched"));
    CHECK(html.contains("Nothing is being withheld"));
}

TEST_CASE("A memory-bound worker is not dressed as a busy one", "[distributed][fleetview]")
{
    // `SlotLimitTable` has four rows and the chip mapping handled two, defaulting
    // the rest to the CPU class -- so a machine held back by MEMORY was coloured
    // "somebody else is using this machine". The remedies are opposite (free
    // memory against buy a machine), which is the whole reason the table carries a
    // remedy per limit rather than one sentence for all of them.
    //
    // The chip is on the WORKER table, not the node table, and the assertion is on
    // the rendered element rather than on the class name -- the stylesheet names
    // every class unconditionally, so `contains("chip--memory")` is true whatever
    // the mapping does. This case was written that way first and passed against the
    // unfixed ladder.
    auto snapshot = LeadingSnapshot();
    auto load = NodeLoad {};
    load.inFlight = 0;
    load.availableMemoryBytes = 1ULL << 30; // one job's budget: a ceiling of 1

    snapshot.workers = { WorkerReport { .info = WorkerInfo { .id = "w1",
                                                             .fingerprint = "gcc-13-abcdef",
                                                             .endpoint = "10.0.0.2:7100",
                                                             .slots = 8,
                                                             .inFlight = 0,
                                                             .capacity = snapshot.nodes[0].capacity,
                                                             .load = load,
                                                             .codecs = {} },
                                        .heartbeatAge = std::chrono::milliseconds { 30 } } };

    REQUIRE(SlotCeilingsFor(snapshot.workers[0].info.capacity, snapshot.workers[0].info.slots, snapshot.workers[0].info.load)
                .binding
            == SlotLimit::Memory);

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(html.contains(R"(<span class="chip chip--memory">memory</span>)"));
    CHECK_FALSE(html.contains(R"(<span class="chip chip--cpu">memory</span>)"));
}

TEST_CASE("A chart with nothing to report renders a dash, not its markup", "[distributed][fleetview]")
{
    // `AbsentText` IS markup -- the entity for an en dash -- and every path that
    // shows it interpolates it raw. The chart headline was escaped instead, so its
    // `&` became `&amp;` and the panel displayed the literal text `&ndash;`. A
    // fleet reporting no cache figures leaves the hit-rate series absent in every
    // bucket while others are present, so this is the ordinary state of that panel
    // rather than an exotic one -- and NOT the same as an empty history, which the
    // page short-circuits before it renders a chart at all.
    FleetHistoryView history;
    history.buckets = { FleetBucket { .startMillis = 0, .sampleMillis = 0, .values = {}, .present = true },
                        FleetBucket { .startMillis = 300'000, .sampleMillis = 300'000, .values = {}, .present = true } };

    auto const snapshot = LeadingSnapshot();
    auto const html = RenderFleetHtml(snapshot, history, 0);

    // The charts really did render -- otherwise this asserts nothing.
    REQUIRE(html.contains("chart-now"));
    // The dash reaches the page as an entity, never as the text of one.
    CHECK(html.contains("&ndash;"));
    CHECK_FALSE(html.contains("&amp;ndash;"));
}

TEST_CASE("A machine running everything it can offers nothing", "[distributed][fleetview]")
{
    // The sharpest form of the same defect, and the one an operator sees first: a
    // ceiling is the total a machine supports with its RUNNING jobs INCLUDED
    // (`Detail::CeilingFrom`), so treating it as free counts this fleet's own work
    // twice. `WorkerRegistry::FreeSlots` subtracts `inFlight` before calling
    // anything free; this has to agree with it, or the page invites an operator to
    // start work on a machine the scheduler will not send any to.
    FleetSnapshot snapshot;
    snapshot.role = SchedulerRole::Leader;
    auto full = Machine("10.0.0.1:7100", 8);
    full.registeredSlots = 8;
    full.fleetJobsInFlight = 8;
    full.load = NodeLoad {};
    full.load.inFlight = 8;
    snapshot.nodes.push_back(full);

    auto const totals = TotalsFor(snapshot);

    CHECK(totals.registered == 8);
    CHECK(totals.inFlight == 8);
    CHECK(totals.free == 0);
    CHECK(totals.withheld == 0);
}

TEST_CASE("Fleet capacity saturates rather than wrapping", "[distributed][fleetview]")
{
    // The three parts come off a heartbeat a machine may have sent at different
    // moments, so a ceiling can legitimately exceed what is left after in-flight
    // work. These are unsigned: wrapping would draw a bar four billion slots wide.
    FleetSnapshot snapshot;
    snapshot.role = SchedulerRole::Leader;
    auto node = Machine("10.0.0.1:7100", 64);
    node.registeredSlots = 1;
    node.fleetJobsInFlight = 8;
    node.load = NodeLoad {};
    node.load.inFlight = 8;
    snapshot.nodes.push_back(node);

    auto const totals = TotalsFor(snapshot);
    CHECK(totals.withheld == 0);
}

TEST_CASE("A fleet with no machines has no capacity to draw", "[distributed][fleetview]")
{
    FleetSnapshot snapshot;
    snapshot.role = SchedulerRole::Leader;

    auto const totals = TotalsFor(snapshot);
    CHECK(totals.registered == 0);
    CHECK(totals.inFlight == 0);
    CHECK(totals.free == 0);
    CHECK(totals.withheld == 0);

    // And the page says so rather than rendering an empty meter.
    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(html.contains("No machine has registered"));
}

TEST_CASE("The page dresses a stale heartbeat differently from a fresh one", "[distributed][fleetview]")
{
    auto snapshot = LeadingSnapshot();
    REQUIRE(!snapshot.nodes.empty());
    snapshot.nodes[0].heartbeatAge = std::chrono::milliseconds { 250 };

    auto const fresh = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(fresh.contains("pill--ok"));

    snapshot.nodes[0].heartbeatAge = std::chrono::minutes { 5 };
    auto const stale = RenderFleetHtml(snapshot, NoHistory(), 0);
    // Everything on that row is as old as this number, which is the reason the
    // column exists at all -- so the row has to look different.
    CHECK(stale.contains("pill--warn"));
}

namespace
{

/// A day of buckets carrying a rising counter, so every chart has something to draw.
[[nodiscard]] FleetHistoryView SomeHistory(FleetRange range = FleetRange::Day, bool durable = true)
{
    FleetHistoryView view { .range = range, .buckets = {}, .durable = durable };
    for (auto const index: std::views::iota(0, 8))
    {
        FleetBucket bucket {};
        bucket.startMillis = index * 300'000;
        bucket.present = true;
        auto const set = [&bucket](FleetMetric metric, std::uint64_t value) {
            bucket.values[static_cast<std::size_t>(metric)] = value;
        };
        set(FleetMetric::DispatchGranted, static_cast<std::uint64_t>(index) * 30);
        set(FleetMetric::DispatchNoCapacity, static_cast<std::uint64_t>(index) * 2);
        set(FleetMetric::CacheHits, static_cast<std::uint64_t>(index) * 9);
        set(FleetMetric::CacheMisses, static_cast<std::uint64_t>(index));
        set(FleetMetric::OfferableSlots, 16);
        set(FleetMetric::JobsInFlight, static_cast<std::uint64_t>(index) % 4);
        view.buckets.push_back(bucket);
    }
    return view;
}

} // namespace

TEST_CASE("The readouts are the strip's table, in its order", "[distributed][fleetview][kpi]")
{
    auto const html = RenderFleetHtml(LeadingSnapshot(), SomeHistory(), 0);

    // The mockup's six, and the order is what a reader's eye follows -- so it is
    // asserted rather than left to whichever order the rows happened to be typed in.
    constexpr std::array<std::string_view, 6> Expected { "Dispatched", "Compiling now",      "Cache hit rate",
                                                         "Refused",    "Leases outstanding", "Oldest heartbeat" };
    std::size_t cursor = 0;
    for (auto const& label: Expected)
    {
        auto const at = html.find(label, cursor);
        INFO("label " << label);
        REQUIRE(at != std::string::npos);
        cursor = at;
    }

    // Free and withheld are the capacity meter's own two segments directly below.
    // A number repeated a hand's width from the picture of itself is a number that
    // will one day disagree with it.
    CHECK_FALSE(html.contains(">Free now<"));
    CHECK_FALSE(html.contains(">Withheld<"));
}

TEST_CASE("A tile whose range holds nothing shows a dash rather than a zero", "[distributed][fleetview][kpi]")
{
    auto const html = RenderFleetHtml(LeadingSnapshot(), NoHistory(), 0);

    // Absent is not zero, at the tile as everywhere else on this page: "0 compiles
    // dispatched" is a claim about a fleet, and nobody was watching.
    CHECK(html.contains("&ndash;"));
    CHECK(html.contains("Nothing has been recorded for this range yet"));
    // No frames with nothing in them: a chart panel drawn over no data reads as a
    // broken chart rather than as an honest absence.
    CHECK_FALSE(html.contains("/fleet/chart/"));
}

TEST_CASE("The range control offers every range and marks the one in view", "[distributed][fleetview][charts]")
{
    auto const day = RenderFleetHtml(LeadingSnapshot(), SomeHistory(FleetRange::Day), 0);
    auto const week = RenderFleetHtml(LeadingSnapshot(), SomeHistory(FleetRange::Week), 0);

    for (auto const& row: FleetRangeTable)
    {
        INFO("range " << row.key);
        CHECK(day.contains(std::format(R"(href="?range={}")", row.key)));
        CHECK(week.contains(std::format(R"(href="?range={}")", row.key)));
    }
    // Links, not buttons: the control is two URLs, so it survives a bookmark and is
    // what the page's own auto-refresh comes back to.
    CHECK_FALSE(day.contains("<button"));
    CHECK(day.contains(R"(<a class="on" href="?range=24h")"));
    CHECK(week.contains(R"(<a class="on" href="?range=7d")"));
}

TEST_CASE("Every chart the table names becomes a panel and its own image", "[distributed][fleetview][charts]")
{
    auto const html = RenderFleetHtml(LeadingSnapshot(), SomeHistory(FleetRange::Week), 0);

    for (auto const& chart: FleetChartTable)
    {
        INFO("chart " << chart.key);
        CHECK(html.contains(EscapeMarkup(chart.title)));
        CHECK(html.contains(EscapeMarkup(chart.caption)));
        // Its own resource, and the URL carries no cache-buster: a generation in the
        // query would make every bucket a new URL and the conditional GET this whole
        // arrangement exists for would never fire.
        auto const src = std::format(R"(src="/fleet/chart/{}.svg?range=7d")", chart.key);
        CHECK(html.contains(src));
        CHECK_FALSE(html.contains(src.substr(0, src.size() - 1) + "&"));

        for (auto const offset: std::views::iota(std::size_t { 0 }, chart.count))
        {
            auto const& series = FleetSeriesTable[chart.first + offset];
            INFO("series " << series.key);
            CHECK(html.contains(EscapeMarkup(series.label)));
            // A class per palette token, never an inline style: `style="…var(--x)"`
            // ends in the two characters that terminate a raw string literal.
            CHECK(html.contains(std::format(R"(class="tone tone--{}")", series.colour)));
        }
    }
}

TEST_CASE("The page says whether the history it draws will survive a restart", "[distributed][fleetview][charts]")
{
    CHECK(RenderFleetHtml(LeadingSnapshot(), SomeHistory(FleetRange::Day, true), 0).contains("survives a restart"));
    // A node with nowhere to write it is a legitimate way to run, and the promise it
    // makes is different -- so the page says so rather than letting an operator find
    // out at the next restart.
    CHECK(RenderFleetHtml(LeadingSnapshot(), SomeHistory(FleetRange::Day, false), 0).contains("kept in memory only"));
}

TEST_CASE("The dispatched tile carries its sparkline inline", "[distributed][fleetview][charts]")
{
    auto const html = RenderFleetHtml(LeadingSnapshot(), SomeHistory(), 0);

    // Inline rather than a seventh request: it is part of the tile's layout at
    // roughly two hundred bytes, and being inside the page is also what lets it
    // resolve the page's own custom properties instead of carrying a palette.
    CHECK(html.contains(R"(<span class="spark"><svg )"));
    CHECK(html.contains("var(--accent)"));
    CHECK_FALSE(RenderFleetHtml(LeadingSnapshot(), NoHistory(), 0).contains(R"(<span class="spark">)"));
}

TEST_CASE("A follower's page draws no charts at all", "[distributed][fleetview][charts]")
{
    auto snapshot = LeadingSnapshot();
    snapshot.role = SchedulerRole::Follower;
    snapshot.leaderEndpoint = "10.0.0.9:6676";

    // A follower samples nothing, so it has nothing to draw -- and a chart there
    // would be a fraction of the fleet presented as the whole of it.
    auto const html = RenderFleetHtml(snapshot, SomeHistory(), 0);
    CHECK_FALSE(html.contains("/fleet/chart/"));
    CHECK_FALSE(html.contains("Over time"));
    CHECK(html.contains("10.0.0.9:6676"));
}

TEST_CASE("Each machine's software version reaches the page and the JSON", "[distributed][fleetview][version]")
{
    auto snapshot = LeadingSnapshot();
    snapshot.nodes.push_back(Machine("10.0.0.3:7100", 16));
    REQUIRE(snapshot.nodes.size() == 2);
    snapshot.nodes[0].version = "1.5.0";
    // The second machine is still on the old build, which is the state this column
    // exists to make visible: mid-upgrade, and which box is which.
    snapshot.nodes[1].version = "1.4.2";

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(html.contains(">version<"));
    CHECK(html.contains(">1.5.0<"));
    CHECK(html.contains(">1.4.2<"));

    // One table drives both renderers, so the JSON gets it for free -- and the test
    // asserts that rather than trusting it.
    auto const json = RenderFleetJson(snapshot);
    CHECK(json.contains(R"("version":"1.5.0")"));
    CHECK(json.contains(R"("version":"1.4.2")"));
}

TEST_CASE("A machine too old to report a version renders an absence, not a blank", "[distributed][fleetview][version]")
{
    auto snapshot = LeadingSnapshot();
    for (auto& node: snapshot.nodes)
        node.version.clear();

    // A node that predates the field is exactly the node an operator is hunting for
    // during a rolling upgrade, so it must not render as the emptiest-looking cell
    // in the table. It gets the page's dash and the JSON's null, like everything
    // else nobody told us.
    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK(html.contains("&ndash;"));
    CHECK_FALSE(html.contains("<td></td>"));
    CHECK(RenderFleetJson(snapshot).contains(R"("version":null)"));
}

TEST_CASE("A worker row names its compiler beside the fingerprint", "[distributed][fleetview][toolchain]")
{
    // #194. The fingerprint is the right identity and the wrong label: it stopped
    // being something a person can derive, so a machine with two MSVC toolsets -- what
    // an ordinary Visual Studio update leaves behind -- showed two opaque hashes and
    // no way to tell which was which.
    auto snapshot = LeadingSnapshot();
    snapshot.workers = { WorkerReport { .info = WorkerInfo { .id = "w1",
                                                             .fingerprint = "bb1558fddcdf8b604cacc58e3f175adc",
                                                             .endpoint = "10.0.0.2:7100",
                                                             .toolchainLabel = "cl 19.44.35207",
                                                             .slots = 8,
                                                             .capacity = snapshot.nodes[0].capacity,
                                                             .codecs = {} },
                                        .heartbeatAge = std::chrono::milliseconds { 30 } } };

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    auto const json = RenderFleetJson(snapshot);

    CHECK(html.contains("cl 19.44.35207"));
    CHECK(json.contains(R"("compiler":"cl 19.44.35207")"));

    // BESIDE, never instead of. The digest is what a launcher compares and what
    // decides every match, so removing it in favour of a prettier column would take
    // away the only thing an operator can check a mismatch against.
    CHECK(json.contains(R"("toolchain":)"));
    CHECK(html.contains(snapshot.workers[0].info.fingerprint));
}

TEST_CASE("A worker that did not say what its compiler is renders absent", "[distributed][fleetview][toolchain]")
{
    // An operator's `<fingerprint>=<compiler>` override is never probed, so there is
    // no banner to read a label out of -- and a node built before the field cannot
    // report one either. Both are rows somebody is looking for, so neither may render
    // as the emptiest-looking cell in the table.
    auto snapshot = LeadingSnapshot();
    snapshot.workers = { WorkerReport { .info = WorkerInfo { .id = "w1",
                                                             .fingerprint = "bb1558fddcdf8b604cacc58e3f175adc",
                                                             .endpoint = "10.0.0.2:7100",
                                                             .slots = 8,
                                                             .capacity = snapshot.nodes[0].capacity,
                                                             .codecs = {} },
                                        .heartbeatAge = std::chrono::milliseconds { 30 } } };

    CHECK(RenderFleetJson(snapshot).contains(R"("compiler":null)"));
    CHECK(RenderFleetHtml(snapshot, NoHistory(), 0).contains("&ndash;"));
}

TEST_CASE("A hostile version string is escaped rather than interpolated", "[distributed][fleetview][version]")
{
    auto snapshot = LeadingSnapshot();
    // It came off a wire: whatever a peer sent is what lands here, and the decoder
    // deliberately does not validate its SHAPE -- a version is a string an operator
    // chooses, so one this build does not recognise is a peer to report rather than
    // a peer to refuse. Being text at all is a separate question, and the scheduler
    // does refuse that: see `SchedulerService::Register`. A shape nobody anticipated
    // still has to reach a reader, and reaching one safely is this escape's job.
    snapshot.nodes[0].version = R"(<script>alert(1)</script>)";

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK_FALSE(html.contains("<script>alert(1)</script>"));
    CHECK(html.contains("&lt;script&gt;"));
}

TEST_CASE("A peer that got its bytes past the door cannot make the whole fleet's JSON unparseable",
          "[distributed][fleetview]")
{
    // The reported defect, at the grain it was reported at: one worker's row makes
    // the WHOLE document something a strict parser may reject, so `/fleet.json`
    // stops answering for every other machine in the fleet as well.
    //
    // `SchedulerService::Register` refuses such a registration now, which is where
    // the fix belongs -- the page, `--cluster-status` and the logs read the same
    // strings back out, so repairing them in one renderer would leave the others
    // carrying the originals. This asserts the other half: that an encoder cannot
    // emit a document its own format forbids, whatever it is handed. A value can
    // still arrive through a door this build does not control, and `members[]` is
    // exactly that door -- a consensus entry is applied AFTER it is committed, so a
    // peer built before that refusal existed can replicate a member id straight
    // into it with nobody left to refuse it.
    auto snapshot = LeadingSnapshot();
    snapshot.leaderEndpoint = "10.0.0.2:7100\xFF";
    snapshot.cluster = Cluster::ClusterState { .members = { Cluster::ClusterMember { .id = "n\x80\x80",
                                                                                     .raftEndpoint = "10.0.0.2:6675\xC3",
                                                                                     .schedulerEndpoint = "\xE2\x82" } },
                                               .settings = {} };
    snapshot.workers = { WorkerReport { .info = WorkerInfo { .id = "w1",
                                                             .fingerprint = "gcc-13-ab\x80\x80",
                                                             .endpoint = "10.0.0.2:7100\xFF",
                                                             .version = "1.2.3-\xE2\x82",
                                                             .slots = 8,
                                                             .inFlight = 1,
                                                             .capacity = snapshot.nodes[0].capacity,
                                                             .load = Busy(1),
                                                             .codecs = {} },
                                        .heartbeatAge = std::chrono::milliseconds { 30 } } };
    snapshot.nodes[0].endpoint = "10.0.0.2:7100\xFF";
    snapshot.nodes[0].version = "1.2.3-\xE2\x82";

    // Both surfaces, because each has an encoding its consumer enforces: JSON must
    // be UTF-8, and the page is served as UTF-8 and embeds SVG, which is XML.
    CHECK(IsValidUtf8(RenderFleetJson(snapshot)));
    CHECK(IsValidUtf8(RenderFleetHtml(snapshot, NoHistory(), 10)));
}
