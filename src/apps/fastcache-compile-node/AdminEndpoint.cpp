// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"

#include "CacheTier.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/FleetView.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

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

    std::string secret { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };

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
                                        AdminCredential credential,
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
    routes.push_back(AdminRoute {
        .path = "/fleet",
        .handler = answer([refreshSeconds](
                              Distributed::FleetSnapshot const& s) { return Distributed::RenderFleetHtml(s, refreshSeconds); },
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
