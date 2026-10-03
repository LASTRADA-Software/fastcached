// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeMachineStanding.hpp"
#include "NodeStatusResponder.hpp"

#include <FastCache/Cluster/Roster.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// @file MachineStandingTestUtils.hpp
/// The one stated `Node::IMachineStandingSource`, and the one silent `Node::INodeStatusSource`, for
/// every node test that builds a `NodeStatusResponder`: shared rather than written per file, so no
/// copy answers a roster or a pending id differently from the one the handler's own cases are
/// written against.

/// A node that reports nothing about itself, for a case whose subject is not what `node-status`
/// says: a membership gate answered before any of it is read, or a counter snapshot that never
/// reads it.
class SilentNodeStatus final: public Node::INodeStatusSource
{
  public:
    [[nodiscard]] CompileCacheWire::NodeStatusFields Describe() const override
    {
        return {};
    }
};

/// A standing source a case states outright: the roster it holds, if any, and the ids its
/// enrollment window is waiting on.
class FixedStanding final: public Node::IMachineStandingSource
{
  public:
    /// @param roster The roster this node holds; nothing for one that holds none.
    /// @param pending The ids the enrollment window holds undecided requests under.
    explicit FixedStanding(std::optional<Cluster::Roster> roster = std::nullopt, std::vector<std::string> pending = {}):
        _roster { std::move(roster) },
        _pending { std::move(pending) }
    {
    }

    [[nodiscard]] std::optional<Cluster::Roster> Roster() const override
    {
        return _roster;
    }

    [[nodiscard]] bool Pending(std::string_view id) const override
    {
        return std::ranges::contains(_pending, id);
    }

  private:
    std::optional<Cluster::Roster> _roster;
    std::vector<std::string> _pending;
};

} // namespace FastCache::Testing
