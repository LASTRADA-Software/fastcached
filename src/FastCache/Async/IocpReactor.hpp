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

/// Header struct that every IOCP socket-completion OVERLAPPED extends.
/// The reactor reinterprets each socket completion's LPOVERLAPPED as an
/// IocpCompletion* and calls dispatch(); the socket / listener layer
/// fills in dispatch to route the completion back to its awaitable.
struct IocpCompletion
{
    OVERLAPPED overlapped {};
    void (*dispatch)(IocpCompletion* self, DWORD bytesTransferred, DWORD err) { nullptr };
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
