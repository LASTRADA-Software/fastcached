// SPDX-License-Identifier: Apache-2.0
#include "FleetVersions.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

namespace FastCache::Distributed
{

FleetVersionSpread SpreadOfVersions(std::span<NodeReport const> nodes,
                                    std::string_view ownVersion,
                                    std::string_view ownEndpoint)
{
    // Ordered by version, so equal-sized groups come out in a reproducible order below.
    auto byVersion = std::map<std::string, std::vector<std::string>, std::less<>> {};
    auto unstated = std::size_t { 0 };
    auto const file = [&byVersion, &unstated](std::string_view version, std::string name) {
        if (version.empty())
        {
            ++unstated;
            return;
        }
        byVersion[std::string { version }].push_back(std::move(name));
    };

    file(ownVersion, std::string { ownEndpoint });
    for (auto const& node: nodes)
        if (node.endpoint != ownEndpoint)
            file(node.version, node.displayName.empty() ? node.endpoint : node.displayName);

    auto spread = FleetVersionSpread { .groups = {}, .unstated = unstated };
    for (auto& [version, machines]: byVersion)
    {
        std::ranges::sort(machines);
        spread.groups.push_back(VersionGroup { .version = version, .machines = std::move(machines) });
    }
    std::ranges::stable_sort(
        spread.groups, std::ranges::greater {}, [](VersionGroup const& group) { return group.machines.size(); });
    return spread;
}

} // namespace FastCache::Distributed
