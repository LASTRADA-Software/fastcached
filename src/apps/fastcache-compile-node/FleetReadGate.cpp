// SPDX-License-Identifier: Apache-2.0
#include "FleetReadGate.hpp"

#include <FastCache/Core/HostPort.hpp>

namespace FastCache::Node
{

FleetReadVerdict DecideFleetRead(std::optional<LiveLeadership> const& leadership,
                                 AdminCredential const& dashboard,
                                 std::string_view token,
                                 std::string_view peer)
{
    if (!leadership.has_value())
        return { .decision = FleetReadDecision::NoScheduler,
                 .detail = "this node runs no scheduler, so it has no fleet; ask the fleet's scheduler" };

    auto const admitted = dashboard.Required() ? dashboard.Matches(token) : IsLoopbackHost(peer);
    if (!admitted)
        return { .decision = FleetReadDecision::Unauthenticated,
                 .detail = dashboard.Required()
                               ? "the fleet is served to a caller presenting the dashboard credential"
                               : "with no --dashboard-token-file the fleet is served to this machine only" };

    if (!leadership->leads)
        return { .decision = FleetReadDecision::NotLeader, .detail = leadership->leaderEndpoint };
    return { .decision = FleetReadDecision::Admitted, .detail = {} };
}

} // namespace FastCache::Node
