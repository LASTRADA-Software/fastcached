// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/AnnouncedEndpoints.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Cluster
{

std::string CoupledRaftEndpoint(ClusterMember const& recorded, std::string_view announced)
{
    auto const raft = ParseDialEndpoint(recorded.raftEndpoint);
    auto const scheduler = ParseDialEndpoint(recorded.schedulerEndpoint);
    auto const moved = ParseDialEndpoint(announced);
    if (!raft.has_value() || !scheduler.has_value() || !moved.has_value() || !SameHost(raft->first, scheduler->first))
        return recorded.raftEndpoint;
    return FormatHostPort(moved->first, raft->second);
}

bool SpeaksForRaftEndpoint(DesiredMember const& announcement)
{
    auto const raft = ParseDialEndpoint(announcement.raftEndpoint);
    auto const scheduler = ParseDialEndpoint(announcement.schedulerEndpoint.value_or(std::string {}));
    return raft.has_value() && scheduler.has_value() && SameHost(raft->first, scheduler->first);
}

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
                                          .raftEndpoint = CoupledRaftEndpoint(*recorded, endpoint),
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
        {
            existing->schedulerEndpoint = announcement.schedulerEndpoint;
            if (SpeaksForRaftEndpoint(announcement))
                existing->raftEndpoint = announcement.raftEndpoint;
        }
    }
    return desired;
}

std::vector<DesiredMember> WithAnnouncedRaftEndpoints(std::vector<DesiredMember> held,
                                                      std::span<DesiredMember const> announcements)
{
    for (auto const& announcement: announcements | std::views::filter(SpeaksForRaftEndpoint))
    {
        auto const existing = std::ranges::find(held, announcement.id, &DesiredMember::id);
        if (existing != held.end() && !existing->schedulerEndpoint.has_value())
            existing->raftEndpoint = announcement.raftEndpoint;
    }
    return held;
}

} // namespace FastCache::Cluster
