// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/NetError.hpp>

#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace FastCache
{

/// Result type used by the I/O awaitables. `value` is the number of bytes
/// transferred (or 0 on EOF for reads); `error` is the failure cause.
using IoResult = std::expected<std::size_t, NetError>;

/// A connected socket, or why one could not be produced.
///
/// One name because accept and connect answer the same question and their
/// results are the same type. `IListener.hpp`'s `AcceptResult` is an alias of
/// this, so a helper that consumes one consumes the other -- and so a connect
/// path needs no result-carrying awaitable of its own, which would have been a
/// verbatim copy of `AcceptAwaitable`.
class ISocket;
using SocketResult = std::expected<std::unique_ptr<ISocket>, NetError>;

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
    ///
    /// **A socket has ONE read operation, and this shares it with
    /// `WaitReadable`.** Every reactor socket keeps a single `awaitable` pointer per
    /// direction, and both read verbs begin by claiming it. So arming either while
    /// the other is parked drops the parked awaitable's pointer: that coroutine is
    /// never resumed and never freed -- one leaked frame plus everything it
    /// captured, per occurrence, with no assertion, no error and no log
    /// ([#663](https://github.com/LASTRADA-Software/fastcached/issues/663)).
    ///
    /// The rule was stated only in `RedisResp.cpp`, about that file's own watcher,
    /// so somebody writing the next `WaitReadable` user read this interface and got
    /// no warning at all. It is stated here because here is where it has to be
    /// obeyed, and it is enforced by `Detail::ClaimReadSlot`
    /// (`FastCache/Net/ReadSlot.hpp`) in Debug builds, watched refusing by
    /// `ctest -R read-slot-guard-canary`.
    ///
    /// What a caller must therefore do: hold at most one outstanding read per
    /// socket, and resolve or abandon a parked `WaitReadable` before issuing a
    /// `Read`. A parked wait can only be retrieved by `Close()`, which is what makes
    /// this a caller-side discipline rather than something a socket can fix
    /// ([#710](https://github.com/LASTRADA-Software/fastcached/issues/710) is one
    /// caller that does not).
    ///
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

    /// Suspend until the socket is readable (data or EOF pending) WITHOUT
    /// consuming any bytes. Used by the pub/sub subscribe loop, which must wake
    /// on either an incoming client command OR a delivered message and cannot
    /// afford to block in `Read` while messages queue.
    ///
    /// **The count says WHICH kind of readable, and it is not advisory.**
    ///
    ///   - `0`  the peer has closed its write side. A `Read` here returns EOF.
    ///   - `>0` bytes are pending. A `Read` here returns some of them.
    ///
    /// It used to be documented as advisory, and it was: `EpollSocket` and
    /// `KqueueSocket` answered `1` whatever their `recv(MSG_PEEK)` had just
    /// measured, and `IocpSocket` completed with the byte count of a ZERO-byte
    /// `WSARecv`, which is `0` however much data is waiting. So the same call
    /// reported opposite numbers on Windows and Linux for the same event, and
    /// "advisory" is what let that stand
    /// ([#677](https://github.com/LASTRADA-Software/fastcached/issues/677)).
    ///
    /// The kernel draws the distinction, the probe already computes it, and only
    /// the reporting threw it away -- which is why making it mean something is a
    /// correction rather than a design decision. What a CALLER should do about EOF
    /// is a separate question and deliberately not settled here
    /// ([#671](https://github.com/LASTRADA-Software/fastcached/issues/671)).
    ///
    /// **The default answers `1`, and that is the fail-safe direction.** A transport
    /// that cannot tell must not claim EOF: a false `>0` costs one `Read` that
    /// discovers the truth, while a false `0` tells a caller its peer is gone. For
    /// blocking and in-memory transports the subsequent `Read` either returns data or
    /// sees EOF inline, so always-ready remains correct for them.
    ///
    /// **This shares the socket's single read-op slot with `Read`**, and arming one
    /// over the other silently drops the parked coroutine. The rule, the enforcement
    /// and what a caller has to do about it are on `Read` above
    /// ([#663](https://github.com/LASTRADA-Software/fastcached/issues/663)); it is
    /// repeated here only as a pointer, because a second copy of the reasoning is a
    /// second thing to be wrong.
    ///
    /// **"Consumes nothing" is about bytes the CALLER could have read**, not about
    /// the transport's own buffering. A decorator may have to consume and decode raw
    /// bytes to answer at all -- `TlsSocket` decrypts a record, because a TLS peer's
    /// close is a record and a raw peek reads it as pending data
    /// ([#712](https://github.com/LASTRADA-Software/fastcached/issues/712)) -- and
    /// what the contract forbids is losing a byte the next `Read` would have
    /// returned.
    /// @return Awaitable resolving when readable; `0` means EOF, `>0` means data.
    [[nodiscard]] virtual IoAwaitable WaitReadable()
    {
        return IoAwaitable { IoResult { std::size_t { 1 } } };
    }

    /// The remote peer's address as a printable host string ("203.0.113.7" /
    /// "::1"), captured at accept time. Used by the `--log-source` connection
    /// log prefix. The default returns "" so transports that have no peer
    /// address (the in-memory test transport) need no override.
    /// @return Printable peer host, or "" when unknown.
    [[nodiscard]] virtual std::string PeerAddress() const
    {
        return {};
    }

    /// Close the socket. Idempotent; subsequent Read/Write resolves with
    /// NetErrorCode::BadFileHandle.
    ///
    /// **A parked read is retrieved here, and this is the only thing that can
    /// retrieve one.** Nothing else takes a wait back off a socket, so an
    /// implementation that leaves a parked `Read` or `WaitReadable` alone leaves a
    /// coroutine frame nobody can free. Every transport this library hands out
    /// completes such an awaitable with `NetErrorCode::Cancelled`.
    ///
    /// The order is load-bearing: detach the awaitable from the operation FIRST,
    /// complete it LAST, and touch no member afterwards. Completing resumes the
    /// awaiting coroutine, and a coroutine that OWNS the socket runs to its end and
    /// destroys it before `Complete` returns -- which is a heap-use-after-free this
    /// tree has already had (see `EpollSocket::Close`).
    virtual void Close() noexcept = 0;

    /// Close this socket's WRITE half, leaving the read half open, so the peer
    /// observes EOF while this side can still receive.
    ///
    /// **What EOF means on this wire, decided once**
    /// ([#671](https://github.com/LASTRADA-Software/fastcached/issues/671)):
    ///
    /// > EOF means *"this peer has finished sending"*. A server answers everything
    /// > already determined, and abandons anything still pending.
    ///
    /// So a half-close is a statement about INPUT, not a departure: replies the peer
    /// is already owed still arrive, and only work that has not happened yet is given
    /// up. `.agent/rules/wire-and-protocol.md` carries the derivation and the
    /// measurement it rests on; do not re-decide it here.
    ///
    /// **Why the interface has it at all.** It did not, and the absence is what made
    /// the question unanswerable: `InMemorySocket` had one -- public, and called by
    /// some forty-odd tests -- but it was on the CONCRETE type and not on this
    /// interface, so no code holding an `ISocket&` could reach it and every consumer
    /// was therefore a test. Not a visibility problem, which is why "make it public"
    /// would have changed nothing: production code never names `InMemorySocket`. A
    /// rule nothing can express is a rule nothing can be held to.
    ///
    /// **The default does nothing, and that is for FAKES.** Every transport this
    /// library hands out overrides it; a scripted test double that has no write half
    /// to close has nothing to do here, and making it pure virtual would reach into
    /// five test files across three lanes to say so. A no-op costs a peer the early
    /// EOF -- it learns at the eventual `Close()` instead -- which delays a
    /// notification rather than falsifying one.
    ///
    /// Idempotent, and not a `Close()`: reads keep working, and `IsClosed()` stays
    /// false. A caller that wants both calls both.
    ///
    /// `TlsSocket` qualifies the "reads keep working" half and says why on its own
    /// override -- TLS reads write, so they are the one case a half-close can reach.
    virtual void ShutdownWrite() noexcept {}

    /// @return true if Close() has been called or the peer has closed and a
    /// Read has observed EOF. Used by Connection to break its loop.
    [[nodiscard]] virtual bool IsClosed() const noexcept = 0;
};

} // namespace FastCache
