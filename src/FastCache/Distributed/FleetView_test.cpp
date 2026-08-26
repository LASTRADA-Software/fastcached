// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Clock.hpp>
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
    for (auto const& name: { "slots", "in-flight", "available", "limited-by", "toolchain", "heartbeat-age" })
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
    // The whole point of the split: what is not running is not therefore
    // available, and the difference is what an operator has to act on.
    CHECK(totals.inFlight + totals.free + totals.withheld == totals.registered);
    CHECK(totals.withheld > 0);
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

TEST_CASE("A hostile version string is escaped rather than interpolated", "[distributed][fleetview][version]")
{
    auto snapshot = LeadingSnapshot();
    // It came off a wire: whatever a peer sent is what lands here, and the decoder
    // deliberately does not validate it -- refusing a registration over a
    // diagnostic field would take a working machine out of the fleet.
    snapshot.nodes[0].version = R"(<script>alert(1)</script>)";

    auto const html = RenderFleetHtml(snapshot, NoHistory(), 0);
    CHECK_FALSE(html.contains("<script>alert(1)</script>"));
    CHECK(html.contains("&lt;script&gt;"));
}
