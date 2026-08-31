// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "NodeConfig.hpp"
#include "Responders.hpp"

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace FastCache::Node
{

class NodeIoLoop;

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
    /// Build the tier.
    ///
    /// **Binds nothing since #290.** The scheduler verbs are answered on the node's
    /// one `0xFC` listener, beside the cache's, so this is a component rather than a
    /// surface; `StartNodeSurfaceOrExplain` opens that listener.
    ///
    /// Reads `--cluster-key-file` when one is named, because that key is what a
    /// lease grant is signed with. An unreadable one is fatal here rather than a
    /// warning: an operator who named a key file and got a scheduler handing out
    /// unsigned grants has a fleet that looks configured and is not.
    /// @param cfg The parsed configuration.
    /// @param clock Time source for registry expiry and lease timeouts.
    /// @param wallClock Where a grant's absolute expiry comes from.
    /// @param metrics Where dispatch outcomes are counted.
    /// @param logger Where the tier reports what it is doing.
    /// @return The tier, or why it could not be built.
    [[nodiscard]] static std::expected<std::unique_ptr<SchedulerTier>, std::string> Start(
        NodeConfig const& cfg,
        Distributed::IMembershipOracle const& membership,
        IClock& clock,
        IWallClock const& wallClock,
        IMetricsSink& metrics,
        ILogger& logger);

    ~SchedulerTier() = default;

    SchedulerTier(SchedulerTier const&) = delete;
    SchedulerTier& operator=(SchedulerTier const&) = delete;
    SchedulerTier(SchedulerTier&&) = delete;
    SchedulerTier& operator=(SchedulerTier&&) = delete;

    /// Tell the scheduler what this node is, and who leads if it does not.
    ///
    /// The seam consensus drives. Without a `--node-id` nobody ever calls it and the
    /// constructor's standalone leadership stands, which is what one machine wants.
    /// @param role What this node is now.
    /// @param leaderEndpoint Where the leader answers, empty when nobody leads.
    void SetRole(Distributed::SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t epoch)
    {
        _service.SetRole(role, leaderEndpoint, epoch);
    }

    /// Give this surface a cluster to administer.
    ///
    /// The second seam consensus drives, and it is a setter for the same reason
    /// `SetRole` is: consensus is constructed after this surface, because it needs
    /// the port this one bound. Left uncalled, the cluster verbs answer
    /// `NoCluster`, which is what a node running no cluster should say.
    /// @param admin The cluster; must outlive this tier.
    void Administer(Distributed::IClusterAdmin& admin) noexcept
    {
        _service.AdministerWith(admin);
    }

    /// The scheduler itself, for reporting.
    ///
    /// `const`, so a report can read the fleet and cannot change it -- the whole
    /// mutable surface stays behind the verbs the protocol drives.
    /// @return The service this tier owns.
    [[nodiscard]] Distributed::SchedulerService const& Service() const noexcept
    {
        return _service;
    }

    /// Route handed-over history somewhere, or nowhere.
    ///
    /// A WIRING door rather than a verb, like `SetRole` and `Administer` above it:
    /// the sink lives with the sampler, which is built after this tier that it
    /// receives for. A forwarder rather than a mutable `Service()`, so the rule
    /// stated there -- a report can read the fleet and cannot change it -- still
    /// holds for every other caller.
    /// @param sink Where to route it; must outlive this tier.
    void SetHistorySink(Distributed::IFleetHistorySink* sink) noexcept
    {
        _service.SetHistorySink(sink);
    }

    /// What answers the scheduler verbs on this node's `0xFC` listener.
    ///
    /// Handed to `MergedResponder`, which routes each frame to the component owning
    /// its verb family. It also owns the CREDENTIAL, so `AUTH` is routed here too --
    /// the cache requires none, and a credential every local build can read is not a
    /// credential (#290).
    /// @return This tier's responder; outlives no longer than the tier.
    [[nodiscard]] IFrameResponder& Responder() noexcept
    {
        return _responder;
    }

  private:
    SchedulerTier(Distributed::IMembershipOracle const& membership,
                  IClock& clock,
                  IWallClock const& wallClock,
                  IMetricsSink& metrics,
                  ILogger& logger,
                  std::span<std::byte const> signingKey,
                  std::string_view clusterId,
                  std::shared_ptr<AuthPolicy const> policy);

    // Declaration order IS construction order, and each is referenced by the one
    // below it.
    Distributed::SchedulerService _service;
    Distributed::SchedulerProtocol _protocol;
    SchedulerResponder _responder;
};

} // namespace FastCache::Node
