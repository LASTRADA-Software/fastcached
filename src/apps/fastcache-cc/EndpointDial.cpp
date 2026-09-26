// SPDX-License-Identifier: Apache-2.0
#include "EndpointDial.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <core/async/SyncRun.hpp>
#include <core/net/TcpClient.hpp>

namespace FastCache::Cc
{

core::async::Task<std::unique_ptr<core::net::ISocket>> DialEndpoint(core::net::IConnector* connector,
                                                                    std::string_view hostPort,
                                                                    core::net::DialOptions options)
{
    // `ParseDialEndpoint` and not `SplitHostPort`/`ParseEndpoint`: the reasoning for
    // each of its three refusals -- a sentence that merely splits, an empty host, a
    // bare port that would mean this machine -- is in `Core/HostPort.hpp`, spelled
    // once because `Cc::RedirectTarget` has to answer the identical question about
    // the identical text (#237).
    //
    // Refused HERE rather than left to the connector, which matters even though
    // `core::net::detail::runConnectFlow` refuses an empty host too: the difference is between
    // "no connector was asked" and "a connector was asked and said no", which is the
    // distinction `EndpointDial_test`'s `RecordingConnector` exists to make and the
    // one a caller holding a connector outside the funnel depends on.
    auto const target = ParseDialEndpoint(hostPort);
    if (!target.has_value())
        co_return nullptr;

    auto socket = co_await connector->connect(target->first, target->second, options);
    if (!socket.has_value())
        co_return nullptr;
    co_return std::move(*socket);
}

/// @copydoc DialEndpointBlocking
std::unique_ptr<core::net::ISocket> DialEndpointBlocking(core::net::BlockingConnector& connector,
                                                         std::string_view hostPort,
                                                         core::net::DialOptions options)
{
    // Sound because the parameter is a `core::net::BlockingConnector` and not an
    // `core::net::IConnector`: that connector resolves inline and waits with a syscall, so
    // its task is never left suspended, which is exactly what `core::async::syncRun` requires.
    // Over a reactor connector this would throw.
    return core::async::syncRun(DialEndpoint(&connector, hostPort, options));
}

} // namespace FastCache::Cc
