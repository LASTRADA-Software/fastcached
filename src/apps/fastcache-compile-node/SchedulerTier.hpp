// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "Responders.hpp"

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseSigner.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

#include <core/platform/Clock.hpp>

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
/// Why a scheduler with no identity key is refused. Unreachable from any configuration -- a
/// scheduler runs consensus, and a consensus node always holds a key -- so this is the answer
/// to a caller that did not resolve one, never a fallback (#178).
inline constexpr std::string_view SchedulerNeedsIdentityKeyRefusal =
    "a scheduler signs every lease with this node's identity key, and this node holds none: it runs consensus, so "
    "its state directory should have minted one -- check that --cluster-dir is writable";

class SchedulerTier
{
  public:
    /// Build the tier.
    ///
    /// **Binds nothing since #290.** The scheduler verbs are answered on the node's
    /// one `0xFC` listener, beside the cache's, so this is a component rather than a
    /// surface; `StartNodeSurfaceOrExplain` opens that listener.
    ///
    /// Signs every grant with this node's identity key (#178): a worker verifies it against
    /// the key its roster holds for this node, and refuses a signer the cluster revoked.
    /// @param cfg The parsed configuration.
    /// @param clock Time source for registry expiry and lease timeouts.
    /// @param wallClock Where a grant's absolute expiry comes from.
    /// @param metrics Where dispatch outcomes are counted.
    /// @param logger Where the tier reports what it is doing.
    /// @param identityKey This node's identity key pair, as its start resolved it; copied,
    ///        so the tier signs with its own copy for as long as it lives.
    /// @return The tier, or why it could not be built.
    [[nodiscard]] static std::expected<std::unique_ptr<SchedulerTier>, std::string> Start(
        NodeConfig const& cfg,
        Distributed::IMembershipOracle const& membership,
        core::platform::IClock& clock,
        core::platform::WallClockRef wallClock,
        IMetricsSink& metrics,
        ILogger& logger,
        std::optional<Ed25519KeyPair> const& identityKey);

    ~SchedulerTier() = default;

    SchedulerTier(SchedulerTier const&) = delete;
    SchedulerTier& operator=(SchedulerTier const&) = delete;
    SchedulerTier(SchedulerTier&&) = delete;
    SchedulerTier& operator=(SchedulerTier&&) = delete;

    /// Tell the scheduler what this node is, and who leads if it does not.
    ///
    /// The seam consensus drives, and the only one: every scheduler runs consensus.
    /// @param role What this node is now.
    /// @param leaderEndpoint Where the leader answers, empty when nobody leads.
    void SetRole(Distributed::SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t epoch)
    {
        _service.SetRole(role, leaderEndpoint, epoch);
    }

    /// Take this node's own endorsement of the roster it applied (#178).
    ///
    /// The door a node's OWN endorsement reaches its own scheduler through, so a lone
    /// scheduler certifies its roster without dialling itself; every other voter's arrives on
    /// NODE-ANNOUNCE, through the same `AcceptEndorsement`.
    ///
    /// Kept as well as offered, because it can arrive BEFORE `Administer`: the consensus tier's
    /// reconciler starts inside its own start, and the tier is handed to this one only after.
    /// An endorsement offered to a scheduler with no cluster is refused `NoState`, and dropping
    /// it would leave a lone scheduler with no certified roster until the next refresh -- a
    /// quarter of an hour of every grant refused. `Administer` offers it again.
    /// @param endorsement The endorsement.
    void Endorse(Cluster::RosterEndorsement const& endorsement);

    /// Give this surface a cluster to administer.
    ///
    /// The second seam consensus drives, and it is a setter for the same reason
    /// `SetRole` is: consensus is constructed after this surface, because it needs
    /// the port this one bound. Left uncalled, the cluster verbs answer
    /// `NoCluster`, which is what a node running no cluster should say.
    /// And the node's own endorsement, when one arrived before the cluster did, is offered again
    /// now: see `Endorse`.
    /// @param admin The cluster; must outlive this tier.
    void Administer(Distributed::IClusterAdmin& admin);

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
                  core::platform::IClock& clock,
                  core::platform::WallClockRef wallClock,
                  IMetricsSink& metrics,
                  ILogger& logger,
                  std::string signerId,
                  Ed25519KeyPair identityKey,
                  std::string_view clusterId,
                  std::shared_ptr<AuthPolicy const> policy);

    // Declaration order IS construction order, and each is referenced by the one
    // below it.

    /// This node's identity, as every grant names and signs it (#178). Declared before
    /// `_service`, which borrows it.
    Distributed::KeyPairLeaseSigner _signer;

    Distributed::SchedulerService _service;
    Distributed::SchedulerProtocol _protocol;

    /// The credential every surface on this node requires, or null. Declared before
    /// `_responder`, which is handed the same object.
    std::shared_ptr<AuthPolicy const> _policy;

    SchedulerResponder _responder;

    /// This node's latest own endorsement, and the lock that serialises it against
    /// `Administer`: the reconciler thread offers endorsements while `main` hands the service
    /// its cluster, and the service's cluster pointer is not otherwise guarded until the
    /// surfaces start serving.
    std::mutex _ownEndorsementMutex;
    std::optional<Cluster::RosterEndorsement> _ownEndorsement; ///< Guarded by `_ownEndorsementMutex`.
};

} // namespace FastCache::Node
