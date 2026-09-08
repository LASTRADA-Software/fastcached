// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/ParkedWork.hpp>

#include <atomic>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace FastCache
{

/// Fire-and-forget coroutine. The body runs to its first suspend on
/// construction; the frame self-destroys on final return. Used by the
/// server to spawn one coroutine per connection without keeping a handle
/// around — the chain of I/O awaitables (paired with the reactor's
/// resumption path) drives progress.
///
/// Exceptions escaping the body terminate the process — connection-level
/// errors must be caught and turned into NetError/ProtocolError responses
/// inside the handler before reaching final_suspend.
///
/// **Declared before `Task` because it is what makes a chain UNOWNED**, which
/// `Detail::UnownedRootOf` below has to be able to name. Every other coroutine frame
/// in this tree is owned by something — a `Task` object, or the `Task::Awaiter` of
/// whatever awaits it — and this one is owned by nobody, which is exactly the case a
/// reactor may free when it is torn down before resuming it
/// ([#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
struct DetachedTask
{
    struct promise_type
    {
        [[nodiscard]] DetachedTask get_return_object() noexcept
        {
            return {};
        }
        [[nodiscard]] std::suspend_never initial_suspend() const noexcept
        {
            return {};
        }
        [[nodiscard]] std::suspend_never final_suspend() const noexcept
        {
            return {};
        }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept
        {
            std::terminate();
        }
    };
};

/// Coroutine task representing a deferred computation that yields a T (or
/// void). Lazy: starts suspended; the first co_await resumes the body. The
/// final_suspend point performs symmetric transfer to the continuation, so
/// `co_await chain1(); co_await chain2(); ...` does not grow the call stack.
///
/// Tasks are move-only. Once the task is started via co_await (or Run on an
/// IReactor — added later), destroying it without awaiting is undefined
/// behaviour. Default-constructed / moved-from tasks are safe to destroy.
template <typename T = void>
class Task;

namespace Detail
{

    /// Common base for the per-type promise. Holds the continuation and any
    /// exception captured during the body.
    struct TaskPromiseBase
    {
        std::coroutine_handle<> continuation { std::noop_coroutine() };

        /// The root of the await chain this coroutine belongs to, when that chain is
        /// owned by NOBODY -- otherwise empty.
        ///
        /// Propagated downward at each `co_await`, so every frame in a chain carries the
        /// same answer and a parked coroutine can state it without walking anything. It
        /// is non-empty exactly when the chain bottoms out in a `DetachedTask`; a chain
        /// rooted in a `Task` object somebody holds keeps it empty, because that object's
        /// destructor is what frees the frame and a second owner would double free.
        ///
        /// A pointer's worth per frame, and the only thing that can answer *may this
        /// reactor free what it is holding* at teardown
        /// ([#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
        std::coroutine_handle<> unownedRoot {};

        std::exception_ptr exception {};

        struct FinalAwaiter
        {
            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }

            /// Symmetric transfer: resume the continuation directly so the
            /// caller's stack frame does not grow per `co_await`.
            template <typename Promise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept
            {
                return self.promise().continuation;
            }

            void await_resume() const noexcept {}
        };

        [[nodiscard]] std::suspend_always initial_suspend() const noexcept
        {
            return {};
        }
        [[nodiscard]] FinalAwaiter final_suspend() const noexcept
        {
            return {};
        }
        void unhandled_exception() noexcept
        {
            exception = std::current_exception();
        }
    };

    template <typename T>
    struct TaskPromise: TaskPromiseBase
    {
        std::variant<std::monostate, T, std::exception_ptr> result;

        Task<T> get_return_object() noexcept;

        template <typename U>
            requires std::is_convertible_v<U&&, T>
        void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        {
            result.template emplace<1>(std::forward<U>(value));
        }
    };

    template <>
    struct TaskPromise<void>: TaskPromiseBase
    {
        Task<void> get_return_object() noexcept;
        void return_void() const noexcept {}
    };

    /// The frame an executor may free if it never resumes `handle`, or an empty handle
    /// when something else owns this chain.
    ///
    /// Three answers, and the third is the one that keeps this safe to add:
    ///
    /// - A `DetachedTask` is owned by nobody, so it is its own root.
    /// - A `Task` carries whatever its awaiter told it (`unownedRoot`), which is the
    ///   root of the chain rather than this frame — freeing the frame a reactor happens
    ///   to hold would leave the caller waiting on it unreachable and still leaked.
    /// - Anything else — a coroutine type this header does not know, including the
    ///   type-erased `std::coroutine_handle<>` (`Promise` deduces to `void`) — answers
    ///   *not mine*, which is the pre-existing behaviour.
    ///
    /// @tparam Promise The parking coroutine's promise type, as the compiler passes it
    ///         to `await_suspend`.
    /// @param handle The coroutine about to park.
    /// @return The chain root to free on abandonment, or an empty handle.
    template <typename Promise>
    [[nodiscard]] std::coroutine_handle<> UnownedRootOf(std::coroutine_handle<Promise> handle) noexcept
    {
        if constexpr (std::is_same_v<Promise, DetachedTask::promise_type>)
            return handle;
        else if constexpr (std::is_base_of_v<TaskPromiseBase, Promise>)
            return handle.promise().unownedRoot;
        else
            return {};
    }

    /// How a coroutine parks itself on an `IExecutor` or an `IReactor`.
    ///
    /// The one place the ownership question is answered, so no awaitable has to decide
    /// it and none can get it wrong by omission: `SleepUntil` and `ResumeOn` both build
    /// their `ParkedWork` here.
    /// @tparam Promise The parking coroutine's promise type.
    /// @param handle The coroutine about to park.
    /// @return The handle to resume, paired with the chain root to free if it is not.
    template <typename Promise>
    [[nodiscard]] ParkedWork ParkedWorkFor(std::coroutine_handle<Promise> handle) noexcept
    {
        return ParkedWork { .resume = handle, .abandon = UnownedRootOf(handle) };
    }

    template <typename T>
    class TaskAwaiterBase
    {
      public:
        explicit TaskAwaiterBase(std::coroutine_handle<TaskPromise<T>> handle) noexcept:
            _handle { handle }
        {
        }

        [[nodiscard]] bool await_ready() const noexcept
        {
            return !_handle || _handle.done();
        }

        /// Park the caller on this task and transfer to it.
        ///
        /// Templated on the CALLER's promise, which is the only place the chain's
        /// ownership is visible: the answer is copied down so the task -- and anything
        /// it goes on to await -- can state it when it parks on a reactor, without
        /// walking a continuation chain whose links are type-erased.
        /// @tparam Promise The awaiting coroutine's promise type.
        /// @param continuation The awaiting coroutine, resumed when this task ends.
        /// @return This task's handle, for symmetric transfer.
        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> continuation) noexcept
        {
            auto& promise = _handle.promise();
            promise.continuation = continuation;
            promise.unownedRoot = UnownedRootOf(continuation);
            return _handle;
        }

      protected:
        [[nodiscard]] std::coroutine_handle<TaskPromise<T>> Coroutine() const noexcept
        {
            return _handle;
        }
        void SetCoroutine(std::coroutine_handle<TaskPromise<T>> handle) noexcept
        {
            _handle = handle;
        }

      private:
        std::coroutine_handle<TaskPromise<T>> _handle;
    };

} // namespace Detail

template <typename T>
class Task
{
  public:
    using promise_type = Detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle handle) noexcept:
        _handle { handle }
    {
    }

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    Task(Task&& other) noexcept:
        _handle { std::exchange(other._handle, {}) }
    {
    }

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    ~Task()
    {
        if (_handle)
            _handle.destroy();
    }

    /// @return true if this task has run to completion (or has never started
    /// and is in the empty / moved-from state).
    [[nodiscard]] bool IsReady() const noexcept
    {
        return !_handle || _handle.done();
    }

    /// Raw access to the underlying coroutine handle. Used by reactors to
    /// post the task onto their ready queue without going through co_await.
    [[nodiscard]] Handle Native() const noexcept
    {
        return _handle;
    }

    /// Release ownership of the handle — caller now owns destruction.
    [[nodiscard]] Handle Release() noexcept
    {
        return std::exchange(_handle, {});
    }

    /// Awaiter for `co_await task`. Owns the handle for the duration of the
    /// suspension — the rvalue Task that produced this Awaiter is left
    /// empty so its destructor cannot tear the coroutine down underneath
    /// us. The Awaiter destroys the handle when it goes out of scope (at
    /// the end of the full co_await expression in the caller).
    class Awaiter: public Detail::TaskAwaiterBase<T>
    {
      public:
        explicit Awaiter(Handle handle) noexcept:
            Detail::TaskAwaiterBase<T> { handle }
        {
        }
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;
        Awaiter(Awaiter&& other) noexcept:
            Detail::TaskAwaiterBase<T> { std::exchange(other._owned, Handle {}) }
        {
            this->SetCoroutine(_owned);
        }
        Awaiter& operator=(Awaiter&&) = delete;
        ~Awaiter()
        {
            if (_owned)
                _owned.destroy();
        }

        T await_resume()
        {
            auto& promise = this->Coroutine().promise();
            if (promise.exception)
                std::rethrow_exception(promise.exception);
            return std::move(std::get<1>(promise.result));
        }

      private:
        Handle _owned { this->Coroutine() };
    };

    Awaiter operator co_await() && noexcept
    {
        return Awaiter { std::exchange(_handle, {}) };
    }

  private:
    Handle _handle {};
};

template <>
class Task<void>
{
  public:
    using promise_type = Detail::TaskPromise<void>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle handle) noexcept:
        _handle { handle }
    {
    }

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    Task(Task&& other) noexcept:
        _handle { std::exchange(other._handle, {}) }
    {
    }

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    ~Task()
    {
        if (_handle)
            _handle.destroy();
    }

    [[nodiscard]] bool IsReady() const noexcept
    {
        return !_handle || _handle.done();
    }
    [[nodiscard]] Handle Native() const noexcept
    {
        return _handle;
    }
    [[nodiscard]] Handle Release() noexcept
    {
        return std::exchange(_handle, {});
    }

    class Awaiter: public Detail::TaskAwaiterBase<void>
    {
      public:
        explicit Awaiter(Handle handle) noexcept:
            Detail::TaskAwaiterBase<void> { handle }
        {
        }
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;
        Awaiter(Awaiter&& other) noexcept:
            Detail::TaskAwaiterBase<void> { std::exchange(other._owned, Handle {}) }
        {
            this->SetCoroutine(_owned);
        }
        Awaiter& operator=(Awaiter&&) = delete;
        ~Awaiter()
        {
            if (_owned)
                _owned.destroy();
        }

        void await_resume()
        {
            auto& promise = this->Coroutine().promise();
            if (promise.exception)
                std::rethrow_exception(promise.exception);
        }

      private:
        Handle _owned { this->Coroutine() };
    };

    Awaiter operator co_await() && noexcept
    {
        return Awaiter { std::exchange(_handle, {}) };
    }

  private:
    Handle _handle {};
};

namespace Detail
{

    template <typename T>
    Task<T> TaskPromise<T>::get_return_object() noexcept
    {
        return Task<T> { std::coroutine_handle<TaskPromise<T>>::from_promise(*this) };
    }

    inline Task<void> TaskPromise<void>::get_return_object() noexcept
    {
        return Task<void> { std::coroutine_handle<TaskPromise<void>>::from_promise(*this) };
    }

} // namespace Detail

/// Synchronously drive a coroutine to completion. Used by tests and the
/// top-level main() body before the reactor is in place. Spins suspending
/// continuations are not supported — the task must be self-driving (i.e.,
/// only await other already-ready awaiters).
///
/// That precondition is *checked*, and the check is load-bearing rather than
/// defensive. A task still suspended when resume() returns has no result to
/// read: `std::get<1>(promise.result)` names an alternative that was never
/// engaged, and — far worse — `~Task()` then destroys a frame that whatever
/// parked the coroutine is still pointing into. InMemorySocket records the
/// awaitable living in that frame as its pending read, so the next Push or
/// CloseWrite on the pipe dereferences freed memory: a SIGSEGV on Linux,
/// heap corruption (0xC0000374) on Windows, an abort on macOS — none of them
/// naming the test that caused it. Throwing here turns that whole class of
/// bug (a drain loop that parks because a reply is an exact multiple of its
/// chunk size, say) into one legible failure at the call site.
/// @tparam T Result type of the task.
/// @param task Task to drive; must not be empty.
/// @return The task's result (or rethrows its exception).
/// @throws std::logic_error if the task is still suspended after being
///         resumed, i.e. it awaited something this function cannot complete.
template <typename T>
T SyncRun(Task<T> task)
{
    auto handle = task.Native();
    handle.resume();
    if (!handle.done())
        throw std::logic_error { "SyncRun: the task is still suspended after resume(). It awaited something "
                                 "SyncRun cannot complete (a socket read with no data and no closed peer, "
                                 "typically). Reading its result would be undefined behaviour." };
    if constexpr (std::is_same_v<T, void>)
    {
        if (auto const& exc = handle.promise().exception; exc)
            std::rethrow_exception(exc);
    }
    else
    {
        auto& promise = handle.promise();
        if (promise.exception)
            std::rethrow_exception(promise.exception);
        return std::move(std::get<1>(promise.result));
    }
}

} // namespace FastCache
