// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/NetError.hpp>

#include <coroutine>
#include <cstdint>
#include <expected>
#include <memory>

namespace FastCache
{

/// Whether a listener sets SO_REUSEPORT so several listeners can share one
/// port and have the kernel load-balance connections across them (one listener
/// per reactor thread). A strong enum rather than a bare bool so call sites
/// read `ReusePort::Yes` instead of an opaque `true`.
enum class ReusePort : bool
{
    No = false,
    Yes = true,
};

/// Result of an asynchronous accept: either a newly-connected ISocket or an
/// error (typically Cancelled on shutdown or a transient OS condition).
/// Alias of `SocketResult`: accept and connect answer the same question, so
/// they hand back the same type and a helper written for one works for the
/// other. Kept as a name because seventy call sites read better saying what
/// they are doing.
using AcceptResult = SocketResult;

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

    /// @return The local TCP port this listener actually bound, in host byte
    ///         order, or 0 for a transport that has no port.
    ///
    /// On the interface rather than on one implementation because a bind to port
    /// 0 means "pick a free one", so the port an operator, a log line or a test
    /// needs is the one the kernel chose -- and a caller holding an `IListener`
    /// had no way to ask. Every script-driven test here allocates its ports this
    /// way rather than hard-coding them, for the reason `dist-compile-e2e`
    /// records: a fixed port collides with whatever else the machine is doing,
    /// and the failure reads as the feature under test being broken.
    [[nodiscard]] virtual std::uint16_t BoundPort() const noexcept = 0;
};

} // namespace FastCache
