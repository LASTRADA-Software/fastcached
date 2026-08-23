// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Consensus::Membership
{

/// The cluster's member set, as it travels through the replicated log.
///
/// ## Why a log entry and not a setting
///
/// A member set that could be edited on each node independently is not a
/// configuration, it is a way to build two clusters that both think they are
/// one. Putting it in the log makes every node adopt the same changes in the
/// same order, under the same rules that already order everything else.
///
/// ## One server at a time
///
/// A change is restricted to adding **or** removing a single member (§4.3).
/// That restriction is what makes the change safe without joint consensus: any
/// majority of the old configuration and any majority of the new one must share
/// at least one member, so the two cannot elect different leaders in the same
/// term. Change two at once — three members to five, say — and
/// `{n1,n2}` is a majority of the old while `{n3,n4,n5}` is a majority of the
/// new, with nobody in common and nothing to stop both electing.
///
/// Joint consensus lifts that restriction and is deliberately not implemented:
/// it is a large, subtle mechanism, and a peer-to-peer compile fleet grows and
/// shrinks one machine at a time anyway.
///
/// ## The rule that surprises people
///
/// A node uses the **latest configuration in its log, committed or not**. That
/// looks unsafe and is the opposite: a configuration that only took effect once
/// committed could not be used to *reach* the commitment, because committing it
/// requires a quorum of the very set it describes. The consequence to keep in
/// mind is that an uncommitted change can be rolled back by a log truncation,
/// so the active configuration has to be re-derived from the log whenever the
/// log changes — it is not a value that only ever moves forward.

/// Encode a member set as an entry payload.
///
/// A length-prefixed field per member, over the grammar the wire already uses,
/// so a member id may contain any byte and there is no separator to escape.
/// @param members The member set, in any order.
/// @return The payload bytes.
[[nodiscard]] inline std::vector<std::byte> Encode(std::span<NodeId const> members)
{
    std::vector<std::span<std::byte const>> fields;
    fields.reserve(members.size());
    for (auto const& member: members)
        fields.push_back(WireFields::AsBytes(member));
    return WireFields::Encode(fields);
}

/// Decode a member set from an entry payload.
/// @param payload The entry's bytes.
/// @return The members, or nullopt when the payload is malformed.
[[nodiscard]] inline std::optional<std::vector<NodeId>> Decode(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitAll(payload);
    if (!fields.has_value())
        return std::nullopt;

    std::vector<NodeId> members;
    members.reserve(fields->size());
    for (auto const& field: *fields)
        members.emplace_back(WireFields::AsStringView(field));
    return members;
}

/// How one member set differs from another.
///
/// Named rather than reported as a count, because the two one-member cases are
/// the only legal ones and a caller that has to compare a number against 1 is a
/// caller that can compare it wrongly.
enum class ChangeShape : std::uint8_t
{
    Unchanged = 0, ///< The same set; nothing to do.
    AddedOne,      ///< Exactly one member gained.
    RemovedOne,    ///< Exactly one member lost.
    Unsafe,        ///< Anything else: more than one change, or both at once.
};

/// Classify the difference between two member sets.
/// @param from The current members.
/// @param to The proposed members.
/// @return What kind of change this would be.
[[nodiscard]] inline ChangeShape Classify(std::span<NodeId const> from, std::span<NodeId const> to)
{
    auto const contains = [](std::span<NodeId const> where, NodeId const& who) {
        return std::ranges::find(where, who) != where.end();
    };

    std::size_t added = 0;
    for (auto const& member: to)
        if (!contains(from, member))
            ++added;

    std::size_t removed = 0;
    for (auto const& member: from)
        if (!contains(to, member))
            ++removed;

    if (added == 0 && removed == 0)
        return ChangeShape::Unchanged;
    if (added == 1 && removed == 0)
        return ChangeShape::AddedOne;
    if (added == 0 && removed == 1)
        return ChangeShape::RemovedOne;
    return ChangeShape::Unsafe;
}

/// Whether `members` is a set a cluster could operate as.
///
/// Checked where a change is proposed rather than trusted from the log, because
/// a configuration entry is replicated to every node and a set that is empty or
/// holds a duplicate would be adopted by all of them at once.
/// @param members The proposed member set.
/// @return Nothing on success, or why it was refused.
[[nodiscard]] inline std::expected<void, ConsensusError> Validate(std::span<NodeId const> members)
{
    if (members.empty())
        return std::unexpected { InvalidConfiguration("a cluster must have at least one member") };

    for (auto const& member: members)
        if (member.empty())
            return std::unexpected { InvalidConfiguration("a member id may not be empty") };

    // A duplicate would make one node count twice toward a quorum, which is a
    // quorum that does not exist -- the same defect the vote tally is a set to
    // avoid, reached through configuration instead.
    auto sorted = std::vector<NodeId> { members.begin(), members.end() };
    std::ranges::sort(sorted);
    auto const duplicate = std::ranges::adjacent_find(sorted);
    if (duplicate != sorted.end())
        return std::unexpected { InvalidConfiguration(std::format("member {} appears more than once", *duplicate)) };

    return {};
}

} // namespace FastCache::Consensus::Membership
