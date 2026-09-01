// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(__APPLE__)

    #include <sys/event.h> // struct kevent, named by the dequeued-batch member below

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

    /// Invoked instead of the two above when kqueue reports EV_ERROR or EV_EOF.
    ///
    /// The member exists on both reactors' handlers so one dial implementation
    /// can be written against either -- see Net/ReactorDial.hpp. kqueue needs it
    /// less badly than epoll does, because a failed connect here fires
    /// EVFILT_WRITE with EV_EOF and so reaches `onWritable` anyway; naming the
    /// error explicitly is what lets a dial tell "the connect settled, go read
    /// SO_ERROR" from "the socket became writable".
    ///
    /// Optional. A handler that leaves it null has the event delivered to the
    /// filter's own callback, exactly as before.
    void (*onError)(KqueueFdHandler* self) { nullptr };
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
    [[nodiscard]] bool CancelPending(std::coroutine_handle<> handle) noexcept override;
    [[nodiscard]] IClock& Clock() noexcept override
    {
        return _clock;
    }

    /// Register an fd with the kqueue. Initial interest is none; use
    /// UpdateInterest to arm EVFILT_READ / EVFILT_WRITE.
    [[nodiscard]] bool Attach(KqueueFdHandler* handler) const noexcept;

    /// Adjust read/write interest. Adds or removes EVFILT_READ /
    /// EVFILT_WRITE as needed.
    [[nodiscard]] bool UpdateInterest(KqueueFdHandler* handler, bool read, bool write) const noexcept;

    /// Remove the fd from the kqueue.
    /// Remove the fd from the kqueue, and withdraw the handler from the batch
    /// `Run()` is currently walking.
    ///
    /// The second half is the load-bearing one, and it is the twin of
    /// `EpollReactor::Detach` -- see there for the full reasoning. In short:
    /// `EV_DELETE` stops FUTURE reports and does not retract what `kevent` has
    /// already written into the caller's array, so a callback that detaches and
    /// then frees its owner leaves a dangling `udata` in an entry the loop has
    /// not reached yet. Issue #475.
    ///
    /// The `serviced` array in `Run()` does NOT cover this. It dedupes one
    /// handler reported under two filters in the same batch, which is a
    /// different case.
    ///
    /// Must be called BEFORE the owner is freed; every path does.
    /// @param handler The handler to remove. Must still be alive.
    void Detach(KqueueFdHandler* handler) const noexcept;

    /// Min-heap entry; public so anonymous-namespace helpers in the .cpp
    /// can name the type. Treat as Detail.
    struct TimerEntry
    {
        TimePoint deadline {};
        std::uint64_t sequence { 0 };
        std::coroutine_handle<> handle {};
    };

  private:
    void FireExpiredTimers();
    void DrainPendingSubmits();

    /// The batch `Run()` is walking, published so `Detach` can withdraw an entry
    /// from it. Null whenever no batch is in flight.
    struct DequeuedBatch
    {
        struct kevent* events { nullptr }; // ::kevent is also a FUNCTION on Darwin, so the
                                           // elaborated form is required, and it resolves to the
                                           // declaration <sys/event.h> above already made
        int count { 0 };
    };
    DequeuedBatch _batch;

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
