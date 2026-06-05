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
/// One reactor instance and one I/O completion port, drained by one *or
/// more* worker threads — every thread that calls Run() pulls completions
/// from the same port. A single connection coroutine only ever has one
/// outstanding operation at a time (it awaits reads/writes sequentially and
/// the protocol handlers schedule no concurrent timers), so a given
/// coroutine is only ever resumed by the one thread that dequeues its
/// completion — two threads never resume the same coroutine concurrently.
/// Running N threads lets a blocking storage call (an fsync on the disk
/// backend) occupy one thread while the others keep accepting and serving;
/// it also means any IStorage reachable from a connection must be
/// thread-safe (the server wraps the disk backend in a ShardedStorage).
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
    /// @param concurrency Maximum number of threads the completion port lets
    ///        run concurrently. 0 means "one per logical processor" (the IOCP
    ///        default). 1 preserves strict single-threaded semantics for tests
    ///        and the in-memory server. Must be paired with the same number of
    ///        threads calling Run().
    explicit IocpReactor(IClock& clock, unsigned concurrency = 1);
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
