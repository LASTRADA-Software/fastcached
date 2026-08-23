// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
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

/// Only hosts on an explicit list are members.
///
/// The list is the cluster's *authenticated* peers, refreshed by whatever
/// established them -- `Cluster::DiscoveryService` in this tree, which admits a peer
/// only after it proves the pre-shared key over a nonce this node chose. Holding a
/// copy rather than a reference to that directory is what keeps this class free of
/// the discovery module, and the copy is small: a fleet is tens of machines, and it
/// changes when membership does rather than per request.
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
class ClusterMembership final: public IMembershipOracle
{
  public:
    /// @param memberEndpoints `host:port` endpoints of the authenticated peers. Only
    ///        the host part is retained; see the class note.
    explicit ClusterMembership(std::vector<std::string> const& memberEndpoints = {})
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
            auto const split = SplitHostPort(endpoint);
            hosts.push_back(split.has_value() ? split->first : endpoint);
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
            return Membership::Member;

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
        return std::ranges::find(_hosts, peerAddress) != _hosts.end() ? Membership::Member : Membership::Outsider;
    }

  private:
    /// Guards `_hosts`. Mutable because `Classify` and `Size` are logically const
    /// and must still take it -- the alternative is a const method that reads a
    /// vector somebody else is replacing.
    mutable std::shared_mutex _mutex;
    std::vector<std::string> _hosts;
};

} // namespace FastCache::Distributed
