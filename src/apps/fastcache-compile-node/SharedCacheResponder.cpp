// SPDX-License-Identifier: Apache-2.0
#include "MembershipGate.hpp"
#include "SharedCacheResponder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Node
{

core::async::Task<FrameReply> SharedCacheResponder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
{
    // The verb, read back out of the frame, for `CacheResponder::Answer`'s reason: a frame too
    // short to carry a header names no verb, and `0xFF` is unassigned.
    auto const decodedHeader = CompileCacheWire::DecodeRequestHeader(frame);
    auto const opRaw = decodedHeader.has_value() ? decodedHeader->opRaw : std::uint8_t { 0xFF };
    if (auto refusal = RefusePeer(peer, opRaw); refusal.has_value())
        co_return *std::move(refusal);

    // Held for the whole answer: a move of the setting mid-request retires the tier, never frees it.
    // Asked again after the door's answer, because the setting can move between the two; the door
    // having refused, this is reached only by that race.
    auto const tier = _host.Current();
    if (tier == nullptr)
        co_return Cc::Refuse(_metrics, NotServingSharedCache, NotServingSharedCacheWhy);
    co_return co_await tier->Answer(frame);
}

std::optional<std::vector<std::byte>> SharedCacheResponder::RefusePeer(PeerIdentity const& peer,
                                                                       std::uint8_t /*opRaw*/) const
{
    auto const decision = Distributed::ExplainConnection(_membership, peer);
    // A revoked key is the forgotten machine, and it is told so on its own row, whatever else
    // the fold found.
    if (decision.verdict == Distributed::Membership::Forgotten)
        return AnswerMembership(decision, _metrics, SharedStranger, SharedStrangerWhy);
    // The narrower bar: a member admitted by where it dials from, or by `--fleet-open`, has proved
    // no key, and the fleet's shared objects are served to keys. On a node running no consensus
    // that is everyone: it holds no key roster, so nothing it could be shown rests on a key -- and
    // it is never the one the setting names, since only a member can be named, and every member
    // runs consensus.
    if (!Distributed::RestsOnMachineKey(decision))
        return Cc::Refuse(_metrics, SharedStranger, SharedStrangerWhy);
    // Admitted, and asked whether this node serves at all BEFORE the payload is read: at the door,
    // a dormant node steps over the body rather than buffering an object it will not keep.
    // Admission stays first, so a stranger learns nothing about what this node is.
    if (_host.Current() == nullptr)
        return Cc::Refuse(_metrics, NotServingSharedCache, NotServingSharedCacheWhy);
    return std::nullopt;
}

CredentialVerdict SharedCacheResponder::CheckCredential(std::span<std::byte const> /*payload*/) const
{
    return NotTheSessionSurface();
}

std::vector<std::byte> SharedCacheResponder::RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                          std::uint8_t /*opRaw*/,
                                                          std::string_view detail) const
{
    return Detail::AnswerCacheRefusal(
        _metrics, CompileCacheWire::ErrorCodeFor(decision), Detail::SharedCachePrePayloadPolicy(decision), detail);
}

std::vector<std::byte> SharedCacheResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                  std::uint8_t /*opRaw*/,
                                                                  std::string_view detail) const
{
    return Detail::AnswerCacheRefusal(_metrics,
                                      ErrorCodeFor(refusal),
                                      Detail::SharedCacheEndpointRefusals[static_cast<std::size_t>(refusal)].policy,
                                      detail);
}

SharedCacheService::SharedCacheService(NodeConfig const& cfg,
                                       Distributed::IMembershipOracle const& membership,
                                       core::platform::IClock& clock,
                                       IMetricsSink& metrics,
                                       ILogger& logger,
                                       NodeConditions* conditions,
                                       ReconcileOn on):
    _directory { cfg.nodeId, cfg.upstream },
    _opener { cfg, clock, metrics, logger },
    _host { cfg.nodeId, _opener, conditions, logger, on },
    _responder { _host, membership, metrics }
{
}

} // namespace FastCache::Node
