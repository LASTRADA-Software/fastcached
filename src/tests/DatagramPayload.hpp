// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IDatagramSocket.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Testing
{

/// The bytes of @p text, as a datagram payload.
///
/// Shared rather than written out in each datagram suite. There is nothing
/// subtle in it -- which is the point: two copies of a `reinterpret_cast` pair
/// are two places to edit and one to forget, and every suite that drives an
/// `IDatagramSocket` needs exactly this.
/// @param text The characters to send; it must outlive the span.
/// @return The same storage, as bytes.
[[nodiscard]] inline std::span<std::byte const> DatagramBytes(std::string_view text)
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// @p datagram's payload as text.
/// @param datagram What arrived.
/// @return Its bytes, as a string.
[[nodiscard]] inline std::string DatagramText(ReceivedDatagram const& datagram)
{
    return { reinterpret_cast<char const*>(datagram.payload.data()), datagram.payload.size() };
}

} // namespace FastCache::Testing
