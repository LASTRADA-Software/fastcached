// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <core/Ranges.hpp>

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

    /// The key that vouches for the cluster @p record commits this node to, as the pin asks it
    /// (`Cluster::AdmitsFleet`) -- always one this node PROVED, never one a peer's bytes merely list:
    /// the key that proved the fleet a pending node asked; the key that signed the admission a joined
    /// node acted on (`FleetMembership::admittedBy`), never the voters its roster claims; this node's
    /// own key for a cluster it founded.
    ///
    /// Which is what makes this check one of its own rather than a restatement of the poll's: an
    /// approval signed by another key whose roster lists a pinned voter is refused here too.
    /// Deterministic over the record and this node's key, so a restart judges what the move did -- and
    /// a fleet that later forgets every pinned voter still starts, since the admission is what is
    /// judged and the fleet's applied state is the authority from then on.
    /// @param record The formation record.
    /// @param cfg The configuration, for this node's own key.
    /// @return The key; nothing when none can be named, which no pin admits.
    [[nodiscard]] std::optional<Ed25519PublicKey> CommitmentSigner(Cluster::FormationRecord const& record,
                                                                   NodeConfig const& cfg)
    {
        if (record.joining.has_value())
            return record.joining->provenKey;
        if (record.fleet.has_value())
            return record.fleet->admittedBy;
        return cfg.identityPublicKey;
    }
} // namespace

std::expected<void, std::string> ApplyFormation(NodeConfig& cfg,
                                                Cluster::FormationRecord const& record,
                                                Cluster::FleetEndpoints const& remembered)
{
    // Before anything is shaped: a record the pin refuses describes a node this one must not be.
    if (auto const committed = Cluster::CommittedClusterId(record); committed.has_value())
    {
        auto const signer = CommitmentSigner(record, cfg);
        auto const signers =
            signer.has_value() ? std::span<Ed25519PublicKey const> { &*signer, 1 } : std::span<Ed25519PublicKey const> {};
        if (!Cluster::AdmitsFleet(FleetPinOf(cfg), *committed, signers))
            return std::unexpected { std::format(
                "the formation record commits this node to cluster {1} (it is {0}) under key {3}, which --fleet-id "
                "does not name, and --fleet-id pins it to {2}: if that is the fleet this node belongs to, reset its "
                "state directory so it starts alone and joins it; if {1} is, change --fleet-id",
                Cluster::NodeModeRowFor(record.mode).name,
                *committed,
                Cluster::PinText(FleetPinOf(cfg)),
                signer.has_value() ? FormatEd25519PublicKey(*signer) : std::string { "none" }) };
    }

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
    // Shaped now, so no earlier reading's refusal describes it any more (the install shapes a
    // configuration `main` could not).
    cfg.formationUnread.reset();
    return {};
}

Cluster::FleetPin FleetPinOf(NodeConfig const& cfg)
{
    return Cluster::FleetPin { .fleet = cfg.fleetPin };
}

bool ModeOpensRaftPort(Cluster::NodeMode mode) noexcept
{
    return Cluster::NodeModeRowFor(mode).raftListener == Cluster::RaftListenerState::Open;
}

bool ModeServesConsensusToPeers(Cluster::NodeMode mode) noexcept
{
    return ModeOpensRaftPort(mode) && Cluster::NodeModeRowFor(mode).consensus == Cluster::ConsensusScope::Fleet;
}

bool ServesEnrollment(NodeConfig const& cfg, bool schedulerRuns)
{
    return !EnrollmentAbsenceOf(cfg, schedulerRuns).has_value();
}

std::optional<EnrollmentAbsence> EnrollmentAbsenceOf(NodeConfig const& cfg, bool schedulerRuns)
{
    if (!RunsConsensus(cfg) || !cfg.formation.has_value())
        return EnrollmentAbsence::NoConsensus;
    // Not while its consensus is confined to this machine: a member admitted there would be told to
    // dial a loopback address, which reaches itself.
    if (ConsensusConfinedToThisMachine(cfg))
        return EnrollmentAbsence::ConfinedToThisMachine;
    if (!schedulerRuns)
        return EnrollmentAbsence::NoScheduler;
    // Under this node's OWN key: what it would sign every answer with, which a pinned joiner takes
    // only from a pinned voter.
    auto const pin = FleetPinOf(cfg);
    auto const& clusterId = cfg.formation->clusterId;
    auto const ownKey = cfg.identityPublicKey.has_value() ? std::span<Ed25519PublicKey const> { &*cfg.identityPublicKey, 1 }
                                                          : std::span<Ed25519PublicKey const> {};
    if (Cluster::AdmitsFleet(pin, clusterId, ownKey))
        return std::nullopt;
    // Refused, so pinned: whether the pin admits this cluster under ANY of its own keys tells the
    // cluster it names apart from the key it does not -- the same predicate, never an id compared here.
    auto const pinnedKeys = pin.fleet.has_value() ? std::span<Ed25519PublicKey const> { pin.fleet->voterKeys }
                                                  : std::span<Ed25519PublicKey const> {};
    return Cluster::AdmitsFleet(pin, clusterId, pinnedKeys) ? EnrollmentAbsence::NotAPinnedVoter
                                                            : EnrollmentAbsence::PinnedToAnotherCluster;
}

bool ServesScheduler(NodeConfig const& cfg) noexcept
{
    // Only while it runs consensus: a scheduler signs every grant with its identity key and hands
    // its workers the cluster's state, so a mode that serves one on a node whose consensus is
    // closed (an empty `--listen-raft=`) serves none. One whose consensus is confined to this
    // machine still serves its own, to its own worker.
    return cfg.formation.has_value()
           && Cluster::NodeModeRowFor(cfg.formation->mode).scheduler == Cluster::SchedulerDuty::Serves && RunsConsensus(cfg);
}

bool RedirectsScheduling(NodeConfig const& cfg) noexcept
{
    return RunsConsensus(cfg) && !ServesScheduler(cfg);
}

std::vector<std::string> SchedulersOf(NodeConfig const& cfg, ActivatedNodeEndpoint const& activated)
{
    if (!cfg.formation.has_value())
        return {};
    if (!ServesScheduler(cfg))
        return cfg.formation->fleetSchedulers;

    // Dialled where it binds. A wildcard bind answers on loopback; a node bound to ONE address
    // answers there alone, and loopback would reach nothing -- its worker would never register
    // with the scheduler in its own process. One rule for both sources of the binding.
    auto const dialledAt = [](std::string_view host, std::uint16_t port) {
        return FormatHostPort(IsWildcardHost(host) ? ThisMachineLoopbackHost : host, port);
    };

    // Its own scheduler, on its own node port: the socket a supervisor handed over when it did,
    // since the configuration then names an address and a port nothing serves.
    if (activated.has_value())
        return { dialledAt(activated->host, activated->port) };
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    if (node.empty())
        return {};
    return { dialledAt(node.front().host, node.front().port) };
}

std::vector<std::string> RecordedSchedulersOf(Cluster::ClusterState const& state, std::string_view self)
{
    auto endpoints = std::vector<std::string> {};
    for (auto const& member: state.members)
        if (member.seat == Cluster::MemberSeat::Voter && member.id != self && !member.schedulerEndpoint.empty()
            && !std::ranges::contains(endpoints, member.schedulerEndpoint))
            endpoints.push_back(member.schedulerEndpoint);
    return endpoints;
}

AppliedSchedulers::AppliedSchedulers(NodeConfig const& cfg, ActivatedNodeEndpoint const& activated):
    _self { cfg.nodeId },
    _servesScheduler { ServesScheduler(cfg) },
    _formation { SchedulersOf(cfg, activated) }
{
}

void AppliedSchedulers::Applied(Cluster::ClusterState const& state)
{
    auto recorded = RecordedSchedulersOf(state, _self);
    auto const guard = std::scoped_lock { _lock };
    _recorded = std::move(recorded);
}

std::vector<std::string> AppliedSchedulers::Current() const
{
    auto const guard = std::scoped_lock { _lock };
    if (!_servesScheduler)
        return _recorded.empty() ? _formation : _recorded;

    // Its own scheduler FIRST, then the other voters the state records: a voter that hears its
    // leader redirects from its own and the rest are never dialled, while one whose address vanished
    // hears nobody -- its own scheduler answers `NotLeader` naming no one -- and is heard by the
    // leader only through a voter that did not move. A list to walk, never a leader choice.
    auto endpoints = _formation;
    for (auto const& recorded: _recorded)
        if (!std::ranges::contains(endpoints, recorded))
            endpoints.push_back(recorded);
    return endpoints;
}

std::vector<Cluster::MemberSpec> BootstrapMembersOf(NodeConfig const& cfg)
{
    if (!cfg.formation.has_value())
        return {};
    if (!cfg.formation->foundedHere)
    {
        // The approved roster's members, as the typed members consensus starts from: each holds
        // the key the roster records for it, which the peer sessions verify it against.
        std::vector<Cluster::MemberSpec> members;
        members.reserve(cfg.formation->fleetMembers.size());
        for (auto const& member: cfg.formation->fleetMembers)
            members.push_back(
                Cluster::MemberSpec { .id = member.id, .raftEndpoint = member.raftEndpoint, .publicKey = member.publicKey });
        return members;
    }

    // This node alone: the cluster it minted, whether it is still solitary or has founded a fleet
    // that others joined -- they are in its log, not in its bootstrap. Its key is the one it
    // proves itself with, when the start has resolved it.
    auto const dial = ConsensusDialAddressOf(cfg);
    return { Cluster::MemberSpec {
        .id = cfg.nodeId, .raftEndpoint = dial.value_or(std::string {}), .publicKey = cfg.identityPublicKey } };
}

Cluster::MemberSeat SeatInFormation(NodeConfig const& cfg, std::string_view id)
{
    if (!cfg.formation.has_value() || cfg.formation->foundedHere)
        return Cluster::MemberSeat::Voter;
    auto const* const member = core::findOrNull(cfg.formation->fleetMembers, id, &Cluster::ClusterMember::id);
    return member != nullptr ? member->seat : Cluster::MemberSeat::Voter;
}

std::optional<std::string> RaftClosedByFormation(NodeConfig const& cfg)
{
    // Not "yet" where a reading refused: no first start mints over a record it could not read, and
    // the mode line above says why (`FormationAbsenceOf`).
    if (!cfg.formation.has_value())
        return std::string { cfg.formationUnread.has_value()
                                 ? "not served (no formation record could be read; see the mode line)"
                                 : "not served (no formation record yet)" };
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

std::expected<void, FormationUnread> ShapeByKeptFormation(NodeConfig& cfg,
                                                          std::expected<KeptFormation, FormationUnread> const& kept)
{
    // A record that was read and that `ApplyFormation` refuses is the formation's own step.
    auto shaped = kept.and_then([&cfg](KeptFormation const& held) {
        return ApplyFormation(cfg, ProspectiveRecord(held), held.remembered).transform_error([](std::string reason) {
            return FormationUnread { .reason = std::move(reason), .stage = StartStage::Formation };
        });
    });
    if (!shaped.has_value())
        cfg.formationUnread = shaped.error();
    return shaped;
}

std::string FormationAbsenceOf(NodeConfig const& cfg)
{
    constexpr std::string_view unshaped = "no formation record shaped this configuration";
    if (!cfg.formationUnread.has_value())
        return std::string { unshaped };
    return std::format("{}: {}", unshaped, cfg.formationUnread->reason);
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
        return std::format("mode: none ({})", FormationAbsenceOf(cfg));
    auto const name = Cluster::NodeModeRowFor(cfg.formation->mode).name;
    if (cfg.formation->clusterId.empty())
        return std::format("mode: {} (no cluster minted yet; the first start mints one)", name);
    return std::format("mode: {}", name);
}

} // namespace FastCache::Node
