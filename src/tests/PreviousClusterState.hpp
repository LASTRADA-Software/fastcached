// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/WireFields.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Testing
{

/// The `ClusterState` version the build before #178's certified roster wrote.
inline constexpr std::uint8_t PreviousClusterStateVersion = 6;

/// The `Command` version the build before #1555 wrote: this build's layout, and other meanings.
inline constexpr std::uint8_t PreviousClusterCommandVersion = 3;

/// The last `Command` version laid out differently: before #178 a command was four fields.
inline constexpr std::uint8_t FourFieldClusterCommandVersion = 2;

/// A one-member cluster state, encoded as the build before #178's certified roster laid it out.
///
/// **A BUILDER shared by every case that needs one, for the reason `ForeignGenerationValue.hpp`
/// is**: each such case asserts a REFUSAL, and a copy that drifted into building something else
/// is refused too -- so every copy goes on passing under a name for what it no longer builds.
/// The one fact that matters lives here alone: the previous LAYOUT, which is not the current
/// one with its version byte changed. Version 6 wrote seven header fields rather than eight --
/// no roster version -- so a decoder that judged the arity before the version refuses this as
/// damage while passing a test that only flips the byte of a current encoding.
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
    auto const neverAnnounced = std::array { std::byte { 0 } };
    auto const voter = std::array { std::byte { 0 } };
    return WireFields::Encode({ std::span<std::byte const> { version },
                                std::span<std::byte const> { members },
                                std::span<std::byte const> { none }, // settings
                                std::span<std::byte const> { none }, // clients
                                std::span<std::byte const> { none }, // forgotten hosts
                                std::span<std::byte const> { none }, // principals
                                std::span<std::byte const> { none }, // revoked keys
                                WireFields::AsBytes(std::string_view { "n1" }),
                                WireFields::AsBytes(std::string_view { "10.0.0.1:6675" }),
                                WireFields::AsBytes(std::string_view {}),
                                std::span<std::byte const> { neverAnnounced },
                                std::span<std::byte const> { voter },
                                std::span<std::byte const> {} }); // no key
}

/// A command removing member `n1`, encoded as the build before #1555 wrote it.
///
/// Here for `EncodePreviousClusterState`'s reason, and this time the point is the VERB rather
/// than the layout: a v3 command is the six fields this build writes, and ordinal 1 was
/// `RemoveMember` there and is `Forget` here, which also revokes the key the removed record
/// held. So nothing but the version byte refuses it -- a decoder that judged anything else
/// first would accept it and replay it as a forget its writer never meant. What a node's own
/// log holds after that upgrade is exactly this, and a node recovering it refuses to start
/// (#1542).
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
/// value, the scheduler endpoint -- where this build writes six, so a decoder that counted
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
