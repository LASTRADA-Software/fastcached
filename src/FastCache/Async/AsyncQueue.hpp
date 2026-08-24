// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace FastCache
{

/// What a full `AsyncQueue` gives up to make room.
///
/// At namespace scope rather than nested in the template so a defaulted
/// parameter can name it without the enclosing type being complete.
enum class AsyncQueueOverflow : std::uint8_t
{
    /// Discard the front. Right when the newest item subsumes the older ones --
    /// a Raft AppendEntries supersedes every earlier one for that peer, so what
    /// is queued behind a dead connection is stale by definition.
    DropOldest,

    /// Discard the item being pushed. Right when the queue holds a sequence
    /// whose prefix is what matters.
    DropNewest,
};

/// How much an `AsyncQueue` holds and what it does when it is full.
struct AsyncQueueOptions
{
    /// Largest number of items held before `overflow` applies; 0 is unbounded.
    ///
    /// A bound, not a target. Unbounded is offered because some producers are
    /// self-limiting, but it is not the default: a peer that is down for an hour
    /// accumulating an hour of heartbeats is the shape of leak a bound exists to
    /// make impossible to write by omission.
    std::size_t capacity { 0 };

    /// Which end is sacrificed when `capacity` is reached.
    AsyncQueueOverflow overflow { AsyncQueueOverflow::DropOldest };
};

/// What one `Push` did.
struct AsyncQueuePush
{
    /// Whether the value entered the queue. False only for a closed queue, or
    /// for `DropNewest` at capacity.
    bool accepted { false };

    /// How many items this push displaced to make room; 0 in the ordinary case.
    ///
    /// Returned rather than only counted internally, because the caller is the
    /// one holding the counter an operator reads and the context to name what
    /// was lost. A silent drop is invisible: a protocol that recovers from loss
    /// looks healthy while running slower than it should.
    std::size_t displaced { 0 };
};

/// A queue a coroutine parks on and any thread pushes to.
///
/// What replaces `std::mutex` + `std::condition_variable` + `std::deque` at the
/// boundary between a thread that produces and a coroutine that consumes. A
/// condition variable parks a *thread*; once the consumer lives on a reactor
/// there is no thread of its own to park, and blocking one would stall every
/// other coroutine sharing that loop.
///
/// ## The one invariant everything else rests on
///
/// **`Push` and `Close` never resume the consumer. They hand its handle to
/// `IReactor::Submit` and return.** Not an optimisation -- it is what makes the
/// type usable at all. A producer commonly calls `Push` while holding a lock of
/// its own (Raft's driver mutex is the case this was built for), and a queue
/// that resumed inline would run the consumer's next step inside that lock, on
/// the producer's thread, at a point where the consumer may call back into the
/// producer's object. `Submit` appends to a ready queue and, on the platform
/// reactors, writes one byte to an eventfd; it never runs user code, from any
/// thread, including the reactor's own.
///
/// ## Single consumer, many producers
///
/// One waiter slot. A second concurrent consumer is a programmer error rather
/// than a runtime condition, so it is asserted rather than handled. Many
/// producers, because a message can be handed over from a timer loop, from a
/// reader coroutine on the same reactor, and from whatever else holds a
/// reference -- three routes by construction.
///
/// ## Lifetime: the queue owns no coroutine frame, and cannot
///
/// It never destroys a frame and never keeps one alive. `Close()` hands the
/// parked consumer back to the reactor; the reactor resumes it; the consumer
/// observes `std::nullopt` and returns; only then is its frame gone. So an owner
/// must **observe** its consumer finishing before destroying the queue -- a
/// counter, not an assumption. Destroying a queue with a waiter still registered
/// leaves a coroutine frame nobody resumes and nobody frees, which is exactly
/// what `IReactor::Run`'s return semantics make possible; `~AsyncQueue` asserts
/// against it, and `HasWaiter()` lets a test assert it in a release build too.
///
/// ## Closing discards
///
/// One behaviour, not a policy. A consumer that had to drain N items during
/// teardown would make shutdown depend on the queue depth and on its own
/// backpressure, which is the one thing shutdown must not depend on. A caller
/// that wants everything delivered stops pushing and lets the consumer see an
/// empty queue.
///
/// @tparam T Item type. Moved in and out; required nothrow-move-constructible so
///         the critical section cannot throw with the waiter half-extracted.
template <typename T>
    requires std::is_nothrow_move_constructible_v<T>
class AsyncQueue final
{
  public:
    /// Construct over the reactor the consumer runs on.
    /// @param reactor Where a woken consumer is posted. Must outlive this queue
    ///        and every producer that can reach it.
    /// @param options Capacity and overflow policy.
    AsyncQueue(IReactor& reactor, AsyncQueueOptions options) noexcept:
        _reactor { reactor },
        _options { options }
    {
    }

    AsyncQueue(AsyncQueue const&) = delete;
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue const&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    /// Asserts that no consumer is parked. See the lifetime section.
    ~AsyncQueue()
    {
        assert(!_waiter && "AsyncQueue destroyed with a consumer still parked on it");
    }

    /// Offer an item, from any thread.
    ///
    /// Never blocks beyond this queue's own mutex, never waits for the consumer,
    /// and never fails for want of space -- it displaces instead, and says how
    /// much. A closed queue refuses.
    /// @param value The item; moved.
    /// @return Whether it was accepted, and what it displaced.
    AsyncQueuePush Push(T value)
    {
        std::coroutine_handle<> waiter {};
        AsyncQueuePush outcome {};
        {
            std::scoped_lock const guard { _mutex };
            if (_closed.load(std::memory_order_relaxed))
                return outcome;

            if (_options.capacity != 0 && _items.size() >= _options.capacity)
            {
                if (_options.overflow == AsyncQueueOverflow::DropNewest)
                {
                    _displaced.fetch_add(1, std::memory_order_relaxed);
                    return AsyncQueuePush { .accepted = false, .displaced = 1 };
                }
                while (_items.size() >= _options.capacity)
                {
                    _items.pop_front();
                    ++outcome.displaced;
                }
                _displaced.fetch_add(outcome.displaced, std::memory_order_relaxed);
            }

            _items.push_back(std::move(value));
            outcome.accepted = true;
            // Exactly one of a Push or a Close can ever obtain a given handle,
            // which is what makes a double resume inexpressible.
            waiter = std::exchange(_waiter, {});
        }

        // Outside the lock: the handle is already ours and nobody else can see
        // it, and `Submit` performs a syscall on the platform reactors. A
        // producer holding a lock of its own across that would serialise its own
        // hot path behind it.
        if (waiter)
            _reactor.Submit(waiter);
        return outcome;
    }

    /// Wake the consumer with "no more items, ever", and discard what is held.
    ///
    /// Idempotent, safe from any thread, and safe before a consumer exists. This
    /// is the stop path: a consumer parked on `Pop()` is resumed at once rather
    /// than after a timeout, which is the property a bounded wait cannot give and
    /// the reason an outbox is a queue rather than a poll.
    void Close() noexcept
    {
        std::coroutine_handle<> waiter {};
        {
            std::scoped_lock const guard { _mutex };
            _closed.store(true, std::memory_order_release);
            _items.clear();
            waiter = std::exchange(_waiter, {});
        }
        if (waiter)
            _reactor.Submit(waiter);
    }

    /// @return Whether `Close()` has been called.
    [[nodiscard]] bool IsClosed() const noexcept
    {
        return _closed.load(std::memory_order_acquire);
    }

    /// @return How many items are held right now. For tests and metrics; racy by
    ///         nature and never a basis for a decision.
    [[nodiscard]] std::size_t Size() const
    {
        std::scoped_lock const guard { _mutex };
        return _items.size();
    }

    /// @return Whether a consumer is currently parked on this queue.
    ///
    /// Exposed for one purpose: a teardown test asserting that no coroutine
    /// frame was left suspended. A production caller reading this is asking a
    /// question whose answer is stale before it returns.
    [[nodiscard]] bool HasWaiter() const noexcept
    {
        std::scoped_lock const guard { _mutex };
        return static_cast<bool>(_waiter);
    }

    /// @return Cumulative items displaced by overflow across this queue's life.
    [[nodiscard]] std::uint64_t Displaced() const noexcept
    {
        return _displaced.load(std::memory_order_relaxed);
    }

    /// Awaiter produced by `Pop()`. Not constructed directly.
    class PopAwaiter final
    {
      public:
        /// @param queue The queue to take from; never null.
        explicit PopAwaiter(AsyncQueue* queue) noexcept:
            _queue { queue }
        {
        }

        /// @return true when an item is already available, or the queue is
        ///         closed, so no suspension is needed.
        [[nodiscard]] bool await_ready() const noexcept
        {
            std::scoped_lock const guard { _queue->_mutex };
            return !_queue->_items.empty() || _queue->_closed.load(std::memory_order_relaxed);
        }

        /// Register `handle` as this queue's single waiter.
        ///
        /// Returns `bool` rather than `void` for the same reason
        /// `IoAwaitable::await_suspend` does: `await_ready` and `await_suspend`
        /// are two separate acquisitions of the mutex, so a push can land between
        /// them. Re-checking under the lock and returning false resumes through
        /// the normal path instead of parking on an item that is already there --
        /// a park that nothing would ever wake, because the push that would have
        /// woken it has already happened.
        /// @param handle The suspended consumer.
        /// @return true to stay suspended; false when an item arrived first.
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            std::scoped_lock const guard { _queue->_mutex };
            if (!_queue->_items.empty() || _queue->_closed.load(std::memory_order_relaxed))
                return false;
            assert(!_queue->_waiter && "AsyncQueue supports one consumer; a second is a programmer error");
            _queue->_waiter = handle;
            return true;
        }

        /// @return The next item, or nullopt when the queue is closed and the
        ///         consumer should end.
        [[nodiscard]] std::optional<T> await_resume() noexcept
        {
            std::scoped_lock const guard { _queue->_mutex };
            if (_queue->_items.empty())
                return std::nullopt;
            auto value = std::move(_queue->_items.front());
            _queue->_items.pop_front();
            return value;
        }

      private:
        AsyncQueue* _queue;
    };

    /// Take the next item, suspending until one arrives or the queue closes.
    ///
    /// `co_await queue.Pop()` yields `std::optional<T>`: a value, or `nullopt`
    /// meaning the queue closed and the consumer should stop. Exactly one
    /// consumer may have an outstanding `Pop` at a time.
    /// @return An awaitable resolving to the next item or nullopt.
    [[nodiscard]] PopAwaiter Pop() noexcept
    {
        return PopAwaiter { this };
    }

  private:
    IReactor& _reactor;
    AsyncQueueOptions _options;

    /// Guards `_items` and `_waiter`, and orders the store to `_closed`. Never
    /// held across `IReactor::Submit`, so the reactor's own submit lock is never
    /// nested under this one and no lock-order inversion is expressible.
    mutable std::mutex _mutex;

    std::deque<T> _items;
    std::coroutine_handle<> _waiter {};

    /// Stored under `_mutex` with release and read outside it with acquire, so a
    /// loop condition can ask without taking the lock. The pairing means a reader
    /// that sees `true` also sees the cleared `_items`.
    std::atomic<bool> _closed { false };

    /// Relaxed: a monotone diagnostic, counted and never compared.
    std::atomic<std::uint64_t> _displaced { 0 };
};

} // namespace FastCache
