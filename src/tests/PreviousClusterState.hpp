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

/// The `ClusterState` version the build before #178 wrote.
inline constexpr std::uint8_t PreviousClusterStateVersion = 5;

/// A one-member cluster state, encoded as the build before #178 laid it out.
///
/// **A BUILDER shared by every case that needs one, for the reason `ForeignGenerationValue.hpp`
/// is**: each such case asserts a REFUSAL, and a copy that drifted into building something else
/// is refused too -- so every copy goes on passing under a name for what it no longer builds.
/// The one fact that matters lives here alone: the previous LAYOUT, which is not the current
/// one with its version byte changed. Version 5 wrote four counts rather than six and five
/// fields a member rather than six, so a decoder that judged the arity before the version
/// refuses this as damage while passing a test that only flips the byte of a current encoding.
///
/// Member `n1` at `10.0.0.1:6675`: never announced a scheduler endpoint, seated as a voter.
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
                                WireFields::AsBytes(std::string_view { "n1" }),
                                WireFields::AsBytes(std::string_view { "10.0.0.1:6675" }),
                                WireFields::AsBytes(std::string_view {}),
                                std::span<std::byte const> { neverAnnounced },
                                std::span<std::byte const> { voter } });
}

} // namespace FastCache::Testing
