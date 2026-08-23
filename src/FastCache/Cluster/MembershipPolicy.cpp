// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>

#include <algorithm>

namespace FastCache::Cluster
{

std::vector<Command> MembershipProposals(ClusterState const& state, std::span<DesiredMember const> desired)
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
        auto const known = it != state.members.end();

        // `value_or` rather than a checked dereference, for the reason `Unwrap` is
        // spelled that way in the tests: it is provably safe, so the "unchecked
        // optional access" analysis has nothing to object to. What it computes is the
        // rule this type exists for -- no opinion means whatever is recorded stands.
        auto const scheduler = member.schedulerEndpoint.value_or(known ? it->schedulerEndpoint : std::string {});

        if (known && it->raftEndpoint == member.raftEndpoint && it->schedulerEndpoint == scheduler)
            continue;

        proposals.push_back(Command { .kind = CommandKind::AddMember,
                                      .key = member.id,
                                      .value = member.raftEndpoint,
                                      .schedulerEndpoint = scheduler });
    }
    return proposals;
}

} // namespace FastCache::Cluster
