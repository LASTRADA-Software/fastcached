// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/MemcachedBinary.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Server/Connection.hpp>

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace FastCache
{

Connection::Connection(std::unique_ptr<ISocket> socket,
                       CacheEngine& engine,
                       ILogger& logger,
                       SessionContext session,
                       LogSource logSource) noexcept:
    _socket { std::move(socket) },
    _engine { engine },
    _logger { logger },
    _session { session },
    _logSource { logSource }
{
}

Task<void> Connection::Run()
{
    // Decorate this connection's logger with the client source (its IP) when
    // --log-source is on. SourceLogger forwards unchanged for an empty source,
    // so a connection with no known peer (or with the flag off) is unaffected.
    // `sourceTag` lives in this coroutine frame and outlives every co_await, so
    // both the decorator and the storage-tag view below can reference it.
    auto sourceTag = std::string {};
    if (_logSource == LogSource::Yes)
    {
        if (auto const peer = _socket->PeerAddress(); !peer.empty())
            sourceTag = std::format("[{}]", peer);
    }
    SourceLogger log { _logger, sourceTag };

    // Wire the session collaborators for trace logging:
    //  - `logger`   : connection-level lines (accept line, non-data commands).
    //  - `sourceTag`: published to Detail::StorageSourceTag by handlers so the
    //                 TracingStorage `storage:` line carries the same client IP.
    // Data operations are logged once, on the storage line — not here.
    _session.logger = &log;
    _session.sourceTag = sourceTag;

    // Drive any transport handshake (TLS) before reading application bytes.
    // No-op for plaintext sockets; runs on this per-connection coroutine so a
    // slow or stalled handshake never blocks the accept loop.
    if (auto const handshake = co_await _socket->HandshakeIfNeeded(); !handshake.has_value())
    {
        log.Logf(LogLevel::Debug, "Connection: handshake failed: {}", handshake.error().ToString());
        _socket->Close();
        co_return;
    }

    auto detect = co_await DetectProtocol(_socket.get());
    if (!detect.has_value())
    {
        log.Logf(LogLevel::Debug, "Connection: autodetect failed: {}", detect.error().ToString());
        _socket->Close();
        co_return;
    }

    // One Trace line per accepted connection, recording the negotiated
    // protocol. Gated on --log-everything so the default trace output is just
    // the per-operation storage lines; with --log-source it carries the IP.
    if (_session.logEverything)
        log.Logf(LogLevel::Trace, "connection accepted ({})", ToStringView(detect->flavor));

    switch (detect->flavor)
    {
        case ProtocolFlavor::MemcachedText: {
            MemcachedTextHandler handler;
            co_await handler.Run(_socket.get(), &_engine, std::move(detect->primer), _session);
            break;
        }
        case ProtocolFlavor::MemcachedBinary: {
            MemcachedBinaryHandler handler;
            co_await handler.Run(_socket.get(), &_engine, std::move(detect->primer), _session);
            break;
        }
        case ProtocolFlavor::RedisResp: {
            RedisRespHandler handler;
            co_await handler.Run(_socket.get(), &_engine, std::move(detect->primer), _session);
            break;
        }
        case ProtocolFlavor::Unknown:
            log.Log(LogLevel::Warn, "Connection: unknown protocol; closing");
            break;
    }

    _socket->Close();
    co_return;
}

} // namespace FastCache
