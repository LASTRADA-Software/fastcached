// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace CowTree
{

/// CRC-32C (Castagnoli polynomial 0x1EDC6F41), bit-reflected, table-based.
///
/// Self-contained copy used internally by the CowTree library. The
/// implementation matches the canonical reflected CRC-32C used by iSCSI,
/// SSE 4.2's `crc32` instruction, and FastCache's own `Core/Crc32c`.
///
/// Duplicated rather than shared because CowTree is a standalone sibling
/// library with no link dependency on FastCache.
class Crc32c
{
  public:
    /// Initial CRC state; what an "empty" running CRC looks like before
    /// any bytes have been consumed.
    static constexpr std::uint32_t Seed = 0xFFFF'FFFFU;

    /// One-shot computation over a byte span.
    /// @param bytes Input bytes (may be empty).
    /// @return Finalised CRC-32C of the input.
    [[nodiscard]] static std::uint32_t Compute(std::span<std::byte const> bytes) noexcept;

    /// Streaming API: feed multiple buffers, finalise once.
    /// @param state In/out CRC state (initialise to Seed for the first call).
    /// @param bytes Input bytes to fold into the state.
    static void Update(std::uint32_t& state, std::span<std::byte const> bytes) noexcept;

    /// Finalise the streaming state into a usable CRC value.
    /// @param state Running CRC state from one or more Update() calls.
    /// @return Finalised CRC-32C value.
    [[nodiscard]] static std::uint32_t Finalise(std::uint32_t state) noexcept
    {
        return state ^ 0xFFFF'FFFFU;
    }
};

} // namespace CowTree
