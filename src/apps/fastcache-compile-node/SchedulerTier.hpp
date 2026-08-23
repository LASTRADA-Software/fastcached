// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "NodeConfig.hpp"
#include "Responders.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <expected>
#include <memory>
#include <string>

namespace FastCache::Node
{

/// The node's scheduler surface: service, protocol, membership, responder and
/// listener, owned as one thing.
///
/// The mirror of `CacheTier`, and owned the same way for the same two reasons: the
/// collaborators form a reference chain whose declaration order is load-bearing and
/// silently so, and `WorkerBody` is a function with a cognitive-complexity budget
/// that two inline surfaces do not fit inside. Both being one object each is also
/// what makes them read as the pair they are.
class SchedulerTier
{
  public:
    /// Build the tier and start serving it.
    /// @param cfg The parsed configuration.
    /// @param clock Time source for registry expiry and lease timeouts.
    /// @param metrics Where dispatch outcomes are counted.
    /// @param logger Where to announce the bound address.
    /// @return The running tier, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<SchedulerTier>, std::string> Start(NodeConfig const& cfg,
                                                                                          IClock& clock,
                                                                                          IMetricsSink& metrics,
                                                                                          ILogger& logger);

    ~SchedulerTier() = default;

    SchedulerTier(SchedulerTier const&) = delete;
    SchedulerTier& operator=(SchedulerTier const&) = delete;
    SchedulerTier(SchedulerTier&&) = delete;
    SchedulerTier& operator=(SchedulerTier&&) = delete;

    /// The address the scheduler surface bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _endpoint->BoundEndpoint();
    }

  private:
    SchedulerTier(NodeConfig const& cfg, IClock& clock, IMetricsSink& metrics);

    // Declaration order IS construction order, and each is referenced by the one
    // below it.
    Distributed::SchedulerService _service;
    Distributed::SchedulerProtocol _protocol;
    Distributed::OpenMembership _open;
    Distributed::ClusterMembership _listed;
    SchedulerResponder _responder;
    std::unique_ptr<FrameEndpoint> _endpoint;
};

} // namespace FastCache::Node
