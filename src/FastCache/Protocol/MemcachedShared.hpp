// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <charconv>
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

} // namespace FastCache
