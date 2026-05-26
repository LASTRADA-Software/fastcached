// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Server/Server.hpp>

#include <FastCache/Server/Connection.hpp>

#include <memory>
#include <utility>

namespace FastCache
{

namespace
{

    /// Per-connection coroutine. Self-owning via DetachedTask; the unique_ptr
    /// ensures the Connection object is freed when the coroutine ends.
    DetachedTask RunConnectionDetached(std::unique_ptr<Connection> connection)
    {
        co_await connection->Run();
        co_return;
    }

} // namespace

Server::Server(IListener& listener, CacheEngine& engine, ILogger& logger) noexcept:
    _listener { listener },
    _engine { engine },
    _logger { logger }
{
}

Task<void> Server::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            _logger.Logf(LogLevel::Debug, "Server: accept ended ({})", accepted.error().ToString());
            co_return;
        }

        _accepted.fetch_add(1, std::memory_order_relaxed);
        auto connection = std::make_unique<Connection>(std::move(*accepted), _engine, _logger);
        RunConnectionDetached(std::move(connection));
    }
    co_return;
}

void Server::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
}

} // namespace FastCache
