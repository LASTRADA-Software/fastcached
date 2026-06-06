// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <charconv>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace FastCache
{

/// Carriage-return / line-feed terminator shared by the memcached text and
/// meta protocols.
inline constexpr std::string_view Crlf = "\r\n";

/// Parse an unsigned integer that must consume the whole view.
/// @tparam T Unsigned integral destination type.
/// @param sv  Text to parse.
/// @param out Destination, written only on a fully-consuming parse.
/// @return True if every character of @p sv was a valid digit of @p out.
template <typename T>
[[nodiscard]] bool ParseUnsigned(std::string_view sv, T& out) noexcept
{
    auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc {} && ptr == sv.data() + sv.size();
}

/// Write an entire payload to a socket, treating an empty payload as a no-op.
/// @param socket  Destination socket.
/// @param payload Bytes to send (interpreted as raw bytes).
/// @return True if the write succeeded (or the payload was empty).
inline Task<bool> WriteAll(ISocket* socket, std::string_view payload)
{
    if (payload.empty())
        co_return true;
    auto const result = co_await socket->Write(AsBytes(payload));
    co_return result.has_value();
}

/// Gather-write an ordered set of byte segments as one logical reply, pinning
/// the payload owner alive across a write that may suspend. Verifies the full
/// byte count was transferred (not merely that the call succeeded), so a short
/// write surfaces as a failure rather than a silently truncated response.
/// @param socket   Destination socket.
/// @param segments Ordered, non-owning views to gather, in send order.
/// @param keepAlive Optional owner pinning the segments' backing storage for
///        the operation's lifetime (e.g. the GetResult holding the value).
/// @return True if every byte of every segment was written.
inline Task<bool> WriteAllVectored(ISocket* socket,
                                   std::span<std::span<std::byte const> const> segments,
                                   std::shared_ptr<void const> keepAlive = {})
{
    std::size_t expected = 0;
    for (auto const seg: segments)
        expected += seg.size();
    if (expected == 0)
        co_return true;
    auto const result = co_await socket->WriteVectored(segments, std::move(keepAlive));
    co_return result.has_value() && *result == expected;
}

} // namespace FastCache
