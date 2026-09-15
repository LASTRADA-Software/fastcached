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

    /// Classify one caller.
    /// @param peerAddress The address the connection came from, as text.
    /// @return Whether that peer may be scheduled onto the fleet.
    [[nodiscard]] virtual Membership Classify(std::string_view peerAddress) const = 0;
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
    [[nodiscard]] Membership Classify(std::string_view /*peerAddress*/) const override
    {
        return Membership::Member;
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
    [[nodiscard]] Membership Classify(std::string_view peerAddress) const override
    {
        auto verdict = Membership::Outsider;
        for (auto const* participant: _participants)
        {
            auto const answer = participant->Classify(peerAddress);
            if (PrecedenceOf(answer) > PrecedenceOf(verdict))
                verdict = answer;
        }
        return verdict;
    }

  private:
    std::vector<IMembershipOracle const*> _participants;
};

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
    explicit HostSetMembership(HostSetVerdicts verdicts, std::vector<std::string> const& memberEndpoints = {}):
        _verdicts { verdicts }
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
    /// @return Whether that peer may be scheduled onto the fleet.
    [[nodiscard]] Membership Classify(std::string_view peerAddress) const override
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
            return _verdicts.onLoopback;

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
        return std::ranges::any_of(_hosts, admits) ? _verdicts.onMatch : Membership::Outsider;
    }

  private:
    /// What this set answers. Fixed at construction: the question a set answers is
    /// what the set IS, where its contents are what changes while it lives.
    HostSetVerdicts _verdicts;

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
    /// @param memberEndpoints `host:port` endpoints of the admitted hosts. Only the
    ///        host part is retained; see `HostSetMembership`.
    explicit ClusterMembership(std::vector<std::string> const& memberEndpoints = {}):
        HostSetMembership { AdmissionVerdicts, memberEndpoints }
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
        HostSetMembership { ForgottenVerdicts, hosts }
    {
    }
};

} // namespace FastCache::Distributed
