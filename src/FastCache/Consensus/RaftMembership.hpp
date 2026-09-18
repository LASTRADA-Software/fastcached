// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Consensus::Membership
{

/// The cluster's configuration, as it travels through the replicated log.
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
/// A change is restricted to ONE member moving (§4.3): added, removed, promoted from
/// learner to voter, or demoted from voter to learner. That restriction is what makes
/// the change safe without joint consensus: any majority of the old configuration's
/// VOTERS and any majority of the new one's must share at least one voter, so the two
/// cannot elect different leaders in the same term. Change two at once — three voters
/// to five, say — and `{n1,n2}` is a majority of the old while `{n3,n4,n5}` is a
/// majority of the new, with nobody in common and nothing to stop both electing.
///
/// Joint consensus lifts that restriction and is deliberately not implemented:
/// it is a large, subtle mechanism, and a peer-to-peer compile fleet grows and
/// shrinks one machine at a time anyway.
///
/// ## Learners, and what one change at a time means for them (#1449)
///
/// A learner is replicated to and counted by nothing, so the argument above is about
/// the VOTERS and a learner change touches no voter majority at all:
///
/// - **Adding or removing a learner changes no quorum**, so the old and new voter sets
///   are the same set and every majority of one is a majority of the other. Always
///   safe -- and still one change at a time, because the rule that a second change
///   waits for the first to commit is about a configuration a truncation can still
///   roll back, which a learner entry is as much as any other.
/// - **A promotion is a voter addition** and a **demotion is a voter removal**, so
///   each is the single-voter delta the paragraph above already argues for.
///
/// **One at a time is load-bearing for READS as well as for commitment**, and that is
/// restated here for voters because it is the half nobody re-derives. CheckQuorum --
/// what everything reading from a leader rests on -- consults the COMMITTED
/// configuration while a change is in flight (`RaftNode::HasQuorumContact`). That is
/// only safe because any majority of the old voters and any majority of the new share
/// one voter, which would have had to move to a higher term to elect a competitor and
/// would have stopped confirming this leader. Promoting and removing in one proposal is
/// two voter changes, so it is `Unsafe` for exactly that reason and not merely by rule.
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
///
/// ## Where the layout is versioned
///
/// Not here. This payload travels inside two containers that each carry a version of
/// their own -- a `FileRaftStorage` record and a `RaftWire` frame -- and it changes
/// only when BOTH move: `FileRaftStorage`'s `FormatVersion` so a store written in the
/// old layout reads as `UnsupportedFormatVersion` rather than as a configuration this
/// build misreads, and `RaftWire::CurrentVersion` so a peer that spells it the old way
/// is refused at the handshake. A third version inside the payload would be a third
/// thing that has to move with those two. `RaftMembership_test` pins the bytes, so a
/// layout change is a red test naming both.

/// Encode a list of ids as one field list.
///
/// A length-prefixed field per id, over the grammar the wire already uses, so an id
/// may contain any byte and there is no separator to escape.
/// @param ids The ids, in any order.
/// @return The encoded list.
[[nodiscard]] inline std::vector<std::byte> EncodeIds(std::span<NodeId const> ids)
{
    std::vector<std::span<std::byte const>> fields;
    fields.reserve(ids.size());
    for (auto const& id: ids)
        fields.push_back(WireFields::AsBytes(id));
    return WireFields::Encode(fields);
}

/// Decode a list of ids.
/// @param payload The encoded list.
/// @return The ids, or nullopt when the bytes are not a field list.
[[nodiscard]] inline std::optional<std::vector<NodeId>> DecodeIds(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitAll(payload);
    if (!fields.has_value())
        return std::nullopt;

    std::vector<NodeId> ids;
    ids.reserve(fields->size());
    for (auto const& field: *fields)
        ids.emplace_back(WireFields::AsStringView(field));
    return ids;
}

/// Encode a configuration as an entry payload: exactly two fields, the voters and
/// then the learners, each a nested id list.
///
/// Two NESTED lists rather than one list with a seat byte per member, because the two
/// sets are what every reader asks for and a per-member tag would have every reader
/// rebuild them.
/// @param configuration The configuration.
/// @return The payload bytes.
[[nodiscard]] inline std::vector<std::byte> Encode(Configuration const& configuration)
{
    auto const voters = EncodeIds(configuration.voters);
    auto const learners = EncodeIds(configuration.learners);
    return WireFields::Encode({ std::span<std::byte const> { voters }, std::span<std::byte const> { learners } });
}

/// Decode a configuration from an entry payload.
///
/// Exact about the arity: two fields or nothing. A payload of any other shape is not a
/// configuration this build wrote, and reading it as one would adopt a member set
/// nobody proposed.
/// @param payload The entry's bytes.
/// @return The configuration, or nullopt when the payload is malformed.
[[nodiscard]] inline std::optional<Configuration> Decode(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, 2);
    if (!fields.has_value())
        return std::nullopt;

    auto voters = DecodeIds((*fields)[0]);
    auto learners = DecodeIds((*fields)[1]);
    if (!voters.has_value() || !learners.has_value())
        return std::nullopt;

    return Configuration { .voters = *std::move(voters), .learners = *std::move(learners) };
}

/// How one configuration differs from another.
///
/// **Private: never transmitted or persisted**, so the enumerators carry no values.
///
/// Named rather than reported as a count, because the one-member cases are the
/// only legal ones and a caller that has to compare a number against 1 is a
/// caller that can compare it wrongly.
enum class ChangeShape : std::uint8_t
{
    Unchanged,   ///< The same configuration; nothing to do.
    AddedOne,    ///< Exactly one member gained, as a voter or as a learner.
    RemovedOne,  ///< Exactly one member lost, voter or learner.
    PromotedOne, ///< Exactly one learner became a voter: a voter addition.
    DemotedOne,  ///< Exactly one voter became a learner: a voter removal.
    Unsafe,      ///< Anything else: more than one member moved.
};

/// Whether a member set names `who`.
///
/// Its own function because three separate questions are asked of a member set
/// -- how it differs, which member it gained, and whether an id is in it -- and a
/// lambda re-spelled per question is where one of them comes to disagree.
/// @param where The member set.
/// @param who The id to look for.
/// @return True when the set contains it.
[[nodiscard]] inline bool Contains(std::span<NodeId const> where, NodeId const& who)
{
    return std::ranges::find(where, who) != where.end();
}

/// Whether `configuration` counts `who` in its quorums.
/// @param configuration The configuration.
/// @param who The id to look for.
/// @return True when it is a voter there.
[[nodiscard]] inline bool IsVoter(Configuration const& configuration, NodeId const& who)
{
    return Contains(configuration.voters, who);
}

/// Whether `configuration` names `who` at all, voter or learner.
///
/// The question REPLICATION asks: a learner is sent the log and its answers are
/// read, so it is a member for every purpose but counting.
/// @param configuration The configuration.
/// @param who The id to look for.
/// @return True when it is a voter or a learner there.
[[nodiscard]] inline bool IsMember(Configuration const& configuration, NodeId const& who)
{
    return Contains(configuration.voters, who) || Contains(configuration.learners, who);
}

/// Where `who` sits in `configuration`.
///
/// The ONE author of the answer, so the node that acts on it and every surface that
/// reports it cannot come to disagree about which of the four a node is.
/// @param configuration The configuration.
/// @param who The id asked about -- ordinarily the asking node's own.
/// @return Its standing.
[[nodiscard]] inline Standing StandingOf(Configuration const& configuration, NodeId const& who)
{
    if (configuration.voters.empty() && configuration.learners.empty())
        return Standing::NoCluster;
    if (Contains(configuration.voters, who))
        return Standing::Voter;
    if (Contains(configuration.learners, who))
        return Standing::Learner;
    return Standing::Outsider;
}

/// Whether `configuration` names nobody: the "no cluster yet" configuration.
/// @param configuration The configuration.
/// @return True when both sets are empty.
[[nodiscard]] inline bool IsEmpty(Configuration const& configuration) noexcept
{
    return configuration.voters.empty() && configuration.learners.empty();
}

/// How many voters must agree for a decision to be taken under `configuration`.
///
/// Strict majority of the VOTERS: `floor(v/2) + 1`. Two overlapping majorities always
/// share a voter, which is the whole mechanism behind Election Safety and Leader
/// Completeness -- so this is `+ 1` and never `>= v/2`. Learners are not in it, and
/// that is the whole of what a learner is (#1449).
///
/// **The one threshold every quorum read takes**, commitment, pre-vote, votes and
/// CheckQuorum alike. CheckQuorum measures a DIFFERENT configuration from the other
/// three while a change is in flight, and each of them computing its own `/ 2 + 1`
/// would be four places for the rule about learners to be applied three times.
///
/// Meaningless for an empty configuration, where it answers 1 arithmetically and
/// nothing at all in fact; `RaftNode` never stands while it has no cluster.
/// @param configuration The configuration.
/// @return The quorum size.
[[nodiscard]] inline std::size_t QuorumOf(Configuration const& configuration) noexcept
{
    return (configuration.voters.size() / 2) + 1;
}

/// Classify the difference between two configurations.
/// @param from The current configuration.
/// @param to The proposed configuration.
/// @return What kind of change this would be.
[[nodiscard]] inline ChangeShape Classify(Configuration const& from, Configuration const& to)
{
    /// One way a member can move, and how many did.
    struct Move
    {
        std::size_t count {};
        ChangeShape shape {};
    };

    // A voter in `to` that was not one in `from` was either a learner there -- a
    // promotion -- or nothing there at all; a learner in `to` likewise was a voter or
    // nothing. Every id is looked at once per side it appears on, so a member that
    // moved is counted as exactly one move.
    auto added = std::size_t { 0 };
    auto promoted = std::size_t { 0 };
    for (auto const& id: to.voters)
    {
        if (IsVoter(from, id))
            continue;
        if (Contains(from.learners, id))
            ++promoted;
        else
            ++added;
    }

    auto demoted = std::size_t { 0 };
    for (auto const& id: to.learners)
    {
        if (Contains(from.learners, id))
            continue;
        if (IsVoter(from, id))
            ++demoted;
        else
            ++added;
    }

    auto const removed = static_cast<std::size_t>(
        std::ranges::count_if(from.voters, [&to](NodeId const& id) { return !IsMember(to, id); })
        + std::ranges::count_if(from.learners, [&to](NodeId const& id) { return !IsMember(to, id); }));

    auto const moves = std::array {
        Move { .count = added, .shape = ChangeShape::AddedOne },
        Move { .count = removed, .shape = ChangeShape::RemovedOne },
        Move { .count = promoted, .shape = ChangeShape::PromotedOne },
        Move { .count = demoted, .shape = ChangeShape::DemotedOne },
    };

    auto const total = Ranges::FoldLeft(moves | std::views::transform(&Move::count), std::size_t { 0 }, std::plus {});
    if (total == 0)
        return ChangeShape::Unchanged;
    if (total != 1)
        return ChangeShape::Unsafe;

    // Exactly one move in total, so exactly one row holds it.
    auto const* const only = FindIfOrNull(moves, [](Move const& move) { return move.count == 1; });
    return only != nullptr ? only->shape : ChangeShape::Unsafe;
}

/// Whether `configuration` is one a cluster could operate as.
///
/// Checked where a change is proposed rather than trusted from the log, because
/// a configuration entry is replicated to every node and a set that is empty or
/// holds a duplicate would be adopted by all of them at once.
/// @param configuration The proposed configuration.
/// @return Nothing on success, or why it was refused.
[[nodiscard]] inline std::expected<void, ConsensusError> Validate(Configuration const& configuration)
{
    // Nobody may lead a cluster of learners, and a configuration of nobody is the
    // "no cluster yet" state -- which is a node's starting point, never something the
    // log may carry.
    if (configuration.voters.empty())
        return std::unexpected { InvalidConfiguration("a cluster must have at least one voter") };

    auto everyone = configuration.voters;
    everyone.insert(everyone.end(), configuration.learners.begin(), configuration.learners.end());

    if (std::ranges::any_of(everyone, [](NodeId const& id) { return id.empty(); }))
        return std::unexpected { InvalidConfiguration("a member id may not be empty") };

    // A duplicate would make one node count twice toward a quorum, which is a quorum
    // that does not exist -- the same defect the vote tally is a set to avoid, reached
    // through configuration instead. Over BOTH sets at once, which is also what makes
    // them disjoint: an id that is a voter and a learner is counted by one rule and
    // excused by the other.
    std::ranges::sort(everyone);
    auto const duplicate = std::ranges::adjacent_find(everyone);
    if (duplicate != everyone.end())
        return std::unexpected { InvalidConfiguration(std::format("member {} appears more than once", *duplicate)) };

    return {};
}

} // namespace FastCache::Consensus::Membership
