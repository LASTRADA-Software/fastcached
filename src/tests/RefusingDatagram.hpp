// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <core/net/IDatagramSocket.hpp>
#include <core/net/NetError.hpp>

namespace FastCache::Testing
{

/// A socket whose local stack refuses a send to some destinations, and passes every other call on.
///
/// The segment (`core::net::testing::DatagramBus`) accepts every send, because a datagram that is
/// lost is scripted there as a DROP -- accepted and never delivered -- which is the opposite fact.
/// A refusal is what a real stack answers for a broadcast without the capability, a firewall's
/// `EACCES`, or a route that is not there yet, and a caller has to tell it from both a drop and
/// from sending nothing at all.
class RefusingDatagramSocket final: public core::net::IDatagramSocket
{
  public:
    /// @param inner The socket everything but a refused send goes to; owned.
    /// @param refusedHosts The destination hosts whose sends are refused; empty refuses every one.
    RefusingDatagramSocket(std::unique_ptr<core::net::IDatagramSocket> inner, std::vector<std::string> refusedHosts):
        _inner { std::move(inner) },
        _refusedHosts { std::move(refusedHosts) }
    {
    }

    /// @param payload The whole message.
    /// @param to Destination.
    /// @return `PermissionDenied` for a refused host, or whatever the inner socket answers.
    [[nodiscard]] std::expected<void, core::net::NetError> send(std::span<std::byte const> payload,
                                                                core::net::DatagramAddress const& to) override
    {
        if (_refusing && (_refusedHosts.empty() || std::ranges::find(_refusedHosts, to.host) != _refusedHosts.end()))
        {
            ++_refused;
            return std::unexpected { core::net::makeNetError(
                core::net::NetErrorCode::PermissionDenied, 0, "refused by the test's local stack") };
        }
        return _inner->send(payload, to);
    }

    /// @param timeout How long to wait.
    /// @return What the inner socket received.
    [[nodiscard]] std::expected<core::net::ReceivedDatagram, core::net::DatagramWait> receive(
        std::chrono::milliseconds timeout) override
    {
        return _inner->receive(timeout);
    }

    void close() noexcept override
    {
        _inner->close();
    }

    /// @return The inner socket's bound address.
    [[nodiscard]] core::net::DatagramAddress boundAddress() const override
    {
        return _inner->boundAddress();
    }

    /// Refuse sends to other hosts from now on, as a link that starts or stops refusing does.
    /// @param refusedHosts The destination hosts whose sends are refused; empty refuses every one.
    void Refuse(std::vector<std::string> refusedHosts)
    {
        _refusedHosts = std::move(refusedHosts);
        _refusing = true;
    }

    /// Refuse nothing from now on, until the next `Refuse`: the stack taking every send again.
    void RefuseNothing() noexcept
    {
        _refusing = false;
    }

    /// @return How many sends were refused, so a case can assert a refusal HAPPENED rather than
    ///         nothing being sent.
    [[nodiscard]] std::size_t Refused() const noexcept
    {
        return _refused;
    }

  private:
    std::unique_ptr<core::net::IDatagramSocket> _inner;
    std::vector<std::string> _refusedHosts;
    std::size_t _refused { 0 };
    bool _refusing { true }; ///< False while `RefuseNothing` holds.
};

} // namespace FastCache::Testing
