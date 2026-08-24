// SPDX-License-Identifier: Apache-2.0
#include "EndpointDial.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/TcpClient.hpp>

#include <chrono>
#include <memory>
#include <string_view>
#include <utility>

namespace FastCache::Cc
{

std::unique_ptr<ISocket> DialEndpoint(std::string_view hostPort, std::chrono::milliseconds ioTimeout)
{
    // `SplitHostPort` and not `ParseEndpoint`, deliberately: the latter accepts a
    // bare port and supplies a default host, which is right for a *bind* address
    // an operator types and wrong here. Every caller up here is dialling something
    // it was configured with (`FASTCACHE_ADDR`, a scheduler endpoint, a redirect
    // from `NotLeader`), and text with no host in it is a misconfiguration worth
    // refusing rather than a request to try this machine.
    auto const split = SplitHostPort(hostPort);
    if (!split.has_value())
        return nullptr;
    auto const port = ParseTcpPort(split->second);
    if (!port.has_value())
        return nullptr;

    auto socket = FastCache::ConnectTcp(split->first, *port, ioTimeout, ioTimeout);
    if (!socket.has_value())
        return nullptr;
    return std::move(*socket);
}

} // namespace FastCache::Cc
