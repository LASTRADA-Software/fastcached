// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

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

} // namespace FastCache::Testing
