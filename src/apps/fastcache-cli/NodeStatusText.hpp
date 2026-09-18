// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/// What a person is told when a node's conditions ask for nothing: the words `node` prints and the
/// `node` panel draws, one spelling so neither can say it differently.
inline constexpr std::string_view NoConditionsRaised = "none raised";

/// One condition a node reported that asks for an operator's attention, as a person is told of it.
///
/// **It OWNS its words.** A view into the row would dangle the moment a caller passed a status it
/// had built on the spot -- `status ? status->conditions : std::nullopt` is a copy that dies at the
/// end of the statement -- and the reading would then be of freed memory rather than a wrong word.
struct ConditionMention
{
    std::string id;          ///< Which condition, as the node spelled it.
    std::string persistence; ///< `latched` or `live`, as the node spelled it.
    /// The state, where it is anything but a plain `raised` -- `undecided`, or a word from a newer
    /// node -- which is a different reason to look and must not read as a plain raise.
    std::optional<std::string> unusualState;
    /// The severity, or nothing for a word this build does not know; a colour would then be a guess.
    std::optional<CompileCacheWire::ConditionSeverity> severity;
};

/// Every condition a node reported that asks for attention, in the order it sent them (#1364).
///
/// **The one reading of a status's conditions** every person-facing surface of this client draws
/// from -- the `node` record's line and the `node` panel's -- so the two cannot disagree about which
/// rows ask for attention. It walks what ARRIVED and restates no list of conditions: a row a newer
/// node reports is mentioned as that node wrote it.
/// @param conditions What the node carried.
/// @return The mentions, empty when nothing asks; nullopt when the node carried no conditions,
///         which is ABSENT and never *none raised*.
[[nodiscard]] std::optional<std::vector<ConditionMention>> ConditionsAskingForAttention(
    std::optional<std::vector<CompileCacheWire::NodeConditionFields>> const& conditions);

/// One line saying what a node's conditions come to (#1364).
///
/// **Three answers, and the third is not the second.** A node with nothing asking for attention
/// says `NoConditionsRaised`; a node with something says each such condition by its id and its
/// persistence -- `scratch-root-unmappable (latched), enrollment-window-open (live)` -- because a
/// latched row will still be there after the operator has done everything a live one needs, and
/// the two must not read alike; and a node that carried NO list at all is ABSENT, answered here as
/// nothing, for the caller to render as its absent marker. A node too old to carry conditions has
/// not said *none*.
///
/// A row whose state is not a plain `raised` -- `undecided`, or a word from a newer node -- says so
/// beside its persistence, since it asks for attention for a different reason.
/// @param conditions What the node carried.
/// @return The line, or nullopt when the node carried no conditions.
[[nodiscard]] std::optional<std::string> DescribeConditions(
    std::optional<std::vector<CompileCacheWire::NodeConditionFields>> const& conditions);

} // namespace FastCache::Cli
