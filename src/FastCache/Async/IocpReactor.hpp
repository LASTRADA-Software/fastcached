// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/ParkedWork.hpp>
#include <FastCache/Async/ReactorWorkerIdentity.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(_WIN32)

    #include <atomic>
    #include <chrono>
    #include <coroutine>
    #include <cstddef>
    #include <cstdint>
    #include <mutex>
    #include <thread>
    #include <vector>

    #include <windows.h>

namespace FastCache
{

/// The completion status of an overlapped operation, as the kernel wrote it.
///
/// **A distinct type because the value is an NTSTATUS and every consumer of it here
/// wants a Win32/WSA code.** `OVERLAPPED::Internal` holds an NTSTATUS -- a cancelled
/// operation reports `STATUS_CANCELLED`, 0xC0000120 -- while `NetError`'s taxonomy is
/// written in `WSAE*` / `ERROR_*` values, where the same fact is
/// `ERROR_OPERATION_ABORTED`, 995. The two spaces share no values, so handing the raw
/// number to a `WSAE*` table matches no row and lands on `SystemError`: every IOCP
/// error, not merely cancellation, reported as *we do not know*.
///
/// That had already happened. `IocpConnector::OnConnectComplete` found it, wrote the
/// mechanism into its own comment, and converted -- at that ONE call site, while the
/// reactor went on handing every other consumer a bare `DWORD`. `IocpSocket`, which
/// carries every read and write this product does on Windows, was the consumer that
/// did not know, and `NetErrorCode::Cancelled` -- whose doc comment names *"IOCP
/// CancelIoEx"* as the case it exists for -- had never once been produced on this
/// platform.
///
/// **So the fix is the TYPE, not a third copy of the conversion.** A guard folded into
/// the operation is self-enforcing; a guard called alongside one needs a scan. There is
/// no `DWORD` to pass along any more: a consumer that wants an error code has to reach
/// for `Detail::WsaErrorOf` (`Net/IocpStatus.hpp`) to get one, and the fourth consumer
/// nobody has written yet cannot omit the step by not knowing about it. The reactor
/// still cannot do the conversion itself -- `WSAGetOverlappedResult` needs the SOCKET,
/// which the reactor does not have -- which is exactly why the obligation is pushed to
/// the consumers rather than discharged here.
///
/// `RawNtStatus()` exists because a diagnostic sometimes wants the untranslated value.
/// It is named so that spelling the mistake takes saying `RawNtStatus` out loud.
class IocpStatus
{
  public:
    IocpStatus() = default;

    /// @param status The raw NTSTATUS, as read from `OVERLAPPED::Internal`.
    explicit IocpStatus(LONG status) noexcept:
        _status { status }
    {
    }

    /// Whether the operation failed.
    /// @return True when the kernel recorded anything but success.
    [[nodiscard]] bool Failed() const noexcept
    {
        return _status != 0;
    }

    /// The untranslated NTSTATUS, for diagnostics that want the number the kernel
    /// wrote. **Not** a Win32 or WSA error code; see the class comment.
    /// @return The raw NTSTATUS.
    [[nodiscard]] LONG RawNtStatus() const noexcept
    {
        return _status;
    }

  private:
    LONG _status { 0 };
};

/// Header struct that every IOCP socket-completion OVERLAPPED extends.
/// The reactor reinterprets each socket completion's LPOVERLAPPED as an
/// IocpCompletion* and calls dispatch(); the socket / listener layer
/// fills in dispatch to route the completion back to its awaitable.
struct IocpCompletion
{
    OVERLAPPED overlapped {};
    void (*dispatch)(IocpCompletion* self, DWORD bytesTransferred, IocpStatus status) { nullptr };
};

/// Windows IOCP-based reactor.
///
/// One reactor instance, one I/O completion port, ONE worker thread (the
/// thread that calls Run()). Coroutines posted via Submit() are resumed on
/// that thread; a connection's socket is associated with this reactor's port,
/// so all its completions are dequeued by this one thread and its coroutine is
/// never resumed concurrently. Scaling across cores is done by running several
/// independent reactors (one per thread), each owning its own connections —
/// NOT by draining one port from many threads (that migrates a coroutine
/// across threads and is unsafe).
///
/// Submit/Schedule are safe to call from any thread; both go through
/// PostQueuedCompletionStatus.
///
/// Timers: no native IOCP support — we keep a min-heap of deadlines and
/// poll GetQueuedCompletionStatusEx with the time-to-next-deadline as the
/// wait timeout. On wakeup (either completion or timeout) we fire any
/// expired timers before processing further work.
class IocpReactor: public IReactor
{
  public:
    /// Construct over an IClock; the clock drives all deadline checks.
    /// @param clock Time provider used for all deadline checks.
    explicit IocpReactor(IClock& clock);
    ~IocpReactor() override;

    IocpReactor(IocpReactor const&) = delete;
    IocpReactor(IocpReactor&&) = delete;
    IocpReactor& operator=(IocpReactor const&) = delete;
    IocpReactor& operator=(IocpReactor&&) = delete;

    void Stop() noexcept override;
    void Submit(std::coroutine_handle<> handle) override;
    void Submit(ParkedWork work) override;
    void Schedule(TimePoint deadline, std::coroutine_handle<> handle) override;
    void Schedule(TimePoint deadline, ParkedWork work) override;
    [[nodiscard]] bool CancelPending(std::coroutine_handle<> handle) noexcept override;
    [[nodiscard]] IClock& Clock() noexcept override
    {
        return _clock;
    }

    /// Attach a SOCKET (or any HANDLE) to this reactor's IOCP. Required
    /// once per socket before any async I/O can complete on it.
    /// @param handle Native HANDLE/SOCKET (passed as void* for header
    ///        purposes; the implementation casts back).
    /// @return true on success.
    [[nodiscard]] bool AttachHandle(void* handle) noexcept;

    /// Native IOCP handle. Used by sockets/listener to validate they
    /// belong to the right reactor.
    [[nodiscard]] void* NativeHandle() const noexcept
    {
        return _iocp;
    }

    /// Min-heap entry; public so anonymous-namespace helpers in the .cpp
    /// can name the type. Treat as Detail.
    struct TimerEntry
    {
        TimePoint deadline;
        std::uint64_t sequence;
        Detail::Parked parked;
    };

  private:
    void FireExpiredTimers();

    /// Free every await chain still parked here that nothing else can free.
    ///
    /// The platform reactors' shared rule
    /// ([#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)): `Stop()`
    /// sets a flag, `RunLoop()` returns with the timer heap where it was, and nothing
    /// resumes it again. Covers the posted-resumption side too -- see `_posted`.
    void AbandonParkedWork() noexcept;

    /// Take a posted resumption off `_posted`, or make a borrowed entry for it.
    ///
    /// Called at the ONE place a `KeyResumeCoroutine` packet is dequeued, and it hands
    /// back a `Detail::Parked` rather than a bare handle so the release and the resume
    /// stay one expression.
    /// @param handle The handle the completion packet named.
    /// @return The entry that was registered for it, or a borrowing one.
    [[nodiscard]] Detail::Parked TakePosted(std::coroutine_handle<> handle) noexcept;

    IClock& _clock;
    void* _iocp { nullptr };
    std::atomic<bool> _stopped { false };

    /// Chain roots for resumptions posted to the completion port and not yet dequeued.
    ///
    /// **A side table rather than an entry, because on this platform there IS no
    /// entry**: `Submit` posts a completion packet the kernel holds, which is the same
    /// fact `CancelPending` reports by answering `false` for a submission. So the
    /// ownership has to be recorded here and reconciled when the packet comes back.
    /// Only OWNED work is registered -- a borrowed submission adds nothing and costs
    /// nothing -- and the lookup is a scan because the set is the number of detached
    /// chains in flight on one reactor, which is single digits.
    std::mutex _postedMutex;
    std::vector<Detail::Parked> _posted;

    /// `_posted.size()`, readable without the lock.
    ///
    /// `TakePosted` runs for every resumption packet and the set is empty for all of
    /// them unless something detached is in flight, so this is what keeps a borrowed
    /// submission off `_postedMutex` entirely. Written under that mutex, so it never
    /// disagrees with the vector; read with acquire, which is enough because a
    /// registration completes before the packet that would make anyone look.
    std::atomic<std::size_t> _postedCount { 0 };

  protected:
    void RunLoop() override;

  private:
    std::uint64_t _nextSequence { 0 };
    std::mutex _timerMutex;
    std::vector<TimerEntry> _timers;
};

} // namespace FastCache

#endif // _WIN32
