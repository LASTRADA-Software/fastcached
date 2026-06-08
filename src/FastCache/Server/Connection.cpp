// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/MemcachedBinary.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Server/Connection.hpp>

#include <memory>
#include <utility>

namespace FastCache
{

Connection::Connection(std::unique_ptr<ISocket> socket,
                       CacheEngine& engine,
                       ILogger& logger,
                       SessionContext session) noexcept:
    _socket { std::move(socket) },
    _engine { engine },
    _logger { logger },
    _session { session }
{
}

Task<void> Connection::Run()
{
    // Drive any transport handshake (TLS) before reading application bytes.
    // No-op for plaintext sockets; runs on this per-connection coroutine so a
    // slow or stalled handshake never blocks the accept loop.
    if (auto const handshake = co_await _socket->HandshakeIfNeeded(); !handshake.has_value())
    {
        _logger.Logf(LogLevel::Debug, "Connection: handshake failed: {}", handshake.error().ToString());
        _socket->Close();
        co_return;
    }

    auto detect = co_await DetectProtocol(_socket.get());
    if (!detect.has_value())
    {
        _logger.Logf(LogLevel::Debug, "Connection: autodetect failed: {}", detect.error().ToString());
        _socket->Close();
        co_return;
    }

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
            _logger.Log(LogLevel::Warn, "Connection: unknown protocol; closing");
            break;
    }

    _socket->Close();
    co_return;
}

} // namespace FastCache
