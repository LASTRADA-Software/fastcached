// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace FastCache
{

/// CRC-32C (Castagnoli polynomial 0x1EDC6F41), bit-reflected, table-based.
/// Used by DiskStorage to detect torn writes during crash recovery.
class Crc32c
{
  public:
    /// Initial seed; what an "empty" CRC32C looks like.
    static constexpr std::uint32_t kSeed = 0xFFFF'FFFFU;

    /// One-shot computation over a byte span.
    /// @param bytes Input bytes.
    /// @return Finalised CRC-32C.
    [[nodiscard]] static std::uint32_t Compute(std::span<std::byte const> bytes) noexcept;

    /// Streaming API: feed multiple buffers, finalise once.
    /// @param state In/out CRC state (seed with kSeed for the first call).
    /// @param bytes Input bytes.
    static void Update(std::uint32_t& state, std::span<std::byte const> bytes) noexcept;

    /// Finalise the streaming state into a usable CRC value.
    [[nodiscard]] static std::uint32_t Finalise(std::uint32_t state) noexcept { return state ^ 0xFFFF'FFFFU; }
};

} // namespace FastCache
