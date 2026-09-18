// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <array>

namespace FastCache::Cluster
{

namespace
{
    /// Whether the transport can dial `member` at all.
    ///
    /// BOTH halves of the question every dialer asks, because a split alone is not
    /// enough -- `10.0.0.4:0` splits and names no port anybody can connect to, so a
    /// member recorded that way would be counted and silently never dialled.
    /// @param member The record.
    /// @return True when its consensus endpoint names a host and a port.
    [[nodiscard]] bool Dialable(ClusterMember const& member)
    {
        auto const split = SplitHostPort(member.raftEndpoint);
        return split.has_value() && ParseTcpPort(split->second).has_value();
    }

    /// The order seat changes are proposed in: promotions, then demotions.
    ///
    /// The seat MOVED INTO, so `Voter` first is a promotion first -- growing the voter
    /// set before shrinking it, for the reason additions precede removals. A list
    /// rather than `MemberSeatTable`'s enumerator order, because that order is a wire
    /// contract and this one is a safety argument, and neither should move the other.
    constexpr std::array SeatChangeOrder { MemberSeat::Voter, MemberSeat::Learner };

    /// A configuration a change may land on: one with a voter left in it.
    ///
    /// `Membership::Validate` refuses the other kind at the leader anyway; asked here so
    /// the policy never proposes what consensus would refuse, and so a refused step does
    /// not stall every step behind it.
    /// @param configuration The candidate.
    /// @return True when somebody would still be counted.
    [[nodiscard]] bool KeepsAVoter(Consensus::Configuration const& configuration) noexcept
    {
        return !configuration.voters.empty();
    }
} // namespace

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

        // The seat by the same rule (#1449), which is what keeps a node's own record
        // from undoing an operator's demotion: a node always desires itself, and a
        // record re-proposed as a voter on every pass would promote it straight back.
        // Absent and unrecorded is a voter, which is what this reconciler has always
        // admitted -- `RecordedSeatOf`, the one reading of *no opinion*.
        auto const seat = member.seat.value_or(RecordedSeatOf(state, member.id));

        // The key by the same rule once more (#178): no opinion is whatever is recorded. The
        // command carries the opinion itself rather than the resolved value, because
        // `AddMember` reads an absent key as *keep*, which is this rule stated once more at
        // the layer that applies it.
        auto const publicKey = member.publicKey.or_else([&] { return known ? it->publicKey : std::nullopt; });

        if (known && it->raftEndpoint == member.raftEndpoint && it->schedulerEndpoint == scheduler && it->seat == seat
            && it->publicKey == publicKey)
            continue;

        proposals.push_back(Command { .kind = MemberSeatTable[static_cast<std::size_t>(seat)].admittedBy,
                                      .key = member.id,
                                      .value = member.raftEndpoint,
                                      .schedulerEndpoint = scheduler,
                                      .publicKey = member.publicKey,
                                      .role = std::nullopt });
    }
    return proposals;
}

std::optional<Consensus::Configuration> NextQuorumChange(ClusterState const& state,
                                                         Consensus::Configuration const& active,
                                                         Consensus::NodeId const& self,
                                                         std::span<Consensus::NodeId const> bootstrap)
{
    // A node with no cluster counts nobody and proposes nothing. It is never a
    // leader either -- see `RaftNode::HasCluster` -- so this is a guard against
    // being asked rather than a case that arises.
    if (Consensus::Membership::IsEmpty(active))
        return std::nullopt;

    auto const rowOf = [](MemberSeat seat) -> MemberSeatRow const& {
        return MemberSeatTable[static_cast<std::size_t>(seat)];
    };

    // Additions first: growing before shrinking keeps the quorum reachable while a
    // replacement is in progress, where the other order passes through a
    // configuration smaller than either endpoint. A member is added into the set its
    // record names -- a learner addition changes no quorum at all.
    for (auto const& member: state.members)
    {
        if (Consensus::Membership::IsMember(active, member.id))
            continue;

        // A member the transport cannot dial must not be counted: the quorum would
        // grow and the votes to satisfy it could never arrive. Asked of a learner too,
        // which counts nothing but is replicated to -- and replication to an address
        // nobody can dial is a member that never catches up.
        if (!Dialable(member))
            continue;

        auto proposed = active;
        (proposed.*rowOf(member.seat).set).push_back(member.id);
        return proposed;
    }

    // Then a seat change (#1449): a member counted in one set whose record names the
    // other. Promotions before demotions, for the reason additions come first.
    for (auto const into: SeatChangeOrder)
    {
        for (auto const& member: state.members)
        {
            // Only a member consensus already counts somewhere: one it counts nowhere
            // is an ADDITION, which the pass above either proposed or refused.
            if (member.seat != into || !Consensus::Membership::IsMember(active, member.id)
                || Consensus::Membership::Contains(active.*rowOf(into).set, member.id))
                continue;

            // A promotion is a voter ADDITION, and follows its rule: never counted
            // before every node can dial it. A demotion counts nobody new.
            if (rowOf(into).counted && !Dialable(member))
                continue;

            auto proposed = active;
            for (auto const& row: MemberSeatTable)
                std::erase(proposed.*row.set, member.id);
            (proposed.*rowOf(into).set).push_back(member.id);

            // Never the last voter: a configuration nobody is counted in can commit
            // nothing, including the change that would undo it. The record then says
            // learner while consensus goes on counting the member, which is the
            // fail-closed direction -- one voter too many rather than none.
            if (!KeepsAVoter(proposed))
                continue;
            return proposed;
        }
    }

    // A node that was given no bootstrap set has nothing to compare against, so
    // every member is equally unexplained to it -- and a `--raft-join` node elected
    // leader would remove all of them, one per commit, which is the failure the
    // parameter exists to prevent reached through the one path with no baseline.
    if (bootstrap.empty())
        return std::nullopt;

    // Removals, from either set. A learner is removed on exactly the terms a voter is
    // -- forgotten by the operator, and never for being ABSENT: nothing here reads
    // whether a member answers, so a learner that has been offline for a week is as
    // safe as one that answered a moment ago.
    for (auto const& row: MemberSeatTable)
    {
        for (auto const& id: active.*row.set)
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

            auto proposed = active;
            std::erase(proposed.*row.set, id);
            if (!KeepsAVoter(proposed))
                continue;
            return proposed;
        }
    }

    return std::nullopt;
}

} // namespace FastCache::Cluster
