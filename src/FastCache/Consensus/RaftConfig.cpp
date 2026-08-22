// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftConfig.hpp>

#include <algorithm>
#include <format>
#include <ranges>

namespace FastCache::Consensus
{

std::expected<void, ConsensusError> RaftConfig::Validate() const
{
    if (self.empty())
        return std::unexpected { InvalidConfiguration("this node has no identity: `self` is empty") };

    if (members.empty())
        return std::unexpected { InvalidConfiguration("the cluster has no members") };

    if (std::ranges::find(members, self) == members.end())
        return std::unexpected { InvalidConfiguration(std::format("`self` ({}) is not among the cluster members", self)) };

    // A duplicate member would be counted twice toward a quorum, so a "majority"
    // could be one physical node agreeing with itself -- which is Election Safety
    // gone, from a typo in a member list.
    auto sorted = members;
    std::ranges::sort(sorted);
    if (auto const duplicate = std::ranges::adjacent_find(sorted); duplicate != sorted.end())
        return std::unexpected { InvalidConfiguration(std::format("member {} is listed more than once", *duplicate)) };

    if (electionTimeoutMin <= std::chrono::milliseconds::zero())
        return std::unexpected { InvalidConfiguration("the election timeout minimum must be positive") };

    if (electionTimeoutMax < electionTimeoutMin)
        return std::unexpected { InvalidConfiguration(
            std::format("the election timeout range is inverted: minimum {}ms exceeds maximum {}ms",
                        electionTimeoutMin.count(),
                        electionTimeoutMax.count())) };

    if (heartbeatInterval <= std::chrono::milliseconds::zero())
        return std::unexpected { InvalidConfiguration("the heartbeat interval must be positive") };

    // Raft needs broadcastTime << electionTimeout. Equality is already too slow:
    // a heartbeat arriving exactly as followers time out deposes a healthy leader
    // roughly half the time, and the cluster spends its life electing.
    if (heartbeatInterval >= electionTimeoutMin)
        return std::unexpected { InvalidConfiguration(
            std::format("the heartbeat interval ({}ms) must be below the election timeout minimum ({}ms)",
                        heartbeatInterval.count(),
                        electionTimeoutMin.count())) };

    return {};
}

std::size_t RaftConfig::Quorum() const noexcept
{
    return (members.size() / 2) + 1;
}

std::vector<NodeId> RaftConfig::Peers() const
{
    auto peers = std::vector<NodeId> {};
    peers.reserve(members.empty() ? 0 : members.size() - 1);
    for (auto const& member: members)
        if (member != self)
            peers.push_back(member);

    return peers;
}

} // namespace FastCache::Consensus
