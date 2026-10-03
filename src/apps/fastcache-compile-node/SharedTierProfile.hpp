// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheTierProfile.hpp"

#include <optional>

namespace FastCache::Node
{

/// The fleet's shared tier: `SHARED-FETCH` and `SHARED-STORE`, counted on the `NodeSharedCache*`
/// series and never on the private tier's.
///
/// No `CACHE-DROP`, and no upstream counters. The shared tier IS the fleet's cache, so its upstream
/// is `NoUpstream`, which never answers a fetch and answers every store `NotConfigured` -- so each
/// upstream member is absent, stating *cannot happen here* rather than borrowing a private-tier
/// series that would then rise for an event on another tier.
inline constexpr CacheTierProfile SharedTierProfile {
    .verbs = TierVerbs { CompileCacheWire::FleetSharedCacheVerbs },
    .drop = DropVerb::Refuses,
    .hits = IMetricsSink::Counter::NodeSharedCacheHits,
    .misses = IMetricsSink::Counter::NodeSharedCacheMisses,
    .storeFailures = IMetricsSink::Counter::NodeSharedCacheStoreFailures,
    .upstreamHits = std::nullopt,
    .fillFailures = std::nullopt,
    .upstreamStores = std::nullopt,
    .upstreamStoreFailures = std::nullopt,
    .refusedUnsupportedVersion = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedUnsupportedVersion,
    .refusedMalformedPayload = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedMalformedPayload,
    .refusedForeignGeneration = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedForeignGeneration,
};

static_assert(StatesEveryMember(SharedTierProfile), "the shared tier's profile states every column");

} // namespace FastCache::Node
