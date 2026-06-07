// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Errors/NetError.hpp>

#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
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

    /// Suspend hook — backends wire the coroutine handle into their resume
    /// path through the SetSuspendCallback hook below.
    ///
    /// Returns `bool` (not `void`) so a backend that can satisfy the operation
    /// *synchronously from inside its suspend callback* — e.g. the TLS decorator
    /// finding a full record already buffered, or a wrapped transport that
    /// resolves its raw I/O inline — does not have to resume re-entrantly. While
    /// the callback runs, a synchronous Complete() only records the result (see
    /// the `_inSuspendCallback` guard there); we then return `false`, telling the
    /// coroutine machinery to resume immediately via the normal await_resume path
    /// rather than suspending and being resumed from within await_suspend (which
    /// is undefined behaviour). A backend that genuinely defers (the reactor
    /// path) leaves `_ready` false, so we return `true` and stay suspended until
    /// Complete() runs later from the reactor thread.
    /// @param handle The suspended coroutine to resume on completion.
    /// @return true to suspend; false if the op already completed synchronously.
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        _handle = handle;
        if (_suspendCallback)
        {
            _inSuspendCallback = true;
            _suspendCallback(this, handle);
            _inSuspendCallback = false;
        }
        return !_ready;
    }

    IoResult await_resume() noexcept
    {
        return _result;
    }

    /// Called by the backend to publish the result and resume the suspended
    /// coroutine. Safe to call from the coroutine's own thread; production
    /// backends marshal back to the reactor thread before calling Complete().
    /// If invoked synchronously from within the suspend callback (a backend that
    /// completed inline), the resume is suppressed — await_suspend observes the
    /// published result and returns `false`, so the coroutine resumes without a
    /// re-entrant `resume()` call.
    void Complete(IoResult result) noexcept
    {
        _result = result;
        _ready = true;
        if (_inSuspendCallback)
            return;
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
    /// True only while await_suspend is running the suspend callback, so a
    /// synchronous Complete() can defer its resume to the await_suspend return.
    bool _inSuspendCallback { false };
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

    /// Gather-write: send every segment in order as one logical write, using
    /// a single scattered syscall (`sendmsg`/`WSASend`) where the platform
    /// allows. Avoids copying large payloads into one contiguous buffer — the
    /// canonical use is a GET reply assembled as `[header][value][trailer]`
    /// where `value` points directly into the cached, reference-counted
    /// payload. Resolves only once *all* bytes are sent (total == the sum of
    /// segment sizes), or a NetError on failure.
    ///
    /// Lifetime contract: every segment's bytes, **and** the `segments` span
    /// itself, must remain valid and at a stable address until the awaitable
    /// resumes — the write may suspend on backpressure and resume on a later
    /// reactor frame. To anchor a reference-counted payload for exactly that
    /// long, pass it as `keepAlive`: the implementation stores the handle
    /// alongside the in-flight operation, so the bytes outlive the suspend
    /// even if the caller's `GetResult` goes out of scope. `keepAlive` is
    /// type-erased (`shared_ptr<void const>`) so any owner shape works.
    ///
    /// @param segments Ordered, non-owning views to gather, in send order.
    /// @param keepAlive Optional owner pinning the segments' backing storage
    ///        for the operation's lifetime.
    /// @return Awaitable resolving to IoResult (total bytes written).
    [[nodiscard]] virtual IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                                    std::shared_ptr<void const> keepAlive = {}) = 0;

    /// Perform any transport-level handshake required before application I/O.
    /// Plaintext sockets need none, so the default resolves immediately; the
    /// TLS decorator overrides it to drive the SSL handshake. The connection
    /// loop awaits this once before protocol autodetection, so it stays
    /// transport-agnostic and a slow handshake runs on the per-connection
    /// coroutine rather than blocking the accept loop.
    /// @return Awaitable resolving to success, or a NetError on failure.
    [[nodiscard]] virtual Task<std::expected<void, NetError>> HandshakeIfNeeded()
    {
        co_return std::expected<void, NetError> {};
    }

    /// Close the socket. Idempotent; subsequent Read/Write resolves with
    /// NetErrorCode::BadFileHandle.
    virtual void Close() noexcept = 0;

    /// @return true if Close() has been called or the peer has closed and a
    /// Read has observed EOF. Used by Connection to break its loop.
    [[nodiscard]] virtual bool IsClosed() const noexcept = 0;
};

} // namespace FastCache
