// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>

namespace FastCache::Node
{

/// Serves the fleet's scheduling verbs.
///
/// The peer's host reaches the membership oracle here and nowhere else, which is
/// what keeps `FrameServer` free of any policy: it hands over an identity and does
/// not know what anybody does with it.
class SchedulerResponder final: public IFrameResponder
{
  public:
    /// @param protocol Answers each request; must outlive this.
    /// @param membership Decides who may spend the fleet's capacity; must outlive this.
    SchedulerResponder(Distributed::SchedulerProtocol& protocol, Distributed::IMembershipOracle const& membership) noexcept:
        _protocol { protocol },
        _membership { membership }
    {
    }

    [[nodiscard]] std::vector<std::byte> Answer(std::span<std::byte const> frame, std::string_view peer) override
    {
        return _protocol.Answer(frame,
                                Distributed::CallerContext { .membership = _membership.Classify(peer), .peerId = peer });
    }

    /// Kilobytes, not megabytes.
    ///
    /// Membership is checked *inside* the service — after the frame is read — so an
    /// unauthenticated peer can make this endpoint buffer whatever it declares. A
    /// scheduler verb carries a fingerprint, an endpoint and a key, so this is the
    /// honest ceiling; sizing it like the cache's would hand a stranger a way to
    /// make the scheduler allocate megabytes.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return 64ULL * 1024ULL;
    }

  private:
    Distributed::SchedulerProtocol& _protocol;
    Distributed::IMembershipOracle const& _membership;
};

/// Serves this node's own cache tier to clients on this machine.
class CacheResponder final: public IFrameResponder
{
  public:
    /// @param proxy Answers each request; must outlive this.
    explicit CacheResponder(CacheProxy& proxy) noexcept:
        _proxy { proxy }
    {
    }

    [[nodiscard]] std::vector<std::byte> Answer(std::span<std::byte const> frame, std::string_view /*peer*/) override
    {
        // The peer is not consulted, and that is the design rather than an omission:
        // this surface binds loopback by default, so "who is asking" is answered by
        // the bind rather than by a policy. A cache deliberately exposed to a network
        // wants `--requirepass`, which is a credential rather than a membership.
        return _proxy.Answer(frame);
    }

    /// Megabytes, because a STORE carries a whole object file.
    ///
    /// Matches the daemon's own default value ceiling: a node that refused what the
    /// shared cache would accept would silently stop caching this machine's largest
    /// translation units, which are exactly the ones worth caching.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return 256ULL * 1024ULL * 1024ULL;
    }

  private:
    CacheProxy& _proxy;
};

} // namespace FastCache::Node
