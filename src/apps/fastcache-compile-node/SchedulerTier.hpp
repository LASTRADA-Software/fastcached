// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeRefusal.hpp"
#include "Responders.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseSigner.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

class NodeIoLoop;

/// How often a scheduler re-asks its fleet-wide conditions, beside the moments that move them.
///
/// **An interval as well as the events, because both rows clear by TIME**: a toolchain nobody has
/// asked for inside `UnservedToolchains::Window`, a machine whose presence has expired. Answered only
/// when a verb arrived, a leader nobody talks to would go on reporting a fleet that has changed. Five
/// seconds against a window of minutes and a presence timeout of ninety seconds: a row lags its cause
/// by at most this, and a pass costs one registry walk.
inline constexpr std::chrono::milliseconds SchedulerConditionInterval { 5000 };

/// What the fleet-wide conditions are read from.
struct SchedulerConditionInputs
{
    Distributed::SchedulerService const& service; ///< The fleet as this scheduler sees it.
    std::string_view ownVersion;                  ///< This build, as `VersionString` spells it.
    std::string_view ownEndpoint;                 ///< Where this node answers; its name in the version spread.
    /// How long this scheduler has led without a break -- one term, never interrupted; none when it
    /// does not lead.
    std::optional<core::platform::SteadyDuration> leadingFor;
};

/// Answer every `ConditionScope::Scheduler` row.
///
/// **A fleet-wide row is the LEADER's.** On the leader each is raised or cleared; on any other
/// scheduler each is `not-evaluated`, naming the leader when one is known -- never `clear`, which from
/// a node that cannot see the fleet would be a confident wrong signal.
///
/// **And a leader may say `clear` only once it has WATCHED what the row is about.** What each row
/// reads -- the leases refused no-worker, the machines that announced -- reaches the leader alone, and
/// nothing another leader saw carries over a failover. So a leader that has led for less than the
/// row's observation span reports `not-evaluated` rather than `clear` (`undecided` must not read as
/// `clear`), with a detail saying WHEN the row decides ("decides in 14 min 30 s"), while anything it
/// does see is raised at once: a refusal observed is a fact whatever came before it.
/// @param conditions Where the answers go.
/// @param inputs What they are read from.
void EvaluateSchedulerConditions(NodeConditions& conditions, SchedulerConditionInputs const& inputs);

/// The longest span a leader must have led before every fleet-wide row may read `clear`.
/// @return The longest row's observation span.
[[nodiscard]] core::platform::SteadyDuration LongestFleetObservation() noexcept;

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
    /// @param conditions The node's condition registry; this tier answers its fleet-wide rows into it
    ///        before `Start` returns, so `Settle` finds none undecided. Must outlive the tier.
    /// @param conditionInterval How often the watch re-asks them: `SchedulerConditionInterval` in
    ///        production, a long interval in a case that drives every evaluation itself.
    /// @return The tier, or why it could not be built.
    [[nodiscard]] static std::expected<std::unique_ptr<SchedulerTier>, NodeRefusal> Start(
        NodeConfig const& cfg,
        Distributed::IMembershipOracle const& membership,
        core::platform::IClock& clock,
        core::platform::WallClockRef wallClock,
        IMetricsSink& metrics,
        ILogger& logger,
        std::optional<Ed25519KeyPair> const& identityKey,
        NodeConditions& conditions,
        std::chrono::milliseconds conditionInterval);

    ~SchedulerTier() = default;

    SchedulerTier(SchedulerTier const&) = delete;
    SchedulerTier& operator=(SchedulerTier const&) = delete;
    SchedulerTier(SchedulerTier&&) = delete;
    SchedulerTier& operator=(SchedulerTier&&) = delete;

    /// Tell the scheduler what this node is, and who leads if it does not.
    ///
    /// The seam consensus drives, and the only one: every scheduler runs consensus.
    /// Re-asks the fleet-wide conditions at once: a new leader starts vouching for them, a demoted
    /// one stops.
    /// @param role What this node is now.
    /// @param leaderEndpoint Where the leader answers, empty when nobody leads.
    void SetRole(Distributed::SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t epoch);

    /// Give this surface a cluster to administer.
    ///
    /// The second seam consensus drives, and it is a setter for the same reason
    /// `SetRole` is: consensus is constructed after this surface, because it needs
    /// the port this one bound. Left uncalled, the cluster verbs answer
    /// `NoCluster`, which is what a node running no cluster should say.
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

    /// Answer this scheduler's fleet-wide condition rows now. Thread-safe: the consensus thread (via
    /// `SetRole`), the watch and a case may all call it, and every evaluation is ordered against every
    /// role change (`_roleMutex`).
    void EvaluateConditions();

  private:
    SchedulerTier(Distributed::IMembershipOracle const& membership,
                  core::platform::IClock& clock,
                  core::platform::WallClockRef wallClock,
                  IMetricsSink& metrics,
                  ILogger& logger,
                  std::string signerId,
                  Ed25519KeyPair identityKey,
                  std::string_view clusterId,
                  NodeConditions& conditions,
                  std::string ownEndpoint);

    /// Start the thread that re-asks the fleet-wide rows every @p interval. Called by `Start` once the
    /// tier is fully built, never from the constructor.
    void WatchConditions(std::chrono::milliseconds interval);

    /// `EvaluateConditions` with `_roleMutex` already held.
    void EvaluateConditionsLocked();

    // Declaration order IS construction order, and each is referenced by the one
    // below it.

    /// This node's identity, as every grant names and signs it (#178). Declared before
    /// `_service`, which borrows it.
    Distributed::KeyPairLeaseSigner _signer;

    Distributed::SchedulerService _service;
    Distributed::SchedulerProtocol _protocol;

    SchedulerResponder _responder;

    /// Where the fleet-wide rows are answered. Borrowed; outlives this tier.
    NodeConditions& _conditions;

    /// What `_leadingSince` is read from: the clock the service expires its registry by. Borrowed.
    core::platform::IClock& _clock;

    /// **A role change and every evaluation are ONE ordered decision.** Without it the watch could
    /// read `Leader`, the consensus thread demote this node and answer `not-evaluated`, and the watch
    /// then write its stale raise or clear over that for up to an interval. Held across the role
    /// change AND the evaluation that follows it, and across every other evaluation, so a pass either
    /// finishes before a role change (which then answers again) or starts after it.
    std::mutex _roleMutex;
    /// When this scheduler's current, unbroken leadership began; none while it does not lead.
    /// Guarded by `_roleMutex`.
    std::optional<core::platform::SteadyTimePoint> _leadingSince;
    /// The term `_leadingSince` belongs to: leading again in another term is a new leadership, since
    /// another node may have led in between. Guarded by `_roleMutex`.
    std::uint64_t _leadingEpoch { 0 };

    /// Where this node answers, as its start resolved it -- its name in the version spread. A snapshot:
    /// after an `--advertise` reload the leader may be listed once more under its own build, which
    /// changes a count and never which builds serve the fleet.
    std::string _ownEndpoint;

    /// Re-asks the fleet-wide rows on an interval. **Declared LAST, and the order is load-bearing**:
    /// its body touches `_service` and `_conditions`, so every member is built before it can start and
    /// destroyed only after `~jthread` has requested a stop and joined.
    std::jthread _conditionWatch;
};

} // namespace FastCache::Node
