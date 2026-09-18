// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftConfig.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>

#include <format>

namespace FastCache::Consensus
{

std::expected<void, ConsensusError> RaftConfig::Validate() const
{
    if (self.empty())
        return std::unexpected { InvalidConfiguration("this node has no identity: `self` is empty") };

    // An empty configuration is accepted, and it is not "no configuration was
    // supplied": it is a node waiting to be admitted to a cluster it has not
    // joined yet. The two rules below are about the shape of a configuration, so
    // they have nothing to say about one that does not exist. What still applies is
    // everything else -- `self` above, because a node with no identity cannot be
    // admitted either, and the timings below, because a node with no cluster still
    // arms an election timer it declines to act on.
    auto const bootstrap = Bootstrap();
    if (!Membership::IsEmpty(bootstrap))
    {
        // In EITHER set: a node may be bootstrapped as a learner, which is what a
        // usually-absent machine is on its own command line (#1449).
        if (!Membership::IsMember(bootstrap, self))
            return std::unexpected { InvalidConfiguration(
                std::format("`self` ({}) is not among the cluster members", self)) };

        // What makes a configuration usable is one rule, and `Membership::Validate`
        // is where it lives -- the same one `ProposeMembership` applies to a set
        // arriving through the log. A second copy here was already not equivalent:
        // it refused a duplicate and accepted an empty id, so a bootstrap list
        // containing `""` started a node that would have refused the identical set
        // had a peer proposed it.
        if (auto valid = Membership::Validate(bootstrap); !valid.has_value())
            return std::unexpected { valid.error() };
    }

    if (electionTimeoutMin <= std::chrono::milliseconds::zero())
        return std::unexpected { InvalidConfiguration("the election timeout minimum must be positive") };

    if (electionTimeoutMax < electionTimeoutMin)
        return std::unexpected { InvalidConfiguration(
            std::format("the election timeout range is inverted: minimum {}ms exceeds maximum {}ms",
                        electionTimeoutMin.count(),
                        electionTimeoutMax.count())) };

    if (heartbeatInterval <= std::chrono::milliseconds::zero())
        return std::unexpected { InvalidConfiguration("the heartbeat interval must be positive") };

    // Raft needs broadcastTime << electionTimeout. Equality is already too slow:
    // a heartbeat arriving exactly as followers time out deposes a healthy leader
    // roughly half the time, and the cluster spends its life electing.
    if (heartbeatInterval >= electionTimeoutMin)
        return std::unexpected { InvalidConfiguration(
            std::format("the heartbeat interval ({}ms) must be below the election timeout minimum ({}ms)",
                        heartbeatInterval.count(),
                        electionTimeoutMin.count())) };

    return {};
}

Configuration RaftConfig::Bootstrap() const
{
    return Configuration { .voters = voters, .learners = learners };
}

} // namespace FastCache::Consensus
