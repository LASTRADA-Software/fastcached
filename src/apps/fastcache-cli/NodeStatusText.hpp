// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// @file NodeStatusText.hpp
/// What a node's status is called when a person reads it: the one spelling `node-status` reports
/// and the `node` panel draws, so the two cannot name one component or one role differently.

/// The components @p mask names, as a comma-separated list.
///
/// **An empty MASK renders as the word `none` rather than as an absent cell**: a
/// node that runs no component is a reading, not a missing one, and the two must
/// not render alike.
/// @param mask What the node reported.
/// @return The list.
[[nodiscard]] std::string DescribeComponents(std::uint32_t mask);

/// What to call one toolchain-survey state.
/// @param state The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view NameOfToolchainState(CompileCacheWire::ToolchainState state) noexcept;

/// What to call one scheduler role.
/// @param role The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view NameOfSchedulerRole(CompileCacheWire::WireSchedulerRole role) noexcept;

} // namespace FastCache::Cli
