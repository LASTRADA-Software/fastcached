// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/EpollReactor.hpp>

#if defined(__linux__)

    #include <sys/epoll.h>
    #include <sys/eventfd.h>

    #include <algorithm>
    #include <chrono>
    #include <cstdint>
    #include <ranges>

    #include <errno.h>
    #include <unistd.h>

namespace FastCache
{

namespace
{

    /// Min-heap comparator: earlier deadline wins, FIFO on ties.
    constexpr auto EntryGreater = [](EpollReactor::TimerEntry const& a, EpollReactor::TimerEntry const& b) noexcept {
        if (a.deadline != b.deadline)
            return a.deadline > b.deadline;
        return a.sequence > b.sequence;
    };

    [[nodiscard]] int DeadlineToMs(TimePoint nextDeadline, TimePoint now) noexcept
    {
        if (nextDeadline == TimePoint::max())
            return -1; // infinite
        if (nextDeadline <= now)
            return 0;
        auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(nextDeadline - now).count();
        if (millis < 0)
            return 0;
        if (millis > std::numeric_limits<int>::max())
            return std::numeric_limits<int>::max();
        return static_cast<int>(millis);
    }

} // namespace

EpollReactor::EpollReactor(IClock& clock):
    _clock { clock }
{
    _epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    _wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_epollFd >= 0 && _wakeFd >= 0)
    {
        epoll_event ev {};
        ev.events = EPOLLIN;
        ev.data.ptr = nullptr; // sentinel: nullptr means "wake event"
        ::epoll_ctl(_epollFd, EPOLL_CTL_ADD, _wakeFd, &ev);
    }
}

EpollReactor::~EpollReactor()
{
    if (_wakeFd >= 0)
        ::close(_wakeFd);
    if (_epollFd >= 0)
        ::close(_epollFd);
}

bool EpollReactor::Attach(EpollFdHandler* handler) noexcept
{
    if (!handler || handler->fd < 0 || _epollFd < 0)
        return false;
    epoll_event ev {};
    ev.events = 0; // start with no interest
    ev.data.ptr = handler;
    return ::epoll_ctl(_epollFd, EPOLL_CTL_ADD, handler->fd, &ev) == 0;
}

bool EpollReactor::UpdateInterest(EpollFdHandler* handler, bool read, bool write) noexcept
{
    if (!handler || handler->fd < 0 || _epollFd < 0)
        return false;
    epoll_event ev {};
    ev.events = 0;
    if (read)
        ev.events |= EPOLLIN;
    if (write)
        ev.events |= EPOLLOUT;
    ev.data.ptr = handler;
    return ::epoll_ctl(_epollFd, EPOLL_CTL_MOD, handler->fd, &ev) == 0;
}

void EpollReactor::Detach(EpollFdHandler* handler) noexcept
{
    if (!handler || handler->fd < 0 || _epollFd < 0)
        return;
    epoll_event ev {};
    ::epoll_ctl(_epollFd, EPOLL_CTL_DEL, handler->fd, &ev);
}

void EpollReactor::Submit(std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    {
        std::lock_guard const lock { _submitMutex };
        _pendingSubmits.push_back(handle);
    }
    std::uint64_t one = 1;
    (void) ::write(_wakeFd, &one, sizeof(one));
}

void EpollReactor::Schedule(TimePoint deadline, std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    {
        std::lock_guard const lock { _timerMutex };
        _timers.push_back(TimerEntry { .deadline = deadline, .sequence = _nextSequence++, .handle = handle });
        std::ranges::push_heap(_timers, EntryGreater);
    }
    std::uint64_t one = 1;
    (void) ::write(_wakeFd, &one, sizeof(one));
}

void EpollReactor::Stop() noexcept
{
    _stopped.store(true, std::memory_order_release);
    std::uint64_t one = 1;
    (void) ::write(_wakeFd, &one, sizeof(one));
}

void EpollReactor::FireExpiredTimers()
{
    auto const now = _clock.Now();
    std::vector<std::coroutine_handle<>> due;
    {
        std::lock_guard const lock { _timerMutex };
        while (!_timers.empty() && _timers.front().deadline <= now)
        {
            std::ranges::pop_heap(_timers, EntryGreater);
            due.push_back(_timers.back().handle);
            _timers.pop_back();
        }
    }
    for (auto handle: due)
        if (handle && !handle.done())
            handle.resume();
}

void EpollReactor::DrainPendingSubmits()
{
    std::deque<std::coroutine_handle<>> drained;
    {
        std::lock_guard const lock { _submitMutex };
        drained.swap(_pendingSubmits);
    }
    while (!drained.empty())
    {
        auto handle = drained.front();
        drained.pop_front();
        if (handle && !handle.done())
            handle.resume();
    }
}

void EpollReactor::Run()
{
    constexpr int Batch = 32;
    epoll_event events[Batch];

    while (!_stopped.load(std::memory_order_acquire))
    {
        TimePoint nextDeadline;
        {
            std::lock_guard const lock { _timerMutex };
            nextDeadline = _timers.empty() ? TimePoint::max() : _timers.front().deadline;
        }
        auto const timeout = DeadlineToMs(nextDeadline, _clock.Now());

        auto const n = ::epoll_wait(_epollFd, events, Batch, timeout);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            auto const& ev = events[i];
            if (ev.data.ptr == nullptr)
            {
                // Wake event — drain the eventfd counter and move on.
                std::uint64_t buf {};
                (void) ::read(_wakeFd, &buf, sizeof(buf));
                continue;
            }
            auto* handler = static_cast<EpollFdHandler*>(ev.data.ptr);
            if ((ev.events & EPOLLIN) && handler->onReadable)
                handler->onReadable(handler);
            if ((ev.events & EPOLLOUT) && handler->onWritable)
                handler->onWritable(handler);
        }

        DrainPendingSubmits();
        FireExpiredTimers();
    }
}

} // namespace FastCache

#endif // __linux__
