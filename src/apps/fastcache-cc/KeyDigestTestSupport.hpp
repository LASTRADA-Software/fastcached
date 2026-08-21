// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "KeyDigest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>

namespace FastCache::Cc::Test
{

/// A deterministic byte source shared by the key-strength cases in
/// `CacheKey_test.cpp` and `DirectManifest_test.cpp`.
///
/// Hand-rolled rather than `std::mt19937` plus a distribution, because the
/// standard leaves a distribution's mapping from engine output to values
/// unspecified: "fixed seed" would not mean the same inputs on libstdc++, libc++
/// and MSVC's STL, and the sample counts in those cases are chosen against a
/// specific observed collision index. It has to be the same sequence everywhere,
/// which is also why it lives here rather than being copied per test file: two
/// copies are two sequences the moment one of them is touched.
class SplitMix64
{
  public:
    /// @param seed Initial state; any value, but it fixes the whole sequence.
    explicit SplitMix64(std::uint64_t seed) noexcept:
        _state { seed }
    {
    }

    /// @return The next 64 pseudorandom bits.
    [[nodiscard]] std::uint64_t Next() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15ULL;
        auto z = _state;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    /// Produce a distinct 64-character text. The width is fixed on purpose: the
    /// defect these cases cover is invisible unless the hashed inputs are of
    /// EQUAL length, so a varying-width generator would defang them.
    /// @return 64 hex characters of fresh pseudorandom state.
    [[nodiscard]] std::string NextFixedWidthText()
    {
        std::string out;
        for ([[maybe_unused]] auto const word: std::views::iota(0, 4))
            out += std::format("{:016x}", Next());
        return out;
    }

  private:
    std::uint64_t _state;
};

/// Split a `KeyDigest::HexLength` digest into its four 32-bit quarters.
/// @param key A digest as ComputeKey / ComputeManifestKey renders one.
/// @return The four quarters, most significant first.
[[nodiscard]] inline std::array<std::uint32_t, 4> DigestQuarters(std::string const& key)
{
    REQUIRE(key.size() == KeyDigest::HexLength);
    std::array<std::uint32_t, 4> out {};
    for (auto const index: std::views::iota(std::size_t { 0 }, out.size()))
        out[index] = static_cast<std::uint32_t>(std::stoul(key.substr(index * 8, 8), nullptr, 16));
    return out;
}

} // namespace FastCache::Cc::Test
