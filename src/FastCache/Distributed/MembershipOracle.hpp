// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Distributed
{

/// Answers whether the peer at an address has earned the fleet's capacity.
///
/// A seam rather than a call into `Cluster::PeerDirectory`, for two reasons. The
/// dependency would run the wrong way -- `Distributed` is the policy and `Cluster`
/// is one way of establishing the fact it needs -- and more importantly the answer
/// is *deployment-shaped*: a single-machine install has no cluster and every caller
/// is legitimately a member, while a shared fleet must refuse anyone who has not
/// proved the key. Those are two implementations of one question, which is what an
/// interface is for.
class IMembershipOracle
{
  public:
    virtual ~IMembershipOracle() = default;

    IMembershipOracle() = default;
    IMembershipOracle(IMembershipOracle const&) = default;
    IMembershipOracle& operator=(IMembershipOracle const&) = default;
    IMembershipOracle(IMembershipOracle&&) = default;
    IMembershipOracle& operator=(IMembershipOracle&&) = default;

    /// Classify one caller, and say which participant decided.
    ///
    /// **The one door.** `Classify` below is derived from this rather than declared beside it,
    /// so an implementation cannot answer the enforced question and the reported one
    /// differently (#1471).
    /// @param peerAddress The address the connection came from, as text.
    /// @return The verdict and the route that produced it.
    [[nodiscard]] virtual MembershipDecision Explain(std::string_view peerAddress) const = 0;

    /// Whether that peer may be scheduled onto the fleet.
    ///
    /// **Non-virtual, and that is load-bearing.** Left virtual this is a second door: an
    /// implementation could override it inconsistently with `Explain`, and the answer a
    /// `--node-status` reports would have a separate place to be right from the answer a surface
    /// enforces. The reported and the enforced answer being one computation is the whole point
    /// of #1471 -- two folds over the same data are kept in step by nothing.
    /// @param peerAddress The address the connection came from, as text.
    /// @return `Explain(peerAddress).verdict`.
    [[nodiscard]] Membership Classify(std::string_view peerAddress) const
    {
        return Explain(peerAddress).verdict;
    }
};

/// Everyone is a member.
///
/// The right answer for a worker or scheduler that is not in a cluster at all --
/// one machine, or a fleet whose reachability is its boundary. It is **not** the
/// default anywhere: a node has to be configured into this, because "no policy" and
/// "a policy that admits everybody" must be a decision somebody made rather than
/// what happens when a field is left unset. `Membership::Outsider` being the zero
/// value is the other half of that rule.
class OpenMembership final: public IMembershipOracle
{
  public:
    [[nodiscard]] MembershipDecision Explain(std::string_view /*peerAddress*/) const override
    {
        return DecidedBy(Membership::Member, MembershipParticipant::OpenPolicy);
    }
};

/// A member of any participant is a member.
///
/// Admission is a **union** of independent routes, and that is the shape the
/// question has rather than a convenience. One list answering two questions is
/// what #251 was: `--fleet-member` names who may spend this node's CPU and read
/// its cache tier -- which is mostly *clients*, developer laptops and CI runners,
/// machines that never join consensus and never should -- while the cluster's
/// agreed member set names who is in the cluster, which is peers only. Publishing
/// the second over the first revoked every listed host at the first replicated
/// membership commit, and agreeing something is routine.
///
/// Composed through `IMembershipOracle` rather than by teaching one oracle to hold
/// several lists, because the routes are not all lists of hosts. A signed lease
/// token is an admission route for a peer holding a valid grant -- a credential
/// check, with no host set to add a row to -- and it *adds* to an address policy
/// rather than replacing it, since that policy is what still gates the cache tier
/// and what an operator sets on a worker taking no leases at all. A design that
/// enumerated sources inside one host-list type could not hold it; a participant
/// list can, and the seam already exists.
///
/// Participants are borrowed and must outlive this object. That is safe by
/// construction where it is used: `Node::NodeMembership` owns both the
/// participants and the composite, and is neither copyable nor movable.
class AnyOfMembership final: public IMembershipOracle
{
  public:
    /// @param participants The routes to consult, in the order they are asked.
    ///        Each must outlive this object. An empty list refuses everybody,
    ///        which is the same direction every other default here fails in.
    explicit AnyOfMembership(std::vector<IMembershipOracle const*> participants):
        _participants { std::move(participants) }
    {
    }

    /// @param peerAddress The connecting peer's **host**, as the participants take it.
    /// @return The participants' answers folded on `PrecedenceOf`: forgotten, else member,
    ///         else outsider.
    ///
    /// **Not `any_of(... == Member)`**, which this was. That flattened every other answer to
    /// `Outsider`, so a participant's `Forgotten` reached no surface as itself and no counted
    /// refusal could fire (#1309) -- the safety survived and the distinction did not, with
    /// nothing to warn anybody. A forget must also OUTRANK a listing, because the host an
    /// operator has just forgotten in the cluster is exactly the one still named by
    /// `--fleet-member` on a node nobody has reconfigured yet.
    [[nodiscard]] MembershipDecision Explain(std::string_view peerAddress) const override
    {
        // The same fold, carrying every route that produced the winning verdict rather than
        // only the verdict. The reported set is then by definition the routes whose answer won,
        // which is what makes the reported and the enforced answer one computation (#1471).
        //
        // **REPLACE on a higher precedence, UNION on a tie**, and the tie arm is the load-bearing
        // one. Two routes answering `Member` are BOTH right -- a host can sit in `--fleet-member`
        // and in the committed set at once -- so keeping the first would report one true route and
        // hide another. An operator told only `FleetMemberList` removes the host from
        // `--fleet-member` and finds it still served, which is the scenario this ticket opens
        // with: the fix would have reproduced the defect it exists to remove.
        //
        // The initial value is `{Outsider, {}}` and stays so when every participant answers
        // `Outsider`: nobody decided, and the refusal is by ABSENCE rather than by row. Those are
        // different facts and an operator asking why a host is refused needs the difference. An
        // `Outsider` answer carries an empty set (`DecidedBy` refuses to attribute one), so the
        // tie arm unions nothing and the distinction survives the fold.
        auto decision = MembershipDecision {};
        for (auto const* participant: _participants)
        {
            auto const answer = participant->Explain(peerAddress);
            auto const rank = PrecedenceOf(answer.verdict);
            auto const winning = PrecedenceOf(decision.verdict);
            if (rank > winning)
                decision = answer;
            else if (rank == winning)
                decision.decidedBy.Add(answer.decidedBy);
        }
        return decision;
    }

  private:
    std::vector<IMembershipOracle const*> _participants;
};

/// The admission decision for one CONNECTION: the address routes, plus what this connection
/// has PROVED.
///
/// ## Why this is not a participant
///
/// Every route in `AnyOfMembership` answers `Explain(peerAddress)`, and one oracle serves every
/// connection a surface accepts. A cluster-key proof is a fact about ONE connection, so a
/// participant carrying it would have to read state some other connection wrote -- which admits
/// callers that proved nothing, on a seam whose whole contract is that the argument decides.
/// There is nowhere in the participant list for a per-connection fact to live, so it is folded
/// here, where the connection is.
///
/// ## Why it is the SAME fold
///
/// `PrecedenceOf`, unchanged, which settles the one question this route raises: a
/// `--cluster-forget-client` tombstone still outranks a proof. That is not a softening of what
/// a proof means -- under a shared key a forget cannot revoke a key holder at all, and
/// `Distributed::NodeProof`'s header says so and says what closing that needs (#178). It is the
/// direction the composition already fails in, and the alternative would be a second precedence
/// rule reachable only from a connection.
///
/// **It takes a `bool` and not the proven id, deliberately.** What the fold needs is *did this
/// connection prove the key*; the id a proof carries is a LABEL the MAC covers so it cannot be
/// swapped, it is legitimately empty on any node that has minted no identity, and admission must
/// not turn on it. A parameter that carried the label would make an empty one indistinguishable
/// from no proof at all.
///
/// `false` is the ordinary case and the one every caller today is in: the fold then returns
/// exactly what the oracle answered, so a surface that never establishes a proof is unaffected by
/// construction.
///
/// @param oracle The address routes, composed.
/// @param peerAddress The connecting peer's host, as the oracle takes it.
/// @param provedClusterKey Whether this connection proved the cluster's pre-shared key.
/// @return The folded decision, naming `ProvenKeyHolder` among the routes when the proof won or
///         tied.
[[nodiscard]] inline MembershipDecision ExplainConnection(IMembershipOracle const& oracle,
                                                          std::string_view peerAddress,
                                                          bool provedClusterKey)
{
    auto const byAddress = oracle.Explain(peerAddress);
    if (!provedClusterKey)
        return byAddress;

    auto const proved = DecidedBy(Membership::Member, MembershipParticipant::ProvenKeyHolder);
    auto const addressRank = PrecedenceOf(byAddress.verdict);
    auto const provedRank = PrecedenceOf(proved.verdict);
    if (provedRank > addressRank)
        return proved;
    if (provedRank < addressRank)
        return byAddress;

    // A tie UNIONS, for `AnyOfMembership`'s reason: a host in `--fleet-member` that also proved
    // the key is admitted by both, and reporting one hides the other -- which is exactly the
    // operator question #1471 opens with, asked of a route an operator cannot see in any file.
    auto tied = byAddress;
    tied.decidedBy.Add(proved.decidedBy);
    return tied;
}

/// What one set of hosts answers, per question the set is asked.
///
/// Two columns rather than one, and `onLoopback` is the load-bearing half. A host
/// set is asked one of two questions here -- *is this caller admitted* and *has this
/// caller been forgotten* -- and the two disagree about THIS MACHINE in a way no
/// derivation from `onMatch` can express: an admission list says its own machine is a
/// member whatever it holds (`ClusterMembership`'s oldest rule), while a forgotten
/// list must say *nothing at all* about its own machine. A forgotten set that
/// answered `Forgotten` for loopback would refuse the local builds that are the
/// entire reason the node is there, silently and on every surface at once.
///
/// `Cluster::Validate` already refuses a loopback host to `ForgetClient`, so today no
/// such entry can be committed. That is a reason the guard is never REACHED, not a
/// reason to leave it out: the consequence of it being wrong is invisible, and a rule
/// enforced only by a validator two modules away is one a later route can bypass with
/// nothing to warn anybody.
struct HostSetVerdicts
{
    Membership onMatch;    ///< What a host IN the set is.
    Membership onLoopback; ///< What this machine is, whatever the set holds.
};

/// The verdicts an admission list answers with: listed hosts and this machine.
inline constexpr HostSetVerdicts AdmissionVerdicts { .onMatch = Membership::Member, .onLoopback = Membership::Member };

/// The verdicts a forgotten list answers with: forgotten hosts, and silence about
/// this machine. `Outsider` is that silence -- it is `PrecedenceOf` 0, so it loses to
/// every other participant in `AnyOfMembership` rather than contributing an opinion.
inline constexpr HostSetVerdicts ForgottenVerdicts { .onMatch = Membership::Forgotten, .onLoopback = Membership::Outsider };

/// One verdict over one set of hosts, published wholesale and asked per connection.
///
/// Not a question in itself -- `ClusterMembership` and `ForgottenMembership` below are
/// the questions, and this is what they share. Extracted when the second one arrived
/// (#1309): a forgotten set is the same object in every respect that has ever cost a
/// bug here -- the host-against-endpoint vocabulary, the two IPv6 spellings, the
/// swap-under-a-shared-lock -- and differs only in what it ANSWERS. A second copy
/// would have been a second place for each of those to be got wrong, in the one file
/// whose comments exist because they already were.
///
/// Holding a copy of what established the set, rather than a reference to it, is what
/// keeps this free of the modules that produce one -- discovery, consensus -- and the
/// copy is small: a fleet is tens of machines, and it changes when membership does
/// rather than per request.
///
/// ## The identity is a HOST, and that is forced rather than chosen
///
/// Discovery admits a peer at a `(node, endpoint)` pair, so the obvious member set is
/// endpoints -- and matching a caller against it would never succeed even once. A peer
/// *connecting* to the scheduler does so from an **ephemeral source port**, which is
/// not its Raft endpoint and differs on every connection, so `ISocket::PeerAddress()`
/// reports a bare host and there is nothing to compare a port against. An
/// endpoint-keyed set would therefore refuse every legitimate member while looking
/// entirely correct, and the fleet would silently never distribute anything.
///
/// So the two vocabularies are collapsed **in the constructor** rather than left to
/// each caller: this takes the endpoints discovery produced and stores their host
/// parts, and `Classify` takes a host. There is no way to publish one vocabulary and
/// query the other, which is the only reliable defence against a mismatch whose
/// failure mode is silent and looks like a healthy fleet.
///
/// What that gives up is stated rather than hidden: two nodes behind one NAT, or two
/// workers on one machine, are indistinguishable here. Both are already inside the
/// trust boundary the pre-shared key establishes -- this refuses *strangers*, not
/// co-located peers -- and separating them needs a credential in the frame, which is a
/// different change with a different threat model.
class HostSetMembership: public IMembershipOracle
{
  public:
    /// @param memberEndpoints `host:port` endpoints of the authenticated peers. Only
    ///        the host part is retained; see the class note.
    /// @param verdicts What this set answers on a match and for this machine.
    /// @param memberEndpoints `host:port` endpoints. Only the host part is retained.
    /// @param participant Which route this list IS, for a report that must name the
    ///        participant that DECIDED (#1471). Required rather than defaulted: a node owns
    ///        one instance per question, so a default would make two lists
    ///        indistinguishable in a report, which is the question this answers.
    explicit HostSetMembership(HostSetVerdicts verdicts,
                               MembershipParticipant participant,
                               std::vector<std::string> const& memberEndpoints = {}):
        _verdicts { verdicts },
        _participant { participant }
    {
        Publish(memberEndpoints);
    }

    /// Replace the member set.
    ///
    /// A setter, and one of the documented carve-outs to configuration-at-
    /// construction: membership is precisely the thing that changes while this
    /// object lives, and rebuilding the oracle on every join would mean handing a
    /// new one to a running server.
    ///
    /// Wholesale, which is right for the *one* question an owner gave this list --
    /// whoever publishes it holds the whole truth about that question -- and wrong
    /// for admission as a whole. A node with more than one admission route composes
    /// one of these per route through `AnyOfMembership`; see #251, where a single
    /// list took the second answer for the first.
    ///
    /// **Thread-safe against `Classify`**, which is not a nicety here: the natural
    /// caller is consensus, publishing what the cluster just agreed from its own
    /// thread, while three surfaces classify callers on theirs. A plain vector
    /// mutated under those readers is a data race whose symptom is a torn string
    /// compare -- a peer admitted or refused at random, on a path where the answer
    /// decides who may spend this machine's CPU.
    /// @param memberEndpoints The new set, as `host:port`.
    void Publish(std::vector<std::string> const& memberEndpoints)
    {
        std::vector<std::string> hosts;
        hosts.reserve(memberEndpoints.size());
        for (auto const& endpoint: memberEndpoints)
        {
            // An endpoint that will not split is kept whole rather than dropped: a
            // member the set cannot represent must not silently stop being one, and a
            // bare host is a legitimate spelling for a peer whose port nobody recorded.
            //
            // `HostOfEndpoint`, not `SplitHostPort` + a fallback, which is where this
            // rule used to be written by hand and got the two IPv6 spellings wrong
            // both ways: an unbracketed `2001:db8::1` split at its LAST colon and
            // published the member as `2001:db8:`, and a bracketed `[2001:db8::1]`
            // with no port would not split at all and was published with its brackets
            // on -- neither of which any kernel ever reports as a peer address, so
            // both are a listed member that silently stops being one.
            hosts.emplace_back(HostOfEndpoint(endpoint));
        }

        // Built outside the lock and swapped in, so a reader never observes a set
        // that is half of the old one and half of the new -- which for a membership
        // set is a window in which a member is neither admitted nor refused but
        // both, depending on which surface asked.
        std::unique_lock const guard { _mutex };
        _hosts = std::move(hosts);
    }

    /// How many member hosts are currently admitted.
    [[nodiscard]] std::size_t Size() const
    {
        std::shared_lock const guard { _mutex };
        return _hosts.size();
    }

    /// @param peerAddress The connecting peer's **host**, as `ISocket::PeerAddress()`
    ///        reports it. Never an endpoint; see the class note.
    /// @return The verdict and, unless it is `Outsider`, this list as its author.
    [[nodiscard]] MembershipDecision Explain(std::string_view peerAddress) const override
    {
        // This machine is always a member of its own node's fleet, whatever the list
        // says, and that is a rule rather than a convenience. Anti-leeching exists to
        // stop OTHER machines spending capacity they do not contribute; a process on
        // this host already has this host's CPU, and the `fastcache-cc` a developer
        // runs against their own node is the entire reason the node is there. Without
        // it, a node whose operator listed only their peers would refuse their own
        // builds -- a fleet that looks configured and serves nobody locally, which is
        // exactly the shape of failure this file's other rules exist to prevent.
        //
        // It is also what makes "off by default" safe: an unconfigured node admits
        // its own machine and nothing else, so it is useful immediately and closed to
        // the network until somebody says otherwise.
        if (IsLoopbackHost(peerAddress))
            return Decided(_verdicts.onLoopback);

        // A shared lock: this is asked once per connection on three surfaces and
        // written only when the cluster agrees a change, so readers must not
        // serialize against each other over a list that is almost always identical
        // to what the last reader saw.
        std::shared_lock const guard { _mutex };

        // An empty list then refuses everybody else rather than admitting them. A node
        // that has not yet discovered a peer, or whose discovery is misconfigured,
        // must not silently become an open scheduler -- the failure would be
        // invisible from both ends and is exactly what `OpenMembership` exists to
        // make an explicit choice instead.
        //
        // Whole-string, never a prefix: `10.0.0.1` must not admit `10.0.0.10`.
        //
        // Through `SameHost`, which is the same fold `IsLoopbackHost` applied three
        // lines above and for the same reason: a node bound to `::` is dual-stack, so
        // an IPv4 member listed as `10.0.0.1` arrives as `::ffff:10.0.0.1` and a raw
        // compare refuses every peer in the set while the loopback branch above keeps
        // working -- a fleet that looks configured, admits its own machine, and
        // distributes nothing. It also refuses an unnameable caller against an empty
        // member entry, which a raw compare admitted.
        auto const admits = [peerAddress](std::string_view host) {
            return SameHost(peerAddress, host);
        };
        return Decided(std::ranges::any_of(_hosts, admits) ? _verdicts.onMatch : Membership::Outsider);
    }

  protected:
    /// A verdict with this list named as its author -- EXCEPT `Outsider`.
    ///
    /// `Outsider` is `PrecedenceOf` 0 and loses to every other participant: it is this oracle
    /// having no OPINION rather than an answer it produced. A forgotten list answers `Outsider`
    /// about loopback deliberately -- "silence about this machine" -- and naming the author of a
    /// silence would report a tombstone as the reason a host is refused when the tombstone said
    /// nothing about it. That is the confident wrong signal #1471 exists to remove, so the
    /// attribution is asked here once rather than at each return.
    /// @param verdict What this list answered.
    /// @return The verdict, attributed unless it is `Outsider`.
    [[nodiscard]] MembershipDecision Decided(Membership verdict) const noexcept
    {
        return DecidedBy(verdict, _participant);
    }

  private:
    /// What this set answers. Fixed at construction: the question a set answers is
    /// what the set IS, where its contents are what changes while it lives.
    HostSetVerdicts _verdicts;
    MembershipParticipant _participant;

    /// Guards `_hosts`. Mutable because `Classify` and `Size` are logically const
    /// and must still take it -- the alternative is a const method that reads a
    /// vector somebody else is replacing.
    mutable std::shared_mutex _mutex;
    std::vector<std::string> _hosts;
};

/// Only hosts on an explicit list are members.
///
/// The list is the cluster's *authenticated* peers, refreshed by whatever established
/// them -- `Cluster::DiscoveryService` in this tree, which admits a peer only after it
/// proves the pre-shared key over a nonce this node chose -- or, on the other route,
/// what `--fleet-member` named. One list per question and composed, never one list
/// answering both; see `AnyOfMembership` and #251.
class ClusterMembership final: public HostSetMembership
{
  public:
    /// @param participant Which route this list IS: `FleetMemberList` for what
    ///        `--fleet-member` named, `ClusterMembers` for the committed set. A node owns one
    ///        instance per question (`NodeMembership`'s `_listed` and `_cluster`), so this is a
    ///        property of the INSTANCE and required rather than defaulted -- defaulted, the two
    ///        would be indistinguishable in a report, which is the question #1471 answers.
    /// @param memberEndpoints `host:port` endpoints of the admitted hosts. Only the
    ///        host part is retained; see `HostSetMembership`.
    explicit ClusterMembership(MembershipParticipant participant, std::vector<std::string> const& memberEndpoints = {}):
        HostSetMembership { AdmissionVerdicts, participant, memberEndpoints }
    {
    }
};

/// Hosts the cluster has agreed to FORGET, and nothing else (#1309).
///
/// The participant that makes a forget reach a node whose own `--fleet-member` list
/// still names the machine. Removing a client used to be an edit on every other
/// machine in the fleet, which fails OPEN: miss one and it serves the retired host
/// indefinitely, with admission succeeding being the ordinary case and nothing to
/// report. A replicated tombstone plus `PrecedenceOf` closes that without asking
/// anybody to reconfigure anything -- the forget outranks the listing.
///
/// It admits nobody. Composed into `AnyOfMembership` it can only ever RAISE a verdict
/// from `Outsider` or `Member` to `Forgotten`, so adding it to a node's participants
/// cannot widen admission -- which is the direction #282's guard is about, and the
/// reason this needed no reload posture of its own.
class ForgottenMembership final: public HostSetMembership
{
  public:
    /// @param hosts The forgotten hosts, as `Cluster::ClusterState::forgotten` holds
    ///        them: bare hosts, though an endpoint would be reduced to one anyway.
    explicit ForgottenMembership(std::vector<std::string> const& hosts = {}):
        // One role, so unlike `ClusterMembership` this names it rather than taking it.
        HostSetMembership { ForgottenVerdicts, MembershipParticipant::ClientTombstone, hosts }
    {
    }
};

} // namespace FastCache::Distributed
