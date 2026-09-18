// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace FastCache::Detail
{

// The pointer bridge between this tree's `std::byte` and Monocypher's `uint8_t`, shared by the
// two units that call into it (`Core/Ed25519.cpp`, `Core/X25519.cpp`). It includes no Monocypher
// header, so it is not part of the seam `ctest -R crypto-seam` guards; it exists so the aliasing
// argument below is made once rather than once per caller.
//
// Sound because `std::uint8_t` is `unsigned char` on every platform this project builds for, and
// an `unsigned char` glvalue may access any object's storage ([basic.lval]/11). The assertion is
// what makes that premise fail the BUILD on a platform where it is false, rather than compile into
// an aliasing violation nothing reports.
static_assert(std::is_same_v<std::uint8_t, unsigned char>,
              "Monocypher's byte type must be unsigned char for the std::byte bridge to be sound");

/// The bytes of @p bytes, as Monocypher reads them.
/// @param bytes A span; may be empty, in which case the pointer may be null.
/// @return A pointer to the first byte.
[[nodiscard]] inline std::uint8_t const* MonocypherIn(std::span<std::byte const> bytes) noexcept
{
    return reinterpret_cast<std::uint8_t const*>(bytes.data());
}

/// The bytes of @p bytes, as Monocypher writes them.
/// @param bytes A writable span.
/// @return A pointer to the first byte.
[[nodiscard]] inline std::uint8_t* MonocypherOut(std::span<std::byte> bytes) noexcept
{
    return reinterpret_cast<std::uint8_t*>(bytes.data());
}

} // namespace FastCache::Detail
