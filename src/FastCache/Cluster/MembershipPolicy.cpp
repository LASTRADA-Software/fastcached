// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Core/HostPort.hpp>

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

std::optional<std::vector<Consensus::NodeId>> NextQuorumChange(ClusterState const& state,
                                                               std::span<Consensus::NodeId const> active,
                                                               Consensus::NodeId const& self,
                                                               std::span<Consensus::NodeId const> bootstrap)
{
    // A node with no cluster counts nobody and proposes nothing. It is never a
    // leader either -- see `RaftNode::HasCluster` -- so this is a guard against
    // being asked rather than a case that arises.
    if (active.empty())
        return std::nullopt;

    auto const counted = [active](Consensus::NodeId const& id) {
        return std::ranges::find(active, id) != active.end();
    };

    // Additions first: growing before shrinking keeps the quorum reachable while a
    // replacement is in progress, where the other order passes through a
    // configuration smaller than either endpoint.
    for (auto const& member: state.members)
    {
        if (counted(member.id))
            continue;

        // A member the transport cannot dial must not be counted: the quorum would
        // grow and the votes to satisfy it could never arrive. BOTH halves of the
        // question every dialer asks, because a split alone is not enough --
        // `10.0.0.4:0` splits and names no port anybody can connect to, so a member
        // recorded that way would be counted here and silently never dialled.
        auto const split = SplitHostPort(member.raftEndpoint);
        if (!split.has_value() || !ParseTcpPort(split->second).has_value())
            continue;

        auto proposed = std::vector<Consensus::NodeId> { active.begin(), active.end() };
        proposed.push_back(member.id);
        return proposed;
    }

    // A node that was given no bootstrap set has nothing to compare against, so
    // every member is equally unexplained to it -- and a `--raft-join` node elected
    // leader would remove all of them, one per commit, which is the failure the
    // parameter exists to prevent reached through the one path with no baseline.
    if (bootstrap.empty())
        return std::nullopt;

    for (auto const& id: active)
    {
        // Never itself: a leader taking itself out of the quorum it leads is an
        // operator's decision, and the next pass would propose putting it back
        // anyway, because a node always desires its own record.
        if (id == self)
            continue;

        // Membership only, and deliberately not dialability. A member already
        // counted whose recorded endpoint has become unreadable is a bad record,
        // and shrinking the quorum over one turns a typo into a cluster that
        // cannot elect.
        if (std::ranges::find(state.members, id, &ClusterMember::id) != state.members.end())
            continue;

        // Absent, but was it ever meant to be there? `--raft-peer` puts a member in
        // the configuration and nothing puts it in the state, so on a typed cluster
        // every peer is absent from birth -- and reading that as "forgotten"
        // proposes removing all of them, one per commit, until the leader is alone.
        // A member an operator typed is a member by their assertion; only one
        // admitted at runtime can be un-admitted at runtime.
        if (std::ranges::find(bootstrap, id) != bootstrap.end())
            continue;

        auto proposed = std::vector<Consensus::NodeId> { active.begin(), active.end() };
        std::erase(proposed, id);
        return proposed;
    }

    return std::nullopt;
}

} // namespace FastCache::Cluster
