// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/NetError.hpp>

#include <coroutine>
#include <cstddef>
#include <expected>
#include <span>
#include <utility>

namespace FastCache
{

/// Result type used by the I/O awaitables. `value` is the number of bytes
/// transferred (or 0 on EOF for reads); `error` is the failure cause.
using IoResult = std::expected<std::size_t, NetError>;

/// Awaitable returned by ISocket::Read / Write. Completion-shaped: the
/// reactor / transport gets to know about the operation, suspends the
/// coroutine, and resumes with an IoResult.
///
/// The buffer passed to Read/Write must outlive the awaitable — that's the
/// caller's contract, documented here so it travels with the type.
class IoAwaitable
{
  public:
    /// Construct an awaitable in an already-completed state. Used when an
    /// implementation can satisfy the operation synchronously.
    /// @param result Eagerly-known result.
    explicit IoAwaitable(IoResult result) noexcept:
        _result { std::move(result) },
        _ready { true }
    {
    }

    /// Construct an awaitable that will be completed asynchronously. The
    /// implementation arranges suspension in await_suspend.
    IoAwaitable() noexcept = default;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return _ready;
    }

    /// Suspend hook — backends override the parent ISocket method to wire
    /// the coroutine handle into their resume path. The default
    /// implementation is "never suspend"; backends do their own subclassing
    /// or use the SetSuspendCallback hook below.
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        _handle = handle;
        if (_suspendCallback)
            _suspendCallback(this, handle);
    }

    IoResult await_resume() noexcept
    {
        return _result;
    }

    /// Called by the backend to publish the result and resume the suspended
    /// coroutine. Safe to call from any thread; the resume happens
    /// immediately on the caller's thread (production backends marshal back
    /// to the reactor thread before calling Complete()).
    void Complete(IoResult result) noexcept
    {
        _result = result;
        _ready = true;
        if (_handle && !_handle.done())
            _handle.resume();
    }

    /// Backend hook: register a callback invoked when the awaitable
    /// suspends. The callback receives this awaitable and the suspended
    /// handle so it can wire up the I/O completion path.
    using SuspendCallback = void (*)(IoAwaitable* self, std::coroutine_handle<> handle);
    void SetSuspendCallback(SuspendCallback callback, void* state) noexcept
    {
        _suspendCallback = callback;
        _suspendCallbackState = state;
    }

    /// Backend-private state pointer (e.g., back-pointer to the socket).
    [[nodiscard]] void* CallbackState() const noexcept
    {
        return _suspendCallbackState;
    }

  private:
    IoResult _result { 0 };
    std::coroutine_handle<> _handle {};
    SuspendCallback _suspendCallback { nullptr };
    void* _suspendCallbackState { nullptr };
    bool _ready { false };
};

/// Connected, streamed, bidirectional byte transport. Implementations:
/// PosixSocket, WindowsSocket (production), InMemoryTransport (tests).
///
/// All I/O is completion-shaped via IoAwaitable. The buffer passed to Read
/// or Write must remain alive (and at the same address) until the awaitable
/// resumes — required for IOCP correctness and a useful invariant elsewhere.
class ISocket
{
  public:
    ISocket() = default;
    ISocket(ISocket const&) = delete;
    ISocket(ISocket&&) = delete;
    ISocket& operator=(ISocket const&) = delete;
    ISocket& operator=(ISocket&&) = delete;
    virtual ~ISocket() = default;

    /// Read up to buffer.size() bytes into buffer. Resolves with the byte
    /// count written, 0 on clean EOF, or a NetError on failure.
    /// @param buffer Destination span; must outlive the awaitable.
    /// @return Awaitable resolving to IoResult.
    [[nodiscard]] virtual IoAwaitable Read(std::span<std::byte> buffer) = 0;

    /// Write all of buffer's bytes. Resolves with the byte count actually
    /// written (== buffer.size() on success), or a NetError on failure.
    /// @param buffer Source span; must outlive the awaitable.
    /// @return Awaitable resolving to IoResult.
    [[nodiscard]] virtual IoAwaitable Write(std::span<std::byte const> buffer) = 0;

    /// Close the socket. Idempotent; subsequent Read/Write resolves with
    /// NetErrorCode::BadFileHandle.
    virtual void Close() noexcept = 0;

    /// @return true if Close() has been called or the peer has closed and a
    /// Read has observed EOF. Used by Connection to break its loop.
    [[nodiscard]] virtual bool IsClosed() const noexcept = 0;
};

} // namespace FastCache
