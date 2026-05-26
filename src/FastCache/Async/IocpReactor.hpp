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
/// One reactor instance, one I/O completion port, one worker thread (the
/// thread that calls Run()). Coroutines posted via Submit() are resumed on
/// that thread. Sockets attach themselves to the reactor's IOCP via
/// AttachHandle so their WSARecv/WSASend completions arrive on the same
/// thread.
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
        std::coroutine_handle<> handle;
    };

  private:
    void FireExpiredTimers();

    IClock& _clock;
    void* _iocp { nullptr };
    std::atomic<bool> _stopped { false };
    std::uint64_t _nextSequence { 0 };
    std::mutex _timerMutex;
    std::vector<TimerEntry> _timers;
};

} // namespace FastCache

#endif // _WIN32
