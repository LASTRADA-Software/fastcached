// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Server/BlockingServerLoop.hpp>
#include <FastCache/Server/Connection.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

namespace FastCache
{

namespace
{

    /// Drive the listener's Accept awaitable to completion synchronously.
    Task<AcceptResult> AcceptOne(IListener* listener)
    {
        co_return co_await listener->Accept();
    }

} // namespace

std::uint64_t RunBlockingServerLoop(IListener& listener, CacheEngine& engine, ILogger& logger, std::atomic<bool>& shouldStop)
{
    std::uint64_t accepted = 0;
    while (!shouldStop.load(std::memory_order_acquire))
    {
        auto result = SyncRun(AcceptOne(&listener));
        if (!result.has_value())
        {
            logger.Logf(LogLevel::Debug, "BlockingServerLoop: accept ended ({})", result.error().ToString());
            break;
        }

        ++accepted;
        auto connection = std::make_unique<Connection>(std::move(*result), engine, logger);
        std::jthread {
            [conn = std::move(connection), &logger]() mutable {
                try
                {
                    SyncRun(conn->Run());
                }
                catch (std::exception const& e)
                {
                    // Connection coroutine should never throw, but if it does
                    // we log + swallow so a single client's bug cannot bring
                    // the server down.
                    logger.Logf(LogLevel::Warn, "Connection coroutine threw: {}", e.what());
                }
                catch (...)
                {
                    logger.Logf(LogLevel::Warn, "Connection coroutine threw an unknown exception");
                }
            },
        }
            .detach();
    }
    return accepted;
}

} // namespace FastCache
