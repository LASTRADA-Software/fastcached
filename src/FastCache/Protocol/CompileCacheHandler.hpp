// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/IProtocolHandler.hpp>

#include <cstddef>
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
/// The wire format itself — magic, version, opcodes, statuses, error codes, and
/// the encoders and parsers for all of them — lives in `Protocol/CompileCacheWire.hpp`,
/// which the launcher and the test client share verbatim. This class holds only
/// the *policy*: the order the checks run in, what each refusal costs the
/// connection, and what a command does once it has parsed.
///
/// A request is `[0xFC][version][op][u32 payloadLength]` followed by exactly that
/// many bytes of length-prefixed fields; a reply is `[status][u32 payloadLength]`
/// followed by its payload, uniformly for every status. Because both lengths are
/// declared, a refusal can be answered and stepped over instead of closing the
/// connection — which is what lets a future version add a verb without a flag day,
/// and what lets a version mismatch produce a diagnosable message instead of a
/// dropped socket indistinguishable from a dead peer.
///
/// Per-command policy:
///
///   - bad magic                      → close (the peer is not speaking this protocol)
///   - unsupported or changed version → Error/UnsupportedVersion, then close
///   - payload over the session cap   → Error/PayloadTooLarge, then close
///   - unknown opcode                 → Error/UnknownOpcode, skip the payload, **continue**
///   - fields ≠ declared payload      → Error/MalformedFrame, continue
///
/// The version is pinned to the first command's: a stream that changes version
/// mid-connection is nonsensical rather than merely unsupported, and saying so is
/// cheaper than carrying two decoders.
class CompileCacheHandler final: public IProtocolHandler
{
  public:
    [[nodiscard]] Task<void> Run(ISocket* socket,
                                 CacheEngine* engine,
                                 std::vector<std::byte> primingBytes,
                                 SessionContext session) override;
};

} // namespace FastCache
