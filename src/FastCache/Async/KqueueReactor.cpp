// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/KqueueReactor.hpp>

#if defined(__APPLE__)

    #include <sys/event.h>
    #include <sys/types.h>

    #include <algorithm>
    #include <chrono>
    #include <cstdint>
    #include <cstring>
    #include <ranges>

    #include <errno.h>
    #include <fcntl.h>
    #include <unistd.h>

namespace FastCache
{

namespace
{

    constexpr auto EntryGreater = [](KqueueReactor::TimerEntry const& a, KqueueReactor::TimerEntry const& b) noexcept {
        if (a.deadline != b.deadline)
            return a.deadline > b.deadline;
        return a.sequence > b.sequence;
    };

    [[nodiscard]] timespec DeadlineToTimespec(TimePoint nextDeadline, TimePoint now) noexcept
    {
        if (nextDeadline <= now)
            return timespec { 0, 0 };
        auto const nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(nextDeadline - now).count();
        if (nanos < 0)
            return timespec { 0, 0 };
        constexpr std::int64_t NanosPerSecond = 1'000'000'000LL;
        return timespec {
            .tv_sec = static_cast<time_t>(nanos / NanosPerSecond),
            .tv_nsec = static_cast<long>(nanos % NanosPerSecond),
        };
    }

} // namespace

KqueueReactor::KqueueReactor(IClock& clock):
    _clock { clock }
{
    _kq = ::kqueue();
    if (::pipe(_wakePipe) == 0)
    {
        // Make both ends non-blocking + CLOEXEC.
        for (auto fd: _wakePipe)
        {
            auto const flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            auto const fdflags = ::fcntl(fd, F_GETFD, 0);
            if (fdflags >= 0)
                ::fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
        }
        // Register the read end with kqueue; udata = nullptr is our
        // sentinel for "this is the wake-up pipe".
        struct kevent ev;
        EV_SET(&ev, _wakePipe[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        ::kevent(_kq, &ev, 1, nullptr, 0, nullptr);
    }
}

KqueueReactor::~KqueueReactor()
{
    if (_wakePipe[0] >= 0)
        ::close(_wakePipe[0]);
    if (_wakePipe[1] >= 0)
        ::close(_wakePipe[1]);
    if (_kq >= 0)
        ::close(_kq);
}

bool KqueueReactor::Attach(KqueueFdHandler* handler) noexcept
{
    // kqueue doesn't have a separate "add fd without filter" call; we
    // register filters on UpdateInterest. Return true if the fd is sane.
    return handler != nullptr && handler->fd >= 0 && _kq >= 0;
}

bool KqueueReactor::UpdateInterest(KqueueFdHandler* handler, bool read, bool write) noexcept
{
    if (!handler || handler->fd < 0 || _kq < 0)
        return false;
    struct kevent changes[2];
    int n = 0;
    EV_SET(&changes[n++], handler->fd, EVFILT_READ, read ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, handler);
    EV_SET(&changes[n++], handler->fd, EVFILT_WRITE, write ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, handler);
    // Ignore ENOENT from EV_DELETE on filters that weren't added yet.
    (void) ::kevent(_kq, changes, n, nullptr, 0, nullptr);
    return true;
}

void KqueueReactor::Detach(KqueueFdHandler* handler) noexcept
{
    if (!handler || handler->fd < 0 || _kq < 0)
        return;
    struct kevent changes[2];
    EV_SET(&changes[0], handler->fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], handler->fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void) ::kevent(_kq, changes, 2, nullptr, 0, nullptr);
}

void KqueueReactor::Submit(std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    {
        std::scoped_lock const lock { _submitMutex };
        _pendingSubmits.push_back(handle);
    }
    char one = 1;
    (void) ::write(_wakePipe[1], &one, 1);
}

void KqueueReactor::Schedule(TimePoint deadline, std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    {
        std::scoped_lock const lock { _timerMutex };
        _timers.push_back(TimerEntry { .deadline = deadline, .sequence = _nextSequence++, .handle = handle });
        std::ranges::push_heap(_timers, EntryGreater);
    }
    char one = 1;
    (void) ::write(_wakePipe[1], &one, 1);
}

void KqueueReactor::Stop() noexcept
{
    _stopped.store(true, std::memory_order_release);
    char one = 1;
    (void) ::write(_wakePipe[1], &one, 1);
}

void KqueueReactor::FireExpiredTimers()
{
    auto const now = _clock.Now();
    std::vector<std::coroutine_handle<>> due;
    {
        std::scoped_lock const lock { _timerMutex };
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

void KqueueReactor::DrainPendingSubmits()
{
    std::deque<std::coroutine_handle<>> drained;
    {
        std::scoped_lock const lock { _submitMutex };
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

void KqueueReactor::Run()
{
    constexpr int Batch = 32;
    struct kevent events[Batch];

    while (!_stopped.load(std::memory_order_acquire))
    {
        TimePoint nextDeadline;
        {
            std::scoped_lock const lock { _timerMutex };
            nextDeadline = _timers.empty() ? TimePoint::max() : _timers.front().deadline;
        }

        timespec ts {};
        timespec* tsPtr = nullptr;
        if (nextDeadline != TimePoint::max())
        {
            ts = DeadlineToTimespec(nextDeadline, _clock.Now());
            tsPtr = &ts;
        }

        auto const n = ::kevent(_kq, nullptr, 0, events, Batch, tsPtr);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            auto const& ev = events[i];
            if (ev.udata == nullptr)
            {
                // Wake-up pipe — drain any bytes so the level-triggered
                // EVFILT_READ doesn't fire again on the same wakes.
                char buf[64];
                while (::read(_wakePipe[0], buf, sizeof(buf)) > 0)
                {
                }
                continue;
            }
            auto* handler = static_cast<KqueueFdHandler*>(ev.udata);
            if (ev.filter == EVFILT_READ && handler->onReadable)
                handler->onReadable(handler);
            else if (ev.filter == EVFILT_WRITE && handler->onWritable)
                handler->onWritable(handler);
        }

        DrainPendingSubmits();
        FireExpiredTimers();
    }
}

} // namespace FastCache

#endif // __APPLE__
