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
/// The two moves that change which cluster a node runs, and resuming one a crash interrupted.
///
/// **A node must have DISSOLVED its solitary cluster before it adopts a roster**: every node
/// bootstraps a cluster of itself, and what keeps a joiner from refusing every leader but its own is
/// that its solitary store is ARCHIVED -- moved out of the state directory's root and kept -- before it
/// starts as a learner. Each move writes the record that describes where it is going BEFORE the first
/// file moves, with `archivePending` naming the store still in the root; a crash anywhere after that
/// leaves a record `ResumeFormation` finishes at the next start, before consensus opens the directory.

/// Whether @p roster, as a fleet handed it over, admits @p self into the fleet @p target PROVED: it
/// decodes, records this node's id under this node's key in the role it asked for, AND records a
/// member under the key that proved the fleet's summary -- the member that spoke, where it named
/// itself.
///
/// The ONE question a pending node asks of an admission, whether it is deciding what a poll said or
/// about to dissolve on it. The second half is what trust on first use trusts: the join was decided
/// on a summary a key signed, and the `Enroll` exchange after it is unauthenticated, so a roster that
/// names none of the proven fleet's keys came from whoever answered at that endpoint -- and joining
/// it would hand the node's cluster, and later its forget, to them.
/// @param self Who this node asked to be admitted as.
/// @param target The fleet it asked, as it was proven.
/// @param roster The roster's bytes, as received.
/// @return Nothing, or why the admission is not believed.
[[nodiscard]] std::expected<void, std::string> CheckAdmission(JoinerIdentity const& self,
                                                              Cluster::JoinTarget const& target,
                                                              std::span<std::byte const> roster);

/// Take on the fleet a pending node was admitted to.
///
/// In order, stopping at the first failure: `CheckAdmission` (a refusal writes nothing); save the
/// learner record, the fleet's roster and age in it and `archivePending` naming the solitary cluster;
/// remember the fleet's voters as seeds (a hint -- a failure is logged, never fatal); archive the
/// solitary store; save the record without `archivePending`.
/// @param pending The pending node's record; its `joining` names the fleet.
/// @param roster The roster the fleet handed over.
/// @param self Who this node asked to be admitted as.
/// @param store Where the record is kept.
/// @param archiver What moves the solitary store out of the root.
/// @param endpoints Where the fleet's voters are remembered.
/// @param logger Where a failure to remember them is said.
/// @return The learner record as finally written, or why the move stopped; the store then holds
///         whatever was written before it did.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> DissolveInto(Cluster::FormationRecord const& pending,
                                                                                std::span<std::byte const> roster,
                                                                                JoinerIdentity const& self,
                                                                                Cluster::IFormationStore& store,
                                                                                IStoreArchiver& archiver,
                                                                                Cluster::FleetEndpointsFile& endpoints,
                                                                                ILogger& logger);

/// Leave the cluster that forgot this node, and start again alone.
///
/// Saves a solitary record under a NEWLY minted cluster id -- never the old one back -- with
/// `archivePending` naming the cluster it leaves; archives that cluster's store; saves the record
/// without `archivePending`. Nothing of the fleet is kept: not its roster, and not the memo of having
/// asked it, since a forget outranks an observation and that memo is evidence a split heals on.
/// @param forgotten The record of the node that was forgotten.
/// @param store Where the record is kept.
/// @param archiver What moves the left cluster's store out of the root.
/// @param random Where the new cluster id comes from.
/// @param wall What the new cluster's creation time is read from.
/// @return The solitary record as finally written, or why the move stopped; the store then holds
///         whatever was written before it did.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> ArchiveAndMint(Cluster::FormationRecord const& forgotten,
                                                                                  Cluster::IFormationStore& store,
                                                                                  IStoreArchiver& archiver,
                                                                                  ISecureRandom& random,
                                                                                  core::platform::IWallClock const& wall);

/// Finish, at start, a move a crash interrupted: archive the store `archivePending` names and save
/// the record without it.
///
/// Run before any consensus tier opens the directory, so a learner never opens the solitary log it
/// was leaving. The archive is idempotent, so a store a crash left half moved is finished.
/// @param record The record the start read.
/// @param store Where the record is kept.
/// @param archiver What moves the store out of the root.
/// @return The record to start from, unchanged when nothing was pending; or a start refusal naming
///         the cluster and what could not be moved.
[[nodiscard]] std::expected<Cluster::FormationRecord, std::string> ResumeFormation(Cluster::FormationRecord record,
                                                                                   Cluster::IFormationStore& store,
                                                                                   IStoreArchiver& archiver);

} // namespace FastCache::Node
