// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include <apps/fastcache-compile-node/NodeConditions.hpp>
#include <core/Ranges.hpp>

/// @file NodeConditionFakes.hpp
/// Reading a node's condition rows the way every surface reads them.
///
/// Apart from `FormationFakes.hpp` because this one reaches an app header, and that one is
/// included by `src/FastCache` tests, which must not.
namespace FastCache::Testing
{

/// The detail a condition row carries right now, read through the snapshot every surface renders.
/// @param conditions The node's conditions.
/// @param condition Which row.
/// @return Its detail; empty when the row carries none.
[[nodiscard]] inline std::string DetailOf(Node::NodeConditions const& conditions, Node::NodeCondition condition)
{
    auto const id = Node::RowFor(condition).id;
    auto const rows = conditions.Snapshot();
    auto const* const row = core::findIfOrNull(rows, [&](auto const& fields) { return fields.id == id; });
    return row == nullptr ? std::string {} : row->detail;
}

} // namespace FastCache::Testing
