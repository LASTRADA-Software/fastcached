// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/KqueueReactor.hpp>
#include <FastCache/Core/Ranges.hpp>

#if defined(__APPLE__)

    #include <sys/event.h>
    #include <sys/types.h>

    #include <algorithm>
    #include <array>
    #include <cerrno>
    #include <chrono>
    #include <cstdint>
    #include <cstring>
    #include <iterator>
    #include <ranges>

    #include <fcntl.h>
    #include <unistd.h>

namespace FastCache
{

namespace
{

    /// What `Detach` writes over a withdrawn batch entry. The twin of the epoll
    /// one -- a distinct value rather than `nullptr`, which already means "the
    /// wake pipe", so that a withdrawal is not two facts sharing one
    /// representation.
    [[nodiscard]] void* WithdrawnBatchEntry() noexcept
    {
        static char tombstone = 0;
        return &tombstone;
    }

    constexpr auto EntryGreater = [](KqueueReactor::TimerEntry const& a, KqueueReactor::TimerEntry const& b) noexcept {
        if (a.deadline != b.deadline)
            return a.deadline > b.deadline;
        return a.sequence > b.sequence;
    };

    /// One kqueue filter and whether the caller wants it armed. Lets
    /// UpdateInterest express "arm or drop" once and drive both filters from a
    /// table instead of two hand-written EV_SET calls that must stay in sync.
    struct FilterInterest
    {
        std::int16_t filter; ///< EVFILT_READ or EVFILT_WRITE.
        bool wanted;         ///< True to arm the filter, false to drop it.
    };

    [[nodiscard]] timespec DeadlineToTimespec(TimePoint nextDeadline, TimePoint now) noexcept
    {
        if (nextDeadline <= now)
            return timespec { .tv_sec = 0, .tv_nsec = 0 };
        auto const nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(nextDeadline - now).count();
        if (nanos < 0)
            return timespec { .tv_sec = 0, .tv_nsec = 0 };
        constexpr std::int64_t NanosPerSecond = 1'000'000'000LL;
        return timespec {
            .tv_sec = static_cast<time_t>(nanos / NanosPerSecond),
            .tv_nsec = static_cast<long>(nanos % NanosPerSecond),
        };
    }

} // namespace

KqueueReactor::KqueueReactor(IClock& clock):
    _clock { clock },
    _kq { ::kqueue() }
{
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
        struct kevent ev {};
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

bool KqueueReactor::CancelPending(std::coroutine_handle<> handle) noexcept
{
    if (!handle)
        return false;

    {
        std::scoped_lock const guard { _submitMutex };
        if (auto const found = std::ranges::find(_pendingSubmits, handle); found != _pendingSubmits.end())
        {
            _pendingSubmits.erase(found);
            return true;
        }
    }

    std::scoped_lock const guard { _timerMutex };
    auto const found = std::ranges::find(_timers, handle, &TimerEntry::handle);
    if (found == _timers.end())
        return false;
    // Erased and re-heaped rather than popped: this entry is somewhere in the
    // middle of the heap, not at its root.
    _timers.erase(found);
    std::ranges::make_heap(_timers, EntryGreater);
    return true;
}

bool KqueueReactor::Attach(KqueueFdHandler* handler) const noexcept
{
    // kqueue doesn't have a separate "add fd without filter" call; we
    // register filters on UpdateInterest. Return true if the fd is sane.
    return handler != nullptr && handler->fd >= 0 && _kq >= 0;
}

bool KqueueReactor::UpdateInterest(KqueueFdHandler* handler, bool read, bool write) const noexcept
{
    if (!handler || handler->fd < 0 || _kq < 0)
        return false;
    // EV_RECEIPT is load-bearing, not a nicety. Without an eventlist, kevent()
    // stops at the FIRST failing change and returns -1 — the remaining changes
    // are silently never applied. Deleting a filter that was never armed fails
    // with ENOENT, so the routine (read=false, write=true) case — a reply write
    // parking while no read is outstanding — used to abort on the EVFILT_READ
    // delete and never arm EVFILT_WRITE. The write then parked forever holding
    // its unsent tail and the client blocked in recv() until it gave up.
    // EV_RECEIPT forces one result entry per change, so every change is
    // processed and each error comes back individually in `data`.
    //
    // One row per filter: the whole routine is "arm it or drop it", so the two
    // filters differ only by which flag decides their fate.
    std::array<FilterInterest, 2> const interests { {
        { .filter = EVFILT_READ, .wanted = read },
        { .filter = EVFILT_WRITE, .wanted = write },
    } };

    std::array<struct kevent, interests.size()> changes {};
    for (auto const i: std::views::iota(std::size_t { 0 }, interests.size()))
        EV_SET(std::next(changes.data(), static_cast<std::ptrdiff_t>(i)),
               handler->fd,
               interests[i].filter,
               (interests[i].wanted ? (EV_ADD | EV_ENABLE) : EV_DELETE) | EV_RECEIPT,
               0,
               0,
               handler);

    std::array<struct kevent, interests.size()> results {};
    auto const applied = ::kevent(
        _kq, changes.data(), static_cast<int>(changes.size()), results.data(), static_cast<int>(results.size()), nullptr);
    if (applied < 0)
        return false;

    // With EV_RECEIPT every returned entry carries EV_ERROR and `data` holds the
    // errno (0 when the change applied cleanly). Results are matched back by
    // filter rather than by position so the check does not depend on the kernel
    // preserving changelist order. ENOENT on a filter we asked to drop just
    // means it was not armed — the normal steady state, not a failure.
    return std::ranges::all_of(std::views::counted(results.begin(), applied), [&](struct kevent const& r) noexcept {
        if (r.data == 0)
            return true;
        // FindOrNull, not std::ranges::find: `interests` is a std::array, whose
        // iterator is a raw pointer only on libc++ (see Core/Ranges.hpp).
        auto const* const row = FindOrNull(interests, r.filter, &FilterInterest::filter);
        return r.data == ENOENT && row != nullptr && !row->wanted;
    });
}

void KqueueReactor::Detach(KqueueFdHandler* handler) const noexcept
{
    if (!handler)
        return;

    // Withdraw this handler from the batch Run() is walking, before anything
    // else. EV_DELETE below stops future reports and does not retract entries
    // kevent() has already written -- #475. The handler is still alive here, so
    // the comparison is against a live pointer.
    //
    // The whole array is scanned, already-dispatched entries included: nulling
    // one of those is a no-op and it avoids an off-by-one against the cursor.
    for (int i = 0; i < _batch.count; ++i)
    {
        if (_batch.events[i].udata == handler)
            _batch.events[i].udata = WithdrawnBatchEntry();
    }

    if (handler->fd < 0 || _kq < 0)
        return;
    // EV_RECEIPT for the same reason as UpdateInterest: without an eventlist an
    // ENOENT on the first delete (a filter that was never armed) would abort the
    // changelist and leave the second filter registered against a handler that
    // is going away.
    std::array<struct kevent, 2> changes {};
    EV_SET(changes.data(), handler->fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
    EV_SET(std::next(changes.data()), handler->fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
    std::array<struct kevent, 2> results {};
    (void) ::kevent(
        _kq, changes.data(), static_cast<int>(changes.size()), results.data(), static_cast<int>(results.size()), nullptr);
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

        // Re-sample before deciding how long to block. The previous iteration's
        // handlers and timers ran after its refresh, so a cached clock is stale
        // by however long that batch took — and computing the timeout from a
        // stale `now` overshoots the deadline by exactly that much, making every
        // timer fire a batch late. Both refreshes are needed and neither is
        // redundant: this one bounds the sleep, the one below the wait is what
        // makes the resumed handlers see the time the wait actually ended at.
        _clock.Refresh();

        timespec ts {};
        timespec* tsPtr = nullptr;
        if (nextDeadline != TimePoint::max())
        {
            ts = DeadlineToTimespec(nextDeadline, _clock.Now());
            tsPtr = &ts;
        }

        auto const n = ::kevent(_kq, nullptr, 0, events, Batch, tsPtr);

        // The wait above may have blocked for an arbitrary time, so this is the
        // point in the loop where a cached clock has to re-sample. Every handler
        // and timer resumed below then reads it for free.
        _clock.Refresh();

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }

        // One callback per HANDLER per batch, not per event. kqueue reports each
        // filter separately, so one fd can appear twice here -- and the first
        // callback resumes a coroutine that may free the object the handler is
        // embedded in, which makes the second entry's `udata` dangle. Whatever is
        // skipped is re-reported on the next kevent(), because these filters are
        // level-triggered, so the cost is one extra loop turn.
        //
        // This covers ONE handler reported twice. It does NOT cover handler i
        // freeing the owner of handler j -- this comment read as though it did,
        // which is how #475 stayed invisible on both POSIX reactors. What covers
        // that is `Detach` nulling the entry, not anything here.
        // Published so `Detach` can withdraw an entry from this array: a callback
        // below can free the object another entry's handler lives in, and
        // EV_DELETE does not retract what kevent() already wrote here (#475).
        // Cleared before leaving the batch.
        _batch = DequeuedBatch { .events = events, .count = n };

        std::array<void*, Batch> serviced {};
        std::size_t servicedCount = 0;

        for (int i = 0; i < n; ++i)
        {
            auto const& ev = events[i];
            if (ev.udata == WithdrawnBatchEntry())
            {
                // `Detach` withdrew this entry after the batch was dequeued: the
                // handler it named is being torn down. Its filters are already
                // deleted from the kqueue.
                continue;
            }
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
            auto const alreadyServiced =
                std::ranges::find(serviced.begin(), serviced.begin() + static_cast<std::ptrdiff_t>(servicedCount), ev.udata)
                != serviced.begin() + static_cast<std::ptrdiff_t>(servicedCount);
            if (alreadyServiced)
                continue;

            auto* handler = static_cast<KqueueFdHandler*>(ev.udata);

            // EV_ERROR/EV_EOF go to `onError` when the owner named one; otherwise
            // they fall through to the filter's own callback, which is what a
            // socket and a listener both want.
            auto* const callback = ((ev.flags & (EV_ERROR | EV_EOF)) != 0 && handler->onError != nullptr) ? handler->onError
                                   : ev.filter == EVFILT_READ  ? handler->onReadable
                                   : ev.filter == EVFILT_WRITE ? handler->onWritable
                                                               : nullptr;
            if (callback == nullptr)
                continue;

            serviced[servicedCount++] = ev.udata;
            callback(handler);
        }

        _batch = DequeuedBatch {};

        DrainPendingSubmits();
        FireExpiredTimers();
    }
}

} // namespace FastCache

#endif // __APPLE__
