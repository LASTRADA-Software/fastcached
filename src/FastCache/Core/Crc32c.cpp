// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Crc32c.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace FastCache
{

namespace
{

    /// Bit-reflected CRC-32C polynomial.
    constexpr std::uint32_t PolyReflected = 0x82F63B78U;

    /// Software CRC-32C table, byte-at-a-time, computed at compile time.
    constexpr std::array<std::uint32_t, 256> BuildTable() noexcept
    {
        std::array<std::uint32_t, 256> table {};
        for (auto const index: std::views::iota(std::uint32_t { 0 }, std::uint32_t { 256 }))
        {
            auto value = index;
            for ([[maybe_unused]] auto const bit: std::views::iota(0, 8))
                value = (value & 1U) ? (value >> 1) ^ PolyReflected : value >> 1;
            table[index] = value;
        }
        return table;
    }

    constexpr auto Table = BuildTable();

} // namespace

void Crc32c::Update(std::uint32_t& state, std::span<std::byte const> bytes) noexcept
{
    auto crc = state;
    for (auto const b: bytes)
        crc = (crc >> 8) ^ Table[(crc ^ std::to_integer<std::uint8_t>(b)) & 0xFFU];
    state = crc;
}

std::uint32_t Crc32c::Compute(std::span<std::byte const> bytes) noexcept
{
    auto state = Seed;
    Update(state, bytes);
    return Finalise(state);
}

} // namespace FastCache
