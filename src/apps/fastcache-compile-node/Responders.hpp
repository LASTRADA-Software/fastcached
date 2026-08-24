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

    /// @copydoc IFrameResponder::Answer
    ///
    /// Never suspends. The scheduler answers from its own tables -- a decision
    /// layer kept pure so every capacity and expiry rule is a `ManualClock` unit
    /// test -- so this is a one-line adapter, and a responder that costs a frame
    /// allocation and no round trip is a legitimate thing to be.
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override
    {
        co_return _protocol.Answer(frame,
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

    /// Hundreds, because a scheduler verb is kilobytes.
    ///
    /// The spread against the cache surface below is the whole reason this is a
    /// per-responder column: at 64 KiB a request, 256 at once is 16 MiB, which is
    /// nothing. Sizing this like the cache's would refuse a busy fleet for no
    /// reason; sizing the cache's like this would let 256 object files be in flight
    /// at once.
    [[nodiscard]] std::size_t MaxConcurrentRequests() const noexcept override
    {
        return 256;
    }

    /// 16 MiB: the connection cap times the request cap, so the byte budget never
    /// refuses anything the connection cap would have allowed.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return 256ULL * 64ULL * 1024ULL;
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

    /// @copydoc IFrameResponder::Answer
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override
    {
        // Refused as a *reply*, never by closing: a client that cannot tell a policy
        // refusal from a dead host retries forever and reports a flaky network, which
        // is the failure the declared frame length exists to make avoidable.
        //
        // Answered before any suspension, deliberately: a stranger must not be able
        // to make this node dial its upstream.
        if (_membership.Classify(peer) != Distributed::Membership::Member)
            co_return CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCode::NotAMember,
                                                         "this node serves its cache to its own machine and its cluster");
        co_return co_await _proxy.Answer(frame);
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

    /// Eight, because a cache STORE carries a whole object file.
    ///
    /// This surface is the one the concurrency change is FOR -- a slow upstream used
    /// to stall every local `fastcache-cc` behind it -- so it must serve several at
    /// once. But it is also the one where "several at once" is expensive, and the
    /// serialized loop was bounding peak memory to a single request by accident.
    /// Eight parallel translation units is a wide build; the byte budget below is
    /// what actually stops it becoming eight times the per-request maximum.
    [[nodiscard]] std::size_t MaxConcurrentRequests() const noexcept override
    {
        return 8;
    }

    /// 256 MiB across all of them, which is deliberately ONE request's worth.
    ///
    /// So the common case -- eight ordinary objects of a few megabytes -- runs fully
    /// in parallel, while a single 256 MiB monster still cannot be joined by seven
    /// more. That keeps this endpoint's peak footprint where the serialized loop
    /// used to hold it, without reintroducing the serialization.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return 256ULL * 1024ULL * 1024ULL;
    }

  private:
    CacheProxy& _proxy;
    Distributed::IMembershipOracle const& _membership;
};

} // namespace FastCache::Node
