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
    std::string endpoint; ///< host:port peers reach this node's scheduler on.

    [[nodiscard]] friend bool operator==(ClusterMember const&, ClusterMember const&) = default;
};

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

    /// The endpoint recorded for `id`, if any.
    /// @param id The member.
    /// @return Its endpoint, or nullopt when it is not a member.
    [[nodiscard]] std::optional<std::string> EndpointOf(std::string_view id) const;

    /// The value of `name`, if it has been set.
    /// @param name The setting.
    /// @return Its value, or nullopt when nobody set it.
    [[nodiscard]] std::optional<std::string> SettingOf(std::string_view name) const;

    /// Every member's endpoint, in id order.
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
    /// The endpoint for `AddMember`, the value for `SetSetting`, empty otherwise.
    std::string value;

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
