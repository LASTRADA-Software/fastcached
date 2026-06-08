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

/// memcached binary protocol handler. Implements the full opcode set
/// listed in the official memcached protocol-binary spec:
///   Get(0x00), Set(0x01), Add(0x02), Replace(0x03), Delete(0x04),
///   Increment(0x05), Decrement(0x06), Quit(0x07), Flush(0x08),
///   GetQ(0x09), NoOp(0x0a), Version(0x0b), GetK(0x0c), GetKQ(0x0d),
///   Append(0x0e), Prepend(0x0f), Stat(0x10), SetQ(0x11), AddQ(0x12),
///   ReplaceQ(0x13), DeleteQ(0x14), IncrementQ(0x15), DecrementQ(0x16),
///   QuitQ(0x17), FlushQ(0x18), AppendQ(0x19), PrependQ(0x1a),
///   Verbosity(0x1b), Touch(0x1c), GAT(0x1d), GATQ(0x1e),
///   GATK(0x23), GATKQ(0x24).
///
/// SASL opcodes (0x20 List, 0x21 Auth, 0x22 Step) implement the PLAIN
/// mechanism against the session's AuthPolicy when one is configured. With no
/// policy they reply auth_error (as before) so non-authing clients fall back to
/// the no-auth path. When a policy is configured, every data command before a
/// successful SASL auth replies auth_error.
class MemcachedBinaryHandler final: public IProtocolHandler
{
  public:
    [[nodiscard]] Task<void> Run(ISocket* socket,
                                 CacheEngine* engine,
                                 std::vector<std::byte> primingBytes,
                                 SessionContext session) override;
};

} // namespace FastCache
