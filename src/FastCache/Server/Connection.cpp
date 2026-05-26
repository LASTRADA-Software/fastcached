// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Server/Connection.hpp>

#include <FastCache/Protocol/MemcachedText.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>

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
    auto detect = co_await DetectProtocol(*_socket);
    if (!detect.has_value())
    {
        _logger.Logf(LogLevel::Debug, "Connection: autodetect failed: {}", detect.error().ToString());
        _socket->Close();
        co_return;
    }

    switch (detect->flavor)
    {
        case ProtocolFlavor::MemcachedText:
        case ProtocolFlavor::MemcachedBinary: // binary not yet implemented; treat as text for now
        case ProtocolFlavor::RedisResp:       // RESP not yet implemented; treat as text for now
        {
            MemcachedTextHandler handler;
            co_await handler.Run(*_socket, _engine, std::move(detect->primer));
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
