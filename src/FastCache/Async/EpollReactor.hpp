// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(__linux__)

    #include <atomic>
    #include <coroutine>
    #include <cstdint>
    #include <deque>
    #include <mutex>
    #include <vector>

namespace FastCache
{

/// Per-fd handler registered with EpollReactor. Sockets and listeners
/// embed one of these and set the callbacks; the reactor dispatches
/// readiness events to them via the stored function pointers.
///
/// The handler carries an explicit `owner` back-pointer so the callbacks
/// can recover their enclosing struct without `offsetof` (which is UB on
/// non-standard-layout types — and EpollSocket::Impl is non-standard-
/// layout because it holds an EpollReactor reference).
///
/// Lifetime: the handler must outlive its registration. Sockets clear
/// the registration via EpollReactor::Detach() before destruction.
struct EpollFdHandler
{
    int fd { -1 };
    void* owner { nullptr };
    void (*onReadable)(EpollFdHandler* self) { nullptr };
    void (*onWritable)(EpollFdHandler* self) { nullptr };
};

/// Linux epoll-based reactor.
///
/// Single-threaded by contract (Run() must be called from one thread).
/// Submit/Schedule are safe to call from any thread; they wake the
/// reactor thread via eventfd.
///
/// Sockets are non-blocking; readiness events fire EpollFdHandler
/// callbacks which perform the actual recv/send. This makes the
/// awaitable surface completion-shaped from the caller's perspective
/// while keeping the reactor backend in the readiness model.
class EpollReactor: public IReactor
{
  public:
    explicit EpollReactor(IClock& clock);
    ~EpollReactor() override;

    EpollReactor(EpollReactor const&) = delete;
    EpollReactor(EpollReactor&&) = delete;
    EpollReactor& operator=(EpollReactor const&) = delete;
    EpollReactor& operator=(EpollReactor&&) = delete;

    void Run() override;
    void Stop() noexcept override;
    void Submit(std::coroutine_handle<> handle) override;
    void Schedule(TimePoint deadline, std::coroutine_handle<> handle) override;
    [[nodiscard]] IClock& Clock() noexcept override
    {
        return _clock;
    }

    /// Register an EpollFdHandler with the reactor. Initial interest is
    /// none; the caller adjusts via UpdateInterest after registration.
    /// @param handler Stable address; lifetime owned by the caller.
    /// @return true on success.
    [[nodiscard]] bool Attach(EpollFdHandler* handler) const noexcept;

    /// Update the epoll interest mask on an attached fd. Pass `read=true`
    /// to register interest in EPOLLIN, `write=true` for EPOLLOUT.
    /// Setting both to false re-arms the handler with edge-triggered
    /// no-interest (used to mute an fd after one-shot completion).
    [[nodiscard]] bool UpdateInterest(EpollFdHandler* handler, bool read, bool write) const noexcept;

    /// Remove the fd from the epoll set. Safe even if Attach was never
    /// called.
    void Detach(EpollFdHandler* handler) const noexcept;

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

    IClock& _clock;
    int _epollFd { -1 };
    int _wakeFd { -1 }; ///< eventfd used for cross-thread wakeup.
    std::atomic<bool> _stopped { false };
    std::uint64_t _nextSequence { 0 };

    std::mutex _submitMutex;
    std::deque<std::coroutine_handle<>> _pendingSubmits;

    std::mutex _timerMutex;
    std::vector<TimerEntry> _timers;
};

} // namespace FastCache

#endif // __linux__
