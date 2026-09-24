// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <span>
#include <string_view>

#include <core/async/Task.hpp>
#include <core/net/ISocket.hpp>

namespace FastCache
{

/// memcached meta protocol (introduced in memcached 1.6). The meta
/// commands replace the classic text storage / retrieval / arithmetic /
/// delete commands with a single flexible interface controlled by
/// single-letter flags. Six commands are implemented in full:
///
///   mg <key> <flags...>           — meta get
///   ms <key> <datalen> <flags...> — meta set (payload on the next line)
///   md <key> <flags...>           — meta delete
///   ma <key> <flags...>           — meta arithmetic (incr / decr)
///   me <key> [b]                  — meta debug (dump metadata)
///   mn                            — meta no-op (pipeline barrier)
///
/// Responses follow the spec's two-letter token format: HD / NS / EX /
/// NF / VA / EN / ME / MN. See the per-command pages under docs/commands/
/// memcached/meta/ for the full flag matrix.
///
/// Dispatched from `MemcachedTextHandler::Run` — meta commands share
/// the same connection as the classic text commands.
class MemcachedMeta
{
  public:
    /// Dispatch a single meta command. Returns true if the connection
    /// loop should continue, false on socket write failure.
    /// @param socket  Per-connection socket.
    /// @param engine  Shared cache engine.
    /// @param reader  Buffered reader (needed by `ms` to read the
    ///                value bytes after the command line).
    /// @param command Command name ("mg", "ms", ...).
    /// @param args    Tokens after the command name (key + flags).
    [[nodiscard]] static core::async::Task<bool> Dispatch(core::net::ISocket* socket,
                                                          CacheEngine* engine,
                                                          ByteReader* reader,
                                                          std::string_view command,
                                                          std::span<std::string_view const> args);
};

} // namespace FastCache
