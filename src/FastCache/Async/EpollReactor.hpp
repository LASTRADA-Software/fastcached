// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(__linux__)

    #include <sys/epoll.h> // EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP, named by SelectEpollCallback

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

    /// Invoked instead of the two above when the kernel reports EPOLLERR or
    /// EPOLLHUP.
    ///
    /// It exists because an error has to have somewhere to go. EPOLLERR and
    /// EPOLLHUP are reported whether or not they were requested, so they can
    /// arrive with neither EPOLLIN nor EPOLLOUT set -- and an event that
    /// matches no branch is not simply ignored: the fd is level-triggered, so
    /// it is reported again immediately, and the loop spins at 100% CPU while
    /// never telling anyone. A failed outbound connect is exactly that case,
    /// which is why this was added with the connectors.
    ///
    /// Optional. A handler that leaves it null has an error delivered to
    /// whichever direction it does watch, which is what both existing users
    /// want: a listener's next accept reports it, and a socket's parked
    /// operation fails with it.
    void (*onError)(EpollFdHandler* self) { nullptr };
};

/// Which of a handler's callbacks services one epoll event set, or nullptr when
/// none does.
///
/// Pure, and separate from the loop, because the rule it encodes is where a real
/// defect lived and a unit test can reach it here without a socket, a reactor or
/// a way to provoke a kernel error.
///
/// Two rules, and each was a bug:
///
/// - **An error is routed, never dropped.** EPOLLERR/EPOLLHUP arrive whether or
///   not they were asked for, and can arrive with neither EPOLLIN nor EPOLLOUT.
///   An event matching no branch is not harmlessly ignored: the fd is
///   level-triggered, so it is reported again on the very next iteration and the
///   loop spins forever without ever telling its owner. That is what a failed
///   outbound connect looked like before `onError` existed.
/// - **At most one callback per event.** A callback resumes a coroutine, which
///   may run to completion and free the object this handler is embedded in, so
///   dereferencing `handler` a second time afterwards is a use-after-free.
///   Servicing one condition costs nothing, because level-triggering reports
///   whatever was left over on the next iteration.
///
/// @param handler Handler the event was reported for.
/// @param events The `epoll_event::events` bitset as the kernel reported it.
/// @return The callback to invoke, or nullptr when the handler watches nothing
///         this event speaks to.
[[nodiscard]] inline auto SelectEpollCallback(EpollFdHandler const& handler, std::uint32_t events) noexcept
    -> void (*)(EpollFdHandler*)
{
    constexpr auto ErrorBits = static_cast<std::uint32_t>(EPOLLERR | EPOLLHUP);

    auto const failed = (events & ErrorBits) != 0;
    if (failed && handler.onError != nullptr)
        return handler.onError;
    if ((events & static_cast<std::uint32_t>(EPOLLIN)) != 0 && handler.onReadable != nullptr)
        return handler.onReadable;
    if ((events & static_cast<std::uint32_t>(EPOLLOUT)) != 0 && handler.onWritable != nullptr)
        return handler.onWritable;
    // An error with no dedicated handler goes to whichever direction is watched.
    if (failed)
        return handler.onReadable != nullptr ? handler.onReadable : handler.onWritable;
    return nullptr;
}

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
    [[nodiscard]] bool CancelPending(std::coroutine_handle<> handle) noexcept override;
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
