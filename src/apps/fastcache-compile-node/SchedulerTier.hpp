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
#include <cstdint>
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
/// Whether this node is the only scheduler there will ever be.
///
/// **An enum rather than a `bool`, because the two answers are opposite claims about
/// the FUTURE rather than a setting**: `Yes` says nothing will ever publish a role, so
/// standalone leadership at term 0 is the final answer; `No` says a consensus driver
/// will publish one and this tier must not pretend to a leadership it has not been
/// given (#613). A bare `true` at the call site reads as neither.
enum class LeadsAlone : std::uint8_t
{
    /// A consensus driver will report a role and a real term; wait for it.
    No,
    /// Nothing will ever call `SetRole`, so lead now and at term 0.
    Yes,
};

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
        WallClockRef wallClock,
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

    /// The scheduler a SECOND surface answers verbs against.
    ///
    /// **Non-const, and that is not a relaxation of `Service()` above -- it is a
    /// different question with a different audience.** That one is for REPORTING, and
    /// its constness is the whole statement: a report reads the fleet and cannot change
    /// it. This is for a surface, which by definition changes things, and it is exactly
    /// what `_responder` already holds one layer down through `_protocol`.
    ///
    /// Its one consumer is `EnrollmentResponder`, whose approval goes through
    /// `SchedulerService::ClusterAdmit` -- the same entry point `--cluster-admit`
    /// reaches. That reuse is the point: one leadership-and-membership gate, one
    /// `Cluster::Validate`, and one mapping from a consensus refusal onto a wire code,
    /// so what an operator is told does not depend on which door they came through.
    /// @return The service this tier owns.
    [[nodiscard]] Distributed::SchedulerService& ServiceForSurfaces() noexcept
    {
        return _service;
    }

    /// The credential this node's surfaces require, or null when none is configured.
    ///
    /// Handed out so a second surface requires the SAME one. `AUTH` is a `Session` verb
    /// and the merged listener routes it here, so a surface holding a policy of its own
    /// would gate against a credential nothing on this node ever accepts -- which is a
    /// port that looks guarded and refuses everybody.
    /// @return The policy, shared; null means membership is the only gate.
    [[nodiscard]] std::shared_ptr<AuthPolicy const> Policy() const noexcept
    {
        return _policy;
    }

  private:
    SchedulerTier(Distributed::IMembershipOracle const& membership,
                  IClock& clock,
                  WallClockRef wallClock,
                  IMetricsSink& metrics,
                  ILogger& logger,
                  std::span<std::byte const> signingKey,
                  std::string_view clusterId,
                  std::shared_ptr<AuthPolicy const> policy,
                  LeadsAlone leadsAlone);

    // Declaration order IS construction order, and each is referenced by the one
    // below it.
    Distributed::SchedulerService _service;
    Distributed::SchedulerProtocol _protocol;

    /// The credential every surface on this node requires, or null. Declared before
    /// `_responder`, which is handed the same object.
    std::shared_ptr<AuthPolicy const> _policy;

    SchedulerResponder _responder;
};

} // namespace FastCache::Node
