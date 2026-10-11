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
/// It also holds `ReportsConsensus`, the one predicate the `node` record and the `node` panel
/// share for whether a node has a leader to report, and `NamedLeader`, the one reading of WHICH
/// leader, so the two cannot disagree about either.

/// The components @p mask names, as a comma-separated list.
///
/// **An empty MASK renders as the word `none` rather than as an absent cell**: a
/// node that runs no component is a reading, not a missing one, and the two must
/// not render alike.
/// @param mask What the node reported.
/// @return The list.
[[nodiscard]] std::string DescribeComponents(std::uint32_t mask);

/// Whether @p status is of a node running consensus: its `Consensus` component bit, or a scheduler
/// role, which only a consensus node reports.
///
/// **The one answer** to *does this node have a leader to report* that both the `node` record's
/// `leader` row and the `node` panel's consensus lines key on, so the two cannot disagree (#1641).
/// The component bit rather than the consensus standing: a learner's standing is absent until its
/// tier attaches, while the bit says from the first reply that it runs consensus.
///
/// Named `ReportsConsensus` rather than `RunsConsensus` so it is not misread as the node's own
/// `Node::RunsConsensus`, which decides from the CONFIGURATION; this reads what a reply SAID.
/// @param status The status, or nullptr when none was read.
/// @return False for nullptr.
[[nodiscard]] bool ReportsConsensus(CompileCacheWire::NodeStatusFields const* status) noexcept;

/// What a leading node's leader reads: it names no endpoint because it IS the leader (#1647).
///
/// The one spelling the `node` record's `leader` row and the `node` panel's leader line both draw,
/// so the two cannot say it differently.
inline constexpr std::string_view SelfLeader = "this node";

/// The leader @p status names, as a person reads it (#1647).
///
/// **Decides only the reading**: whether a leader is reported at all is `ReportsConsensus`'s
/// question, asked first by every caller. An empty endpoint is a reading, and which one depends on
/// the role beside it: beside `Leader` this node leads, so it is `SelfLeader`; anywhere else (an
/// election, a learner that has heard from no leader, a known leader whose member record names no
/// scheduling endpoint) nothing is named, which a caller renders as its absent marker.
/// @param status The node's status.
/// @return The leader's endpoint, `SelfLeader` on the leader itself, or nullopt when no leader is
///         named. Owned, so it outlives @p status.
[[nodiscard]] std::optional<std::string> NamedLeader(CompileCacheWire::NodeStatusFields const& status);

/// What to call one toolchain-survey state.
/// @param state The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view NameOfToolchainState(CompileCacheWire::ToolchainState state) noexcept;

/// What to call one scheduler role.
/// @param role The wire tag.
/// @return A stable lower-case name.
[[nodiscard]] std::string_view NameOfSchedulerRole(CompileCacheWire::WireSchedulerRole role) noexcept;

/// What to call where a node's idea of the fleet's shared cache comes from.
/// @param source The wire tag.
/// @return A stable lower-case name, one token in every format.
[[nodiscard]] std::string_view NameOfSharedCacheSource(CompileCacheWire::WireSharedCacheSource source) noexcept;

/// What to call how a node's last dealing with the fleet's shared cache went.
/// @param state The wire tag.
/// @return A stable lower-case name, one token in every format.
[[nodiscard]] std::string_view NameOfSharedCacheState(CompileCacheWire::WireSharedCacheState state) noexcept;

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
