// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/IProtocolHandler.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace FastCache
{

/// memcached ASCII / text protocol handler.
///
/// Implements the full sccache-compatible command set:
///   set / add / replace / append / prepend / cas
///   get / gets
///   delete
///   incr / decr
///   stats
///   flush_all
///   version
///   quit
///
/// Caps:
///   - Single line: 4 KiB (header lines, not the data payload).
///   - Per-value payload: 16 MiB (configurable later via CacheEngine).
class MemcachedTextHandler final: public IProtocolHandler
{
  public:
    /// @return Server version string emitted by the `version` command.
    [[nodiscard]] static std::string_view ServerVersion() noexcept;

    [[nodiscard]] Task<void>
    Run(ISocket& socket, CacheEngine& engine, std::vector<std::byte> primingBytes) override;
};

} // namespace FastCache
