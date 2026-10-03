// SPDX-License-Identifier: Apache-2.0
#include "FormationRuntime.hpp"
#include "NodeDefaults.hpp"

#include <FastCache/Core/StopAwareWait.hpp>

#include <format>
#include <utility>

namespace FastCache::Node
{

void LateClusterAdmin::Attach(Distributed::IClusterAdmin* admin) noexcept
{
    _admin.store(admin, std::memory_order_release);
}

Cluster::ClusterState LateClusterAdmin::ClusterState() const
{
    auto* const admin = _admin.load(std::memory_order_acquire);
    return admin != nullptr ? admin->ClusterState() : Cluster::ClusterState {};
}

std::expected<void, ConsensusError> LateClusterAdmin::ProposeToCluster(Cluster::Command const& command)
{
    auto* const admin = _admin.load(std::memory_order_acquire);
    if (admin == nullptr)
        return std::unexpected(NotLeader(std::nullopt));
    return admin->ProposeToCluster(command);
}

std::vector<Cluster::SeedCandidate> SeedsNow(Cluster::FleetEndpointsFile& endpoints,
                                             std::span<std::string const> typed,
                                             ISrvResolver const& srv,
                                             std::string_view dnsSuffix)
{
    // A hint file: no state of it is an error, and anything but `Loaded` is no seed.
    auto loaded = endpoints.Load();
    auto const remembered = loaded.outcome == Cluster::FleetEndpointsLoad::Loaded
                                ? Cluster::RememberedSeeds(loaded.endpoints)
                                : std::vector<std::string> {};

    // No domain asks no question (`SrvQueryName` answers empty), and a lookup that fails costs this
    // source and nothing else: the typed and remembered seeds are still tried.
    auto targets = std::vector<SrvTarget> {};
    if (auto const name = Cluster::SrvQueryName(dnsSuffix); !name.empty())
        if (auto found = srv.Lookup(name); found.has_value())
            targets = *std::move(found);

    return Cluster::OrderSeeds(remembered, typed, targets);
}

namespace
{
    /// Who this node is, as every summary it announces and every `Enroll` it sends says it.
    /// @param cfg The configuration its record shaped.
    /// @param identityKey This node's identity key.
    /// @return The facts; the Raft endpoint EMPTY for a mode nobody dials.
    [[nodiscard]] SelfFacts SelfFactsOf(NodeConfig const& cfg, Ed25519PublicKey const& identityKey)
    {
        return SelfFacts { .nodeId = cfg.nodeId,
                           .publicKey = identityKey,
                           .nodeEndpoint = AdvertisedEndpoint(cfg),
                           .raftEndpoint = ConsensusDialAddressOf(cfg).value_or(std::string {}),
                           .reach = ConsensusConfinedToThisMachine(cfg) ? FleetReachability::ThisMachineAlone
                                                                        : FleetReachability::Open };
    }
} // namespace

StartupShapeJudge::StartupShapeJudge(INodeConfigSource const& config,
                                     Cluster::FleetEndpointsFile& endpoints,
                                     StartupRules rules):
    _config { config },
    _endpoints { endpoints },
    _rules { rules }
{
}

std::optional<std::string> StartupShapeJudge::RefusalOf(Cluster::FormationRecord const& next) const
{
    auto shaped = _config.Current();
    auto const remembered = _endpoints.Load();
    if (auto applied = ApplyFormation(shaped, next, remembered.endpoints); !applied.has_value())
        return std::move(applied).error();
    return _rules(shaped);
}

FormationRuntime::FormationRuntime(NodeConfig const& cfg,
                                   Ed25519PublicKey const& identityKey,
                                   Cluster::FormationRecord record,
                                   FormationDurables durables,
                                   NodeReloader const* reloader,
                                   Cluster::IAnnouncedJoinMemos const* memos,
                                   IMetricsSink& metrics,
                                   ILogger& logger,
                                   NodeConditions* conditions):
    _cfg { cfg },
    _durables { durables },
    _live { cfg, reloader },
    _judge { _live, durables.endpoints, StartupPolicyRejection },
    _enroll { durables.parts.dialer },
    _probe { durables.parts.dialer, durables.parts.random, durables.parts.wait.Clock() },
    _controller { FormationParts {
                      .store = durables.store,
                      .enroll = _enroll,
                      .probe = _probe,
                      .endpoints = durables.endpoints,
                      .announced = memos != nullptr ? *memos : static_cast<Cluster::IAnnouncedJoinMemos const&>(_noMemos),
                      .admin = _admin,
                      .seeds =
                          [this] {
                              return SeedsNow(_durables.endpoints,
                                              _cfg.fleetSeeds,
                                              _durables.parts.srv,
                                              _cfg.hostNames.has_value() ? _cfg.hostNames->dnsSuffix : std::string {});
                          },
                      .reform = durables.reform,
                      .clock = durables.parts.wait.Clock(),
                      .wall = durables.parts.wall,
                      .random = durables.parts.random,
                      .metrics = metrics,
                      .logger = logger,
                      .judge = _judge,
                      .conditions = conditions },
                  SelfFactsOf(cfg, identityKey),
                  std::move(record) }
{
}

FormationRuntime::~FormationRuntime()
{
    // The beat first, while everything it reads is still here. Its `Beat` guard has normally stopped
    // it already, before the consensus tier went; this is for a body that never began one.
    Stop();
}

FormationHooks FormationRuntime::Hooks()
{
    return FormationHooks {
        .onState =
            [this](Cluster::ClusterState const& state,
                   std::string_view clusterId,
                   std::optional<Consensus::NodeId> const& leader,
                   std::string_view leaderNodeEndpoint) {
                _controller.OnClusterState(state, clusterId, leader, leaderNodeEndpoint);
            },
        .onOwnKeyRevoked = [this](Consensus::NodeId const& acceptor) { _controller.OnOwnKeyRevoked(acceptor); },
    };
}

void FormationRuntime::Stop() noexcept
{
    _beat.request_stop();
    if (_beat.joinable())
        _beat.join();
    _admin.Attach(nullptr);
}

void FormationRuntime::Start(Distributed::IClusterAdmin* tier)
{
    _admin.Attach(tier);
    _beat = std::jthread { [this](std::stop_token const& stop) {
        while (!stop.stop_requested())
        {
            _controller.Tick();
            if (_durables.parts.wait.WaitFor(stop, TickInterval) == WaitEnd::Stopped)
                break;
        }
    } };
}

std::expected<std::unique_ptr<FormationRuntime>, std::string> MakeFormationRuntime(NodeConfig const& cfg,
                                                                                   FormationBody const& body,
                                                                                   SchedulerTier* scheduler,
                                                                                   IMetricsSink& metrics,
                                                                                   ILogger& logger,
                                                                                   NodeConditions* conditions)
{
    if (!RunsConsensus(cfg))
        return std::unique_ptr<FormationRuntime> {};
    if (!cfg.identityPublicKey.has_value())
        return std::unexpected { std::string { FormationNeedsIdentityKey } };
    auto const* const memos =
        scheduler != nullptr ? static_cast<Cluster::IAnnouncedJoinMemos const*>(&scheduler->Service()) : nullptr;
    return std::make_unique<FormationRuntime>(
        cfg, *cfg.identityPublicKey, body.record, body.durables, body.reloader, memos, metrics, logger, conditions);
}

Cluster::IFleetSummarySource const& SummarySourceOf(FormationRuntime* runtime,
                                                    Cluster::IFleetSummarySource const* fallback) noexcept
{
    if (runtime == nullptr)
        return *fallback;
    return runtime->Controller();
}

Cluster::IAskedJoinsSource const* AskedJoinsOf(FormationRuntime* runtime) noexcept
{
    return runtime != nullptr ? &runtime->Controller() : nullptr;
}

FormationHooks FormationHooksOf(FormationRuntime* runtime)
{
    return runtime != nullptr ? runtime->Hooks() : FormationHooks {};
}

DiscoveryFormation DiscoveryFormationOf(FormationRuntime* runtime) noexcept
{
    if (runtime == nullptr)
        return DiscoveryFormation {};
    return DiscoveryFormation { .evidence = &runtime->Controller(), .fleets = &runtime->Controller() };
}

std::unique_ptr<FormationRuntime::Beat> BeginFormation(FormationRuntime* runtime, Distributed::IClusterAdmin* tier)
{
    if (runtime != nullptr)
        runtime->Start(tier);
    return std::make_unique<FormationRuntime::Beat>(runtime);
}

} // namespace FastCache::Node
