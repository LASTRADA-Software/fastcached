// SPDX-License-Identifier: Apache-2.0
#include "NodeIoLoop.hpp"
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
    // Standalone leadership, which is now a DEFAULT rather than a placeholder. A node
    // with no `--node-id` runs no consensus and is the only scheduler there is: it
    // hands out its own machine's slots and nobody else's, which is exactly right for
    // the one-machine deployment and is what most people run.
    //
    // With consensus configured, `ConsensusTier` calls `SetRole` the moment the
    // driver decides anything, and this initial value is superseded before the first
    // lease. It is deliberately not `Undecided`: a node that refused every verb until
    // an election completed would be strictly worse than what it replaces at exactly
    // the moment somebody is watching it start.
    _service.SetRole(Distributed::SchedulerRole::Leader, {});
}

std::expected<std::unique_ptr<SchedulerTier>, std::string> SchedulerTier::Start(
    NodeIoLoop& io,
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
    auto started = FrameEndpoint::Start(io, cfg.schedulerListen, "0.0.0.0", tier->_responder, "scheduler", logger);
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
