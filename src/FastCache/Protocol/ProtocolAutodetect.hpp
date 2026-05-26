// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace FastCache
{

/// Identifier for the protocol that should drive a connection.
enum class ProtocolFlavor : std::uint8_t
{
    Unknown = 0,
    MemcachedText,
    MemcachedBinary, ///< Not yet implemented; recognised but currently treated like MemcachedText.
    RedisResp,       ///< Not yet implemented; recognised but currently treated like MemcachedText.
};

/// Outcome of peeking the start of a stream: which protocol to dispatch to
/// and the bytes consumed during the peek (must be replayed by the chosen
/// handler — see ByteReader::PrimeWith).
struct AutodetectResult
{
    ProtocolFlavor flavor { ProtocolFlavor::Unknown };
    std::vector<std::byte> primer;
};

/// Read enough bytes from `socket` to determine the protocol.
/// Heuristic: the very first byte is sufficient.
///   0x80 → memcached binary
///   '*'/'+'/'-'/':'/'$' → Redis RESP
///   anything else → memcached text
///
/// On EOF before any byte arrives, returns NetErrorCode::Eof.
/// @param socket Source socket; bytes peeked are returned in the primer.
/// @return Task resolving to the detected flavor + primer bytes.
[[nodiscard]] Task<std::expected<AutodetectResult, NetError>> DetectProtocol(ISocket* socket);

} // namespace FastCache
