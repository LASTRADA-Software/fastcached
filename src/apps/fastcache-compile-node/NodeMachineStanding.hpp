// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollmentWindow.hpp"
#include "NodeRoster.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <optional>
#include <string_view>

namespace FastCache::Node
{

/// @file NodeMachineStanding.hpp
/// What `explain-admission <machine>` reads a machine's standing from.

/// The roster this node holds, and whether its enrollment window is waiting on an id.
///
/// A seam rather than the `NodeRoster` itself, so the handler's cases state a roster outright.
class IMachineStandingSource
{
  public:
    IMachineStandingSource() = default;
    IMachineStandingSource(IMachineStandingSource const&) = delete;
    IMachineStandingSource(IMachineStandingSource&&) = delete;
    IMachineStandingSource& operator=(IMachineStandingSource const&) = delete;
    IMachineStandingSource& operator=(IMachineStandingSource&&) = delete;
    virtual ~IMachineStandingSource() = default;

    /// @return The roster this node holds, or nothing when it holds none -- in which case a
    ///         machine question has no answer here, which is not the same as `Unknown`.
    [[nodiscard]] virtual std::optional<Cluster::Roster> Roster() const = 0;

    /// @param id A machine id.
    /// @return Whether this node's enrollment window holds an undecided request under @p id.
    [[nodiscard]] virtual bool Pending(std::string_view id) const = 0;
};

/// The production standing: the roster `NodeRoster` holds, and the enrollment window this node
/// serves, when it serves one.
class NodeMachineStanding final: public IMachineStandingSource
{
  public:
    /// @param roster The roster this node verifies grants against; must outlive this.
    /// @param window The enrollment window, or null on a node that serves none; must outlive this.
    NodeMachineStanding(NodeRoster const& roster, EnrollmentWindow const* window) noexcept:
        _roster { roster },
        _window { window }
    {
    }

    [[nodiscard]] std::optional<Cluster::Roster> Roster() const override
    {
        return _roster.HeldRoster();
    }

    [[nodiscard]] bool Pending(std::string_view id) const override
    {
        if (_window == nullptr)
            return false;
        auto const entry = _window->Find(id);
        return entry.has_value() && entry->decision == CompileCacheWire::EnrollmentDecision::Pending;
    }

  private:
    NodeRoster const& _roster;
    EnrollmentWindow const* _window;
};

} // namespace FastCache::Node
