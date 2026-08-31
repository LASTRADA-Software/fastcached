// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// This node's one answer to "who is this caller to us".
///
/// Owns every oracle and hands out whichever the operator chose, so that **every**
/// surface asks the same object. That is the whole point of the type existing rather
/// than each tier building its own: the scheduler decides who may spend the fleet's
/// CPU and the cache decides who may read this machine's objects, and a node that
/// answered those two questions differently for one peer would admit it to the fleet
/// and refuse it the objects that fleet produced — or, worse, the reverse.
///
/// It also outlives both tiers by construction, which matters because a node may run
/// a cache surface with no scheduler at all: the oracle used to live inside
/// `SchedulerTier`, which made the cache's access policy depend on whether this node
/// happened to be scheduling.
///
/// ## Two lists, because there are two questions
///
/// `--fleet-member` says who may spend this node's CPU and read its cache tier, and
/// that includes machines which are not cluster peers and never will be: a
/// developer's laptop, a CI runner, anything running `fastcache-cc` against the
/// fleet. The cluster's agreed member set says who is in the cluster. Answering both
/// with one list meant the first replicated membership commit discarded everything an
/// operator had listed, and agreeing something is routine -- a node joining, a node
/// being forgotten, a settings change (#251).
///
/// So this owns one `ClusterMembership` per question and composes them, rather than
/// letting either publisher speak for the other. Each is still replaced wholesale by
/// whoever owns it, which is right: a publisher holds the whole truth about *its*
/// question.

/// Whether this configuration admits a machine that is not this one.
///
/// The same three routes `NodeMembership`'s constructor composes, asked of the
/// configuration instead of a caller, and it lives HERE for that reason: when the
/// fourth route lands -- the rulebook says it will, and that it will be a credential
/// rather than a host list -- it is added a few lines below, and a reader adding it
/// has this function in front of them. Left in `NodeConfig.cpp` it was a second
/// reader of the same policy with nothing pointing at it, and a new route would
/// silently stop the startup rule firing: an open compile port, every refusal counter
/// at zero, and a fleet green from both ends.
///
/// It cannot be answered by asking the oracle, which is why it is a separate function
/// rather than a method. `Oracle()` answers "is this caller admitted" in the present
/// tense; this asks whether the admitted set will EVER contain another machine, and
/// `_cluster` is empty at construction by design. The `raftJoin` / `raftPeers` half
/// predicts what consensus will later `Publish()` into this object, which no runtime
/// query can see.
///
/// Only the host is looked at, matching what the oracle itself compares: a peer
/// arrives from an ephemeral source port, so a port was never something a connection
/// could be matched on.
/// @param cfg The parsed configuration.
/// @return Whether the policy admits anything but this machine.
[[nodiscard]] inline bool AdmitsRemotePeers(NodeConfig const& cfg)
{
    auto const remote = [](std::string_view endpoint) {
        return !IsLoopbackHost(HostOfEndpoint(endpoint));
    };

    // `--fleet-open` first: it admits every machine there is, and says so as a flag
    // rather than as an empty list, so nothing here has to infer it.
    if (cfg.fleetOpen)
        return true;
    if (std::ranges::any_of(cfg.fleetMembers, remote))
        return true;

    // A node waiting to be admitted has an EMPTY member set by construction and is
    // about to be handed one -- so the absence of peers here is the strongest signal
    // that remote ones are coming, not the weakest.
    if (cfg.raftJoin)
        return true;
    return std::ranges::any_of(cfg.raftPeers,
                               [&](Cluster::ClusterMember const& member) { return remote(member.raftEndpoint); });
}

class NodeMembership
{
  public:
    /// @param cfg The parsed configuration.
    explicit NodeMembership(NodeConfig const& cfg):
        _open {},
        _listed { cfg.fleetMembers },
        _cluster {},
        // Pointers into this object's own members, which is safe because the type is
        // neither copyable nor movable and the composite is declared after both.
        _admitted { { &_listed, &_cluster } },
        _isOpen { cfg.fleetOpen }
    {
    }

    NodeMembership(NodeMembership const&) = delete;
    NodeMembership& operator=(NodeMembership const&) = delete;
    NodeMembership(NodeMembership&&) = delete;
    NodeMembership& operator=(NodeMembership&&) = delete;
    ~NodeMembership() = default;

    /// Record what the cluster agreed, alongside what the operator listed.
    ///
    /// The seam consensus drives, and it does nothing under `--fleet-open` -- which
    /// is right rather than an oversight: that flag says "admit everybody", and a
    /// replicated member set narrows nothing an operator has already opened.
    ///
    /// It writes the cluster's list and only that, so `--fleet-member` survives every
    /// commit. Which list is written is decided here rather than passed in, so the
    /// observer consensus installs cannot name the wrong one. A node admitted at
    /// runtime is still served without anybody editing a config file on every other
    /// machine, which is what this seam was for; it simply no longer costs the
    /// operator's own answer to get that.
    ///
    /// Safe to call from the consensus thread while surfaces classify callers on
    /// theirs.
    /// @param endpoints The cluster's members, as `host:port`.
    void Publish(std::vector<std::string> const& endpoints)
    {
        _cluster.Publish(endpoints);
    }

    /// The oracle every surface on this node consults.
    ///
    /// The open one is only ever the operator's stated choice: `--fleet-open` is a
    /// flag rather than what an unset field decays to, so nothing here guesses its
    /// way into serving strangers. Everything else is the union of the two lists,
    /// which for a node that named no members and has agreed nothing admits this
    /// machine and refuses the network -- the safe default rather than a
    /// misconfiguration.
    ///
    /// That default is exactly what a WORKER must be able to leave behind. It could
    /// not until #235: the startup table refused `--fleet-member` on any node
    /// without a scheduler, so a pure worker's oracle was an empty list by
    /// construction and its compile port refused every dispatched job.
    /// @return The oracle; valid for this object's lifetime.
    [[nodiscard]] Distributed::IMembershipOracle const& Oracle() const noexcept
    {
        return _isOpen ? static_cast<Distributed::IMembershipOracle const&>(_open)
                       : static_cast<Distributed::IMembershipOracle const&>(_admitted);
    }

  private:
    Distributed::OpenMembership _open;

    /// What `--fleet-member` named. Fixed for this process's life, and the only
    /// route by which a machine that is not a cluster peer is admitted at all.
    Distributed::ClusterMembership _listed;

    /// What the cluster agreed, replaced on every committed membership change.
    Distributed::ClusterMembership _cluster;

    /// The union the surfaces actually consult. Declared after both participants,
    /// which it borrows.
    Distributed::AnyOfMembership _admitted;

    /// Whether `--fleet-open` was given.
    ///
    /// A `bool` member rather than a stored reference, deliberately: a reference
    /// member would delete this type's assignment operators for a choice that is
    /// fixed at construction anyway, and the branch is one predictable test on a
    /// path that already crosses a network.
    bool _isOpen;
};

} // namespace FastCache::Node
