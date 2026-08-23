// SPDX-License-Identifier: Apache-2.0
#include "SchedulerTier.hpp"

#include <format>
#include <utility>

namespace FastCache::Node
{

SchedulerTier::SchedulerTier(NodeConfig const& cfg, IClock& clock, IMetricsSink& metrics):
    _service { clock, metrics },
    _protocol { _service },
    _listed { cfg.fleetMembers },
    // Which oracle is the operator's stated choice, never a fall-back:
    // `SchedulerPolicyRejection` has already refused the case where neither
    // `--fleet-open` nor `--fleet-member` was given, so nothing here guesses.
    _responder { _protocol,
                 cfg.fleetOpen ? static_cast<Distributed::IMembershipOracle const&>(_open)
                               : static_cast<Distributed::IMembershipOracle const&>(_listed) }
{
    // Static leadership, and it is a placeholder said out loud rather than a silent
    // one. Consensus is what will publish this -- `SetRole` exists for exactly that --
    // but until the node runs a `RaftDriver`, a scheduler that never became leader
    // would refuse every verb with `NotLeader` and be indistinguishable from a
    // permanent election. That is strictly worse than what `--listen-dispatch` did,
    // and a replacement must never be a regression on what it replaces.
    _service.SetRole(Distributed::SchedulerRole::Leader, {});
}

std::expected<std::unique_ptr<SchedulerTier>, std::string> SchedulerTier::Start(NodeConfig const& cfg,
                                                                                IClock& clock,
                                                                                IMetricsSink& metrics,
                                                                                ILogger& logger)
{
    auto tier = std::unique_ptr<SchedulerTier> { new SchedulerTier { cfg, clock, metrics } };

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
                cfg.fleetOpen ? std::string { "every caller admitted" }
                              : std::format("{} member host(s)", tier->_listed.Size()));
    return tier;
}

} // namespace FastCache::Node
