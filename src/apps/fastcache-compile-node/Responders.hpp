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
///
/// **Local callers and cluster members only.** The bind already answers most of it
/// -- this surface is loopback by default -- but a bind is not a policy: an operator
/// who widens it to share the tier with their peers would otherwise be sharing it
/// with everybody who can route to the port, and the objects in it are this
/// machine's whole build output.
///
/// This is deliberately *stricter* than `fastcached`'s own cache, which serves
/// non-members on purpose. That one is shared infrastructure somebody operates; this
/// is a developer's private tier, and the two are different things that happen to
/// speak one protocol.
class CacheResponder final: public IFrameResponder
{
  public:
    /// @param proxy Answers each request; must outlive this.
    /// @param membership Decides who may read this machine's tier; must outlive this.
    CacheResponder(CacheProxy& proxy, Distributed::IMembershipOracle const& membership) noexcept:
        _proxy { proxy },
        _membership { membership }
    {
    }

    [[nodiscard]] std::vector<std::byte> Answer(std::span<std::byte const> frame, std::string_view peer) override
    {
        // Refused as a *reply*, never by closing: a client that cannot tell a policy
        // refusal from a dead host retries forever and reports a flaky network, which
        // is the failure the declared frame length exists to make avoidable.
        if (_membership.Classify(peer) != Distributed::Membership::Member)
            return CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCode::NotAMember,
                                                      "this node serves its cache to its own machine and its cluster");
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
    Distributed::IMembershipOracle const& _membership;
};

} // namespace FastCache::Node
