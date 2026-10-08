// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <ranges>
#include <vector>

namespace FastCache::Testing
{

/// @file VerbFamilies.hpp
/// The verbs of one family, as `OpTable` lists them, for a case that sweeps a whole family.
///
/// Shared rather than written per file: a sweep is only as good as the set it walks, and a private
/// copy that drifted from `OpTable` would sweep a smaller set and stay green.

/// Every `OpTable` row of @p family, in table order.
/// @param family The verb family.
/// @return Its rows; empty for a family no row names.
[[nodiscard]] inline std::vector<CompileCacheWire::OpDescriptor> OpsOfFamily(CompileCacheWire::VerbFamily family)
{
    auto rows = CompileCacheWire::OpTable
                | std::views::filter([family](CompileCacheWire::OpDescriptor const& row) { return row.family == family; });
    return { rows.begin(), rows.end() };
}

} // namespace FastCache::Testing
