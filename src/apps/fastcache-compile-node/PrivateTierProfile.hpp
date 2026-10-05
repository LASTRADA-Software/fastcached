// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheTierProfile.hpp"

namespace FastCache::Node
{

/// The node's own tier: this machine's `FETCH`, `STORE` and `CACHE-DROP`, counted on the
/// `NodeCache*` series and reading through to whatever upstream the node was given.
///
/// Every upstream counter is present, because this tier may have a real upstream behind it; a
/// node with none answers every store `NotConfigured`, which `LocalCache` counts nowhere.
inline constexpr CacheTierProfile PrivateTierProfile {
    .verbs = TierVerbs { CompileCacheWire::DaemonCacheVerbs },
    .drop = DropVerb::Serves,
    .hits = IMetricsSink::Counter::NodeCacheHits,
    .misses = IMetricsSink::Counter::NodeCacheMisses,
    .storeFailures = IMetricsSink::Counter::NodeCacheStoreFailures,
    .upstreamHits = IMetricsSink::Counter::NodeCacheUpstreamHits,
    .fillFailures = IMetricsSink::Counter::NodeCacheFillFailures,
    .upstreamStores = IMetricsSink::Counter::NodeCacheUpstreamStores,
    .upstreamStoreFailures = IMetricsSink::Counter::NodeCacheUpstreamStoreFailures,
    .refusedUnsupportedVersion = IMetricsSink::Counter::NodeCacheRequestsRefusedUnsupportedVersion,
    .refusedMalformedPayload = IMetricsSink::Counter::NodeCacheRequestsRefusedMalformedPayload,
    .refusedForeignGeneration = IMetricsSink::Counter::NodeCacheRequestsRefusedForeignGeneration,
};

static_assert(StatesEveryMember(PrivateTierProfile), "the private tier's profile states every column");

} // namespace FastCache::Node
