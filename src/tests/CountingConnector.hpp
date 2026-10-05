// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftInboundLinks.hpp>
#include <FastCache/Consensus/RaftSessionLink.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <core/async/Task.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/NetError.hpp>

namespace FastCache::Testing
{

/// @file CountingConnector.hpp
/// The two doubles a case needs to see what an acceptor-side Raft node did NOT do: dial
/// somebody, or attach a session.
///
/// Shared rather than written per file, for `MembershipFakes.hpp`'s reason: both are asked the
/// one question a learner case turns on -- did the leader reach the learner some way other than
/// the session the learner opened -- and two copies are two answers to it.

/// A dialler that goes nowhere and counts every attempt.
///
/// Every dial is refused, so a transport given it can never reach anybody through it: what a
/// case asserts is `Attempts()`, the count of dials this node made at all.
class CountingConnector final: public core::net::IConnector
{
  public:
    /// @copydoc IConnector::Connect
    [[nodiscard]] core::async::Task<core::net::SocketResult> connect(std::string host,
                                                                     std::uint16_t port,
                                                                     core::net::DialOptions options) override
    {
        std::ignore = port;
        std::ignore = options;
        _attempts.fetch_add(1, std::memory_order_relaxed);
        co_return std::unexpected { core::net::NetError { .code = core::net::NetErrorCode::ConnRefused,
                                                          .systemCode = 0,
                                                          .context = "CountingConnector refuses every dial, to "
                                                                     + std::move(host) } };
    }

    /// @return How many dials were attempted, all of them refused.
    [[nodiscard]] std::size_t Attempts() const noexcept
    {
        return _attempts.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::size_t> _attempts { 0 };
};

/// An `IRaftInboundLinks` that keeps no link and counts what it was handed.
///
/// For a server whose case is not about writing to a dialler: it takes the place of a transport
/// and remembers only WHO was attached, so a case can assert that a one-way session attached
/// nothing and a two-way one attached exactly its proven peer -- and was detached again.
class NoInboundLinks final: public Consensus::IRaftInboundLinks
{
  public:
    /// One attach, as the server made it.
    struct Attached
    {
        Consensus::NodeId peer;  ///< Who the session proved.
        Ed25519PublicKey key {}; ///< The key it proved that with.
    };

    /// @copydoc IRaftInboundLinks::Attach
    void Attach(std::shared_ptr<Consensus::RaftSessionLink> link) override
    {
        auto const guard = std::scoped_lock { _mutex };
        _attached.push_back(Attached { .peer = link->Peer(), .key = link->ProvenKey() });
    }

    /// @copydoc IRaftInboundLinks::Detach
    void Detach(Consensus::RaftSessionLink const& link) noexcept override
    {
        std::ignore = link;
        _detaches.fetch_add(1, std::memory_order_relaxed);
    }

    /// @return How many sessions were attached.
    [[nodiscard]] std::size_t Attaches() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _attached.size();
    }

    /// @return How many sessions were detached.
    [[nodiscard]] std::size_t Detaches() const noexcept
    {
        return _detaches.load(std::memory_order_relaxed);
    }

    /// @return Every attach, in order.
    [[nodiscard]] std::vector<Attached> AttachedPeers() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _attached;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<Attached> _attached;
    std::atomic<std::size_t> _detaches { 0 };
};

} // namespace FastCache::Testing
