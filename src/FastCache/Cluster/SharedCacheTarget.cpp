// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/SharedCacheTarget.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Cluster
{

ResolvedSharedCache ResolveSharedCache(ClusterState const& state, std::string_view selfId)
{
    auto const named = state.SettingOf(SharedCacheSetting).value_or(std::string {});
    if (named.empty())
        return ResolvedSharedCache {};

    auto const answer = [&named](SharedCacheResolution resolution) {
        return ResolvedSharedCache { .resolution = resolution, .machineId = named, .endpoint = {}, .key = std::nullopt };
    };

    if (named == selfId)
        return answer(SharedCacheResolution::ThisMachine);

    // A recorded member holds its identity key by type, so a machine the cluster holds is one with
    // a key to prove.
    auto const key = LiveKeyOf(state, named);
    if (!key.has_value())
        return answer(SharedCacheResolution::UnknownMachine);

    if (state.IsRevoked(*key))
        return answer(SharedCacheResolution::KeyRevoked);

    // The member's advertised 0xFC endpoint, recorded when that machine joins or next announces
    // itself; absent or empty is "not yet said".
    auto endpoint = state.SchedulerEndpointOf(named);
    if (!endpoint.has_value() || endpoint->empty())
        return answer(SharedCacheResolution::NoEndpoint);

    return ResolvedSharedCache {
        .resolution = SharedCacheResolution::Resolved, .machineId = named, .endpoint = *std::move(endpoint), .key = key
    };
}

} // namespace FastCache::Cluster
