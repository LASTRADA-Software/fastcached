// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cluster
{

/// One member of the cluster, as the replicated state records it.
///
/// **The endpoint is the point.** `Consensus::RaftMembership` carries ids and
/// nothing else, which is correct for consensus — an id is all the algorithm needs
/// to count a quorum — and leaves a node the cluster has agreed to admit
/// unreachable until something else supplies its address. That residual was
/// recorded when membership landed; this is where it is closed. A member is a
/// `(id, endpoint)` pair here, so agreeing to admit somebody and knowing where they
/// answer are one decision replicated together rather than two facts that can
/// disagree.
struct ClusterMember
{
    Consensus::NodeId id; ///< Stable identity; what consensus counts.

    /// host:port this member's consensus port answers on.
    ///
    /// Always present -- a member with no address is the thing this struct exists to
    /// make impossible -- and always dialable, because it is what every other member
    /// opens a socket to.
    std::string raftEndpoint;

    /// host:port clients reach the fleet on while this member LEADS; may be empty.
    ///
    /// **A second endpoint rather than one**, and the reason is a defect this pairing
    /// closes rather than a generality. `NotLeader` carries a redirect, and the one
    /// address recorded here used to be the consensus port -- so a follower answered
    /// "ask the leader, at its Raft peer port", and a client that took the advice
    /// spoke the scheduler protocol at a socket that has never heard of it. Two
    /// ports, two facts, and collapsing them made every redirect in a real cluster
    /// point somewhere nothing could be done with.
    ///
    /// Empty is legitimate and means "this member has not said". Only a *leader's*
    /// matters, and a leader announces its own record on election -- so the value is
    /// absent exactly for the members whose value nobody needs, and a bootstrap peer
    /// that has never led carries none rather than carrying a guess.
    std::string schedulerEndpoint;

    [[nodiscard]] friend bool operator==(ClusterMember const&, ClusterMember const&) = default;
};

/// Parse one `<id>=<host>:<port>` member specification.
///
/// The grammar an operator types, in the one place the type it produces lives. It
/// has two callers that must not disagree — `--raft-peer` names a member at
/// startup and `--cluster-admit` names one at runtime, and the documentation tells
/// an operator to copy the same token between them — so a second implementation
/// would be two flags accepting different token sets for one concept, with only one
/// of them being what the transport actually dials.
///
/// Split at the **first** `=`, so an endpoint may contain one and an id may not.
/// The other way round makes `n1=host=1:6675` parse as an id of `n1=host`, which is
/// an id no operator wrote and which would silently never match a vote.
///
/// The endpoint must be one a peer can dial, which takes both halves of what a
/// dialer asks: a bare port names no machine, and `host:0` names no port, and a
/// member recorded either way is one the cluster counts towards quorum and cannot
/// reach.
/// @param spec The token as an operator wrote it.
/// @return The member, or nullopt when the token is not one.
[[nodiscard]] std::optional<ClusterMember> ParseMemberSpec(std::string_view spec);

/// A setting every member of the cluster must agree on.
///
/// A **table** rather than an open string map, so an unknown key is refused at the
/// leader rather than replicated to every node and ignored differently by each. The
/// failure that prevents is the quiet one: a setting somebody typo'd would be
/// accepted, stored, snapshotted and carried across restarts while doing nothing,
/// and the only symptom would be that the thing they configured did not happen.
///
/// Adding a setting is adding a row.
struct SettingSpec
{
    std::string_view name;    ///< The key as an operator writes it.
    std::string_view summary; ///< What it does, for `--help` and diagnostics.
};

/// Every replicated setting, in one place.
///
/// Deliberately short. A setting belongs here when **every node must agree** on it
/// — otherwise it is local configuration and belongs on the command line, where it
/// can differ per machine because the machines differ. `--slots` is the counter-
/// example worth naming: it describes one host and replicating it would impose one
/// machine's size on all of them.
inline constexpr std::array<SettingSpec, 2> SettingTable {
    SettingSpec { .name = "upstream", .summary = "host:port of the shared fastcached every member reads through to" },
    SettingSpec { .name = "fleet-open", .summary = R"('1' to admit every caller to the fleet, '0' for members only)" },
};

/// Whether `name` is a setting this cluster replicates.
/// @param name The key.
/// @return Its row, or nullptr when this build does not know it.
[[nodiscard]] constexpr SettingSpec const* FindSetting(std::string_view name) noexcept
{
    for (auto const& row: SettingTable)
        if (row.name == name)
            return &row;
    return nullptr;
}

/// One replicated setting and its value.
struct Setting
{
    std::string name;  ///< A key from `SettingTable`.
    std::string value; ///< Whatever the operator set it to.

    [[nodiscard]] friend bool operator==(Setting const&, Setting const&) = default;
};

/// Everything the cluster agrees on.
///
/// Deliberately small, and deliberately **not** the cache. The log that carries this
/// is replicated to every member and kept until it is snapshotted, so what goes in it
/// has to be state that changes rarely and matters everywhere. Cache entries are the
/// opposite of both — multi-megabyte objects written constantly — which is why they
/// live in a `fastcached` this state merely names.
struct ClusterState
{
    /// Members, sorted by id so two nodes that applied the same entries hold
    /// byte-identical state. A snapshot is compared across machines only by
    /// accident today, but a state whose serialization depended on insertion order
    /// would make that comparison impossible the day somebody wants it.
    std::vector<ClusterMember> members;

    /// Settings, sorted by name for the same reason.
    std::vector<Setting> settings;

    [[nodiscard]] friend bool operator==(ClusterState const&, ClusterState const&) = default;

    /// The consensus endpoint recorded for `id`, if any.
    /// @param id The member.
    /// @return Its Raft endpoint, or nullopt when it is not a member.
    [[nodiscard]] std::optional<std::string> RaftEndpointOf(std::string_view id) const;

    /// Where clients reach the fleet while `id` leads, if it has said.
    ///
    /// Absent for a member that is not known **and** for one that has never
    /// announced itself, which are deliberately the same answer here: both mean
    /// there is nowhere to send a client, and a caller that told them apart would
    /// have nothing different to do about it.
    /// @param id The member.
    /// @return Its scheduler endpoint, or nullopt.
    [[nodiscard]] std::optional<std::string> SchedulerEndpointOf(std::string_view id) const;

    /// The value of `name`, if it has been set.
    /// @param name The setting.
    /// @return Its value, or nullopt when nobody set it.
    [[nodiscard]] std::optional<std::string> SettingOf(std::string_view name) const;

    /// Every member's consensus endpoint, in id order.
    ///
    /// The Raft one rather than the scheduler one, because this feeds
    /// `Distributed::ClusterMembership`, which matches on the HOST part and admits a
    /// peer whatever port it dialed from. Both endpoints name the same host, and only
    /// this one is guaranteed to be there at all.
    /// @return The endpoints, which is what `Distributed::ClusterMembership` takes.
    [[nodiscard]] std::vector<std::string> Endpoints() const;
};

/// What a command does to the state.
///
/// An `enum class` rather than three payload types, so the wire carries one byte a
/// receiver switches on and an unknown verb is refused rather than mistaken for a
/// known one.
enum class CommandKind : std::uint8_t
{
    /// Add a member, or update the endpoint of one already present.
    ///
    /// One verb for both, because they are one intention: a node that moved has the
    /// same identity and a new address, and making the operator remove it first
    /// would leave a window in which the cluster has agreed it does not exist.
    AddMember = 0,
    RemoveMember,
    SetSetting,
};

/// One change to the cluster's state, as it travels in a log entry.
struct Command
{
    CommandKind kind { CommandKind::AddMember };
    /// The member id for `AddMember`/`RemoveMember`, the setting name for
    /// `SetSetting`.
    std::string key;
    /// The consensus endpoint for `AddMember`, the value for `SetSetting`, empty
    /// otherwise.
    std::string value;

    /// `AddMember` only: where clients reach the fleet while this member leads.
    ///
    /// Applied **wholesale**, so an empty one clears whatever was recorded rather
    /// than leaving it. That is the right way round: a member is re-admitted when its
    /// record has changed, and a node that moved has moved both ports -- keeping the
    /// old scheduler endpoint would redirect clients to an address that member no
    /// longer answers, which is worse than redirecting them nowhere. Refused for the
    /// other two verbs, because a field a verb ignores is a field somebody
    /// misunderstood.
    std::string schedulerEndpoint;

    [[nodiscard]] friend bool operator==(Command const&, Command const&) = default;
};

/// Serialize a command for a log entry's payload.
/// @param command The change.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> Encode(Command const& command);

/// Read a command back.
/// @param payload The entry's payload.
/// @return The command, or nullopt when it is malformed or names an unknown verb.
[[nodiscard]] std::optional<Command> DecodeCommand(std::span<std::byte const> payload);

/// Serialize a whole state, for a snapshot.
/// @param state The state.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> Encode(ClusterState const& state);

/// Read a whole state back.
/// @param bytes A snapshot previously produced by `Encode`.
/// @return The state, or nullopt when the bytes are malformed.
[[nodiscard]] std::optional<ClusterState> DecodeState(std::span<std::byte const> bytes);

/// Apply one command, in place.
///
/// Total: every well-formed command has an effect or is a no-op, and none can fail.
/// That is a property consensus needs rather than a convenience — an entry is
/// applied *after* it is committed, so there is nobody left to report a failure to
/// and no way to un-commit it. Anything that could be refused has to be refused
/// **before** it is proposed, which is what `Validate` is for.
/// @param state The state to change.
/// @param command The change.
void Apply(ClusterState& state, Command const& command);

/// Whether `command` may be proposed at all.
///
/// The only place a change can be refused, for the reason `Apply` cannot be. Called
/// by the leader before it appends, so an operator gets an error rather than a
/// silently ignored entry replicated to the whole cluster.
/// @param command The change.
/// @return Nothing when it may be proposed, or why it may not.
[[nodiscard]] std::expected<void, ConsensusError> Validate(Command const& command);

} // namespace FastCache::Cluster
