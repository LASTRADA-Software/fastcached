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

Connection::Connection(std::unique_ptr<ISocket> socket, CacheEngine& engine, ILogger& logger) noexcept:
    _socket { std::move(socket) },
    _engine { engine },
    _logger { logger }
{
}

Task<void> Connection::Run()
{
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
            co_await handler.Run(_socket.get(), &_engine, std::move(detect->primer));
            break;
        }
        case ProtocolFlavor::MemcachedBinary: {
            MemcachedBinaryHandler handler;
            co_await handler.Run(_socket.get(), &_engine, std::move(detect->primer));
            break;
        }
        case ProtocolFlavor::RedisResp: {
            RedisRespHandler handler;
            co_await handler.Run(_socket.get(), &_engine, std::move(detect->primer));
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
