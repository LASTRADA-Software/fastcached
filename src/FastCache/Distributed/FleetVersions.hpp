// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/WorkerRegistry.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Distributed
{

/// One build, and the machines running it.
struct VersionGroup
{
    std::string version;               ///< As `VersionString` spells it.
    std::vector<std::string> machines; ///< Display name, else endpoint; sorted.
};

/// Which builds serve this fleet, as the leader sees it -- what `mixed-node-versions` reports.
struct FleetVersionSpread
{
    /// Most machines first, then by version, so the build to upgrade TO reads first.
    std::vector<VersionGroup> groups;
    /// Machines that stated no version. Counted apart and never grouped: *did not say* is not a build.
    std::size_t unstated { 0 };

    /// Whether more than one build serves the fleet.
    /// @return True when two or more versions were stated.
    [[nodiscard]] bool Mixed() const noexcept
    {
        return groups.size() > 1;
    }
};

/// Group the fleet's machines by the build they run.
///
/// Over `NodeReports()` and never over registry entries: a machine with two toolchains is one
/// machine running one build. Machines of ANOTHER wire never appear -- they are refused
/// `unsupported-version` before they can announce -- so every group here speaks this wire, which is
/// exactly why nothing else reports a mix: nothing refuses it.
///
/// The leader's own build is always included, because a leader on the odd build with every worker
/// agreeing is a mix too; a report under @p ownEndpoint is its own announcement and is skipped, so it
/// is not counted twice. That endpoint is the one this node resolved at start: after an `--advertise`
/// reload the leader may be counted once more in its own group, which changes a count and never the
/// SET of builds, which is what decides.
/// @param nodes Every live machine, as `WorkerRegistry::NodeReports()` lists it.
/// @param ownVersion The leader's own build.
/// @param ownEndpoint Where the leader answers; its name in its own group.
/// @return The groups and how many machines did not say.
[[nodiscard]] FleetVersionSpread SpreadOfVersions(std::span<NodeReport const> nodes,
                                                  std::string_view ownVersion,
                                                  std::string_view ownEndpoint);

} // namespace FastCache::Distributed
