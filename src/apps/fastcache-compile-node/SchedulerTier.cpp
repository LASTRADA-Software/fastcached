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
    // loopback, and why is on `SchedulerListenDefaultHost`. Named rather than spelled
    // here so that whatever else comes to judge this value is judging the address
    // this tier will actually take.
    auto started =
        FrameEndpoint::Start(io, cfg.schedulerListen, SchedulerListenDefaultHost, tier->_responder, "scheduler", logger);
    if (!started.has_value())
        return std::unexpected { started.error() };

    tier->_endpoint = std::move(*started);
    // The phrase is `AdmissionSummary`'s rather than this tier's, because the policy
    // is the NODE's: the worker's own ready line reports the identical fact, and two
    // surfaces spelling one policy differently is how an operator comes to believe
    // their compile port is configured because their scheduler said so (#235).
    logger.Logf(LogLevel::Info, "scheduling for the fleet on {} ({})", tier->BoundEndpoint(), AdmissionSummary(cfg));
    return tier;
}

} // namespace FastCache::Node
