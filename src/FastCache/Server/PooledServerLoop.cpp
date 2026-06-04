// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Server/Connection.hpp>
#include <FastCache/Server/PooledServerLoop.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    /// Drive the listener's Accept awaitable to completion synchronously.
    Task<AcceptResult> AcceptOne(IListener* listener)
    {
        co_return co_await listener->Accept();
    }

    /// Bounded blocking queue of accepted sockets. One producer (the
    /// accept thread) and N consumers (the worker threads). Workers
    /// receive a null pointer to signal shutdown.
    class WorkQueue
    {
      public:
        explicit WorkQueue(std::size_t maxDepth) noexcept:
            _maxDepth { maxDepth }
        {
        }

        /// Push a socket onto the queue. Blocks if the queue is at its
        /// bounded depth; that's how the accept thread gets back-pressure
        /// without us dropping connections on the floor.
        void Push(std::unique_ptr<ISocket> socket)
        {
            std::unique_lock lock { _mu };
            _notFull.wait(lock, [this] { return _stopping || _items.size() < _maxDepth; });
            _items.push_back(std::move(socket));
            _notEmpty.notify_one();
        }

        /// Pop a socket. Blocks until one is available. Returns nullptr
        /// when the queue is stopping and empty — the sentinel that
        /// tells a worker to exit its loop.
        std::unique_ptr<ISocket> Pop()
        {
            std::unique_lock lock { _mu };
            _notEmpty.wait(lock, [this] { return !_items.empty(); });
            auto socket = std::move(_items.front());
            _items.pop_front();
            _notFull.notify_one();
            return socket;
        }

        /// Wake every blocked Pop()/Push() and refuse new work. Workers
        /// already parked in Pop() will receive the sentinel pushes that
        /// follow.
        void Stop()
        {
            std::scoped_lock const lock { _mu };
            _stopping = true;
            _notFull.notify_all();
            _notEmpty.notify_all();
        }

      private:
        std::mutex _mu;
        std::condition_variable _notEmpty;
        std::condition_variable _notFull;
        std::deque<std::unique_ptr<ISocket>> _items;
        std::size_t const _maxDepth;
        bool _stopping { false };
    };

    /// Worker body: pop sockets and drive them to completion.
    /// @param queue Shared work queue this worker drains.
    /// @param engine Cache engine the connection handlers operate on.
    /// @param logger Logger for connection-level diagnostics.
    /// @param workerIndex Zero-based index used only to name the Tracy thread.
    void WorkerLoop(WorkQueue& queue, CacheEngine& engine, ILogger& logger, [[maybe_unused]] std::size_t workerIndex)
    {
        // Name the OS thread in the Tracy timeline (no-op when Tracy is off, in
        // which case the std::format argument is discarded unevaluated).
        FC_THREAD_NAME(std::format("fc-worker-{}", workerIndex).c_str());
        while (true)
        {
            auto socket = queue.Pop();
            if (!socket)
                return; // sentinel — exit

            try
            {
                Connection connection { std::move(socket), engine, logger };
                SyncRun(connection.Run());
            }
            catch (std::exception const& e)
            {
                logger.Logf(LogLevel::Warn, "Connection coroutine threw: {}", e.what());
            }
            catch (...)
            {
                logger.Logf(LogLevel::Warn, "Connection coroutine threw an unknown exception");
            }
        }
    }

    /// RAII owner for the fixed thread pool + queue. Destruction signals
    /// every worker to drain and joins them.
    class WorkerPool
    {
      public:
        WorkerPool(std::size_t poolSize, CacheEngine& engine, ILogger& logger):
            _queue { std::max<std::size_t>(poolSize * 4, 64) }
        {
            _threads.reserve(poolSize);
            for (std::size_t i = 0; i < poolSize; ++i)
                _threads.emplace_back(WorkerLoop, std::ref(_queue), std::ref(engine), std::ref(logger), i);
        }

        ~WorkerPool()
        {
            // Push N sentinels — one per worker — so each Pop()
            // unblocks and exits.
            _queue.Stop();
            for (std::size_t i = 0; i < _threads.size(); ++i)
                _queue.Push(nullptr);
            for (auto& t: _threads)
            {
                if (t.joinable())
                    t.join();
            }
        }

        WorkerPool(WorkerPool const&) = delete;
        WorkerPool(WorkerPool&&) = delete;
        WorkerPool& operator=(WorkerPool const&) = delete;
        WorkerPool& operator=(WorkerPool&&) = delete;

        void Submit(std::unique_ptr<ISocket> socket)
        {
            _queue.Push(std::move(socket));
        }

      private:
        WorkQueue _queue;
        std::vector<std::thread> _threads;
    };

    [[nodiscard]] std::size_t ResolvedPoolSize(std::size_t requested) noexcept
    {
        if (requested != 0)
            return requested;
        auto const hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<std::size_t>(hw) : 1;
    }

} // namespace

std::uint64_t RunPooledServerLoop(
    IListener& listener, CacheEngine& engine, ILogger& logger, std::atomic<bool>& shouldStop, std::size_t poolSize)
{
    auto const resolvedSize = ResolvedPoolSize(poolSize);
    WorkerPool pool { resolvedSize, engine, logger };

    std::uint64_t accepted = 0;
    while (!shouldStop.load(std::memory_order_acquire))
    {
        auto result = SyncRun(AcceptOne(&listener));
        if (!result.has_value())
        {
            logger.Logf(LogLevel::Debug, "PooledServerLoop: accept ended ({})", result.error().ToString());
            break;
        }
        ++accepted;
        pool.Submit(std::move(*result));
    }
    // Pool destructor drains and joins.
    return accepted;
}

} // namespace FastCache
