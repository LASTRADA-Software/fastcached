// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <vector>

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

    /// This node's own removal, once the operator has forgotten it (#1539).
    ///
    /// FORGOTTEN is both facts `RemoveMember` writes: its record gone, and its host
    /// tombstoned. Either alone is not a forget -- a fresh leader's first pass has
    /// recorded nothing yet, and a client forget may name a member's host -- so
    /// removing on the first would take every new cluster's only voter out of it.
    /// @param state The replicated state.
    /// @param active The configuration consensus holds.
    /// @param self This node's own record, as it announces it.
    /// @return The configuration without this node, or nullopt when it is not
    ///         forgotten, is counted nowhere, or is the last voter.
    [[nodiscard]] std::optional<Consensus::Configuration> OwnRemoval(ClusterState const& state,
                                                                     Consensus::Configuration const& active,
                                                                     ClusterMember const& self)
    {
        auto const recorded = std::ranges::find(state.members, self.id, &ClusterMember::id) != state.members.end();
        if (recorded || !state.HasForgotten(HostOfEndpoint(self.raftEndpoint)))
            return std::nullopt;

        for (auto const& row: MemberSeatTable)
        {
            if (!Consensus::Membership::Contains(active.*row.set, self.id))
                continue;

            // Never the last voter. `ValidateForget` refuses that forget by name before
            // it is proposed, so arriving here means the voters changed after it
            // committed -- and the fail-closed answer is still to stay counted.
            auto proposed = active;
            std::erase(proposed.*row.set, self.id);
            if (!KeepsAVoter(proposed))
                return std::nullopt;
            return proposed;
        }
        return std::nullopt;
    }

    /// The seat `id` is recorded in: wherever something already placed it, else a newcomer's.
    ///
    /// In precedence order, and the order is the rule (#1535). The operator's record
    /// first, even where consensus has not caught up with it -- a demotion in flight is
    /// the record running ahead, and reading the configuration first would undo it.
    /// Then the configuration, for the member it counts and the state never recorded:
    /// every `--raft-peer` member, and this node itself before its first pass. Only
    /// then `NewcomerSeat`.
    /// @param state The replicated state.
    /// @param active The configuration consensus holds.
    /// @param id The member.
    /// @return Its seat.
    [[nodiscard]] MemberSeat SeatFor(ClusterState const& state,
                                     Consensus::Configuration const& active,
                                     Consensus::NodeId const& id)
    {
        if (auto const it = std::ranges::find(state.members, id, &ClusterMember::id); it != state.members.end())
            return it->seat;
        for (auto const& row: MemberSeatTable)
            if (Consensus::Membership::Contains(active.*row.set, id))
                return row.seat;
        return NewcomerSeat;
    }
} // namespace

MembershipPlan MembershipProposals(ClusterState const& state,
                                   Consensus::Configuration const& active,
                                   std::span<DesiredMember const> desired)
{
    MembershipPlan plan;
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

        // Never the desire's to decide (#1535): whatever placed the member keeps it
        // there, which is what stops this node's own record undoing a demotion and a
        // rediscovered peer undoing a promotion, and a member nothing placed joins as
        // a learner an operator promotes.
        auto const seat = SeatFor(state, active, member.id);

        if (known && it->raftEndpoint == member.raftEndpoint && it->schedulerEndpoint == scheduler && it->seat == seat)
            continue;

        // A forget outranks an observation (#1528). Asked with the host `Apply` would
        // lift the tombstone for and through the same comparison, so what is refused
        // here is exactly a proposal that would have undone the forget -- and only
        // once something would be proposed, since a record that already matches lifts
        // nothing and is no refusal.
        if (state.HasForgotten(HostOfEndpoint(member.raftEndpoint)))
        {
            plan.forgotten.push_back(member);
            continue;
        }

        plan.proposals.push_back(Command { .kind = MemberSeatTable[static_cast<std::size_t>(seat)].admittedBy,
                                           .key = member.id,
                                           .value = member.raftEndpoint,
                                           .schedulerEndpoint = scheduler });
    }
    return plan;
}

bool Replication::CaughtUp(Consensus::NodeId const& id) const
{
    auto const found = matchIndex.find(id);
    return found != matchIndex.end() && found->second >= commitIndex;
}

namespace
{
    /// Whether `member` may be COUNTED yet: dialable, and holding every committed entry.
    ///
    /// The leader itself has trivially caught up -- its log is the log -- and appears in
    /// no match index, so it is answered by name rather than looked up.
    /// @param member The record.
    /// @param self This node's id.
    /// @param replication What the leader knows of each member's log.
    /// @return True when a quorum may count it.
    [[nodiscard]] bool Countable(ClusterMember const& member, Consensus::NodeId const& self, Replication const& replication)
    {
        return Dialable(member) && (member.id == self || replication.CaughtUp(member.id));
    }

    /// Members recorded as voters that consensus holds as learners until they catch up.
    /// @param state The replicated state.
    /// @param active The configuration consensus holds.
    /// @param self This node's id.
    /// @param replication What the leader knows of each member's log.
    /// @return Their ids, in state order.
    [[nodiscard]] std::vector<Consensus::NodeId> CatchingUp(ClusterState const& state,
                                                            Consensus::Configuration const& active,
                                                            Consensus::NodeId const& self,
                                                            Replication const& replication)
    {
        auto ids = std::vector<Consensus::NodeId> {};
        for (auto const& member: state.members)
            if (member.seat == MemberSeat::Voter && Consensus::Membership::Contains(active.learners, member.id)
                && Dialable(member) && !Countable(member, self, replication))
                ids.push_back(member.id);
        return ids;
    }

    /// The one change `NextQuorumChange` proposes; see there for the rules.
    /// @param state The replicated state.
    /// @param active The configuration consensus holds.
    /// @param self This node's own record.
    /// @param bootstrap The ids this node was started with.
    /// @param replication What the leader knows of each member's log.
    /// @return The configuration to propose, or nullopt.
    [[nodiscard]] std::optional<Consensus::Configuration> NextStep(ClusterState const& state,
                                                                   Consensus::Configuration const& active,
                                                                   ClusterMember const& self,
                                                                   std::span<Consensus::NodeId const> bootstrap,
                                                                   Replication const& replication)
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
        // configuration smaller than either endpoint. Every member joins the configuration
        // as a LEARNER, whatever its record names (#1537): a learner addition changes no
        // quorum, and a member consensus has never replicated to has caught up with
        // nothing. One the record names a voter is then PROMOTED, below, once it has.
        for (auto const& member: state.members)
        {
            if (Consensus::Membership::IsMember(active, member.id))
                continue;

            // A member the transport cannot dial is not added: replication to an address
            // nobody can dial is a member that never catches up.
            if (!Dialable(member))
                continue;

            auto proposed = active;
            (proposed.*rowOf(MemberSeat::Learner).set).push_back(member.id);
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

                // A promotion is what COUNTS a member, so it waits for the two things a
                // counted member must already have (#1537): an address every node can dial,
                // and every committed entry. Counted before it has caught up, a voter that
                // is away makes every commit wait for it -- in a one-voter cluster the
                // promotion itself cannot commit, and nothing after it can either. A
                // demotion counts nobody new and waits for neither.
                if (rowOf(into).counted && !Countable(member, self.id, replication))
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
        // It still removes ITSELF once forgotten, below: that question is answered by the
        // tombstone, not by the bootstrap set.
        if (bootstrap.empty())
            return OwnRemoval(state, active, self);

        // Removals, from either set. A learner is removed on exactly the terms a voter is
        // -- forgotten by the operator, and never for being ABSENT: nothing here reads
        // whether a member answers, so a learner that has been offline for a week is as
        // safe as one that answered a moment ago.
        for (auto const& row: MemberSeatTable)
        {
            for (auto const& id: active.*row.set)
            {
                // Not itself here: this node's own removal is decided LAST, below, and
                // only once it is forgotten -- absent a forget the next pass would propose
                // putting it back, because a node always desires its own record.
                if (id == self.id)
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

        // Last, this node itself (#1539): after every change it can still make as the
        // leader, since once its own removal commits it leads nothing.
        return OwnRemoval(state, active, self);
    }
} // namespace

QuorumPlan NextQuorumChange(ClusterState const& state,
                            Consensus::Configuration const& active,
                            ClusterMember const& self,
                            std::span<Consensus::NodeId const> bootstrap,
                            Replication const& replication)
{
    // A node with no cluster counts nobody and proposes nothing, and waits on nobody.
    if (Consensus::Membership::IsEmpty(active))
        return QuorumPlan { .change = std::nullopt, .catchingUp = {} };

    return QuorumPlan { .change = NextStep(state, active, self, bootstrap, replication),
                        .catchingUp = CatchingUp(state, active, self.id, replication) };
}

std::expected<void, ConsensusError> ValidateForget(Consensus::Configuration const& active, Consensus::NodeId const& id)
{
    // The one forget no quorum can follow: removing the only voter. Any other member --
    // a learner, one of several voters, one consensus does not count at all -- leaves a
    // configuration somebody is counted in.
    if (active.voters.size() != 1 || active.voters.front() != id)
        return {};

    return std::unexpected { ConsensusError {
        .code = ConsensusErrorCode::InvalidConfiguration,
        .context = std::format("cannot forget {}: it is the cluster's only voter, and a configuration with no voter "
                               "can commit nothing -- admit or promote another voter first",
                               id),
        .knownLeader = std::nullopt } };
}

} // namespace FastCache::Cluster
