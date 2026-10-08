// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/AnnouncedEndpoints.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Cluster
{

namespace
{
    /// Whether @p recorded is COUPLED: a seat that is dialled, whose recorded Raft and `0xFC`
    /// endpoints both parse as dial endpoints on the same host.
    /// @param recorded The member's record.
    /// @return Its Raft endpoint, split, when coupled; nullopt otherwise.
    [[nodiscard]] std::optional<std::pair<std::string, std::uint16_t>> CoupledRaft(ClusterMember const& recorded)
    {
        if (!SeatNeedsEndpoint(recorded.seat))
            return std::nullopt;
        auto raft = ParseDialEndpoint(recorded.raftEndpoint);
        auto const scheduler = ParseDialEndpoint(recorded.schedulerEndpoint);
        if (!raft.has_value() || !scheduler.has_value() || !SameHost(raft->first, scheduler->first))
            return std::nullopt;
        return raft;
    }
} // namespace

std::string CoupledRaftEndpoint(ClusterMember const& recorded, std::string_view announced)
{
    auto const raft = CoupledRaft(recorded);
    auto const moved = ParseDialEndpoint(announced);
    if (!raft.has_value() || !moved.has_value())
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

std::vector<DesiredMember> WithCoupledRaftEndpointsKept(ClusterState const& state, std::vector<DesiredMember> desired)
{
    for (auto& desire: desired)
    {
        if (desire.schedulerEndpoint.has_value())
            continue;
        auto const* const recorded = core::findOrNull(state.members, desire.id, &ClusterMember::id);
        if (recorded == nullptr)
            continue;
        auto const raft = CoupledRaft(*recorded);
        auto const wanted = ParseDialEndpoint(desire.raftEndpoint);
        if (raft.has_value() && (!wanted.has_value() || !SameHost(wanted->first, raft->first)))
            desire.raftEndpoint = recorded->raftEndpoint;
    }
    return desired;
}

} // namespace FastCache::Cluster
