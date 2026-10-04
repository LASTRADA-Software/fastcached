// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Testing
{

/// The `ClusterState` version the build before the replicated dissolve wrote -- TWO layouts back:
/// the one immediately before this build's is `PrincipalEraClusterStateVersion`.
inline constexpr std::uint8_t PreviousClusterStateVersion = 8;

/// The `ClusterState` version the last build with principals wrote: what a node installed before
/// principal mode retired holds on disk.
inline constexpr std::uint8_t PrincipalEraClusterStateVersion = 9;

/// The `Command` version the build before the replicated dissolve wrote: six fields, no age.
inline constexpr std::uint8_t PreviousClusterCommandVersion = 4;

/// The last `Command` version laid out differently: before #178 a command was four fields.
inline constexpr std::uint8_t FourFieldClusterCommandVersion = 2;

/// A one-member cluster state, encoded as the build before the replicated dissolve laid it out.
///
/// **A BUILDER shared by every case that needs one, for the reason `ForeignGenerationValue.hpp`
/// is**: each such case asserts a REFUSAL, and a copy that drifted into building something else
/// is refused too -- so every copy goes on passing under a name for what it no longer builds.
/// The one fact that matters lives here alone: the previous LAYOUT, which is not the current
/// one with its version byte changed. Version 8 wrote six header fields, as this build does, but
/// the fourth was a principal count where this build has the revoked count and the last a roster
/// version where this build has the dissolve count -- so a decoder that read the fields before the
/// version would MISREAD this rather than refuse it, which no test flipping the byte of a current
/// encoding can show. `EncodePrincipalEraClusterState` is the layout whose ARITY differs.
///
/// Member `n1` at `10.0.0.1:6675`: never announced a scheduler endpoint, seated as a voter,
/// stating no key.
/// @return The encoded state.
[[nodiscard]] inline std::vector<std::byte> EncodePreviousClusterState()
{
    auto const count = [](std::uint32_t value) {
        auto const bytes = WireFields::ToBigEndian<std::uint32_t>(value);
        return std::vector<std::byte> { bytes.begin(), bytes.end() };
    };
    auto const version = std::array { std::byte { PreviousClusterStateVersion } };
    auto const members = count(1);
    auto const none = count(0);
    auto const rosterVersionBytes = WireFields::ToBigEndian<std::uint64_t>(1);
    auto const rosterVersion = std::vector<std::byte> { rosterVersionBytes.begin(), rosterVersionBytes.end() };
    auto const neverAnnounced = std::array { std::byte { 0 } };
    auto const voter = std::array { std::byte { 0 } };
    return WireFields::Encode({ std::span<std::byte const> { version },
                                std::span<std::byte const> { members },
                                std::span<std::byte const> { none }, // settings
                                std::span<std::byte const> { none }, // principals
                                std::span<std::byte const> { none }, // revoked keys
                                std::span<std::byte const> { rosterVersion },
                                WireFields::AsBytes(std::string_view { "n1" }),
                                WireFields::AsBytes(std::string_view { "10.0.0.1:6675" }),
                                WireFields::AsBytes(std::string_view {}),
                                std::span<std::byte const> { neverAnnounced },
                                std::span<std::byte const> { voter },
                                std::span<std::byte const> {} }); // no key
}

/// A one-member, one-principal cluster state, encoded as the last build with principals laid it out.
///
/// For `EncodePreviousClusterState`'s reason, and the sharper case of the two: version 9 wrote SEVEN
/// header fields -- the principal count fourth, the dissolve count last -- where this build writes
/// six, and a principal group of (id, key, role) triples after the settings. A decoder that judged
/// the arity before the version would call this snapshot damage, and this is the layout the one
/// installation actually holds.
///
/// Member `n1` at `10.0.0.1:6675`, a voter that never announced a scheduler endpoint, keyed
/// `0x11`; principal `w9`, keyed `0x39`, in the worker role (0); no setting, no revoked key, no
/// dissolve order.
/// @return The encoded state.
[[nodiscard]] inline std::vector<std::byte> EncodePrincipalEraClusterState()
{
    auto const count = [](std::uint32_t value) {
        auto const bytes = WireFields::ToBigEndian<std::uint32_t>(value);
        return std::vector<std::byte> { bytes.begin(), bytes.end() };
    };
    auto const keyOf = [](std::uint8_t fill) {
        auto key = std::vector<std::byte>(32);
        std::ranges::fill(key, std::byte { fill });
        return key;
    };
    auto const version = std::array { std::byte { PrincipalEraClusterStateVersion } };
    auto const one = count(1);
    auto const none = count(0);
    auto const rosterVersionBytes = WireFields::ToBigEndian<std::uint64_t>(2);
    auto const rosterVersion = std::vector<std::byte> { rosterVersionBytes.begin(), rosterVersionBytes.end() };
    auto const neverAnnounced = std::array { std::byte { 0 } };
    auto const voter = std::array { std::byte { 0 } };
    auto const memberKey = keyOf(0x11);
    auto const principalKey = keyOf(0x39);
    auto const workerRole = std::array { std::byte { 0 } };
    return WireFields::Encode({ std::span<std::byte const> { version },
                                std::span<std::byte const> { one },  // members
                                std::span<std::byte const> { none }, // settings
                                std::span<std::byte const> { one },  // principals
                                std::span<std::byte const> { none }, // revoked keys
                                std::span<std::byte const> { rosterVersion },
                                std::span<std::byte const> { none }, // dissolve orders
                                WireFields::AsBytes(std::string_view { "n1" }),
                                WireFields::AsBytes(std::string_view { "10.0.0.1:6675" }),
                                WireFields::AsBytes(std::string_view {}),
                                std::span<std::byte const> { neverAnnounced },
                                std::span<std::byte const> { voter },
                                std::span<std::byte const> { memberKey },
                                WireFields::AsBytes(std::string_view { "w9" }),
                                std::span<std::byte const> { principalKey },
                                std::span<std::byte const> { workerRole } });
}

/// A command forgetting member `n1`, encoded as the build before the replicated dissolve wrote it.
///
/// Here for `EncodePreviousClusterState`'s reason: a v4 command is SIX fields where this build
/// writes seven, so a decoder that counted the fields first would call an intact entry damage.
/// What a node's own log holds after that upgrade is exactly this, and a node recovering it
/// refuses to start (#1542).
/// @return The encoded command.
[[nodiscard]] inline std::vector<std::byte> EncodePreviousClusterCommand()
{
    auto const header = std::array { std::byte { PreviousClusterCommandVersion }, std::byte { 1 } };
    return WireFields::Encode({ std::span<std::byte const> { header },
                                WireFields::AsBytes(std::string_view { "n1" }),
                                WireFields::AsBytes(std::string_view {}),
                                WireFields::AsBytes(std::string_view {}),
                                std::span<std::byte const> {},    // no key
                                std::span<std::byte const> {} }); // no role
}

/// A command adding member `n1` at `10.0.0.1:6675`, encoded as the build before #178 laid it out.
///
/// The LAYOUT is the point here: a v2 command is FOUR fields -- the header, the key, the
/// value, the scheduler endpoint -- where this build writes seven, so a decoder that counted
/// the fields first would call an intact entry from an older build damage.
///
/// The verb byte is `CommandKind::AddMember`'s, which is 0 in both builds.
/// @return The encoded command.
[[nodiscard]] inline std::vector<std::byte> EncodeFourFieldClusterCommand()
{
    auto const header = std::array { std::byte { FourFieldClusterCommandVersion }, std::byte { 0 } };
    return WireFields::Encode({ std::span<std::byte const> { header },
                                WireFields::AsBytes(std::string_view { "n1" }),
                                WireFields::AsBytes(std::string_view { "10.0.0.1:6675" }),
                                WireFields::AsBytes(std::string_view {}) });
}

} // namespace FastCache::Testing
