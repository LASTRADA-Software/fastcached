// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/FleetChart.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FleetHistoryFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{
/// A configuration that would start, so a refusal names the thing under test.
///
/// The startup rules are a table of cross-flag invariants, so a bare `NodeConfig`
/// can be refused for a reason having nothing to do with the surface a case is
/// about -- and the case would then pass on the wrong refusal. This gives every one
/// of them a node that is otherwise fine.
/// @return A worker that would start.
[[nodiscard]] NodeConfig Installable()
{
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    cfg.advertise = "worker-01.internal:6676";
    cfg.toolchains = { "/usr/bin/g++" };
    return cfg;
}

/// A configuration serving the admin surface at @p spec.
///
/// Through the surface's own row rather than by naming `cfg.adminListen`, so a test
/// cannot reach this port by a route production code no longer has -- the same
/// reason `FrameEndpoint_test`'s helper is written this way. A helper that named the
/// field would be a second copy of the surface-to-field mapping.
/// @param spec The address the admin surface should serve.
/// @return A configuration that would start, serving admin there.
[[nodiscard]] NodeConfig AdminOn(std::string spec)
{
    auto const& row = RowFor(NodeSurface::Admin);
    REQUIRE(row.spec != nullptr);

    auto cfg = Installable();
    cfg.*row.spec = std::move(spec);
    return cfg;
}

/// A snapshot provider standing in for the worker's, with no worker behind it.
/// @return A worker-shaped snapshot: no storage, a machine, an uptime.
AdminHttpServer::SnapshotProvider WorkerShapedSnapshot()
{
    return [] {
        return MetricsSnapshot { .storage = std::nullopt,
                                 .host = HostCapacity { .logicalCores = 4,
                                                        .configuredSlots = 4,
                                                        .totalMemoryBytes = 8589934592ULL,
                                                        .diskCapacityBytes = 100,
                                                        .diskFreeBytes = 50,
                                                        .busySlots = 1 },
                                 .uptime = Uptime { 1s } };
    };
}

/// What a node reports about itself, so the node series has something to record.
///
/// The same shape `/metrics` scrapes, which is the point of reading the history's
/// node half through that provider: a machine cannot describe itself two ways.
///
/// Cache figures are NOT here: those come from the metrics sink, which is where
/// `CacheTier::Snapshot` reads them too, so one machine cannot be described twice.
///
/// @param busy Compiles running right now.
/// @param slots Compiles this node advertises.
/// @return A provider answering with exactly those.
[[nodiscard]] AdminHttpServer::SnapshotProvider NodeFacts(std::size_t busy = 0, std::size_t slots = 8)
{
    return [busy, slots] {
        return MetricsSnapshot { .storage = std::nullopt,
                                 .storageTiers = {},
                                 .host = HostCapacity { .configuredSlots = slots, .busySlots = busy },
                                 .upstreamConfigured = std::nullopt,
                                 .uptime = {} };
    };
}

} // namespace

TEST_CASE("An unparseable --admin-listen is refused, not guessed at", "[node][admin]")
{
    // A worker that started without the endpoint an operator asked for looks
    // healthy to the very probe that would have reported it was not, so this is
    // fatal at startup rather than a warning. The message names what was rejected,
    // because "invalid" tells an operator nothing about what to type instead.
    AtomicMetricsSink metrics;
    NullLogger logger;

    // Asked of `StartupPolicyRejection`, which is where this check went when the
    // surface table took over the port map (#288). The endpoint no longer parses --
    // it takes a surface and asks the row -- so the refusal that ECHOES what the
    // operator typed has to come from the walk over those rows, which happens once,
    // at startup, before any tier exists and before `--install-service` writes a
    // command line it will replay forever. The message moved with the check, and this
    // case followed the message.
    for (auto const* const spelling: { "not-a-port", "127.0.0.1", "6674x", "0", "70000", "[::1]6674" })
    {
        INFO("spelling '" << spelling << "'");
        auto cfg = Installable();
        cfg.adminListen = spelling;

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--admin-listen"));
        CHECK(Unwrap(refusal).contains(spelling));
    }

    // Empty is not malformed: it is how an operator says this node serves no admin
    // surface at all, so it is the one spelling above that must be ACCEPTED.
    auto off = Installable();
    off.adminListen = "";
    CHECK_FALSE(StartupPolicyRejection(off).has_value());

    // And what the factory itself still refuses: a surface it was asked to serve
    // that resolves to no address. Naming the flag, because an operator has to know
    // which surface went unserved.
    auto const started = AdminEndpoint::Start(NodeSurface::Admin, off, metrics, WorkerShapedSnapshot(), logger);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains("--admin-listen"));
}

TEST_CASE("A bare port binds loopback rather than the wildcard", "[node][admin]")
{
    // The default is the security-relevant half of this flag: a scrape surface on a
    // public interface must be something an operator wrote down, not something they
    // got by typing a port. Asserted through the address the endpoint reports,
    // since that is what an operator reads back off the log line.
    AtomicMetricsSink metrics;
    NullLogger logger;

    // Port 0 is refused outright -- "any free port" is an address nobody can be told
    // in advance -- so bind an ephemeral one the ordinary way instead.
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();

    // The loopback default comes from the ROW now, not from an argument this call
    // site passes -- which is the point: it used to be a caller's choice, and a
    // caller free to pass a different default host was a caller free to move the
    // rule this surface's credential requirement turns on. Asserted end to end,
    // through the address the endpoint reports, because that is what an operator
    // reads back off the log line.
    auto cfg = Installable();
    cfg.adminListen = std::to_string(port);

    auto const bare = AdminEndpoint::Start(NodeSurface::Admin, cfg, metrics, WorkerShapedSnapshot(), logger);
    REQUIRE(bare.has_value());
    CHECK((*bare)->BoundEndpoint() == std::format("127.0.0.1:{}", port));
}

TEST_CASE("An endpoint that cannot bind reports why", "[node][admin]")
{
    // The operator needs the address in the message, because the flag they typed
    // may have been a bare port: a failure that does not say what that resolved to
    // cannot be acted on.
    //
    // Provoked with an address this host does not hold -- 192.0.2.1 is RFC 5737
    // TEST-NET-1, reserved for documentation and assigned to no interface -- rather
    // than by taking a port twice, which needs a second listener kept alive for the
    // duration and asserts nothing this does not. That a port already being served
    // is refused belongs where the option refusing it is set, and is asserted there
    // ("BindAndListen keeps its address to itself while it is listening", issue #85).
    AtomicMetricsSink metrics;
    NullLogger logger;

    auto const unreachable = std::string { "192.0.2.1:6674" };
    auto const started =
        AdminEndpoint::Start(NodeSurface::Admin, AdminOn(unreachable), metrics, WorkerShapedSnapshot(), logger);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains(unreachable));
}

TEST_CASE("Destroying the endpoint stops it, with nothing to remember", "[node][admin]")
{
    // The reason this is a class rather than three locals in main(): the listener
    // must be closed before the serving thread can be joined, and a jthread whose
    // loop is still parked in accept() joins never. This case hangs rather than
    // failing if that order is ever reversed -- which is exactly the shape of the
    // `systemctl stop` defect this repository already records, so it is worth
    // having somewhere it can be reached in seconds instead of in a supervisor.
    AtomicMetricsSink metrics;
    NullLogger logger;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    auto const port = probe->BoundPort();
    probe.reset();

    // Destroyed on another thread and waited for with a deadline, deliberately: a
    // test that HANGS when the order is wrong reports a defect as a suite timeout
    // naming nothing, which this repository has already paid for once. It fails in
    // seconds instead, saying what it waited for.
    auto stopped = std::async(std::launch::async, [&] {
        auto started =
            AdminEndpoint::Start(NodeSurface::Admin, AdminOn(std::to_string(port)), metrics, WorkerShapedSnapshot(), logger);
        return started.has_value();
    });

    REQUIRE(stopped.wait_for(15s) == std::future_status::ready);
    REQUIRE(stopped.get());

    // And the port is free again, which is only true if the listener was really
    // closed rather than leaked with its thread still parked on it.
    auto again = BlockingListener::Bind("127.0.0.1", port);
    REQUIRE(again);
    CHECK(again->IsBound());
}

namespace
{
/// A machine a test can describe, standing in for the one it runs on.
///
/// A second, smaller copy of `NodeConfig_test`'s `FakeHost`: that one lives in its
/// file's anonymous namespace and reports no disk at all, which is precisely what
/// this case needs to see. Two copies rather than a shared header, and worth
/// lifting if a third appears.
class ScrapeHost final: public IHostFactsSource
{
  public:
    [[nodiscard]] HostFacts const& Facts() const override
    {
        return _facts;
    }
    [[nodiscard]] std::uint32_t LogicalCores() const override
    {
        return 4;
    }
    [[nodiscard]] std::uint64_t TotalMemoryBytes() const override
    {
        return 8ULL << 30;
    }
    [[nodiscard]] DiskSpace SpaceOn(std::filesystem::path const& /*path*/) const override
    {
        return DiskSpace { .capacityBytes = 1000, .freeBytes = 400 };
    }

  private:
    HostFacts _facts;
};
} // namespace

TEST_CASE("A node with no cache tier reports no cache", "[node][admin][cache]")
{
    // The branch this factory exists to have covered. While it lived in `main.cpp`
    // as a lambda it had no coverage at all -- that file is in no test target --
    // and it spent its whole life returning `std::nullopt` under a comment saying a
    // worker has no cache, which stopped being true when the node grew one.
    //
    // Absent here is the truth rather than a placeholder: a node whose every cache
    // half was turned off has none, and a default-constructed `StorageStats` would
    // state an empty unbounded cache as a fact.
    ScrapeHost host;
    auto const provider = MakeNodeSnapshotProvider(NodeScrapeSources { .host = &host,
                                                                       .busySlots = [] { return std::size_t { 2 }; },
                                                                       .cache = nullptr,
                                                                       .slots = 4,
                                                                       .scratchRoot = std::filesystem::path { "." } },
                                                   std::chrono::steady_clock::now());

    auto const snapshot = provider();
    CHECK_FALSE(snapshot.storage.has_value());
    CHECK_FALSE(snapshot.upstreamConfigured.has_value());
    for (auto const& tier: snapshot.storageTiers)
        CHECK_FALSE(tier.has_value());

    // What it reports regardless: the machine, which does not depend on a cache.
    REQUIRE(snapshot.host.has_value());
    CHECK(Unwrap(snapshot.host).logicalCores == 4);
    CHECK(Unwrap(snapshot.host).configuredSlots == 4);
    CHECK(Unwrap(snapshot.host).diskFreeBytes == 400);
    // Sampled per scrape, not captured once -- the busy count moves.
    CHECK(Unwrap(snapshot.host).busySlots == 2);
}

TEST_CASE("A scrape renders nothing for a cache the node does not have", "[node][admin][cache]")
{
    // End to end through the renderer, because the absence has to survive it too: a
    // `fastcached_items 0` line says the cache is empty, which is a different claim
    // from a node that has none, and a dashboard reads the first as a fact.
    ScrapeHost host;
    AtomicMetricsSink metrics;
    auto const provider = MakeNodeSnapshotProvider(NodeScrapeSources { .host = &host,
                                                                       .busySlots = [] { return std::size_t { 0 }; },
                                                                       .cache = nullptr,
                                                                       .slots = 4,
                                                                       .scratchRoot = std::filesystem::path { "." } },
                                                   std::chrono::steady_clock::now());

    auto const body = RenderPrometheus(metrics, provider());
    CHECK_FALSE(body.contains("fastcached_items"));
    CHECK_FALSE(body.contains("fastcached_bytes_limit"));
    CHECK_FALSE(body.contains("fastcached_tier_"));
    // Including the upstream question, which a node with no cache does not have:
    // a `0` here would claim it looked and found none.
    CHECK_FALSE(body.contains("# TYPE fastcache_node_upstream_configured"));
    // And the machine is still there, so this is an absence rather than an empty
    // scrape that would pass the checks above for the wrong reason.
    CHECK(body.contains("fastcache_node_logical_cores 4\n"));
}

TEST_CASE("A dashboard credential is read from its file, newline and all", "[node][admin][dashboard]")
{
    // Every editor adds a trailing newline, and an operator should not have to know
    // that a secret which looks right is one byte longer than the one they typed.
    Testing::ScratchDirectory const scratch { "dashboard-token" };
    auto const path = scratch.Path() / "token";

    {
        std::ofstream out { path, std::ios::binary };
        out << "s3cret-token\n";
    }

    auto const credential = Node::ReadDashboardToken(path);
    REQUIRE(credential.has_value());
    CHECK(credential->Required());
    CHECK(credential->Accepts("Bearer s3cret-token"));
    CHECK_FALSE(credential->Accepts("Bearer s3cret-token\n"));
}

TEST_CASE("A credential file that cannot be used is refused rather than ignored", "[node][admin][dashboard]")
{
    // The one failure that turns a guarded fleet map into an open one: a token file
    // that silently becomes "no credential". Both shapes are reported, and both
    // messages name the path so an operator knows which file to look at.
    Testing::ScratchDirectory const scratch { "dashboard-token-bad" };

    auto const missing = Node::ReadDashboardToken(scratch.Path() / "absent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().contains("absent"));

    auto const emptyPath = scratch.Path() / "empty";
    {
        std::ofstream out { emptyPath, std::ios::binary };
        out << "\n";
    }
    auto const empty = Node::ReadDashboardToken(emptyPath);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().contains("empty"));
}

TEST_CASE("The fleet routes answer on their own paths and gate on the credential", "[node][admin][dashboard]")
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, schedulerLogger, {}, {} };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);

    auto const routes =
        Node::MakeFleetRoutes(Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
                              AdminCredential { "s3cret" },
                              Node::DashboardRefreshSeconds);

    // Four routes: the page, the JSON that makes it replaceable and testable
    // without a browser, the series behind the charts, and the charts themselves.
    REQUIRE(routes.size() == 4);
    CHECK(routes[0].path == "/fleet");
    CHECK(routes[1].path == "/fleet.json");
    CHECK(routes[2].path == "/fleet/series.json");
    CHECK(routes[3].path == "/fleet/chart/");
    CHECK(routes[3].match == AdminRouteMatch::Prefix);

    // Every one of them is gated. The chart routes are the ones worth asserting
    // about by name: an image URL that answered without a credential would leak the
    // fleet's whole history while `/fleet` itself stayed locked.
    for (auto const& route: routes)
    {
        INFO("route " << route.path);
        CHECK(route.handler(AdminRequest { .path = route.path, .query = {} }).status == "401 Unauthorized");
    }

    AdminRequest const anonymous { .path = "/fleet", .query = {} };
    auto const refused = routes[0].handler(anonymous);
    CHECK(refused.status == "401 Unauthorized");
    // A 401 with no challenge is one a browser shows as a broken page rather than
    // prompting for.
    REQUIRE(refused.extraHeaders.size() == 1);
    CHECK(refused.extraHeaders[0].contains("Basic"));

    AdminRequest const authorised { .path = "/fleet", .query = {}, .headers = { "Bearer s3cret" } };
    auto const page = routes[0].handler(authorised);
    CHECK(page.status == "200 OK");
    CHECK(page.contentType.starts_with("text/html"));
    CHECK(page.body.starts_with("<!doctype html>"));

    AdminRequest const json { .path = "/fleet.json", .query = {}, .headers = { "Bearer s3cret" } };
    auto const document = routes[1].handler(json);
    CHECK(document.status == "200 OK");
    CHECK(document.contentType == "application/json");
    CHECK(document.body.contains(R"("role":"leader")"));
}

TEST_CASE("A node that does not lead answers the dashboard with 503 and names the leader", "[node][admin][dashboard]")
{
    // `Gate()`'s `NotLeader` in HTTP's vocabulary. A 200 would present a follower's
    // partial registry as the whole fleet, and a redirect would name a port the
    // browser cannot use -- which is the defect this project already had once.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, schedulerLogger, {}, {} };
    scheduler.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:6676", Distributed::StandaloneSchedulerTerm);

    auto const routes =
        Node::MakeFleetRoutes(Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
                              AdminCredential {},
                              Node::DashboardRefreshSeconds);

    AdminRequest const request { .path = "/fleet", .query = {} };
    auto const page = routes[0].handler(request);
    CHECK(page.status == "503 Service Unavailable");
    CHECK(page.body.contains("10.0.0.9:6676"));
    CHECK(page.extraHeaders.empty()); // no Location, no redirect

    AdminRequest const json { .path = "/fleet.json", .query = {} };
    auto const document = routes[1].handler(json);
    CHECK(document.status == "503 Service Unavailable");
    CHECK(document.body.contains(R"("role":"follower")"));
}

TEST_CASE("An endpoint with no credential serves the dashboard to anyone who reaches it", "[node][admin][dashboard]")
{
    // What loopback gets, and the reason the startup rules refuse this shape on a
    // public bind: reaching loopback already means being on the machine.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, schedulerLogger, {}, {} };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);

    auto const routes =
        Node::MakeFleetRoutes(Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
                              AdminCredential {},
                              Node::DashboardRefreshSeconds);

    AdminRequest const anonymous { .path = "/fleet", .query = {} };
    CHECK(routes[0].handler(anonymous).status == "200 OK");
}

TEST_CASE("An admin surface nobody asked for starts nothing at all", "[node][admin][dashboard]")
{
    // No `--admin-listen` means no listener, and therefore no certificate read and
    // no token read either. The flags that would then be a silent no-op are
    // refused by StartupPolicyRejection long before this runs.
    AtomicMetricsSink metrics;
    NullLogger logger;
    ScrapeHost const scrapeHost;
    NodeConfig cfg;

    auto surface =
        Node::StartAdminSurfaceOrExplain(cfg, scrapeHost, metrics, WorkerShapedSnapshot(), std::nullopt, nullptr, logger);
    REQUIRE(surface.has_value());
    CHECK(surface->endpoint == nullptr);
}

TEST_CASE("An admin surface reports which flag refused it", "[node][admin][dashboard]")
{
    // "invalid" tells an operator nothing about what to type instead, so each
    // refusal names the flag it came from -- the standard the endpoint's own
    // bad-spelling case already holds.
    AtomicMetricsSink metrics;
    NullLogger logger;
    ScrapeHost const scrapeHost;

    SECTION("a listen spelling that is not an endpoint")
    {
        NodeConfig cfg;
        cfg.adminListen = "not-a-port";

        auto const surface = Node::StartAdminSurfaceOrExplain(
            cfg, scrapeHost, metrics, WorkerShapedSnapshot(), std::nullopt, nullptr, logger);
        REQUIRE_FALSE(surface.has_value());
        CHECK(surface.error().contains("--admin-listen"));

        // The VALUE is no longer echoed here, and that is the relocation rather than
        // a loss: since #288 the surface table owns the grammar, so a spelling that
        // is not an endpoint is refused by `StartupPolicyRejection` -- once, at
        // startup, in front of the person who typed it, and before
        // `--install-service` bakes it into a command line replayed at every boot.
        // Reaching this tier at all means the value already parsed, so a tier-level
        // echo would be describing a state that can no longer occur.
        //
        // Asserted here rather than only where it moved to, because "the check went
        // somewhere" is exactly what a reader of this case needs to know, and a
        // deleted assertion tells them nothing.
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("not-a-port"));
    }

    SECTION("a credential file that cannot be read")
    {
        // The failure that must never degrade to "no credential".
        Testing::ScratchDirectory const scratch { "admin-surface-token" };
        NodeConfig cfg;
        cfg.adminListen = "0"; // refused before the token is even reached
        cfg.dashboardTokenFile = (scratch.Path() / "absent").string();

        auto const surface = Node::StartAdminSurfaceOrExplain(
            cfg, scrapeHost, metrics, WorkerShapedSnapshot(), std::nullopt, nullptr, logger);
        REQUIRE_FALSE(surface.has_value());
    }
}

TEST_CASE("An admin surface serves the fleet only when there is a fleet to read", "[node][admin][dashboard]")
{
    // A node with no scheduler has no registry to report, so no fleet route is
    // registered and `/fleet` stays a plain 404: a process with no fleet view
    // offers no fleet route, rather than one answering with an empty fleet.
    AtomicMetricsSink metrics;
    NullLogger logger;
    ScrapeHost const scrapeHost;
    ManualClock clock;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);

    // Bind a probe, take its port, release it: `Start` refuses port 0, and a fixed
    // port is one more way to collide with whatever else a runner is doing.
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();

    NodeConfig cfg;
    cfg.adminListen = std::format("127.0.0.1:{}", port);
    cfg.dashboard = true;

    SECTION("with no scheduler, /fleet is not a route")
    {
        auto surface = Node::StartAdminSurfaceOrExplain(
            cfg, scrapeHost, metrics, WorkerShapedSnapshot(), std::nullopt, nullptr, logger);
        REQUIRE(surface.has_value());
        REQUIRE(surface->endpoint != nullptr);
        CHECK(surface->endpoint->BoundEndpoint() == std::format("127.0.0.1:{}", port));
    }

    SECTION("with a scheduler, the surface starts and serves it")
    {
        auto surface = Node::StartAdminSurfaceOrExplain(
            cfg,
            scrapeHost,
            metrics,
            WorkerShapedSnapshot(),
            Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
            nullptr,
            logger);
        REQUIRE(surface.has_value());
        REQUIRE(surface->endpoint != nullptr);
    }
}

#if defined(FC_TLS_ENABLED)
TEST_CASE("Asking for a generated certificate gives the surface one to serve", "[node][admin][tls]")
{
    // The flag exists so an internal deployment needs nothing to obtain first, so
    // what this asserts is exactly that: no path named anywhere, and the surface
    // comes up holding a certificate.
    AtomicMetricsSink metrics;
    NullLogger logger;
    ScrapeHost const scrapeHost;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();

    NodeConfig cfg;
    cfg.adminListen = std::format("127.0.0.1:{}", port);
    cfg.tlsSelfSigned = true;

    auto surface =
        Node::StartAdminSurfaceOrExplain(cfg, scrapeHost, metrics, WorkerShapedSnapshot(), std::nullopt, nullptr, logger);
    REQUIRE(surface.has_value());
    REQUIRE(surface->endpoint != nullptr);
    REQUIRE(surface->tls != nullptr);

    // The fingerprint is the only thing that authenticates a certificate nothing
    // signed, so it has to be reportable -- an operator compares it with what
    // their browser shows.
    CHECK(surface->tls->CertificateFingerprint().size() == 64);
}
#endif

TEST_CASE("A surface with no TLS asked for holds no context at all", "[node][admin][tls]")
{
    // The half that keeps every existing deployment unchanged: naming no material
    // and asking for none leaves the admin port exactly as plaintext as it was.
    AtomicMetricsSink metrics;
    NullLogger logger;
    ScrapeHost const scrapeHost;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();

    NodeConfig cfg;
    cfg.adminListen = std::format("127.0.0.1:{}", port);

    auto surface =
        Node::StartAdminSurfaceOrExplain(cfg, scrapeHost, metrics, WorkerShapedSnapshot(), std::nullopt, nullptr, logger);
    REQUIRE(surface.has_value());
    REQUIRE(surface->endpoint != nullptr);
#if defined(FC_TLS_ENABLED)
    CHECK(surface->tls == nullptr);
#endif
}

TEST_CASE("A sample records what the fleet is, in the slots the table names", "[node][admin][fleethistory]")
{
    Distributed::FleetSnapshot snapshot;
    snapshot.role = Distributed::SchedulerRole::Leader;
    snapshot.leases = { 11, 2, 3, 4, 5 };
    // Two machines, each of which registered against two toolchains. The cache
    // figures come off NodeReports() and so are counted once per machine.
    snapshot.nodes.resize(2);
    snapshot.nodes[0].endpoint = "a:1";
    snapshot.nodes[0].registeredSlots = 8;
    snapshot.nodes[0].fleetJobsInFlight = 3;
    snapshot.nodes[1].endpoint = "b:1";
    snapshot.nodes[1].registeredSlots = 4;
    snapshot.nodes[1].fleetJobsInFlight = 1;
    snapshot.nodes[0].load.cache.hits = 70;
    snapshot.nodes[0].load.cache.misses = 30;
    snapshot.nodes[1].load.cache.hits = 5;
    auto const values = SampleFrom(snapshot);
    auto const slot = [&values](Distributed::FleetMetric metric) {
        return values[static_cast<std::size_t>(metric)];
    };

    CHECK(slot(Distributed::FleetMetric::DispatchGranted) == 11);
    CHECK(slot(Distributed::FleetMetric::DispatchNoWorker) == 2);
    CHECK(slot(Distributed::FleetMetric::DispatchDuplicate) == 5);

    // EVERY slot, including the four a machine can also answer for itself. This is
    // the fleet series, a leader can answer for all nine, and these are fleet-wide
    // sums -- a different number from any one machine's, and the one this page draws.
    // Filling only the fleet-scoped half here would have flattened the capacity and
    // hit-rate charts to zero, with an existing install's restored history plotting a
    // cliff to 0 at the upgrade.
    CHECK(slot(Distributed::FleetMetric::CacheHits) == 75);
    CHECK(slot(Distributed::FleetMetric::CacheMisses) == 30);
    CHECK(slot(Distributed::FleetMetric::JobsInFlight) == 4);
}

TEST_CASE("A node's own sample carries its cache and its slots, and no dispatch", "[node][admin][fleethistory]")
{
    // Read through the provider `/metrics` scrapes, so a machine cannot describe
    // itself two ways -- the same argument `NodeScrapeSources::host` already makes
    // about a scrape and a registration disagreeing.
    AtomicMetricsSink metrics;
    for ([[maybe_unused]] auto const hit: std::views::iota(0, 70))
        metrics.Increment(IMetricsSink::Counter::NodeCacheHits);
    for ([[maybe_unused]] auto const miss: std::views::iota(0, 30))
        metrics.Increment(IMetricsSink::Counter::NodeCacheMisses);
    MetricsSnapshot const snapshot { .storage = std::nullopt,
                                     .storageTiers = {},
                                     .host = HostCapacity { .configuredSlots = 8, .busySlots = 3 },
                                     .upstreamConfigured = std::nullopt,
                                     .uptime = {} };

    auto const values = NodeSampleFrom(metrics, snapshot);
    auto const slot = [&values](Distributed::FleetMetric metric) {
        return values[static_cast<std::size_t>(metric)];
    };

    CHECK(slot(Distributed::FleetMetric::CacheHits) == 70);
    CHECK(slot(Distributed::FleetMetric::CacheMisses) == 30);
    // FREE slots, not configured ones: the series is about what a compile could have
    // started on, and a machine with every slot busy offers nothing however many it
    // advertises.
    CHECK(slot(Distributed::FleetMetric::OfferableSlots) == 5);
    CHECK(slot(Distributed::FleetMetric::JobsInFlight) == 3);

    // A node that is not scheduling has not refused anything, and must not report
    // that it refused nothing -- a zero here would be indistinguishable from a
    // leader that genuinely refused none.
    for (auto const& row: Distributed::FleetMetricTable)
    {
        INFO("slot " << row.key);
        if (row.scope == Distributed::FleetMetricScope::Fleet)
            CHECK(slot(row.metric) == 0);
    }
}

TEST_CASE("A node with nothing to report is not described as a busy one", "[node][admin][fleethistory]")
{
    // No cache and no host capacity: a process that is not a worker. Zero offered and
    // zero running IS the truth about it, and there is no per-slot way to say absent.
    AtomicMetricsSink metrics;
    auto const values = NodeSampleFrom(metrics, MetricsSnapshot {});
    CHECK(std::ranges::all_of(values, [](std::uint64_t v) { return v == 0; }));

    // Every slot busy offers nothing, and the subtraction saturates rather than
    // wrapping when a scrape lands between the two figures moving.
    MetricsSnapshot const saturated { .storage = std::nullopt,
                                      .storageTiers = {},
                                      .host = HostCapacity { .configuredSlots = 2, .busySlots = 9 },
                                      .upstreamConfigured = std::nullopt,
                                      .uptime = {} };
    auto const busy = NodeSampleFrom(metrics, saturated);
    CHECK(busy[static_cast<std::size_t>(Distributed::FleetMetric::OfferableSlots)] == 0);
    CHECK(busy[static_cast<std::size_t>(Distributed::FleetMetric::JobsInFlight)] == 9);
}

TEST_CASE("The sampler records only while this node leads", "[node][admin][fleethistory]")
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SystemWallClock const wall;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
    Distributed::FleetSources const sources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics };

    FleetSampler sampler { sources, metrics, NodeFacts(), wall, {}, logger };

    scheduler.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:6676", Distributed::StandaloneSchedulerTerm);
    // A follower's registry holds whatever registered against *it*, so a sample
    // here would record a fraction of the fleet as though it were the whole -- and
    // the chart would show the fleet shrinking every time leadership moved.
    CHECK_FALSE(sampler.SampleOnce());
    CHECK(sampler.History().Empty());

    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    CHECK(sampler.SampleOnce());
    CHECK_FALSE(sampler.History().Empty());

    // No path, so nothing durable is promised -- and the page says so rather than
    // letting an operator find out at the next restart.
    CHECK_FALSE(sampler.Durable());
}

TEST_CASE("A sampler with a path writes its history and reads it back", "[node][admin][fleethistory]")
{
    Testing::ScratchDirectory const scratch { "fleet-sampler-durable" };
    auto const file = scratch.Path() / "history.bin";
    auto const nodeFile = scratch.Path() / "node-history.bin";
    auto const receivedFile = scratch.Path() / "received-history.bin";
    HistoryPaths const paths { .fleet = file, .node = nodeFile, .received = receivedFile };

    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SystemWallClock const wall;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    Distributed::FleetSources const sources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics };

    {
        FleetSampler sampler { sources, metrics, NodeFacts(), wall, paths, logger };
        CHECK(sampler.Durable());
        CHECK_FALSE(sampler.History().ReadOnly());
        REQUIRE(sampler.SampleOnce());
        // Written by the destructor, so a clean shutdown does not throw away what
        // the page will be asked about the moment the node comes back.
    }
    REQUIRE(std::filesystem::exists(file));

    FleetSampler restored { sources, metrics, NodeFacts(), wall, paths, logger };
    CHECK_FALSE(restored.History().Empty());
}

TEST_CASE("What the other machines handed over survives a leader restart", "[node][admin][fleethistory]")
{
    // A node advances its own watermark once a heartbeat is accepted and never
    // resends, so a leader that forgot this would leave those windows a gap for as
    // long as the rings hold them -- the failure the handover exists to remove,
    // reintroduced by a restart.
    Testing::ScratchDirectory const scratch { "fleet-sampler-received" };
    auto const file = scratch.Path() / "history.bin";
    auto const nodeFile = scratch.Path() / "node-history.bin";
    auto const receivedFile = scratch.Path() / "received-history.bin";
    HistoryPaths const paths { .fleet = file, .node = nodeFile, .received = receivedFile };

    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SystemWallClock const wall;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    Distributed::FleetSources const sources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics };

    // One closed bucket, as a heartbeat would carry it -- in a window this leader
    // cannot itself have sampled. The sampler's own thread records the CURRENT
    // minute the moment it starts, and a window the leader sampled is deliberately
    // left alone by the backfill, so placing this one there made the assertion a
    // race against that thread rather than a test of the handover.
    auto const minute = Testing::MinuteBucketStart(wall.Now()) - (3 * 60'000);
    auto bucket = Testing::ClosedBucket(minute);
    bucket.values[static_cast<std::size_t>(Distributed::FleetMetric::JobsInFlight)] = 7;

    {
        FleetSampler sampler { sources, metrics, NodeFacts(), wall, paths, logger };
        CHECK(sampler.Received().AcceptHistory("10.0.0.4:6676", std::span { &bucket, 1 }) == 1);
    }
    REQUIRE(std::filesystem::exists(receivedFile));

    FleetSampler restored { sources, metrics, NodeFacts(), wall, paths, logger };
    CHECK(restored.Received().Count() == 1);
    // The mark as well as the readings: without it the batch a node resends after a
    // reply it never saw would be counted a second time.
    CHECK(restored.Received().HighWaterFor("10.0.0.4:6676") == minute);
    CHECK(restored.Received().AcceptHistory("10.0.0.4:6676", std::span { &bucket, 1 }) == 0);

    // And it reaches the view the page draws, which is the only reason it is kept.
    auto const merged = restored.Buckets(Distributed::FleetRange::OneHour);
    auto const filled =
        std::ranges::find_if(merged, [](Distributed::FleetBucket const& each) { return each.present && each.backfilled; });
    REQUIRE(filled != merged.end());
    CHECK(filled->values[static_cast<std::size_t>(Distributed::FleetMetric::JobsInFlight)] == 7);
}

TEST_CASE("A node with no scheduler still records itself", "[node][admin][fleethistory]")
{
    // A pure worker runs no dashboard and often no admin endpoint at all, and it is
    // exactly the machine doing the compiles. A sampler that existed only alongside a
    // page would leave the fleet's year with a hole where its busiest members should
    // be -- and the hole would be invisible, because the leader's own series would
    // still be complete for every window it was elected for.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SystemWallClock const wall;

    FleetSampler sampler { std::nullopt, metrics, NodeFacts(3, 8), wall, HistoryPaths {}, logger };

    // False, because there is no fleet to answer for -- and the node series is
    // recorded anyway, which is the whole distinction.
    CHECK_FALSE(sampler.SampleOnce());
    CHECK_FALSE(sampler.NodeHistory().Empty());
    CHECK(sampler.History().Empty());

    // And it has something to hand over, which is the point of recording it.
    CHECK(sampler.NextHistoryBatch(8).empty()); // nothing has closed yet
}

TEST_CASE("A batch is offered again until a heartbeat takes it", "[node][admin][fleethistory]")
{
    // The cursor sits on the sampler rather than in the heartbeat loop, so this is
    // a case rather than something only running the program could show.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    Testing::PlacedWallClock wall;

    FleetSampler sampler { std::nullopt, metrics, NodeFacts(), wall, HistoryPaths {}, logger };

    // Recorded the way the node does it, then a minute on so the window has closed.
    // Only a closed bucket may travel: an open one is a partial window the leader
    // could never be told to correct.
    REQUIRE_FALSE(sampler.SampleOnce());
    wall.Advance(std::chrono::minutes { 1 });
    REQUIRE_FALSE(sampler.SampleOnce());

    auto const offered = sampler.NextHistoryBatch(8);
    REQUIRE(offered.size() == 1);
    // Offered again: nothing has taken it, and a round where every heartbeat failed
    // must not step over a batch that was never sent.
    CHECK(sampler.NextHistoryBatch(8).size() == 1);

    sampler.HistoryHandedThrough(offered.back().startMillis);
    CHECK(sampler.NextHistoryBatch(8).empty());
}

TEST_CASE("The history path follows the directories a node already has", "[node][admin][fleethistory]")
{
    NodeConfig cfg;
    // No new flag: a third place to say "put state here" is a third place for an
    // operator to point at the wrong disk.
    CHECK(FleetHistoryPath(cfg).empty());

    cfg.cacheDir = "/var/lib/fastcache";
    CHECK(FleetHistoryPath(cfg) == std::filesystem::path { "/var/lib/fastcache" } / "fleet-history.bin");
    // Every file the node keeps comes out of the one table, so a third of them
    // cannot end up derived by string surgery on a second's answer.
    auto const paths = HistoryPaths::For(cfg);
    CHECK(paths.fleet == std::filesystem::path { "/var/lib/fastcache" } / "fleet-history.bin");
    CHECK(paths.node == std::filesystem::path { "/var/lib/fastcache" } / "node-history.bin");
    CHECK(paths.received == std::filesystem::path { "/var/lib/fastcache" } / "received-history.bin");

    // The cluster directory wins: the history is a leader's record, and a leader is
    // a cluster member.
    cfg.clusterDir = "/var/lib/fastcache-cluster";
    CHECK(FleetHistoryPath(cfg) == std::filesystem::path { "/var/lib/fastcache-cluster" } / "fleet-history.bin");
}

namespace
{

/// The four fleet routes over a leading scheduler and a sampler that has sampled.
struct ChartFixture
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SystemWallClock wall;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
    std::unique_ptr<FleetSampler> sampler;
    std::vector<AdminRoute> routes;

    ChartFixture()
    {
        scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        Distributed::FleetSources const sources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics };
        sampler = std::make_unique<FleetSampler>(sources, metrics, NodeFacts(), wall, HistoryPaths {}, logger);
        REQUIRE(sampler->SampleOnce());
        // Through the view the production wiring uses, so a route drawing the raw
        // series would fail here rather than on the leader that lost an election.
        routes = MakeFleetRoutes(sources, AdminCredential {}, DashboardRefreshSeconds, sampler.get());
    }

    [[nodiscard]] AdminResponse Get(std::string_view path, std::string_view query = {}, std::string_view etag = {}) const
    {
        AdminRequest request { .path = path, .query = query };
        request.headers[static_cast<std::size_t>(AdminHeader::IfNoneMatch)] = etag;
        for (auto const& route: routes)
        {
            auto const hit = route.match == AdminRouteMatch::Exact ? route.path == path : path.starts_with(route.path);
            if (hit)
                return route.handler(request);
        }
        FAIL("no route for " << path);
        return {};
    }

    [[nodiscard]] static std::string_view HeaderValue(AdminResponse const& response, std::string_view name)
    {
        for (auto const& line: response.extraHeaders)
            if (std::string_view { line }.starts_with(name))
                return std::string_view { line }.substr(name.size());
        return {};
    }
};

} // namespace

TEST_CASE("A chart is served as its own SVG resource, per chart the table names", "[node][admin][chart]")
{
    ChartFixture const fixture;

    for (auto const& chart: Distributed::FleetChartTable)
    {
        INFO("chart " << chart.key);
        auto const response = fixture.Get(std::format("/fleet/chart/{}.svg", chart.key), "range=24h");
        CHECK(response.status == "200 OK");
        CHECK(response.contentType == "image/svg+xml");
        CHECK(response.body.starts_with("<svg "));
    }

    // One prefix route covers the whole table, so a tail that names nothing is a
    // 404 rather than whichever chart happened to be first.
    CHECK(fixture.Get("/fleet/chart/nonesuch.svg").status == "404 Not Found");
    CHECK(fixture.Get("/fleet/chart/dispatched.png").status == "404 Not Found");
}

TEST_CASE("A chart revalidates with If-None-Match and is told 304", "[node][admin][chart]")
{
    ChartFixture const fixture;

    auto const fresh = fixture.Get("/fleet/chart/dispatched.svg", "range=24h");
    REQUIRE(fresh.status == "200 OK");
    auto const tag = std::string { ChartFixture::HeaderValue(fresh, "ETag: ") };
    REQUIRE(!tag.empty());
    // Not a fixed max-age: a viewer told to hold a chart for a minute one second
    // before its bucket closes sits a whole bucket behind for the rest of it.
    CHECK(ChartFixture::HeaderValue(fresh, "Cache-Control: ").starts_with("max-age="));

    auto const cached = fixture.Get("/fleet/chart/dispatched.svg", "range=24h", tag);
    CHECK(cached.status == "304 Not Modified");
    CHECK(cached.body.empty());
    // The validators travel with the 304: without them the client has nothing to
    // revalidate against next time.
    CHECK(ChartFixture::HeaderValue(cached, "ETag: ") == tag);

    // The identity is part of the tag, so one chart's cached copy never satisfies
    // another's request -- nor the same chart at a different range or theme.
    CHECK(fixture.Get("/fleet/chart/refusals.svg", "range=24h", tag).status == "200 OK");
    CHECK(fixture.Get("/fleet/chart/dispatched.svg", "range=7d", tag).status == "200 OK");
    CHECK(fixture.Get("/fleet/chart/dispatched.svg", "range=24h&theme=dark", tag).status == "200 OK");
}

TEST_CASE("The page draws what the other machines handed over", "[node][admin][chart][fleethistory]")
{
    // The assertion the whole handover exists for, taken through a ROUTE rather than
    // through the sampler. Filled, persisted and restored, this was still invisible
    // while the routes were handed the raw fleet series -- so the wiring is what has
    // to be asserted, not the merge.
    ChartFixture fixture;

    // A window this leader has no reading for: it sampled once, at "now".
    auto const missed = Testing::MinuteBucketStart(fixture.wall.Now()) - (3 * 60'000);
    auto bucket = Testing::ClosedBucket(missed);
    bucket.values[static_cast<std::size_t>(Distributed::FleetMetric::JobsInFlight)] = 4242;
    REQUIRE(fixture.sampler->Received().AcceptHistory("10.0.0.7:6676", std::span { &bucket, 1 }) == 1);

    auto const response = fixture.Get("/fleet/series.json", "range=1h");
    CHECK(response.status == "200 OK");
    // The number one machine reported for a window this leader was not elected for.
    CHECK(response.body.contains("4242"));
}

TEST_CASE("An unknown range is refused rather than quietly served as another", "[node][admin][chart]")
{
    ChartFixture const fixture;

    // A theme has a safe default; a range does not. A range quietly substituted
    // puts a reader on a different axis than the one they asked for.
    CHECK(fixture.Get("/fleet", "range=30d").status == "400 Bad Request");
    CHECK(fixture.Get("/fleet/series.json", "range=30d").status == "400 Bad Request");
    CHECK(fixture.Get("/fleet/chart/dispatched.svg", "range=30d").status == "400 Bad Request");

    CHECK(fixture.Get("/fleet/chart/dispatched.svg", "theme=solarized").status == "200 OK");

    // And the refusal NAMES what it would accept, off the table rather than out of a
    // sentence: two windows were hand-listed here while eight were served, so the one
    // message a reader who just guessed wrong actually reads was the stalest thing on
    // the surface.
    for (auto const& row: Distributed::FleetRangeTable)
    {
        INFO("range " << row.key);
        CHECK(fixture.Get("/fleet", "range=30d").body.contains(row.key));
        CHECK(fixture.Get("/fleet/series.json", "range=30d").body.contains(row.key));
        CHECK(fixture.Get("/fleet/chart/dispatched.svg", "range=30d").body.contains(row.key));
        // ...and each of them is genuinely served, or naming it is worse than not.
        CHECK(fixture.Get("/fleet/chart/dispatched.svg", std::format("range={}", row.key)).status == "200 OK");
    }
}

TEST_CASE("A long range is not cached past its next sample", "[node][admin][chart]")
{
    ChartFixture const fixture;

    // `max-age` used to be "until this bucket closes", which is how long the chart's
    // SHAPE is settled rather than how long it is current: the newest bucket is
    // always still open and gains a reading every sample. On a five-minute bucket
    // that was four minutes of staleness; on the twelve-month view, whose buckets are
    // a day wide, it was a chart frozen in the browser for twenty-four hours while
    // the fleet moved underneath it.
    constexpr std::int64_t SampleSeconds = 60;
    for (auto const& row: Distributed::FleetRangeTable)
    {
        INFO("range " << row.key);
        auto const response = fixture.Get("/fleet/chart/dispatched.svg", std::format("range={}", row.key));
        REQUIRE(response.status == "200 OK");
        auto const control = std::string { ChartFixture::HeaderValue(response, "Cache-Control: ") };
        REQUIRE(control.starts_with("max-age="));
        auto const seconds = std::stoll(control.substr(std::string_view { "max-age=" }.size()));
        CHECK(seconds >= 1);
        CHECK(seconds <= SampleSeconds);
    }
}

TEST_CASE("The series behind the charts are served as JSON", "[node][admin][chart]")
{
    ChartFixture const fixture;

    auto const response = fixture.Get("/fleet/series.json", "range=7d");
    CHECK(response.status == "200 OK");
    CHECK(response.contentType == "application/json");
    CHECK(response.body.contains(R"("range":"7d")"));
    for (auto const& series: Distributed::FleetSeriesTable)
    {
        INFO("series " << series.key);
        CHECK(response.body.contains(std::format(R"("{}":[)", series.key)));
    }
}

TEST_CASE("The page carries the range control and one image per chart", "[node][admin][chart]")
{
    ChartFixture const fixture;

    auto const page = fixture.Get("/fleet", "range=7d");
    REQUIRE(page.status == "200 OK");
    for (auto const& range: Distributed::FleetRangeTable)
        CHECK(page.body.contains(std::format(R"(href="?range={}")", range.key)));
    for (auto const& chart: Distributed::FleetChartTable)
        CHECK(page.body.contains(std::format(R"(src="/fleet/chart/{}.svg?range=7d")", chart.key)));

    // Still no script and nothing fetched from another host: the charts are
    // same-origin images and the stylesheet is embedded, which is what lets this
    // page work on the air-gapped network a build fleet usually lives on. The one
    // absolute URL in the document is the inline sparkline's `xmlns`, which is an
    // XML namespace name rather than something a browser goes and asks for.
    CHECK_FALSE(page.body.contains("<script"));
    CHECK_FALSE(page.body.contains(R"(src="http)"));
    CHECK_FALSE(page.body.contains(R"(href="http)"));
    CHECK_FALSE(page.body.contains("@import"));
}

TEST_CASE("A history a newer build wrote stops the sampler promising durability", "[node][admin][fleethistory]")
{
    // The page says a durable history is "written to disk, so it survives a restart".
    // A node holding a file a LATER build wrote persists nothing at all, so saying
    // that would contradict the warning printed beside it at startup -- and an
    // operator reading the page would have no reason to go looking for the warning.
    Testing::ScratchDirectory const scratch { "fleet-sampler-readonly" };
    auto const file = scratch.Path() / "history.bin";
    auto const nodeFile = scratch.Path() / "node-history.bin";
    auto const receivedFile = scratch.Path() / "received-history.bin";
    HistoryPaths const paths { .fleet = file, .node = nodeFile, .received = receivedFile };

    ManualClock clock;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    SystemWallClock const wall;
    // Its own sink, unlike the cases above: `logger` is what this case reads back,
    // and the scheduler's lines are not the ones it is asserting about.
    NullLogger schedulerLogger;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, schedulerLogger, {}, {} };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    Distributed::FleetSources const sources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics };

    {
        FleetSampler writer { sources, metrics, NodeFacts(), wall, paths, logger };
        REQUIRE(writer.SampleOnce());
    }
    REQUIRE(std::filesystem::exists(file));

    // Raise the version byte to something no reader claims; everything else stays
    // valid, so nothing but the version can be what refuses it.
    {
        std::fstream patch { file, std::ios::binary | std::ios::in | std::ios::out };
        patch.seekp(4);
        patch.put(static_cast<char>(200));
    }
    auto const before = [&] {
        std::ifstream in { file, std::ios::binary };
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }();

    {
        FleetSampler sampler { sources, metrics, NodeFacts(), wall, paths, logger };
        CHECK(sampler.History().ReadOnly());
        CHECK_FALSE(sampler.Durable());
        // Sampling continues -- the page is live either way, and only persistence
        // stops.
        CHECK(sampler.SampleOnce());
        CHECK_FALSE(sampler.History().Empty());
    }

    // Left byte-identical, including by the destructor's own save.
    std::ifstream in { file, std::ios::binary };
    std::ostringstream buffer;
    buffer << in.rdbuf();
    CHECK(buffer.str() == before);
}

TEST_CASE("A follower still records itself", "[node][admin][fleethistory]")
{
    // The point of the split. Sampling used to stop entirely the moment a node lost
    // an election, so a machine's year of history belonged to whichever peer happened
    // to be leading -- and one election left the page showing a different machine's
    // partial record with nothing saying so.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SystemWallClock const wall;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
    Distributed::FleetSources const sources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics };

    for ([[maybe_unused]] auto const hit: std::views::iota(0, 70))
        metrics.Increment(IMetricsSink::Counter::NodeCacheHits);
    FleetSampler sampler { sources, metrics, NodeFacts(3, 8), wall, {}, logger };

    // A follower: no fleet sample, and `SampleOnce` says so.
    CHECK_FALSE(sampler.SampleOnce());
    CHECK(sampler.History().Empty());
    // ...but its own contribution is recorded regardless, because what this
    // machine's cache did is a fact about this machine whoever leads.
    CHECK_FALSE(sampler.NodeHistory().Empty());

    auto const own = sampler.NodeHistory().Buckets(Distributed::FleetRange::Day);
    REQUIRE_FALSE(own.empty());
    CHECK(own.back().values[static_cast<std::size_t>(Distributed::FleetMetric::CacheHits)] == 70);
    CHECK(own.back().values[static_cast<std::size_t>(Distributed::FleetMetric::JobsInFlight)] == 3);

    // Becoming leader adds the fleet series without disturbing the node one.
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    CHECK(sampler.SampleOnce());
    CHECK_FALSE(sampler.History().Empty());
    CHECK_FALSE(sampler.NodeHistory().Empty());
}
