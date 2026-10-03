// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "NodeIoLoop.hpp"
#include "SchedulerTier.hpp"

#include <FastCache/Core/StopAwareWait.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/FleetVersions.hpp>
#include <FastCache/Distributed/UnservedToolchains.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    /// One fleet-wide condition: its row, and what it says when raised -- nothing when it is clear.
    struct FleetConditionRow
    {
        NodeCondition condition; ///< The row this answers.
        /// The detail to raise it with, or nothing when the leader finds it benign.
        std::optional<std::string> (*raisedDetail)(SchedulerConditionInputs const& inputs);
        /// How long a leader must have led before "nothing raised" may read `clear`: the span over
        /// which what this row reads reaches the leader, none of which survives a failover.
        core::platform::SteadyDuration observation;
        /// That span in words, for the `not-evaluated` detail a leader gives until it has watched it.
        std::string_view observationWords;
    };

    /// What an operator calls @p toolchain: the client's label, else its fingerprint, marked.
    [[nodiscard]] std::string ToolchainName(Distributed::UnservedToolchain const& toolchain)
    {
        // An operator's pinned `<fingerprint>=<compiler>` is never probed, so it has no banner; the
        // fingerprint is then all there is, and naming nothing would hide the refusals it counts.
        return toolchain.label.empty() ? std::format("toolchain {} (unlabelled)", toolchain.fingerprint) : toolchain.label;
    }

    /// `unserved-toolchain`: the toolchains clients asked for that no live worker serves.
    [[nodiscard]] std::optional<std::string> UnservedToolchainDetail(SchedulerConditionInputs const& inputs)
    {
        auto const unserved = inputs.service.UnservedToolchainsNow();
        if (unserved.empty())
            return std::nullopt;
        auto names = std::vector<std::string> {};
        names.reserve(unserved.size());
        for (auto const& toolchain: unserved)
            names.push_back(std::format("{} ({} lease(s) refused)", ToolchainName(toolchain), toolchain.refusals));
        // `ListDetail` escapes each name as text, so a label a host spelled in another encoding
        // reaches the leader's page as `\xNN` rather than making it refuse this machine's rows.
        return ListDetail(std::format("{} toolchain(s) clients asked for that no live worker serves:", unserved.size()),
                          names);
    }

    /// @p machines as one run of names, space-separated -- the list separator is `ListDetail`'s comma.
    [[nodiscard]] std::string MachineRun(std::vector<std::string> const& machines)
    {
        auto run = std::string {};
        for (auto const& machine: machines)
        {
            if (!run.empty())
                run += ' ';
            run += machine;
        }
        return run;
    }

    /// `mixed-node-versions`: more than one build serving this wire.
    [[nodiscard]] std::optional<std::string> MixedNodeVersionsDetail(SchedulerConditionInputs const& inputs)
    {
        // Over `NodeReports()`, never registry entries: a machine with two toolchains runs one build.
        auto const reports = inputs.service.Workers().NodeReports();
        auto const spread = Distributed::SpreadOfVersions(reports, inputs.ownVersion, inputs.ownEndpoint);
        if (!spread.Mixed())
            return std::nullopt;
        auto builds = std::vector<std::string> {};
        builds.reserve(spread.groups.size());
        for (auto const& group: spread.groups)
            builds.push_back(std::format("{} on {}", group.version, MachineRun(group.machines)));
        auto const silent =
            spread.unstated == 0 ? std::string {} : std::format(" ({} more machine(s) did not say)", spread.unstated);
        return ListDetail(std::format("{} builds of one wire serve this fleet{}:", spread.groups.size(), silent), builds);
    }

    /// Every `ConditionScope::Scheduler` row, with its question. A row joins HERE and in
    /// `NodeConditionTable` together; `EveryFleetRowHasAnEvaluator` holds that at compile time.
    ///
    /// The observation spans: a toolchain counts as unserved for `UnservedToolchains::Window` after the
    /// last refusal, so a leader has seen every refusal that could still raise the row only once it
    /// has led that long; and every live machine announces to the leader within the heartbeat
    /// timeout, after which a single build in its registry is a fact rather than an absence of reports.
    constexpr std::array FleetConditionTable {
        FleetConditionRow { .condition = NodeCondition::UnservedToolchain,
                            .raisedDetail = &UnservedToolchainDetail,
                            .observation = Distributed::UnservedToolchains::Window,
                            .observationWords = "fifteen minutes" },
        FleetConditionRow { .condition = NodeCondition::MixedNodeVersions,
                            .raisedDetail = &MixedNodeVersionsDetail,
                            .observation = Distributed::WorkerRegistry::DefaultHeartbeatTimeout,
                            .observationWords = "ninety seconds" },
    };

    /// Whether every `Scheduler`-scope row has exactly one evaluator here, and nothing else does.
    [[nodiscard]] consteval bool EveryFleetRowHasAnEvaluator() noexcept
    {
        return std::ranges::all_of(
                   NodeConditionTable,
                   [](NodeConditionRow const& row) {
                       return row.scope != ConditionScope::Scheduler
                              || std::ranges::count(FleetConditionTable, row.condition, &FleetConditionRow::condition) == 1;
                   })
               && std::ranges::all_of(FleetConditionTable, [](FleetConditionRow const& fleet) {
                      return RowFor(fleet.condition).scope == ConditionScope::Scheduler;
                  });
    }
    static_assert(EveryFleetRowHasAnEvaluator(),
                  "every Scheduler-scope condition needs exactly one FleetConditionTable row, and only those may "
                  "have one -- a row nothing evaluates reads `undecided` forever");

    static_assert(Distributed::UnservedToolchains::Window == std::chrono::minutes { 15 },
                  "the unserved-toolchain remedy and its observationWords tell the operator fifteen minutes");

    static_assert(Distributed::WorkerRegistry::DefaultHeartbeatTimeout == std::chrono::seconds { 90 },
                  "the mixed-node-versions remedy and its observationWords tell the operator ninety seconds");
} // namespace

void EvaluateSchedulerConditions(NodeConditions& conditions, SchedulerConditionInputs const& inputs)
{
    if (inputs.service.Role() != Distributed::SchedulerRole::Leader || !inputs.leadingFor.has_value())
    {
        auto const leader = inputs.service.LeaderEndpoint();
        auto const reason =
            leader.empty()
                ? std::string { "this scheduler is not leading and knows no leader yet; the fleet's leader evaluates this" }
                : std::format("this scheduler is not leading; the fleet's leader, {}, evaluates this", leader);
        for (auto const& row: FleetConditionTable)
            conditions.NotEvaluated(row.condition, reason);
        return;
    }
    for (auto const& row: FleetConditionTable)
    {
        auto const detail = row.raisedDetail(inputs);
        if (detail.has_value())
            conditions.Raise(row.condition, *detail);
        else if (*inputs.leadingFor < row.observation)
            conditions.NotEvaluated(row.condition,
                                    std::format("this scheduler has led for less than {}, the span this row must watch "
                                                "before it may read clear; nothing another leader saw carries over",
                                                row.observationWords));
        else
            conditions.Clear(row.condition);
    }
}

core::platform::SteadyDuration LongestFleetObservation() noexcept
{
    return std::ranges::max(FleetConditionTable | std::views::transform(&FleetConditionRow::observation));
}

SchedulerTier::SchedulerTier(Distributed::IMembershipOracle const& membership,
                             core::platform::IClock& clock,
                             core::platform::WallClockRef wallClock,
                             IMetricsSink& metrics,
                             ILogger& logger,
                             std::string signerId,
                             Ed25519KeyPair identityKey,
                             std::string_view clusterId,
                             NodeConditions& conditions,
                             std::string ownEndpoint):
    _signer { std::move(signerId), std::move(identityKey) },
    _service { clock, wallClock, metrics, logger, _signer, clusterId },
    _protocol { _service, metrics },
    // The oracle is the NODE's, not this tier's: the cache surface consults the same
    // object, and a node that answered "is this peer one of ours" differently at its
    // two surfaces would admit a peer to the fleet and refuse it the objects that
    // fleet produced. It also outlives this tier, which is what lets a node serve a
    // cache with no scheduler at all.
    _responder { _protocol, membership, metrics },
    _conditions { conditions },
    _clock { clock },
    _ownEndpoint { std::move(ownEndpoint) }
{
    // No standalone leadership any more (#178). Every scheduler runs consensus -- a lone one
    // is a cluster of one -- so the service keeps its own `Undecided` default until the
    // consensus tier publishes a role and a term, which `Gate()` answers with `NotLeader`
    // meanwhile. "Leader at term 0" is not a weaker answer than "leader at term N"; it is a
    // different and wrong one (#613), and there is no longer a node for whom it is right.
}

std::expected<std::unique_ptr<SchedulerTier>, std::string> SchedulerTier::Start(
    NodeConfig const& cfg,
    Distributed::IMembershipOracle const& membership,
    core::platform::IClock& clock,
    core::platform::WallClockRef wallClock,
    IMetricsSink& metrics,
    ILogger& logger,
    std::optional<Ed25519KeyPair> const& identityKey,
    NodeConditions& conditions,
    std::chrono::milliseconds conditionInterval)
{
    // The key every grant is signed with is this node's OWN (#178), resolved out of its state
    // directory before any tier exists. A scheduler runs consensus and a consensus node always
    // holds one, so this is the answer to a caller that did not -- never a fallback to
    // unsigned grants, which no longer exist.
    if (!identityKey.has_value())
        return std::unexpected { std::string { SchedulerNeedsIdentityKeyRefusal } };

    auto tier = std::unique_ptr<SchedulerTier> { new SchedulerTier { membership,
                                                                     clock,
                                                                     wallClock,
                                                                     metrics,
                                                                     logger,
                                                                     cfg.nodeId,
                                                                     *identityKey,
                                                                     cfg.clusterId,
                                                                     conditions,
                                                                     AdvertisedEndpoint(cfg) } };

    // Answered BEFORE `main` settles the table, so no reader ever sees these rows `undecided`: a
    // scheduler no consensus has told a role says so -- `not-evaluated` -- rather than nothing.
    tier->EvaluateConditions();
    tier->WatchConditions(conditionInterval);

    // No address in this line since #290: the scheduler verbs are answered on the
    // node's one 0xFC listener, and that listener names itself when it binds.
    //
    // The phrase is `AdmissionSummary`'s rather than this tier's, because the policy
    // is the NODE's: the worker's own ready line reports the identical fact, and two
    // surfaces spelling one policy differently is how an operator comes to believe
    // their compile port is configured because their scheduler said so (#235).
    logger.Logf(LogLevel::Info, "scheduling for the fleet ({})", AdmissionSummary(cfg));
    return tier;
}

void SchedulerTier::SetRole(Distributed::SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t epoch)
{
    // The role change and the evaluation answering it are one decision under `_roleMutex`, so no
    // watch pass that read the OLD role can write after them (see `_roleMutex`).
    std::scoped_lock const lock { _roleMutex };
    _service.SetRole(role, leaderEndpoint, epoch);
    if (role != Distributed::SchedulerRole::Leader)
        _leadingSince.reset();
    else if (!_leadingSince.has_value() || epoch != _leadingEpoch)
    {
        // A new leadership: either this node did not lead, or it leads in another term, and some
        // other node may have led -- and seen what the rows read -- in between.
        _leadingSince = _clock.now();
        _leadingEpoch = epoch;
    }
    // A role change moves every fleet-wide row at once -- a new leader starts watching, a demoted
    // one stops -- so it is answered now rather than up to an interval later.
    EvaluateConditionsLocked();
}

void SchedulerTier::EvaluateConditions()
{
    std::scoped_lock const lock { _roleMutex };
    EvaluateConditionsLocked();
}

void SchedulerTier::EvaluateConditionsLocked()
{
    auto const leadingFor = _leadingSince.transform(
        [this](core::platform::SteadyTimePoint since) { return core::platform::SteadyDuration { _clock.now() - since }; });
    EvaluateSchedulerConditions(
        _conditions,
        SchedulerConditionInputs {
            .service = _service, .ownVersion = VersionString, .ownEndpoint = _ownEndpoint, .leadingFor = leadingFor });
}

void SchedulerTier::WatchConditions(std::chrono::milliseconds interval)
{
    _conditionWatch = std::jthread { [this, interval](std::stop_token const& stop) {
        // A stop ends the wait at once, so tearing the tier down never sits out an interval.
        while (WaitForStopOr(stop, interval) == WaitEnd::Elapsed)
            EvaluateConditions();
    } };
}

void SchedulerTier::Endorse(Cluster::RosterEndorsement const& endorsement)
{
    std::scoped_lock const lock { _ownEndorsementMutex };
    _ownEndorsement = endorsement;
    std::ignore = _service.AcceptEndorsement(endorsement);
}

void SchedulerTier::Administer(Distributed::IClusterAdmin& admin)
{
    std::scoped_lock const lock { _ownEndorsementMutex };
    _service.AdministerWith(admin);
    if (_ownEndorsement.has_value())
        std::ignore = _service.AcceptEndorsement(*_ownEndorsement);
}

} // namespace FastCache::Node
