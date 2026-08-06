// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/IProtocolHandler.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace FastCache
{

/// fastcached's own compile-cache protocol handler (the "executor").
///
/// Unlike the redis/memcached handlers, this one knows a cached value is a
/// **compile result** — an untouched object blob plus captured compiler text
/// (`/showIncludes`, diagnostics) carrying machine-specific paths. It closes
/// the cross-machine "value portability" gap by:
///
///   - **On STORE**: canonicalizing every path in the value's text regions,
///     using the *producing* machine's layout supplied in the frame, into
///     machine-neutral tokens (see PathCanon). The object blob is stored
///     verbatim, never rewritten. The stored value therefore has no machine
///     identity, so no producer can poison a shared entry for another.
///   - **On FETCH**: returning the canonical form **verbatim**. The client
///     localizes the tokens back to its own layout — the server stays off the
///     per-consumer transform path.
///
/// The wire protocol is length-prefixed binary (all lengths big-endian u32),
/// framed after a leading magic byte 0xFC (see ProtocolAutodetect):
///
///   STORE : [0xFC][op=0x01][key][cohort][srcRoot][buildTree][compile-value]
///           reply [0x01] on success, or [0x00][msg] on a typed error.
///   FETCH : [0xFC][op=0x02][key]
///           reply [0x01][u32 len][canonical compile-value bytes] on hit,
///           or [0x00] on miss.
///
/// where each bracketed string field is `[u32 len][bytes]` and the
/// compile-value is the EncodeCompileValue framing.
class CompileCacheHandler final: public IProtocolHandler
{
  public:
    /// Wire opcodes. One byte after the magic.
    enum class Op : std::uint8_t
    {
        Store = 0x01,
        Fetch = 0x02,
    };

    /// Reply status byte prefixing every response.
    enum class Status : std::uint8_t
    {
        Err = 0x00, ///< STORE failed (typed message follows) — also FETCH miss.
        Ok = 0x01,  ///< STORE succeeded — also FETCH hit (payload follows).
    };

    [[nodiscard]] Task<void> Run(ISocket* socket,
                                 CacheEngine* engine,
                                 std::vector<std::byte> primingBytes,
                                 SessionContext session) override;
};

} // namespace FastCache
