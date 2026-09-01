// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(_WIN32)

    #include <atomic>
    #include <chrono>
    #include <coroutine>
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

    void Run() override;
    void Stop() noexcept override;
    void Submit(std::coroutine_handle<> handle) override;
    void Schedule(TimePoint deadline, std::coroutine_handle<> handle) override;
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

    /// Whether the calling thread is the one inside `Run()`.
    ///
    /// The one-worker-thread property above has always been true and was, until
    /// the completion-lifetime fix, only documented. `IocpSocket` and
    /// `IocpListener` now clear a pending awaitable in their destructors, and
    /// that is safe **only** because the destructor and the completion dispatch
    /// cannot run concurrently -- so the property became load-bearing and needs
    /// something better than a paragraph. Their destructors assert on this.
    ///
    /// False before `Run()` has been entered and after it returns, which is the
    /// honest answer: with nothing dequeuing, there is no worker thread to be on.
    /// Callers that legitimately tear down outside a running reactor check
    /// `Running()` first rather than treating false as a violation.
    /// @return True when this thread is the reactor's worker thread.
    [[nodiscard]] bool IsOnWorkerThread() const noexcept
    {
        return _running.load(std::memory_order_acquire)
               && _workerThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

    /// Whether a thread is currently inside `Run()`.
    /// @return True between entry to and return from `Run()`.
    [[nodiscard]] bool Running() const noexcept
    {
        return _running.load(std::memory_order_acquire);
    }

    /// Min-heap entry; public so anonymous-namespace helpers in the .cpp
    /// can name the type. Treat as Detail.
    struct TimerEntry
    {
        TimePoint deadline;
        std::uint64_t sequence;
        std::coroutine_handle<> handle;
    };

  private:
    void FireExpiredTimers();

    IClock& _clock;
    void* _iocp { nullptr };
    std::atomic<bool> _stopped { false };
    /// Identity of the thread inside `Run()`, and whether one is there at all.
    /// Two variables because a default-constructed `thread::id` is a valid value
    /// to compare against and would make "nobody is running" indistinguishable
    /// from "some thread whose id happens to compare equal".
    std::atomic<std::thread::id> _workerThread {};
    std::atomic<bool> _running { false };
    std::uint64_t _nextSequence { 0 };
    std::mutex _timerMutex;
    std::vector<TimerEntry> _timers;
};

} // namespace FastCache

#endif // _WIN32
