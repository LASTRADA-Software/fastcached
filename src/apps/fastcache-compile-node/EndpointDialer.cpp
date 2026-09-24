// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialer.hpp"

#include <utility>

#include <EndpointDial.hpp>
#include <core/net/BlockingConnector.hpp>

namespace FastCache::Node
{

BlockingEndpointDialer::BlockingEndpointDialer(std::chrono::milliseconds ioTimeout) noexcept:
    _ioTimeout { ioTimeout }
{
}

std::unique_ptr<core::net::ISocket> BlockingEndpointDialer::Dial(std::string_view endpoint, core::net::DialOptions options)
{
    // **The concrete type never leaves this function**, which is what preserves
    // `Cc::DialEndpointBlocking`'s guard: it takes a `BlockingConnector&` because its
    // soundness rests on the connector resolving inline and never leaving its task
    // suspended. Injecting one level up keeps that rule intact while making the
    // REPLIES scriptable.
    core::net::BlockingConnector connector { core::net::defaultAddressResolver(),
                                             core::net::BlockingConnectorOptions { .ioTimeout = _ioTimeout } };
    return Cc::DialEndpointBlocking(connector, endpoint, options);
}

IEndpointDialer& DefaultOneShotDialer() noexcept
{
    static BlockingEndpointDialer instance { OneShotIoTimeout };
    return instance;
}

std::optional<ReachedEndpoint> DialFirstReachable(IEndpointDialer& dialer,
                                                  std::span<std::string const> endpoints,
                                                  core::net::DialOptions options)
{
    for (auto const& endpoint: endpoints)
        if (auto socket = dialer.Dial(endpoint, options); socket != nullptr)
            return ReachedEndpoint { .socket = std::move(socket), .endpoint = endpoint };
    return std::nullopt;
}

std::string JoinEndpoints(std::span<std::string const> endpoints)
{
    std::string joined;
    for (auto const& endpoint: endpoints)
    {
        if (!joined.empty())
            joined += ", ";
        joined += endpoint;
    }
    return joined;
}

} // namespace FastCache::Node
