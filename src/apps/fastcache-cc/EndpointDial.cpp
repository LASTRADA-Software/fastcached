// SPDX-License-Identifier: Apache-2.0
#include "EndpointDial.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/TcpClient.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace FastCache::Cc
{

Task<std::unique_ptr<ISocket>> DialEndpoint(IConnector* connector,
                                            std::string_view hostPort,
                                            std::chrono::milliseconds connectTimeout)
{
    // `SplitHostPort` and not `ParseEndpoint`, deliberately: the latter accepts a
    // bare port and supplies a default host, which is right for a *bind* address
    // an operator types and wrong here. Every caller up here is dialling something
    // it was configured with (`FASTCACHE_ADDR`, a scheduler endpoint, a redirect
    // from `NotLeader`), and text with no host in it is a misconfiguration worth
    // refusing rather than a request to try this machine.
    auto const split = SplitHostPort(hostPort);
    if (!split.has_value())
        co_return nullptr;
    auto const port = ParseTcpPort(split->second);
    if (!port.has_value())
        co_return nullptr;

    auto socket = co_await connector->Connect(std::string { split->first }, *port, connectTimeout);
    if (!socket.has_value())
        co_return nullptr;
    co_return std::move(*socket);
}

/// @copydoc DialEndpointBlocking
std::unique_ptr<ISocket> DialEndpointBlocking(BlockingConnector& connector,
                                              std::string_view hostPort,
                                              std::chrono::milliseconds connectTimeout)
{
    // Sound because the parameter is a `BlockingConnector` and not an
    // `IConnector`: that connector resolves inline and waits with a syscall, so
    // its task is never left suspended, which is exactly what `SyncRun` requires.
    // Over a reactor connector this would throw.
    return SyncRun(DialEndpoint(&connector, hostPort, connectTimeout));
}

} // namespace FastCache::Cc
