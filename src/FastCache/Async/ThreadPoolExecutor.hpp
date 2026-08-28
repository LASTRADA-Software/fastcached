// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IExecutor.hpp>

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace FastCache
{

/// An executor whose "somewhere else" is a fixed set of threads.
///
/// For work that BLOCKS. A reactor multiplexes many coroutines on one thread and is
/// the right answer whenever every one of them suspends; it is the wrong answer for
/// a compile, which spawns a process and occupies its thread for seconds. Awaiting
/// one on a reactor would stall every other connection that reactor owns.
///
/// `co_await ResumeOn{pool}` is the whole interface. A caller writes its work
/// linearly and hops threads on one line, rather than splitting into a callback or
/// handing a socket to a queue and losing the shape of what it is doing.
///
/// The pool does NOT bound admission. It runs what it is given as threads come free,
/// and a caller that must refuse rather than queue -- which is what a worker's slot
/// cap is -- decides that before submitting. Sizing the pool to that cap means an
/// admitted job always finds a thread.
class ThreadPoolExecutor final: public IExecutor
{
  public:
    /// @param threads How many run at once. Clamped to at least one, because a pool
    ///        nothing runs on would accept handles and never resume them, which is a
    ///        coroutine frame nobody frees rather than an idle pool.
    explicit ThreadPoolExecutor(std::size_t threads);

    /// Stops and joins. Every handle still queued is resumed first.
    ///
    /// Draining rather than discarding: a coroutine that is never resumed never runs
    /// its destructors and never frees its frame, so dropping the queue would leak
    /// every job in it along with whatever it holds -- a socket, a scratch directory,
    /// a slot in somebody's counter.
    ~ThreadPoolExecutor() override;

    ThreadPoolExecutor(ThreadPoolExecutor const&) = delete;
    ThreadPoolExecutor& operator=(ThreadPoolExecutor const&) = delete;
    ThreadPoolExecutor(ThreadPoolExecutor&&) = delete;
    ThreadPoolExecutor& operator=(ThreadPoolExecutor&&) = delete;

    /// Post a coroutine for resumption on one of the threads.
    ///
    /// After `Stop()`, the handle is resumed **inline on the calling thread** rather
    /// than dropped, for the reason the destructor drains: a refused handle is a
    /// leaked frame. A caller that must not run work on its own thread should stop
    /// submitting, which it can see by having called `Stop()`.
    /// @param handle Coroutine to resume; must remain alive until it is.
    void Submit(std::coroutine_handle<> handle) override;

    /// Ask the threads to finish and stop taking new work. Idempotent.
    void Stop() noexcept;

    /// How many threads this pool runs work on.
    [[nodiscard]] std::size_t Threads() const noexcept
    {
        return _threads.size();
    }

  private:
    void Worker();

    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<std::coroutine_handle<>> _queue;
    bool _stopping { false };

    /// Declared LAST, so the threads are joined before the queue they read is gone.
    std::vector<std::jthread> _threads;
};

} // namespace FastCache
