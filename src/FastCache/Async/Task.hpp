// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace FastCache
{

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

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            _handle.promise().continuation = continuation;
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

/// Fire-and-forget coroutine. The body runs to its first suspend on
/// construction; the frame self-destroys on final return. Used by the
/// server to spawn one coroutine per connection without keeping a handle
/// around — the chain of I/O awaitables (paired with the reactor's
/// resumption path) drives progress.
///
/// Exceptions escaping the body terminate the process — connection-level
/// errors must be caught and turned into NetError/ProtocolError responses
/// inside the handler before reaching final_suspend.
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

/// Synchronously drive a coroutine to completion. Used by tests and the
/// top-level main() body before the reactor is in place. Spins suspending
/// continuations are not supported — the task must be self-driving (i.e.,
/// only await other already-ready awaiters).
/// @tparam T Result type of the task.
/// @param task Task to drive; must not be empty.
/// @return The task's result (or rethrows its exception).
template <typename T>
T SyncRun(Task<T> task)
{
    auto handle = task.Native();
    handle.resume();
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
