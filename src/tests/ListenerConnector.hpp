// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

namespace FastCache::Testing
{

/// @file ListenerConnector.hpp
/// A dialler whose every dial lands on one in-memory listener, whatever address it names.
///
/// What wires a real `RaftPeerTransport` to a real `RaftPeerServer` in one process, with the
/// production handshake between them and no socket. Shared since #178, when the library's
/// link case and the node's revocation case both needed it: two copies of a connector are two
/// answers to "where did that dial go", which is the one question a link case asks of it.
class ListenerConnector final: public IConnector
{
  public:
    /// @param listener Where every dial arrives; must outlive the connector.
    explicit ListenerConnector(InMemoryListener& listener) noexcept:
        _listener { listener }
    {
    }

    /// @copydoc IConnector::Connect
    [[nodiscard]] Task<SocketResult> connect(std::string host, std::uint16_t port, DialOptions options) override
    {
        std::ignore = host;
        std::ignore = port;
        std::ignore = options;
        co_return std::unique_ptr<ISocket> { _listener.connectClient() };
    }

  private:
    InMemoryListener& _listener;
};

} // namespace FastCache::Testing
