// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Cluster
{

namespace
{
    /// Fields in an encoded roster: the layout version, then the three groups.
    constexpr std::size_t RosterFields = 4;

    /// Fields one member occupies: id, consensus endpoint, seat, key (empty when none).
    constexpr std::size_t MemberFields = 4;

    /// Fields one principal occupies: id, key, role.
    constexpr std::size_t PrincipalFields = 3;

    /// Fields one revoked key occupies: whose it was, and the key.
    constexpr std::size_t RevokedFields = 2;

    /// Encode each entry of @p entries as a nested record of its own, then the group as one.
    /// @param entries The entries.
    /// @param encodeEntry How one entry is written.
    /// @return The group's encoding.
    template <typename Entry, typename EncodeEntry>
    [[nodiscard]] std::vector<std::byte> EncodeGroup(std::vector<Entry> const& entries, EncodeEntry encodeEntry)
    {
        std::vector<std::vector<std::byte>> encoded;
        encoded.reserve(entries.size());
        for (auto const& entry: entries)
            encoded.push_back(encodeEntry(entry));
        std::vector<std::span<std::byte const>> spans;
        spans.reserve(encoded.size());
        for (auto const& bytes: encoded)
            spans.emplace_back(bytes);
        return WireFields::Encode(WireFields::FieldList { spans });
    }

    /// A key field, which must be exactly one key wide.
    /// @param field The field.
    /// @return The key, or nothing when it is any other width.
    [[nodiscard]] std::optional<Ed25519PublicKey> RequiredKey(std::span<std::byte const> field)
    {
        if (field.size() != Ed25519PublicKeyBytes)
            return std::nullopt;
        Ed25519PublicKey key {};
        std::ranges::copy(field, key.begin());
        return key;
    }

    /// Decode every nested record in @p group with @p decodeEntry.
    /// @param group The group's encoding.
    /// @param arity How many fields each entry must hold.
    /// @param decodeEntry How one entry is read, from its fields.
    /// @return The entries, or nothing when any is malformed.
    template <typename Entry, typename DecodeEntry>
    [[nodiscard]] std::optional<std::vector<Entry>> DecodeGroup(std::span<std::byte const> group,
                                                                std::size_t arity,
                                                                DecodeEntry decodeEntry)
    {
        auto const records = WireFields::SplitAll(group);
        if (!records.has_value())
            return std::nullopt;
        std::vector<Entry> entries;
        entries.reserve(records->size());
        for (auto const record: *records)
        {
            auto const fields = WireFields::SplitExactly(record, arity);
            if (!fields.has_value())
                return std::nullopt;
            auto entry = decodeEntry(*fields);
            if (!entry.has_value())
                return std::nullopt;
            entries.push_back(*std::move(entry));
        }
        return entries;
    }
} // namespace

Roster ProjectRoster(ClusterState const& state)
{
    Roster roster;
    roster.members.reserve(state.members.size());
    for (auto const& member: state.members)
        roster.members.push_back(RosterMember {
            .id = member.id, .raftEndpoint = member.raftEndpoint, .seat = member.seat, .publicKey = member.publicKey });
    roster.principals = state.principals;
    roster.revoked = state.revokedKeys;
    return roster;
}

std::vector<std::byte> EncodeRoster(Roster const& roster)
{
    auto const members = EncodeGroup(roster.members, [](RosterMember const& member) {
        auto const seat = std::array { static_cast<std::byte>(member.seat) };
        auto const key =
            member.publicKey.has_value() ? std::span<std::byte const> { *member.publicKey } : std::span<std::byte const> {};
        return WireFields::Encode({ WireFields::AsBytes(member.id),
                                    WireFields::AsBytes(member.raftEndpoint),
                                    std::span<std::byte const> { seat },
                                    key });
    });
    auto const principals = EncodeGroup(roster.principals, [](ClusterPrincipal const& principal) {
        auto const role = std::array { static_cast<std::byte>(principal.role) };
        return WireFields::Encode({ WireFields::AsBytes(principal.id),
                                    std::span<std::byte const> { principal.publicKey },
                                    std::span<std::byte const> { role } });
    });
    auto const revoked = EncodeGroup(roster.revoked, [](RevokedKey const& entry) {
        return WireFields::Encode({ WireFields::AsBytes(entry.id), std::span<std::byte const> { entry.publicKey } });
    });
    auto const version = std::array { static_cast<std::byte>(RosterFormatVersion) };
    return WireFields::Encode({ std::span<std::byte const> { version },
                                std::span<std::byte const> { members },
                                std::span<std::byte const> { principals },
                                std::span<std::byte const> { revoked } });
}

std::expected<Roster, ConsensusError> DecodeRoster(std::span<std::byte const> bytes)
{
    auto const fields = WireFields::SplitAll(bytes);
    if (!fields.has_value() || fields->empty() || (*fields)[0].size() != 1)
        return std::unexpected(MalformedWireFrame("the bytes are not a roster"));

    // By name, before the layout is judged: a roster another build wrote is intact, and
    // reading it as this build's would report damage that is not there.
    if (auto const version = static_cast<std::uint8_t>((*fields)[0][0]); version != RosterFormatVersion)
        return std::unexpected(UnsupportedWireVersion(
            std::format("roster encoding version {} (this build reads {})", version, RosterFormatVersion)));
    if (fields->size() != RosterFields)
        return std::unexpected(MalformedWireFrame("a roster is not its version and three groups"));

    auto members =
        DecodeGroup<RosterMember>((*fields)[1], MemberFields, [](auto const& entry) -> std::optional<RosterMember> {
            auto const seat = entry[2].size() == 1
                                  ? Consensus::DecodeWireEnum<MemberSeat>(static_cast<std::uint8_t>(entry[2][0]))
                                  : std::nullopt;
            if (!seat.has_value())
                return std::nullopt;
            auto key = std::optional<Ed25519PublicKey> {};
            if (!entry[3].empty())
            {
                key = RequiredKey(entry[3]);
                if (!key.has_value())
                    return std::nullopt;
            }
            return RosterMember { .id = std::string { WireFields::AsStringView(entry[0]) },
                                  .raftEndpoint = std::string { WireFields::AsStringView(entry[1]) },
                                  .seat = *seat,
                                  .publicKey = key };
        });
    auto principals = DecodeGroup<ClusterPrincipal>(
        (*fields)[2], PrincipalFields, [](auto const& entry) -> std::optional<ClusterPrincipal> {
            auto const key = RequiredKey(entry[1]);
            auto const role = entry[2].size() == 1
                                  ? Consensus::DecodeWireEnum<PrincipalRole>(static_cast<std::uint8_t>(entry[2][0]))
                                  : std::nullopt;
            if (!key.has_value() || !role.has_value())
                return std::nullopt;
            return ClusterPrincipal { .id = std::string { WireFields::AsStringView(entry[0]) },
                                      .publicKey = *key,
                                      .role = *role };
        });
    auto revoked = DecodeGroup<RevokedKey>((*fields)[3], RevokedFields, [](auto const& entry) -> std::optional<RevokedKey> {
        auto const key = RequiredKey(entry[1]);
        if (!key.has_value())
            return std::nullopt;
        return RevokedKey { .id = std::string { WireFields::AsStringView(entry[0]) }, .publicKey = *key };
    });
    if (!members.has_value() || !principals.has_value() || !revoked.has_value())
        return std::unexpected(MalformedWireFrame("a roster entry is malformed"));

    return Roster { .members = *std::move(members), .principals = *std::move(principals), .revoked = *std::move(revoked) };
}

RosterDigest DigestOfRoster(std::span<std::byte const> encoded)
{
    return Sha256::Hash(encoded);
}

RosterDigest DigestOfRoster(Roster const& roster)
{
    return DigestOfRoster(EncodeRoster(roster));
}

std::string RenderRosterFingerprint(RosterDigest const& digest)
{
    return "SHA256:" + Base64UrlEncode(digest);
}

} // namespace FastCache::Cluster
