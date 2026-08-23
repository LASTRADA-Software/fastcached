// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <chrono>
#include <format>
#include <utility>

namespace FastCache::Node
{

AdminEndpoint::AdminEndpoint(std::unique_ptr<BlockingListener> listener,
                             IMetricsSink& metrics,
                             AdminHttpServer::SnapshotProvider snapshot,
                             std::string boundEndpoint,
                             ILogger& logger):
    _listener { std::move(listener) },
    _server { std::make_unique<AdminHttpServer>(*_listener, metrics, std::move(snapshot), logger) },
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
                                                                                ILogger& logger)
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
                                                                logger } };
}

} // namespace FastCache::Node
