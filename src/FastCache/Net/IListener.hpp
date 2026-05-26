// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <coroutine>
#include <expected>
#include <memory>

namespace FastCache
{

/// Result of an asynchronous accept: either a newly-connected ISocket or an
/// error (typically Cancelled on shutdown or a transient OS condition).
using AcceptResult = std::expected<std::unique_ptr<ISocket>, NetError>;

/// Awaitable returned by IListener::Accept. Same shape as IoAwaitable but
/// carries a socket pointer instead of a byte count.
class AcceptAwaitable
{
  public:
    explicit AcceptAwaitable(AcceptResult result) noexcept:
        _result { std::move(result) },
        _ready { true }
    {
    }
    AcceptAwaitable() noexcept = default;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return _ready;
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        _handle = handle;
        if (_suspendCallback)
            _suspendCallback(this, handle);
    }

    AcceptResult await_resume() noexcept
    {
        return std::move(_result);
    }

    void Complete(AcceptResult result) noexcept
    {
        _result = std::move(result);
        _ready = true;
        if (_handle && !_handle.done())
            _handle.resume();
    }

    using SuspendCallback = void (*)(AcceptAwaitable* self, std::coroutine_handle<> handle);
    void SetSuspendCallback(SuspendCallback callback, void* state) noexcept
    {
        _suspendCallback = callback;
        _suspendCallbackState = state;
    }

    [[nodiscard]] void* CallbackState() const noexcept
    {
        return _suspendCallbackState;
    }

  private:
    AcceptResult _result { std::unexpected(NetError {}) };
    std::coroutine_handle<> _handle {};
    SuspendCallback _suspendCallback { nullptr };
    void* _suspendCallbackState { nullptr };
    bool _ready { false };
};

/// Server-side endpoint that produces ISockets via Accept().
class IListener
{
  public:
    IListener() = default;
    IListener(IListener const&) = delete;
    IListener(IListener&&) = delete;
    IListener& operator=(IListener const&) = delete;
    IListener& operator=(IListener&&) = delete;
    virtual ~IListener() = default;

    /// @return Awaitable resolving to the next accepted connection.
    [[nodiscard]] virtual AcceptAwaitable Accept() = 0;

    /// Stop accepting. Any in-flight Accept() awaitable resolves with
    /// NetErrorCode::Cancelled.
    virtual void Close() noexcept = 0;
};

} // namespace FastCache
