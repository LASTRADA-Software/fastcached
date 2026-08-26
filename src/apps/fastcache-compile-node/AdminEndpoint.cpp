// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/HostPort.hpp>
#if defined(FC_TLS_ENABLED)
    #include <FastCache/Net/TlsContext.hpp>
#endif
#include <FastCache/Distributed/FleetView.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <utility>
#include <vector>

namespace FastCache::Node
{

AdminHttpServer::SnapshotProvider MakeNodeSnapshotProvider(NodeScrapeSources sources,
                                                           std::chrono::steady_clock::time_point startedAt)
{
    return [sources = std::move(sources), startedAt] {
        auto const disk = sources.host->SpaceOn(sources.scratchRoot);
        return MetricsSnapshot {
            // The node's own cache, when it has one. It usually does --
            // `--listen-cache` is on by default and a local tier is what this
            // program is FOR -- and this scrape reported `std::nullopt` regardless,
            // so a node holding a quarter of a gigabyte of objects and one holding
            // none produced the same bytes. Null only when the operator turned
            // every half of the tier off, and then absent IS the truth.
            .storage = sources.cache != nullptr ? std::optional { sources.cache->Snapshot() } : std::nullopt,
            // And the halves that merged view cannot show apart: with `--cache-dir`
            // the composite reports the on-disk store alone, so the in-memory tier
            // an operator sized with `--cache-memory` would otherwise be invisible.
            .storageTiers = sources.cache != nullptr ? sources.cache->SnapshotTiers() : TieredStorageStats {},
            .host = HostCapacity { .logicalCores = sources.host->LogicalCores(),
                                   .configuredSlots = sources.slots,
                                   .totalMemoryBytes = sources.host->TotalMemoryBytes(),
                                   .diskCapacityBytes = static_cast<std::uint64_t>(disk.capacityBytes),
                                   .diskFreeBytes = static_cast<std::uint64_t>(disk.freeBytes),
                                   .busySlots = sources.busySlots() },
            .uptime =
                Uptime { std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt) },
        };
    };
}

std::expected<AdminCredential, std::string> ReadDashboardToken(std::filesystem::path const& path)
{
    std::ifstream file { path, std::ios::binary };
    if (!file)
        return std::unexpected { std::format("cannot read '{}'", path.string()) };

    // Via the stream buffer rather than `std::istreambuf_iterator`, which is the
    // workaround this codebase has already had to reach for twice (see
    // `Cc::ReadBytes` and `DefaultConfigPath_test`'s `ReadFile`): GCC at -O3
    // inlines the iterator far enough to see a path where the buffer pointer
    // could be null and rejects it under `-Werror=null-dereference`, which for an
    // `ifstream` it never is. Inserting a `streambuf*` handles null by setting
    // failbit, so there is nothing left for it to complain about.
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto secret = std::move(buffer).str();

    // Trailing whitespace is trimmed because every editor adds a newline, and an
    // operator should not have to know that a secret which looks right is one byte
    // longer than the one they typed. Leading whitespace is NOT trimmed: it is not
    // something an editor adds, and a secret that legitimately begins with a space
    // would otherwise be silently a different secret.
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
        secret.pop_back();

    if (secret.empty())
        return std::unexpected { std::format("'{}' is empty; a credential file nobody can fail to match is "
                                             "worse than none, because the surface looks guarded",
                                             path.string()) };

    return AdminCredential { std::move(secret) };
}

namespace
{
#if defined(FC_TLS_ENABLED)
    /// Names a generated certificate should be valid for.
    ///
    /// Every modern client ignores a certificate's common name, so this list is
    /// what decides whether a browser warns once or twice: an unknown issuer is one
    /// warning an operator can accept, and a name mismatch on top of it is a second
    /// one that is much harder to click past.
    ///
    /// Loopback always, because that is where the surface binds by default; the
    /// machine's own network name, because that is what an operator types to reach
    /// another node; and the admin bind address when it names a particular
    /// interface rather than every one of them -- a wildcard is not a name anybody
    /// can dial, so putting it in a certificate would say nothing.
    /// @param cfg The parsed configuration.
    /// @param host Where the machine's own facts come from.
    /// @return The subject names, loopback first.
    [[nodiscard]] std::vector<std::string> SelfSignedSubjectNames(NodeConfig const& cfg, IHostFactsSource const& host)
    {
        std::vector<std::string> names { "localhost", "127.0.0.1", "::1" };

        if (auto const& hostName = host.Facts().hostName; !hostName.empty())
            names.push_back(hostName);

        if (auto const endpoint = ParseEndpoint(cfg.adminListen, "127.0.0.1"); endpoint.has_value())
        {
            constexpr std::array<std::string_view, 3> Wildcards { "0.0.0.0", "::", "[::]" };
            auto const& bindHost = endpoint->first;
            if (!std::ranges::contains(Wildcards, bindHost) && !std::ranges::contains(names, bindHost))
                names.push_back(bindHost);
        }

        return names;
    }
#endif

    /// The challenge an unauthorised caller is answered with.
    ///
    /// `Basic` is named first because it is the one a browser can prompt for, and
    /// the page exists to be opened in one. Without a `WWW-Authenticate` header at
    /// all a browser shows the body and no prompt, which reads as a broken page
    /// rather than as a credential being required.
    [[nodiscard]] AdminResponse Unauthorised(std::string_view contentType, std::string body)
    {
        return AdminResponse { .status = "401 Unauthorized",
                               .contentType = contentType,
                               .body = std::move(body),
                               .extraHeaders = { R"(WWW-Authenticate: Basic realm="fastcache fleet")" } };
    }
} // namespace

std::vector<AdminRoute> MakeFleetRoutes(Distributed::FleetSources sources,
                                        AdminCredential const& credential,
                                        unsigned refreshSeconds)
{
    // Both routes read the fleet the same way and differ only in how they render
    // it, so the collection and the gate are written once and the renderer is the
    // parameter. A second copy of the credential check is the shape of bug that
    // leaves one of two routes open.
    auto const answer = [sources, credential](auto&& render, std::string_view contentType, auto&& refuse) {
        return [sources, credential, render, contentType, refuse](AdminRequest const& request) -> AdminResponse {
            if (!credential.Accepts(request.authorization))
                return refuse();

            auto const snapshot = Distributed::CollectFleet(sources);
            // A follower answers 503 rather than 200: its registry holds whatever
            // registered against it rather than the fleet, so a 200 would be a
            // partial picture presented as the whole one. It is `Gate()`'s
            // `NotLeader` in HTTP's vocabulary -- not me, and here is who.
            return AdminResponse { .status = Distributed::LeadsTheFleet(snapshot) ? "200 OK" : "503 Service Unavailable",
                                   .contentType = contentType,
                                   .body = render(snapshot),
                                   .extraHeaders = {} };
        };
    };

    std::vector<AdminRoute> routes;
    routes.push_back(AdminRoute { .path = "/fleet",
                                  .handler = answer(
                                      [refreshSeconds](Distributed::FleetSnapshot const& s) {
                                          return Distributed::RenderFleetHtml(s, refreshSeconds);
                                      },
                                      "text/html; charset=utf-8",
                                      [] {
                                          return Unauthorised("text/html; charset=utf-8",
                                                              "<!doctype html><title>fastcache fleet</title>"
                                                              "<p>This page needs the credential named by "
                                                              "<code>--dashboard-token-file</code>.</p>");
                                      }) });
    routes.push_back(AdminRoute {
        .path = "/fleet.json",
        .handler = answer([](Distributed::FleetSnapshot const& s) { return Distributed::RenderFleetJson(s); },
                          "application/json",
                          [] { return Unauthorised("application/json", R"({"error":"credential required"})"); }) });
    return routes;
}

std::expected<AdminSurface, std::string> StartAdminSurfaceOrExplain(NodeConfig const& cfg,
                                                                    [[maybe_unused]] IHostFactsSource const& host,
                                                                    IMetricsSink& metrics,
                                                                    AdminHttpServer::SnapshotProvider snapshot,
                                                                    std::optional<Distributed::FleetSources> fleet,
                                                                    ILogger& logger)
{
    AdminSurface surface;

    // Off unless asked for, like every other surface this program serves. Nothing
    // else here runs, so a node with no `--admin-listen` also reads no certificate
    // and no token -- the flags that would be a silent no-op are refused by
    // `StartupPolicyRejection` long before this.
    if (cfg.adminListen.empty())
        return surface;

    // TLS is on by naming material or by asking for material to be made, never by
    // a bare boolean: neither spelling can reach a state where TLS was requested
    // and this node has nothing to serve it with.
    if (cfg.tlsSelfSigned || !cfg.tlsCertFile.empty())
    {
#if defined(FC_TLS_ENABLED)
        auto created = cfg.tlsSelfSigned ? TlsContext::CreateSelfSigned(SelfSignedSubjectNames(cfg, host))
                                         : TlsContext::Create(cfg.tlsCertFile, cfg.tlsKeyFile);
        if (!created.has_value())
            return std::unexpected { std::format(
                "{}: {}", cfg.tlsSelfSigned ? "--tls-self-signed" : "--tls-cert/--tls-key", created.error().ToString()) };
        surface.tls = std::move(*created);
#else
        // Refused rather than warned about, and the daemon answers the same way: a
        // node that started in the clear after being told to serve TLS is one an
        // operator believes is encrypted.
        return std::unexpected { std::format("{} requested but this build has no TLS support "
                                             "(rebuild with -DFASTCACHED_ENABLE_TLS=ON)",
                                             cfg.tlsSelfSigned ? "--tls-self-signed" : "--tls-cert") };
#endif
    }

    // Read once at startup. A file that cannot be read must not become "no
    // credential": that is the single failure that turns a guarded fleet map into
    // an open one.
    AdminCredential credential;
    if (!cfg.dashboardTokenFile.empty())
    {
        auto read = ReadDashboardToken(cfg.dashboardTokenFile);
        if (!read.has_value())
            return std::unexpected { std::format("--dashboard-token-file {}", read.error()) };
        credential = std::move(*read);
    }

    // Contributed only when the operator asked AND there is a fleet to read. A
    // node with no scheduler passes nullopt, and `/fleet` is then a plain 404
    // rather than a route answering with an empty fleet.
    std::vector<AdminRoute> routes;
    if (cfg.dashboard && fleet.has_value())
        routes = MakeFleetRoutes(*fleet, credential, DashboardRefreshSeconds);

    auto started = AdminEndpoint::Start(cfg.adminListen,
                                        "127.0.0.1",
                                        metrics,
                                        std::move(snapshot),
                                        logger,
                                        std::move(routes),
#if defined(FC_TLS_ENABLED)
                                        surface.tls.get());
#else
                                        nullptr);
#endif
    if (!started.has_value())
        return std::unexpected { std::format("--admin-listen {}", started.error()) };

    surface.endpoint = std::move(*started);

    auto const* const scheme =
#if defined(FC_TLS_ENABLED)
        surface.tls ? "https" : "http";
#else
        "http";
#endif
#if defined(FC_TLS_ENABLED)
    // Printed because with a generated certificate this is the ONLY thing that
    // authenticates the node: nothing signs it, so an operator compares what is
    // logged here against what their browser shows and knows they reached the
    // machine rather than something in between.
    if (surface.tls && cfg.tlsSelfSigned)
        logger.Logf(LogLevel::Info,
                    "admin TLS uses a self-signed certificate generated at startup; SHA-256 fingerprint {} "
                    "(it changes on every restart)",
                    surface.tls->CertificateFingerprint());
#endif
    logger.Logf(
        LogLevel::Info, "metrics endpoint on {}://{}/metrics (and /healthz)", scheme, surface.endpoint->BoundEndpoint());
    if (cfg.dashboard && fleet.has_value())
        logger.Logf(LogLevel::Info,
                    "fleet dashboard on {}://{}/fleet (and /fleet.json){}",
                    scheme,
                    surface.endpoint->BoundEndpoint(),
                    credential.Required() ? "" : ", with no credential");

    return surface;
}

AdminEndpoint::AdminEndpoint(std::unique_ptr<BlockingListener> listener,
                             IMetricsSink& metrics,
                             AdminHttpServer::SnapshotProvider snapshot,
                             std::string boundEndpoint,
                             ILogger& logger,
                             std::vector<AdminRoute> routes,
                             TlsContext* tls):
    _listener { std::move(listener) },
    _server { std::make_unique<AdminHttpServer>(*_listener, metrics, std::move(snapshot), logger, std::move(routes), tls) },
    _boundEndpoint { std::move(boundEndpoint) },
    _thread { [server = _server.get()] { SyncRun(server->Run()); } }
{
}

AdminEndpoint::~AdminEndpoint()
{
    // Order, not tidiness: closing the listener is what returns `Run()`, and the
    // jthread destructor that follows joins a loop which would otherwise still be
    // parked in `accept()`.
    _server->Shutdown();
}

std::expected<std::unique_ptr<AdminEndpoint>, std::string> AdminEndpoint::Start(std::string_view listenSpec,
                                                                                std::string_view defaultHost,
                                                                                IMetricsSink& metrics,
                                                                                AdminHttpServer::SnapshotProvider snapshot,
                                                                                ILogger& logger,
                                                                                std::vector<AdminRoute> routes,
                                                                                TlsContext* tls)
{
    auto const endpoint = ParseEndpoint(listenSpec, defaultHost);
    if (!endpoint.has_value())
        return std::unexpected { std::format("'{}' is not [<address>:]<port>", listenSpec) };

    auto listener = BlockingListener::Bind(endpoint->first, endpoint->second);
    if (!listener || !listener->IsBound())
        return std::unexpected { std::format("cannot bind {}:{} ({})",
                                             endpoint->first,
                                             endpoint->second,
                                             listener ? listener->BindError() : std::string_view { "null listener" }) };

    // The endpoint's own values, not this caller's: the daemon serves the same
    // server on the same terms, and two spellings of one decision drift.
    listener->SetTimeouts(AdminHttpServer::AcceptPoll, AdminHttpServer::RequestTimeout);

    // `new` rather than `make_unique` because the constructor is private: the two
    // ways to reach it are this factory, which has already proved the listener is
    // bound, and nothing else.
    return std::unique_ptr<AdminEndpoint> { new AdminEndpoint { std::move(listener),
                                                                metrics,
                                                                std::move(snapshot),
                                                                std::format("{}:{}", endpoint->first, endpoint->second),
                                                                logger,
                                                                std::move(routes),
                                                                tls } };
}

} // namespace FastCache::Node
