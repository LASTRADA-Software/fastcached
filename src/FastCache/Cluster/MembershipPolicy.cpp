// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>

#include <algorithm>

namespace FastCache::Cluster
{

std::vector<Command> MembershipProposals(ClusterState const& state, std::span<ClusterMember const> desired)
{
    std::vector<Command> proposals;
    for (auto const& member: desired)
    {
        // A record with no id or no consensus endpoint is not a member. `Validate`
        // would refuse it at the leader anyway, so proposing it would cost a refusal
        // per interval and change nothing -- and the diagnostic would name the
        // reconciler rather than whatever produced the half-record.
        if (member.id.empty() || member.raftEndpoint.empty())
            continue;

        auto const it = std::ranges::find(state.members, member.id, &ClusterMember::id);
        if (it != state.members.end() && *it == member)
            continue;

        proposals.push_back(Command { .kind = CommandKind::AddMember,
                                      .key = member.id,
                                      .value = member.raftEndpoint,
                                      .schedulerEndpoint = member.schedulerEndpoint });
    }
    return proposals;
}

} // namespace FastCache::Cluster
