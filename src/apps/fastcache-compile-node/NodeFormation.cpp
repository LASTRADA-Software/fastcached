// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// The fleet's members, as the approval's roster carries them.
    /// @param fleet The fleet this node was admitted to.
    /// @param self This node's id, never one of the members returned.
    /// @return The members, or why the roster cannot be read.
    [[nodiscard]] std::expected<std::vector<Cluster::ClusterMember>, std::string> FleetMembersOf(
        Cluster::FleetMembership const& fleet, std::string_view self)
    {
        auto roster = Cluster::DecodeRoster(fleet.roster);
        if (!roster.has_value())
            return std::unexpected { std::format(
                "the formation record's roster for fleet {} cannot be read ({}), so this node cannot say who its fleet "
                "is; it is never replaced by an empty one, which would dial nobody and report nothing",
                fleet.clusterId,
                roster.error().context) };

        std::vector<Cluster::ClusterMember> members;
        for (auto const& member: roster->members)
            if (member.id != self)
                members.push_back(
                    Cluster::ClusterMember { .id = member.id,
                                             .raftEndpoint = member.raftEndpoint,
                                             .schedulerEndpoint = {},
                                             .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                             .seat = member.seat,
                                             .publicKey = member.publicKey });
        return members;
    }

    /// The node endpoints a node that serves no scheduler registers with, in the order tried.
    /// @param record The formation record.
    /// @param remembered The fleet endpoints this node last knew.
    /// @return Remembered voters of THIS fleet first, then the leader a pending node asked.
    [[nodiscard]] std::vector<std::string> FleetSchedulersOf(Cluster::FormationRecord const& record,
                                                             Cluster::FleetEndpoints const& remembered)
    {
        std::vector<std::string> out;
        auto const add = [&out](std::string const& endpoint) {
            if (!endpoint.empty() && !std::ranges::contains(out, endpoint))
                out.push_back(endpoint);
        };
        if (remembered.clusterId == Cluster::CurrentClusterId(record))
            for (auto const& seed: Cluster::RememberedSeeds(remembered))
                add(seed);
        if (record.joining.has_value())
            add(record.joining->summary.leaderNodeEndpoint);
        return out;
    }
} // namespace

std::expected<void, std::string> ApplyFormation(NodeConfig& cfg,
                                                Cluster::FormationRecord const& record,
                                                Cluster::FleetEndpoints const& remembered)
{
    auto view = NodeFormationView { .mode = record.mode,
                                    .clusterId = Cluster::CurrentClusterId(record),
                                    .createdAtUnixSeconds = record.own.createdAtUnixSeconds,
                                    .foundedHere = Cluster::FoundedHere(record),
                                    .fleetMembers = {},
                                    .fleetSchedulers = FleetSchedulersOf(record, remembered) };
    if (record.fleet.has_value())
    {
        auto members = FleetMembersOf(*record.fleet, cfg.nodeId);
        if (!members.has_value())
            return std::unexpected { std::move(members).error() };
        view.fleetMembers = *std::move(members);
    }

    // The cluster field of every lease this node signs or checks (#322) follows the record, so a
    // reader of the old field cannot see a cluster the node is not in. Not before a mint: an
    // empty id is no cluster, and the field keeps what the flags said until one is minted.
    if (!view.clusterId.empty())
        cfg.clusterId = view.clusterId;
    cfg.formation = std::move(view);
    return {};
}

bool ModeOpensRaftPort(Cluster::NodeMode mode) noexcept
{
    return Cluster::NodeModeRowFor(mode).raftListener == Cluster::RaftListenerState::Open;
}

bool ModeServesConsensusToPeers(Cluster::NodeMode mode) noexcept
{
    return ModeOpensRaftPort(mode) && Cluster::NodeModeRowFor(mode).consensus == Cluster::ConsensusScope::Fleet;
}

bool ServesScheduler(NodeConfig const& cfg) noexcept
{
    // Only while it runs consensus: a scheduler signs every grant with its identity key and hands
    // its workers the cluster's state, so a mode that serves one on a node whose consensus is
    // closed (an empty `--listen-raft=`, or a name that reaches only this machine) serves none.
    return cfg.formation.has_value()
           && Cluster::NodeModeRowFor(cfg.formation->mode).scheduler == Cluster::SchedulerDuty::Serves && RunsConsensus(cfg);
}

std::vector<std::string> SchedulersOf(NodeConfig const& cfg)
{
    if (!cfg.formation.has_value())
        return {};
    if (!ServesScheduler(cfg))
        return cfg.formation->fleetSchedulers;

    // Its own scheduler, on its own node port.
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    if (node.empty())
        return {};
    return { FormatHostPort(ThisMachineLoopbackHost, node.front().port) };
}

std::vector<Cluster::ClusterMember> BootstrapMembersOf(NodeConfig const& cfg)
{
    if (!cfg.formation.has_value())
        return {};
    if (!cfg.formation->foundedHere)
        return cfg.formation->fleetMembers;

    // This node alone: the cluster it minted, whether it is still solitary or has founded a fleet
    // that others joined -- they are in its log, not in its bootstrap.
    auto const dial = ConsensusDialAddressOf(cfg);
    return { Cluster::ClusterMember { .id = cfg.nodeId,
                                      .raftEndpoint = dial.value_or(std::string {}),
                                      .schedulerEndpoint = {},
                                      .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                      .seat = Cluster::MemberSeat::Voter,
                                      .publicKey = cfg.identityPublicKey } };
}

std::optional<std::string> RaftClosedByFormation(NodeConfig const& cfg)
{
    if (!cfg.formation.has_value())
        return std::string { "not served (no formation record yet)" };
    auto const& row = Cluster::NodeModeRowFor(cfg.formation->mode);
    if (ModeOpensRaftPort(row.mode))
        return std::nullopt;
    if (row.dials == Consensus::RaftWire::SessionDirection::TwoWay)
        return std::format("not served ({}: dials the leader)", row.name);
    return std::format("not served ({}: its mode opens no consensus port)", row.name);
}

std::expected<KeptFormation, std::string> ReadKeptFormation(Cluster::IFormationStore const& store,
                                                            Cluster::FleetEndpointsFile& endpoints)
{
    auto record = store.Load();
    if (!record.has_value())
        return std::unexpected { std::format(
            "the formation record in this node's state directory cannot be read ({}); it is the only account of the "
            "fleet this node recorded, so it is never replaced by a fresh one",
            record.error().context) };

    // A hint file: no state of it keeps a node from starting, so anything but `Loaded` is none.
    auto loaded = endpoints.Load();
    auto remembered =
        loaded.outcome == Cluster::FleetEndpointsLoad::Loaded ? std::move(loaded.endpoints) : Cluster::FleetEndpoints {};
    return KeptFormation { .record = *std::move(record), .remembered = std::move(remembered) };
}

Cluster::FormationRecord ProspectiveRecord(KeptFormation const& kept)
{
    // The record the first start mints is solitary in a cluster of its own, and its id is the
    // one thing a prospect cannot know: left empty, which every reader takes as "not minted yet".
    return kept.record.value_or(Cluster::FormationRecord {});
}

std::expected<Cluster::FormationRecord, std::string> KeepFormation(KeptFormation const& kept,
                                                                   Cluster::IFormationStore& store,
                                                                   ISecureRandom& random,
                                                                   core::platform::IWallClock const& wall)
{
    if (kept.record.has_value())
        return *kept.record;

    // Only an ABSENT record mints, and it is written BEFORE the node acts on it.
    auto minted = Cluster::MintSolitary(random, wall);
    if (!minted.has_value())
        return std::unexpected { std::format("cannot mint this node's cluster: {}. Nothing was written, and no "
                                             "weaker source is used in its place",
                                             minted.error().ToString()) };
    if (auto saved = store.Save(*minted); !saved.has_value())
        return std::unexpected { std::format("cannot keep this node's formation record: {}", saved.error().context) };
    return *std::move(minted);
}

std::string DescribeFormationMode(NodeConfig const& cfg)
{
    if (!cfg.formation.has_value())
        return "mode: none (no formation record shaped this configuration)";
    auto const name = Cluster::NodeModeRowFor(cfg.formation->mode).name;
    if (cfg.formation->clusterId.empty())
        return std::format("mode: {} (no cluster minted yet; the first start mints one)", name);
    return std::format("mode: {}", name);
}

} // namespace FastCache::Node
