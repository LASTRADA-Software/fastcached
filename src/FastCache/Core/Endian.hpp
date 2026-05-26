// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace FastCache
{

/// Concept covering the integer types the memcached binary header and Redis
/// length prefixes serialise as big-endian on the wire.
template <typename T>
concept WireInteger = std::same_as<T, std::uint8_t>   //
    || std::same_as<T, std::uint16_t>                 //
    || std::same_as<T, std::uint32_t>                 //
    || std::same_as<T, std::uint64_t>;

/// Convert a wire-integer value to big-endian (network byte order).
/// @param value Host-order integer.
/// @return Same bits in big-endian order.
template <WireInteger T>
[[nodiscard]] constexpr T HostToBigEndian(T value) noexcept
{
    if constexpr (std::endian::native == std::endian::big)
        return value;
    else
        return std::byteswap(value);
}

/// Convert a wire-integer from big-endian to host order.
/// @param value Big-endian integer.
/// @return Host-order integer.
template <WireInteger T>
[[nodiscard]] constexpr T BigEndianToHost(T value) noexcept
{
    if constexpr (std::endian::native == std::endian::big)
        return value;
    else
        return std::byteswap(value);
}

/// Read a big-endian wire integer from the front of a byte span.
/// @param bytes Source span; must contain at least sizeof(T) bytes.
/// @return Decoded host-order integer.
template <WireInteger T>
[[nodiscard]] T ReadBigEndian(std::span<std::byte const> bytes) noexcept
{
    T raw {};
    std::memcpy(&raw, bytes.data(), sizeof(T));
    return BigEndianToHost(raw);
}

/// Write a host-order wire integer as big-endian to the front of a byte span.
/// @param bytes Destination span; must have room for sizeof(T) bytes.
/// @param value Host-order integer to encode.
template <WireInteger T>
void WriteBigEndian(std::span<std::byte> bytes, T value) noexcept
{
    auto const wire = HostToBigEndian(value);
    std::memcpy(bytes.data(), &wire, sizeof(T));
}

} // namespace FastCache
