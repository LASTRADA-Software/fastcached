// SPDX-License-Identifier: Apache-2.0
#include "SchedulerTier.hpp"

#include <format>
#include <utility>

namespace FastCache::Node
{

SchedulerTier::SchedulerTier(Distributed::IMembershipOracle const& membership, IClock& clock, IMetricsSink& metrics):
    _service { clock, metrics },
    _protocol { _service },
    // The oracle is the NODE's, not this tier's: the cache surface consults the same
    // object, and a node that answered "is this peer one of ours" differently at its
    // two surfaces would admit a peer to the fleet and refuse it the objects that
    // fleet produced. It also outlives this tier, which is what lets a node serve a
    // cache with no scheduler at all.
    _responder { _protocol, membership }
{
    // Static leadership, and it is a placeholder said out loud rather than a silent
    // one. Consensus is what will publish this -- `SetRole` exists for exactly that --
    // but until the node runs a `RaftDriver`, a scheduler that never became leader
    // would refuse every verb with `NotLeader` and be indistinguishable from a
    // permanent election. That is strictly worse than what `--listen-dispatch` did,
    // and a replacement must never be a regression on what it replaces.
    _service.SetRole(Distributed::SchedulerRole::Leader, {});
}

std::expected<std::unique_ptr<SchedulerTier>, std::string> SchedulerTier::Start(
    NodeConfig const& cfg,
    Distributed::IMembershipOracle const& membership,
    IClock& clock,
    IMetricsSink& metrics,
    ILogger& logger)
{
    auto tier = std::unique_ptr<SchedulerTier> { new SchedulerTier { membership, clock, metrics } };

    // A bare port binds the WILDCARD here, the opposite of the cache surface's
    // loopback: a scheduler no peer can dial is a scheduler that does nothing, so
    // loopback would be a default that silently cannot work.
    auto started = FrameEndpoint::Start(cfg.schedulerListen, "0.0.0.0", tier->_responder, "scheduler", logger);
    if (!started.has_value())
        return std::unexpected { started.error() };

    tier->_endpoint = std::move(*started);
    logger.Logf(LogLevel::Info,
                "scheduling for the fleet on {} ({})",
                tier->BoundEndpoint(),
                // Off the configuration rather than off the oracle: the count is a
                // property of what the operator wrote, and the oracle is now shared
                // with the cache surface and no longer this tier's to inspect. It
                // also says "plus this machine" out loud, because that admission is
                // unconditional and an operator reading "2 member host(s)" would
                // otherwise not know their own builds were covered.
                cfg.fleetOpen ? std::string { "every caller admitted" }
                              : std::format("this machine plus {} member host(s)", cfg.fleetMembers.size()));
    return tier;
}

} // namespace FastCache::Node
