// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <optional>
#include <string>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{
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
} // namespace

TEST_CASE("An unparseable --admin-listen is refused, not guessed at", "[node][admin]")
{
    // A worker that started without the endpoint an operator asked for looks
    // healthy to the very probe that would have reported it was not, so this is
    // fatal at startup rather than a warning. The message names what was rejected,
    // because "invalid" tells an operator nothing about what to type instead.
    AtomicMetricsSink metrics;
    NullLogger logger;

    for (auto const* const spelling: { "", "not-a-port", "127.0.0.1", "6674x", "0", "70000", "[::1]6674" })
    {
        INFO("spelling '" << spelling << "'");
        auto const started = AdminEndpoint::Start(spelling, "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().contains(spelling));
    }
}

TEST_CASE("A bare port binds loopback rather than the wildcard", "[node][admin]")
{
    // The default is the security-relevant half of this flag: a scrape surface on a
    // public interface must be something an operator wrote down, not something they
    // got by typing a port. Asserted through the address the endpoint reports,
    // since that is what an operator reads back off the log line.
    AtomicMetricsSink metrics;
    NullLogger logger;

    auto const started = AdminEndpoint::Start("0", "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
    // Port 0 is refused outright -- "any free port" is an address nobody can be
    // told in advance -- so bind an ephemeral one the ordinary way instead.
    REQUIRE_FALSE(started.has_value());

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();

    auto const bare = AdminEndpoint::Start(std::to_string(port), "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
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
    auto const started = AdminEndpoint::Start(unreachable, "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
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
        auto started = AdminEndpoint::Start(std::to_string(port), "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
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
    Distributed::SchedulerService scheduler { clock, metrics };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {});

    auto const routes =
        Node::MakeFleetRoutes(Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
                              AdminCredential { "s3cret" },
                              Node::DashboardRefreshSeconds);

    // Two routes, and the JSON one exists so the page is replaceable and testable
    // without a browser.
    REQUIRE(routes.size() == 2);
    CHECK(routes[0].path == "/fleet");
    CHECK(routes[1].path == "/fleet.json");

    AdminRequest const anonymous { .path = "/fleet", .query = {}, .authorization = {} };
    auto const refused = routes[0].handler(anonymous);
    CHECK(refused.status == "401 Unauthorized");
    // A 401 with no challenge is one a browser shows as a broken page rather than
    // prompting for.
    REQUIRE(refused.extraHeaders.size() == 1);
    CHECK(refused.extraHeaders[0].contains("Basic"));

    AdminRequest const authorised { .path = "/fleet", .query = {}, .authorization = "Bearer s3cret" };
    auto const page = routes[0].handler(authorised);
    CHECK(page.status == "200 OK");
    CHECK(page.contentType.starts_with("text/html"));
    CHECK(page.body.starts_with("<!doctype html>"));

    AdminRequest const json { .path = "/fleet.json", .query = {}, .authorization = "Bearer s3cret" };
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
    Distributed::SchedulerService scheduler { clock, metrics };
    scheduler.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:6676");

    auto const routes =
        Node::MakeFleetRoutes(Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
                              AdminCredential {},
                              Node::DashboardRefreshSeconds);

    AdminRequest const request { .path = "/fleet", .query = {}, .authorization = {} };
    auto const page = routes[0].handler(request);
    CHECK(page.status == "503 Service Unavailable");
    CHECK(page.body.contains("10.0.0.9:6676"));
    CHECK(page.extraHeaders.empty()); // no Location, no redirect

    AdminRequest const json { .path = "/fleet.json", .query = {}, .authorization = {} };
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
    Distributed::SchedulerService scheduler { clock, metrics };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {});

    auto const routes =
        Node::MakeFleetRoutes(Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
                              AdminCredential {},
                              Node::DashboardRefreshSeconds);

    AdminRequest const anonymous { .path = "/fleet", .query = {}, .authorization = {} };
    CHECK(routes[0].handler(anonymous).status == "200 OK");
}

TEST_CASE("An admin surface nobody asked for starts nothing at all", "[node][admin][dashboard]")
{
    // No `--admin-listen` means no listener, and therefore no certificate read and
    // no token read either. The flags that would then be a silent no-op are
    // refused by StartupPolicyRejection long before this runs.
    AtomicMetricsSink metrics;
    NullLogger logger;
    NodeConfig cfg;

    auto surface = Node::StartAdminSurfaceOrExplain(cfg, metrics, WorkerShapedSnapshot(), std::nullopt, logger);
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

    SECTION("a listen spelling that is not an endpoint")
    {
        NodeConfig cfg;
        cfg.adminListen = "not-a-port";

        auto const surface = Node::StartAdminSurfaceOrExplain(cfg, metrics, WorkerShapedSnapshot(), std::nullopt, logger);
        REQUIRE_FALSE(surface.has_value());
        CHECK(surface.error().contains("--admin-listen"));
        CHECK(surface.error().contains("not-a-port"));
    }

    SECTION("a credential file that cannot be read")
    {
        // The failure that must never degrade to "no credential".
        Testing::ScratchDirectory const scratch { "admin-surface-token" };
        NodeConfig cfg;
        cfg.adminListen = "0"; // refused before the token is even reached
        cfg.dashboardTokenFile = (scratch.Path() / "absent").string();

        auto const surface = Node::StartAdminSurfaceOrExplain(cfg, metrics, WorkerShapedSnapshot(), std::nullopt, logger);
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
    ManualClock clock;
    Distributed::SchedulerService scheduler { clock, metrics };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {});

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
        auto surface = Node::StartAdminSurfaceOrExplain(cfg, metrics, WorkerShapedSnapshot(), std::nullopt, logger);
        REQUIRE(surface.has_value());
        REQUIRE(surface->endpoint != nullptr);
        CHECK(surface->endpoint->BoundEndpoint() == std::format("127.0.0.1:{}", port));
    }

    SECTION("with a scheduler, the surface starts and serves it")
    {
        auto surface = Node::StartAdminSurfaceOrExplain(
            cfg,
            metrics,
            WorkerShapedSnapshot(),
            Distributed::FleetSources { .scheduler = &scheduler, .cluster = nullptr, .metrics = &metrics },
            logger);
        REQUIRE(surface.has_value());
        REQUIRE(surface->endpoint != nullptr);
    }
}
