// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace FastCache
{

/// `IConnector` over the platform socket API.
///
/// Resolves through an injected `IAddressResolver` and tries each candidate in
/// preference order, so a host with both an AAAA and an A record still connects
/// when only one family is routable — the failure a single-candidate dial turns
/// into "the peer is down" when it is really "this machine has no IPv6".
///
/// The connect itself is issued **non-blocking and then waited on**, rather than
/// as a plain blocking `connect()`. That is what makes the timeout parameter
/// mean anything: a blocking connect to a black-holed address is governed by the
/// kernel's own retry schedule, which is measured in minutes, and no caller can
/// shorten it. The socket is returned in blocking mode, which is what
/// `BlockingSocket`'s reads and writes expect.
class BlockingConnector final: public IConnector
{
  public:
    /// @param resolver Name resolution seam; defaults to the process-wide
    ///        getaddrinfo-backed one. Injected so a test can dial a scripted
    ///        endpoint without a DNS lookup.
    explicit BlockingConnector(IAddressResolver& resolver = DefaultAddressResolver()) noexcept;

    /// @copydoc IConnector::Connect
    [[nodiscard]] std::expected<std::unique_ptr<ISocket>, NetError> Connect(std::string_view host,
                                                                            std::uint16_t port,
                                                                            std::chrono::milliseconds timeout) override;

  private:
    IAddressResolver& _resolver;
};

} // namespace FastCache
