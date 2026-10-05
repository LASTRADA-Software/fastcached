// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/AnnouncedEndpoints.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Cluster
{

std::vector<DesiredMember> AnnouncedEndpointDesires(ClusterState const& state,
                                                    AnnouncedEndpointMap const& announced,
                                                    MembersInFlight const& inFlight)
{
    auto desires = std::vector<DesiredMember> {};
    for (auto const& [id, endpoint]: announced)
    {
        auto const* const recorded = core::findOrNull(state.members, id, &ClusterMember::id);
        if (recorded == nullptr || inFlight.contains(id) || !IsPeerDialableEndpoint(endpoint)
            || recorded->schedulerEndpoint == endpoint)
            continue;
        desires.push_back(DesiredMember { .id = recorded->id,
                                          .raftEndpoint = recorded->raftEndpoint,
                                          .schedulerEndpoint = endpoint,
                                          .publicKey = std::nullopt });
    }
    return desires;
}

std::vector<DesiredMember> WithAnnouncedEndpoints(std::vector<DesiredMember> desired,
                                                  std::span<DesiredMember const> announcements)
{
    for (auto const& announcement: announcements)
    {
        auto const existing = std::ranges::find(desired, announcement.id, &DesiredMember::id);
        if (existing == desired.end())
            desired.push_back(announcement);
        else if (!existing->schedulerEndpoint.has_value())
            existing->schedulerEndpoint = announcement.schedulerEndpoint;
    }
    return desired;
}

} // namespace FastCache::Cluster
