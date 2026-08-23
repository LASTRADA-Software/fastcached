// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Distributed/MembershipOracle.hpp>

namespace FastCache::Node
{

/// This node's one answer to "who is this caller to us".
///
/// Owns both oracles and hands out whichever the operator chose, so that **every**
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
class NodeMembership
{
  public:
    /// @param cfg The parsed configuration.
    explicit NodeMembership(NodeConfig const& cfg):
        _open {},
        _listed { cfg.fleetMembers },
        _isOpen { cfg.fleetOpen }
    {
    }

    NodeMembership(NodeMembership const&) = delete;
    NodeMembership& operator=(NodeMembership const&) = delete;
    NodeMembership(NodeMembership&&) = delete;
    NodeMembership& operator=(NodeMembership&&) = delete;
    ~NodeMembership() = default;

    /// The oracle every surface on this node consults.
    ///
    /// Which one it is was the operator's stated choice and never a fall-back:
    /// `SchedulerPolicyRejection` has already refused the case where neither
    /// `--fleet-open` nor `--fleet-member` was given, so nothing here guesses.
    /// @return The oracle; valid for this object's lifetime.
    [[nodiscard]] Distributed::IMembershipOracle const& Oracle() const noexcept
    {
        return _isOpen ? static_cast<Distributed::IMembershipOracle const&>(_open)
                       : static_cast<Distributed::IMembershipOracle const&>(_listed);
    }

  private:
    Distributed::OpenMembership _open;
    Distributed::ClusterMembership _listed;

    /// Whether `--fleet-open` was given.
    ///
    /// A `bool` member rather than a stored reference, deliberately: a reference
    /// member would delete this type's assignment operators for a choice that is
    /// fixed at construction anyway, and the branch is one predictable test on a
    /// path that already crosses a network.
    bool _isOpen;
};

} // namespace FastCache::Node
