// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollmentAbsence.hpp"
#include "SchedulerLink.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Platform/ProcessExit.hpp>

#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

/// @file NodeFormation.hpp
/// What the formation record says, as the rest of the node reads it.
///
/// **A mode is the STATE held in the cluster dir, never a flag or a port.** The record is written
/// by the node's own transitions (`Cluster::FormationRecord`), and this is the one place a
/// configuration learns from it: which consensus runs, whether the Raft port opens, whether the
/// scheduler is served and where the worker registers. Every one of those is a column of
/// `Cluster::NodeModeTable`, read here rather than switched on.
namespace FastCache::Node
{

struct NodeConfig;

/// Why no formation record shaped a configuration, and the step of a start that gives up on it.
///
/// The STAGE is the arm that gave up, carried rather than re-derived: an I/O arm -- a listing, a
/// read, an owner or access list the platform would not answer -- is `Failed` and retried under a
/// supervisor, a verdict on what was found is `Refused` (`StartStageRows`). Flattened to a string,
/// every one of them read as a verdict and a transient read became a node left stopped.
struct FormationUnread
{
    std::string reason;                         ///< The reading's own refusal, naming what and why.
    StartStage stage { StartStage::Formation }; ///< The step a start gives up at, which decides its exit.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(FormationUnread const&, FormationUnread const&) = default;
};

/// What the formation record says, as the rest of the node reads it.
///
/// Not a flag: `ApplyFormation` writes it, and nothing parses it.
struct NodeFormationView
{
    Cluster::NodeMode mode { Cluster::NodeMode::Solitary }; ///< What the node is.
    std::string clusterId;                                  ///< The cluster this node's consensus runs; empty before a mint.
    std::uint64_t createdAtUnixSeconds { 0 };               ///< When its OWN cluster was minted.
    bool foundedHere { true };                        ///< Whether it is in the cluster it minted rather than one it joined.
    std::vector<Cluster::ClusterMember> fleetMembers; ///< The approved roster's members, never this node.
    std::vector<std::string> fleetSchedulers;         ///< Node endpoints a learner registers with, remembered first.

    /// Field-wise equality, so a reload can tell an unchanged record from a changed one.
    [[nodiscard]] friend bool operator==(NodeFormationView const&, NodeFormationView const&) = default;
};

/// Shape @p cfg from the formation record.
///
/// Fills `cfg.formation` from @p record and writes `cfg.clusterId` from it as well -- the lease
/// signature's cluster field (#322) -- so every reader of the old field sees the formation's.
/// The fleet's schedulers are the voters @p remembered holds FOR THIS FLEET, in their recorded
/// order, then the leader a pending node asked, when present and not already listed: remembered
/// endpoints of another fleet are not this node's to register with.
///
/// **A roster that does not decode is a refusal, never an empty fleet.** It is part of the
/// record, and a learner started with no members would dial nobody and report nothing -- the
/// record's own rule, that a record that cannot be read refuses the start by name, arriving at
/// the one field its decoder does not open.
///
/// **A record the pin does not admit is a refusal too** (`--fleet-id`, `Cluster::CommittedClusterId`):
/// a pending node asking, or a learner or voter in, a cluster other than the pinned one. Judged here
/// because every shape the node takes passes through: the start refuses it by name with the remedy,
/// a reload cannot reach it (the pin is `Reloadable::No`), and every formation move is judged by the
/// same call (`StartupShapeJudge`), so no transition writes a record the pin forbids.
/// @param cfg The configuration to shape.
/// @param record The formation record, as loaded or minted.
/// @param remembered The fleet endpoints this node last knew; empty when there is no file.
/// @return Nothing, or why the record cannot shape a configuration.
[[nodiscard]] std::expected<void, std::string> ApplyFormation(NodeConfig& cfg,
                                                              Cluster::FormationRecord const& record,
                                                              Cluster::FleetEndpoints const& remembered);

/// The pin @p cfg carries: `--fleet-id`, or none.
/// @param cfg The configuration.
/// @return The pin every formation decision asks (`Cluster::AdmitsFleet`).
[[nodiscard]] Cluster::FleetPin FleetPinOf(NodeConfig const& cfg);

/// Whether @p mode binds the Raft surface: its row's `raftListener` column.
/// @param mode The mode.
/// @return True when voters dial this node.
[[nodiscard]] bool ModeOpensRaftPort(Cluster::NodeMode mode) noexcept;

/// Whether @p mode serves consensus to OTHER machines, which then have to be able to reach it.
///
/// Derived from two columns rather than stated as a third: a mode whose consensus is the FLEET's
/// and whose Raft port is open is one other members dial. A solitary or pending node opens the
/// port for a fleet that does not exist yet, and a learner dials out, so neither needs a name
/// other machines resolve.
/// @param mode The mode.
/// @return True when other machines dial this node's consensus port.
[[nodiscard]] bool ModeServesConsensusToPeers(Cluster::NodeMode mode) noexcept;

/// Whether @p cfg serves the scheduler: formed, and its mode's row says `Serves`.
/// @param cfg The configuration.
/// @return True when the scheduler tier belongs here.
[[nodiscard]] bool ServesScheduler(NodeConfig const& cfg) noexcept;

/// Whether @p cfg serves the enrollment surface: it runs consensus -- there is a cluster to be
/// admitted to -- AND a scheduler tier was built, without which the responder has nothing to admit
/// through. The second half is a runtime fact about what was BUILT, which the caller states. And
/// not while its consensus is confined to this machine (`ConsensusConfinedToThisMachine`): the
/// member it admitted would be told to dial an address that reaches only itself.
///
/// One predicate for `main`'s two sites -- the surface and what `NodeStatus` reports, which must not
/// disagree, or a window no verb can act on is reported open -- and for the formation harness, so the
/// surface a test builds is the one production builds: a fixture that gave every body a responder
/// answered `Enroll` on a learner, which production never does. No key file is a clause: since #178
/// an approval hands over no key.
///
/// **And the cluster it would admit into is one its own `--fleet-id` admits, under its own key.** A
/// node pinned to another fleet is on its way there, one whose key its pin does not name signs answers
/// its pinned joiners refuse, so a machine it admitted would found a fleet its own pin then
/// refuses at its next start -- and an unpinned machine that tie-breaks towards it would be left in
/// a cluster about to be abandoned. So it records nobody: a joiner reads the family's `NoCluster`
/// and gives the join up, while the pinned node asks the fleet it is pinned to.
/// @param cfg The configuration.
/// @param schedulerRuns Whether this node's scheduler tier was built.
/// @return True when all three hold: exactly when `EnrollmentAbsenceOf` names no reason.
[[nodiscard]] bool ServesEnrollment(NodeConfig const& cfg, bool schedulerRuns);

/// Why @p cfg serves no enrollment surface, or nothing when it serves one: the ONE derivation
/// `ServesEnrollment` answers from, so the surface and the words a joiner is refused with
/// (`EnrollmentAbsenceTable`) cannot disagree about whether, or why.
///
/// In the order the clauses are asked: consensus (and, when it does not run, whether it stood down
/// for want of an address rather than being closed), the scheduler tier, then the pin -- one pinned
/// to another cluster told apart from one pinned to its own under keys that are not its own, through
/// the one predicate, because the two remedies are on different machines.
/// @param cfg The configuration.
/// @param schedulerRuns Whether this node's scheduler tier was built.
/// @return The reason, or nothing when the surface is served.
[[nodiscard]] std::optional<EnrollmentAbsence> EnrollmentAbsenceOf(NodeConfig const& cfg, bool schedulerRuns);

/// Where a supervisor handed this node's `0xFC` surface over (socket activation), read off the
/// adopted socket. Only the socket can say it: the unit chose the address AND the port, so
/// `--listen-node` then describes neither.
struct ActivatedEndpoint
{
    std::string host;      ///< The bound address: the wildcard for `ListenStream=<port>`, else the one address.
    std::uint16_t port {}; ///< The bound port.
};

/// The handed-over endpoint, or disengaged when the node binds its own.
using ActivatedNodeEndpoint = std::optional<ActivatedEndpoint>;

/// What a caller that cannot see a handed-over socket passes, and what it means: the
/// configuration's own answer. A startup rule is such a caller -- it judges the command line
/// before anything is adopted, and at an install nothing ever is.
inline constexpr std::nullopt_t AsConfigured = std::nullopt;

/// Where this node's worker registers.
///
/// Its own node port for a mode that serves a scheduler -- it leads its own cluster, or it is a
/// voter whose scheduler redirects to the leader (`NotLeader`) when it does not -- the fleet's
/// for a mode that serves none, and none for an unformed configuration.
///
/// **Its own node port is the one it SERVES**, dialled at the address it binds -- loopback for a
/// wildcard bind, the address itself for one bound to a single address. Under socket activation
/// both halves are the socket the supervisor handed over (@p activated), never `--listen-node`'s:
/// the packaged unit listens on 6676 while the flag's default is 6674, and a worker registering
/// there is never sent a job -- or registers with whatever else holds 6674, a `fastcached` among
/// them -- and a unit bound to one address answers there alone, so loopback would reach nothing.
/// @param cfg The configuration.
/// @param activated Where a supervisor handed the node surface over, or `AsConfigured`.
/// @return The endpoints, in the order they are tried.
[[nodiscard]] std::vector<std::string> SchedulersOf(NodeConfig const& cfg, ActivatedNodeEndpoint const& activated);

/// The `0xFC` endpoints @p state records for its voters, in the state's order -- every voter but
/// @p self, and none whose record holds no endpoint yet.
///
/// Voters, because only a voter serves a scheduler; a learner's recorded endpoint is where its
/// compile port answers, never a scheduler to register with.
/// @param state An applied state.
/// @param self This node's id.
/// @return The endpoints.
[[nodiscard]] std::vector<std::string> RecordedSchedulersOf(Cluster::ClusterState const& state, std::string_view self);

/// Where this node's worker and presence loop register, re-read at every round
/// (`SchedulerLink::Over`).
///
/// A node that serves a scheduler registers at its own (`SchedulersOf`), whatever the state says.
/// One that serves none registers at the voters its APPLIED state records (`RecordedSchedulersOf`),
/// told every applied state by the consensus tier's apply callback (`StartConsensusOrExplain`) -- so
/// a voter that moves its `0xFC` endpoint, proven, is reached at the next round rather than after a
/// reform or a restart (T26's carry). Until the state records one, and on a node that runs no
/// consensus, the formation's answer (`SchedulersOf`): the voters the approval remembered.
///
/// **The state REPLACES the formation's list rather than joining it**: a voter that moved left its old
/// endpoint behind in the formation record, and a round that walked both would pay the dead one's
/// connect timeout whenever the live one failed first. Thread-safe: told on the consensus apply
/// thread, read on each loop's own.
class AppliedSchedulers final: public ISchedulerEndpointSource
{
  public:
    /// @param cfg The configuration, its formation applied; read here and never kept.
    /// @param activated Where a supervisor handed the node surface over, or `AsConfigured`.
    AppliedSchedulers(NodeConfig const& cfg, ActivatedNodeEndpoint const& activated);

    /// Take what @p state records.
    /// @param state The state the cluster just applied.
    void Applied(Cluster::ClusterState const& state);

    /// @copydoc ISchedulerEndpointSource::Current
    [[nodiscard]] std::vector<std::string> Current() const override;

  private:
    std::string _self;                   ///< This node's id, never a scheduler it registers with.
    bool _servesScheduler;               ///< Whether it registers at its own scheduler, whatever is applied.
    std::vector<std::string> _formation; ///< `SchedulersOf`'s answer: the fallback.
    mutable std::mutex _lock;            ///< Guards `_recorded`.
    std::vector<std::string> _recorded;  ///< The applied state's voters' endpoints; empty until one is.
};

/// The members consensus starts with.
///
/// This node alone where it runs its own cluster (solitary, pending, and a voter that founded the
/// fleet); the approved roster otherwise, never both -- and never this node itself in a fleet it
/// joined, whose seat is the leader's to replicate.
/// @param cfg The configuration.
/// @return The members; empty for an unformed configuration.
[[nodiscard]] std::vector<Cluster::MemberSpec> BootstrapMembersOf(NodeConfig const& cfg);

/// The seat a member of `BootstrapMembersOf` holds, which the `MemberSpec` it is carried as does not.
///
/// From the approved roster in a fleet this node joined; a voter otherwise, which is the one member
/// a founder starts with -- itself.
/// @param cfg The configuration the members were read from.
/// @param id The member.
/// @return Its seat.
[[nodiscard]] Cluster::MemberSeat SeatInFormation(NodeConfig const& cfg, std::string_view id);

/// Why the formation keeps the Raft surface closed, for `--print-surfaces`.
/// @param cfg The configuration.
/// @return The trailing column, or nothing when the formation is not why.
[[nodiscard]] std::optional<std::string> RaftClosedByFormation(NodeConfig const& cfg);

/// What a node's state directory holds about its formation, read and never written.
struct KeptFormation
{
    std::optional<Cluster::FormationRecord> record; ///< The record, DISENGAGED when none was ever written.
    Cluster::FleetEndpoints remembered;             ///< The fleet endpoints last known; empty when none are.
};

/// What a node that is already running says when the record it runs by is no longer in its state
/// directory: a reload declines, and a reform refuses (`ReadoptFormation`). Lost state, never a first
/// start -- only a start mints one.
inline constexpr std::string_view FormationRecordGone =
    "the formation record this node runs by is gone from its state directory";

/// Read what the state directory holds, writing nothing.
///
/// Every invocation reads it -- a one-shot verb and an install as well as a start -- so each is
/// judged by the mode it will run in. A record that cannot be read is a refusal; the endpoints
/// file is a hint, and no state of it refuses anything.
/// @param store Where the record is kept.
/// @param endpoints Where the fleet endpoints are remembered.
/// @return What is kept, or why the record cannot be read.
[[nodiscard]] std::expected<KeptFormation, std::string> ReadKeptFormation(Cluster::IFormationStore const& store,
                                                                          Cluster::FleetEndpointsFile& endpoints);

/// The record a configuration is judged by before a start has minted one.
///
/// The kept record, or the solitary one the first start WILL mint, with no cluster id: an
/// install, a `--print-surfaces` and a start's own startup rules then agree with what the node
/// does, rather than judging a configuration no mode shaped.
/// @param kept What the state directory holds.
/// @return The record to apply.
[[nodiscard]] Cluster::FormationRecord ProspectiveRecord(KeptFormation const& kept);

/// Shape @p cfg by what its state directory keeps, as `main` shapes every configuration before any
/// verb or rule is asked: by the `ProspectiveRecord` of @p kept.
///
/// **And where no record can shape it, @p cfg is told WHY** (`NodeConfig::formationUnread`): a reading
/// the state directory refused, or a record `ApplyFormation` refused. Its mode is then unknown, and
/// the mode line and the `--raft-self` row name that refusal (`FormationAbsenceOf`) -- never a flag,
/// which that row once told an operator who had typed it was missing.
/// @param cfg The configuration to shape.
/// @param kept What `ReadStateDirectoryFormation` read, or why it could not.
/// @return Nothing, or why no record shaped @p cfg.
[[nodiscard]] std::expected<void, FormationUnread> ShapeByKeptFormation(
    NodeConfig& cfg, std::expected<KeptFormation, FormationUnread> const& kept);

/// Why no formation record shaped @p cfg: ONE sentence, which `--print-surfaces`' mode line and the
/// startup rules both print, so the two cannot name different causes.
/// @param cfg A configuration no record shaped.
/// @return `no formation record shaped this configuration`, and the reading's refusal where there was one.
[[nodiscard]] std::string FormationAbsenceOf(NodeConfig const& cfg);

/// The record a starting node runs by: the kept one, or a solitary one minted and SAVED first.
///
/// Only an ABSENT record mints, and the mint is written before the node acts on it, so a start
/// that stops here leaves either nothing or a whole record.
/// @param kept What the state directory holds.
/// @param store Where a minted record is kept.
/// @param random Where a minted cluster id comes from.
/// @param wall The clock a minted cluster's creation time is read from.
/// @return The record, or why none could be had.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> KeepFormation(KeptFormation const& kept,
                                                                                 Cluster::IFormationStore& store,
                                                                                 ISecureRandom& random,
                                                                                 core::platform::IWallClock const& wall);

/// The first line `--print-surfaces` prints: which mode shaped this configuration.
/// @param cfg The configuration.
/// @return `mode: <name>`, with why when no record has been minted.
[[nodiscard]] std::string DescribeFormationMode(NodeConfig const& cfg);

} // namespace FastCache::Node
