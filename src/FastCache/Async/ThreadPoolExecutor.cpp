// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ThreadPoolExecutor.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace FastCache
{

ThreadPoolExecutor::ThreadPoolExecutor(std::size_t threads)
{
    auto const wanted = std::max<std::size_t>(threads, 1);
    _threads.reserve(wanted);
    try
    {
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, wanted))
            _threads.emplace_back([this] { Worker(); });
    }
    catch (...)
    {
        // A thread this process could not create leaves the ones it did create
        // parked on `_wake` forever -- and `~vector<jthread>` then joins them, so
        // construction failing would HANG rather than propagate. No destructor runs
        // for an object whose constructor threw, so releasing them is this
        // handler's job.
        Stop();
        throw;
    }
}

ThreadPoolExecutor::~ThreadPoolExecutor()
{
    Stop();
    // `_threads` are `jthread`s and join here, which is what makes the drain below
    // observable: by the time this returns, nothing is left holding a handle.
}

void ThreadPoolExecutor::Stop() noexcept
{
    {
        auto const guard = std::scoped_lock { _mutex };
        _stopping = true;
    }
    _wake.notify_all();
}

void ThreadPoolExecutor::Submit(std::coroutine_handle<> handle)
{
    {
        auto const guard = std::scoped_lock { _mutex };
        if (!_stopping)
        {
            _queue.push_back(handle);
            // Notified with the lock RELEASED would be the usual advice; held is
            // correct here because a worker waking to an empty queue and a set
            // `_stopping` must not race the push it was notified for.
            _wake.notify_one();
            return;
        }
    }

    // Stopped, so nothing will pick it up. Resumed here rather than dropped: an
    // unresumed coroutine never frees its frame.
    handle.resume();
}

void ThreadPoolExecutor::Worker()
{
    while (true)
    {
        std::coroutine_handle<> handle {};
        {
            auto guard = std::unique_lock { _mutex };
            _wake.wait(guard, [this] { return _stopping || !_queue.empty(); });
            // The queue is drained even while stopping, so work already admitted
            // runs to completion rather than being abandoned mid-frame.
            if (_queue.empty())
                return;
            handle = _queue.front();
            _queue.pop_front();
        }
        handle.resume();
    }
}

} // namespace FastCache
