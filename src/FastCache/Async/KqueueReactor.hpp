// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(__APPLE__)

    #include <atomic>
    #include <coroutine>
    #include <cstdint>
    #include <deque>
    #include <mutex>
    #include <vector>

namespace FastCache
{

/// Per-fd handler analogous to EpollFdHandler. kqueue dispatches readiness
/// events to the registered EV_FILTER_{READ,WRITE} via the data.ptr / udata
/// field, and the handler's callbacks perform the actual recv/send.
///
/// Carries an explicit `owner` back-pointer so callbacks can recover their
/// enclosing struct without `offsetof` (UB on non-standard-layout types —
/// KqueueSocket::Impl is non-standard-layout because it holds a reference).
struct KqueueFdHandler
{
    int fd { -1 };
    void* owner { nullptr };
    void (*onReadable)(KqueueFdHandler* self) { nullptr };
    void (*onWritable)(KqueueFdHandler* self) { nullptr };
};

/// macOS kqueue-based reactor. Same external shape as IocpReactor /
/// EpollReactor; internally uses kevent() with a pipe for cross-thread
/// wakeup and a min-heap of deadlines polled via kevent's timespec
/// timeout.
class KqueueReactor: public IReactor
{
  public:
    explicit KqueueReactor(IClock& clock);
    ~KqueueReactor() override;

    KqueueReactor(KqueueReactor const&) = delete;
    KqueueReactor(KqueueReactor&&) = delete;
    KqueueReactor& operator=(KqueueReactor const&) = delete;
    KqueueReactor& operator=(KqueueReactor&&) = delete;

    void Run() override;
    void Stop() noexcept override;
    void Submit(std::coroutine_handle<> handle) override;
    void Schedule(TimePoint deadline, std::coroutine_handle<> handle) override;
    [[nodiscard]] IClock& Clock() noexcept override
    {
        return _clock;
    }

    /// Register an fd with the kqueue. Initial interest is none; use
    /// UpdateInterest to arm EVFILT_READ / EVFILT_WRITE.
    [[nodiscard]] bool Attach(KqueueFdHandler* handler) noexcept;

    /// Adjust read/write interest. Adds or removes EVFILT_READ /
    /// EVFILT_WRITE as needed.
    [[nodiscard]] bool UpdateInterest(KqueueFdHandler* handler, bool read, bool write) noexcept;

    /// Remove the fd from the kqueue.
    void Detach(KqueueFdHandler* handler) noexcept;

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
    void DrainPendingSubmits();

    IClock& _clock;
    int _kq { -1 };
    int _wakePipe[2] { -1, -1 }; ///< [0]=read, [1]=write; write-end signal wakes kevent.
    std::atomic<bool> _stopped { false };
    std::uint64_t _nextSequence { 0 };

    std::mutex _submitMutex;
    std::deque<std::coroutine_handle<>> _pendingSubmits;

    std::mutex _timerMutex;
    std::vector<TimerEntry> _timers;
};

} // namespace FastCache

#endif // __APPLE__
