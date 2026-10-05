// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/NodeConfig.hpp>
#include <apps/fastcache-compile-node/NodeFormation.hpp>

/// @file NodeFormationFakes.hpp
/// Shape a test configuration the way `main` shapes every configuration it judges.
///
/// Shared rather than written per file: a configuration no record shaped runs no consensus, so
/// every case about a node that runs one has to be shaped first -- and a copy that shaped it by
/// a different record (a minted cluster id, say) would change the lease cluster field under
/// cases that are about something else entirely.
namespace FastCache::Testing
{

/// @p cfg shaped as `main` shapes it before the first start has minted anything: by the
/// solitary record that start WILL mint, with no cluster id yet (`Node::ProspectiveRecord`).
///
/// What a parse, an install, a `--print-surfaces` and the startup rules all see, and it leaves
/// `clusterId` as the flags left it.
/// @param cfg The configuration.
/// @return It, shaped.
[[nodiscard]] inline Node::NodeConfig FirstStart(Node::NodeConfig cfg)
{
    auto const applied = Node::ApplyFormation(cfg, Cluster::FormationRecord {}, Cluster::FleetEndpoints {});
    REQUIRE(applied.has_value());
    return cfg;
}

/// Shape @p cfg in place, as `FirstStart` does.
/// @param cfg The configuration to shape.
inline void ShapeAsFirstStart(Node::NodeConfig& cfg)
{
    cfg = FirstStart(std::move(cfg));
}

/// @p cfg shaped as a LEARNER of a fleet whose schedulers answer at @p schedulers, in order --
/// the route a serving node's worker and presence take to a scheduler (`Node::SchedulersOf`),
/// never `--scheduler`, which a serving node is refused and which aims the one-shot verbs alone.
///
/// The view `ApplyFormation` writes for a learner whose remembered voters answer there, written
/// directly: the cases that take it are about the round that DIALS the list, and a roster would be
/// a second subject. Its cluster field is `cfg`'s own, so a case's lease pin is not moved under it.
/// @param cfg The configuration.
/// @param schedulers Where its fleet's schedulers answer.
/// @return It, shaped.
[[nodiscard]] inline Node::NodeConfig LearnerRegisteringWith(Node::NodeConfig cfg, std::vector<std::string> schedulers)
{
    cfg.formation = Node::NodeFormationView { .mode = Cluster::NodeMode::Learner,
                                              .clusterId = cfg.clusterId,
                                              .createdAtUnixSeconds = 0,
                                              .foundedHere = false,
                                              .fleetMembers = {},
                                              .fleetSchedulers = std::move(schedulers) };
    REQUIRE_FALSE(Node::ServesScheduler(cfg));
    return cfg;
}

} // namespace FastCache::Testing
