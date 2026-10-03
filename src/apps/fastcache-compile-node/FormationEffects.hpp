// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollClient.hpp"
#include "FormationController.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// @file FormationEffects.hpp
/// The two moves that change which cluster a node runs, and finishing one at the next start.
///
/// **A node must have DISSOLVED its solitary cluster before it adopts a roster**: every node
/// bootstraps a cluster of itself, and what keeps a joiner from refusing every leader but its own is
/// that its solitary store is ARCHIVED -- moved out of the state directory's root and kept -- before it
/// starts as a learner.
///
/// **A move RECORDS where the node is going and moves no file.** The record names the store still in
/// the root (`archivePending`), the move asks for a reform, and the store is moved only by
/// `ResumeFormation`, which the start -- and the reform, which is a start of the body -- runs BEFORE any
/// consensus tier opens the directory. Not at the decision, because the decision is taken while that
/// cluster's tier still RUNS: it holds its log open and rewrites its state and snapshot into the root,
/// so a store moved under it is a store still being written. On POSIX the rename succeeds and a vote
/// or a snapshot after it re-creates the old cluster's files in the root, after the record said the
/// archive was done -- a node starting on the cluster it left, with no crash needed.

/// Whether @p roster, as a fleet handed it over, admits @p self into the fleet @p target PROVED: it
/// decodes, records this node's id under this node's key in the role it asked for, AND records a
/// member under the key that proved the fleet's summary -- the member that spoke, where it named
/// itself.
///
/// The ONE question a pending node asks of an admission's CONTENT, whether it is deciding what a poll
/// said or about to dissolve on it. It is asked only of an admission whose signature already bound it
/// to the fleet (`Cluster::VerifyAdmission`): a roster is public -- every member's id and key rides
/// every beacon -- so nothing here can tell the fleet's answer from a copy, and nothing here tries.
/// What it asks is whether the fleet's own answer is one to join: this node recorded as it asked, and
/// the member the join was decided on still in it.
/// @param self Who this node asked to be admitted as.
/// @param target The fleet it asked, as it was proven.
/// @param roster The roster's bytes, as received.
/// @return Nothing, or why the admission is not believed.
[[nodiscard]] std::expected<void, std::string> CheckAdmission(JoinerIdentity const& self,
                                                              Cluster::JoinTarget const& target,
                                                              std::span<std::byte const> roster);

/// Take on the fleet a pending node was admitted to.
///
/// In order, stopping at the first failure: `CheckAdmission` (a refusal writes nothing); remember the
/// fleet's voters as seeds (a hint -- a failure is logged, never fatal), BEFORE the record, because
/// the record's save is where the move is judged and the judge derives the learner's schedulers from
/// that file; save the learner record, the fleet's roster and age in it, the key that signed the
/// admission, and `archivePending` naming the solitary cluster. The solitary store stays where it is:
/// `ResumeFormation` moves it before the learner's tier opens the directory.
/// @param pending The pending node's record; its `joining` names the fleet.
/// @param roster The roster the fleet handed over.
/// @param admittedBy The key the admission was verified under -- one this node PROVED at the endpoint
///                   it asked -- recorded as `FleetMembership::admittedBy`, which the pin judges.
/// @param self Who this node asked to be admitted as.
/// @param store Where the record is kept.
/// @param endpoints Where the fleet's voters are remembered.
/// @param logger Where a failure to remember them is said.
/// @return The learner record as written, `archivePending` naming the solitary cluster; or why nothing
///         was written.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> DissolveInto(Cluster::FormationRecord const& pending,
                                                                                std::span<std::byte const> roster,
                                                                                Ed25519PublicKey const& admittedBy,
                                                                                JoinerIdentity const& self,
                                                                                Cluster::IFormationStore& store,
                                                                                Cluster::FleetEndpointsFile& endpoints,
                                                                                ILogger& logger);

/// Leave a fleet that dissolved into another, for the survivor its order names.
///
/// ONE record: `archivePending` naming the cluster left, a NEWLY minted solitary cluster -- never the
/// left one reopened over an empty directory -- and `joining` naming the survivor as the order proved
/// it, its speaker left EMPTY so `CheckAdmission` requires a member under the key that proved it and
/// the poll proves the survivor's leader before it asks. The memo of asking the survivor is added and
/// every other memo is KEPT: this node was not forgotten, and its history is still evidence. From
/// there, the approval is a first join's.
/// @param member The record of the voter or learner whose fleet dissolved.
/// @param order The fleet's order.
/// @param store Where the record is kept.
/// @param random Where the new cluster id comes from.
/// @param wall What the new cluster's creation time, and the ask's, are read from.
/// @return The pending record as written; or why nothing was written.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> LeaveForSurvivor(Cluster::FormationRecord const& member,
                                                                                    Cluster::DissolveOrder const& order,
                                                                                    Cluster::IFormationStore& store,
                                                                                    ISecureRandom& random,
                                                                                    core::platform::IWallClock const& wall);

/// Leave the cluster that forgot this node, and start again alone.
///
/// Saves a solitary record under a NEWLY minted cluster id -- never the old one back -- with
/// `archivePending` naming the cluster it leaves, whose store `ResumeFormation` moves before the new
/// cluster's tier opens the directory. Nothing of the fleet is kept: not its roster, and not the memo
/// of having asked it, since a forget outranks an observation and that memo is evidence a split is
/// told on.
/// @param forgotten The record of the node that was forgotten.
/// @param store Where the record is kept.
/// @param random Where the new cluster id comes from.
/// @param wall What the new cluster's creation time is read from.
/// @return The solitary record as written, `archivePending` naming the left cluster; or why nothing
///         was written.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> ArchiveAndMint(Cluster::FormationRecord const& forgotten,
                                                                                  Cluster::IFormationStore& store,
                                                                                  ISecureRandom& random,
                                                                                  core::platform::IWallClock const& wall);

/// Finish, at start, the move the record describes: archive the store `archivePending` names and save
/// the record without it.
///
/// **The ONLY place a store is moved**, and run before any consensus tier opens the directory: then
/// nothing holds the store, and a learner never opens the solitary log it was leaving. Every move ends
/// here -- at the start after a crash, and at the reform after a move. The archive is idempotent, so a
/// store a crash left half moved is finished.
/// @param record The record the start read.
/// @param store Where the record is kept.
/// @param archiver What moves the store out of the root.
/// @return The record to start from, unchanged when nothing was pending; or a start refusal naming
///         the cluster and what could not be moved.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> ResumeFormation(Cluster::FormationRecord record,
                                                                                   Cluster::IFormationStore& store,
                                                                                   IStoreArchiver& archiver);

} // namespace FastCache::Node
