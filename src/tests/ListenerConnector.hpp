// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

#include <core/async/Task.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/testing/InMemorySocket.hpp>

namespace FastCache::Testing
{

/// @file ListenerConnector.hpp
/// A dialler whose every dial lands on one in-memory listener, whatever address it names.
///
/// What wires a real `RaftPeerTransport` to a real `RaftPeerServer` in one process, with the
/// production handshake between them and no socket. Shared since #178, when the library's
/// link case and the node's revocation case both needed it: two copies of a connector are two
/// answers to "where did that dial go", which is the one question a link case asks of it.
class ListenerConnector final: public core::net::IConnector
{
  public:
    /// @param listener Where every dial arrives; must outlive the connector.
    explicit ListenerConnector(core::net::testing::InMemoryListener& listener) noexcept:
        _listener { listener }
    {
    }

    /// @copydoc IConnector::Connect
    [[nodiscard]] core::async::Task<core::net::SocketResult> connect(std::string host,
                                                                     std::uint16_t port,
                                                                     core::net::DialOptions options) override
    {
        std::ignore = host;
        std::ignore = port;
        std::ignore = options;
        co_return std::unique_ptr<core::net::ISocket> { _listener.connectClient() };
    }

  private:
    core::net::testing::InMemoryListener& _listener;
};

} // namespace FastCache::Testing
