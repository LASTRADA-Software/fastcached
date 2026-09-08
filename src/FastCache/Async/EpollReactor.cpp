// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/EpollReactor.hpp>

#if defined(__linux__)

    #include <sys/epoll.h>
    #include <sys/eventfd.h>

    #include <algorithm>
    #include <cerrno>
    #include <chrono>
    #include <cstdint>
    #include <ranges>
    #include <tuple>
    #include <utility>

    #include <unistd.h>

namespace FastCache
{

namespace
{

    /// What `Detach` writes over a withdrawn batch entry.
    ///
    /// A distinct value rather than `nullptr`, which already means "the wake
    /// event": reusing it would send every withdrawn entry through the wake
    /// branch and drain the eventfd counter for a reason that has nothing to do
    /// with a wake. That happens to be harmless today -- `DrainPendingSubmits()`
    /// runs unconditionally at the end of every iteration, so a consumed wake
    /// costs nothing -- but it is two facts sharing one representation, which is
    /// how the next reader is misled.
    ///
    /// The address of a private object, so it can equal no handler and no
    /// nullptr.
    [[nodiscard]] void* WithdrawnBatchEntry() noexcept
    {
        static char tombstone = 0;
        return &tombstone;
    }

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
    _clock { clock },
    _epollFd { ::epoll_create1(EPOLL_CLOEXEC) },
    _wakeFd { ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) }
{
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
    // Before the descriptors close: an abandoned connection chain frees its socket on
    // the way down, and `EpollSocket::Close` detaches from this reactor.
    AbandonParkedWork();

    if (_wakeFd >= 0)
        ::close(_wakeFd);
    if (_epollFd >= 0)
        ::close(_epollFd);
}

void EpollReactor::AbandonParkedWork() noexcept
{
    // Swapped out under the locks and freed OUTSIDE them: destroying a chain runs
    // arbitrary destructors, and a socket going down there calls back into this
    // reactor. `~Parked` is the whole of the freeing, so there is no separate destroy
    // step for a future edit to forget.
    //
    // **Looped, and both containers are taken before either is freed.** Freeing a chain
    // can park again -- `AsyncQueue::Close()` resuming a waiter reaches `Submit` -- so a
    // single pass in a fixed order leaves whatever the later pass produced for MEMBER
    // destruction, which runs after the descriptors below are closed: that entry's
    // `Submit` would have written to an fd on its way out. It terminates because a pass
    // that takes nothing returns, and teardown frees chains rather than creating them.
    //
    // **Not covered by a case, and that is stated rather than papered over.** The only
    // thing the single-pass version does differently is free the re-parked entry LATER,
    // and later still frees it -- `swap` leaves the member containers unallocated, so a
    // re-entrant search over them reads nothing. The difference is visible only as an
    // `epoll_ctl` on a descriptor `~EpollReactor` has already closed, whose fd number
    // another thread may by then have reused, and no fixture can stage that
    // deterministically. A case was written for it and DELETED: it passed with the loop
    // and without it, which is a test that cannot fail for its reason.
    while (true)
    {
        std::deque<Detail::Parked> submits;
        std::vector<TimerEntry> timers;
        {
            std::scoped_lock const guard { _submitMutex };
            submits.swap(_pendingSubmits);
        }
        {
            std::scoped_lock const guard { _timerMutex };
            timers.swap(_timers);
        }
        if (submits.empty() && timers.empty())
            return;
        submits.clear();
        timers.clear();
    }
}

bool EpollReactor::CancelPending(std::coroutine_handle<> handle) noexcept
{
    if (!handle)
        return false;

    {
        std::scoped_lock const guard { _submitMutex };
        if (auto const found = std::ranges::find(_pendingSubmits, handle, &Detail::Parked::Handle);
            found != _pendingSubmits.end())
        {
            // Taken rather than only erased: the caller becomes the only one who may
            // resume or destroy it, so this entry must do neither on its way out.
            std::ignore = found->Take();
            _pendingSubmits.erase(found);
            return true;
        }
    }

    std::scoped_lock const guard { _timerMutex };
    auto const found =
        std::ranges::find(_timers, handle, [](TimerEntry const& entry) noexcept { return entry.parked.Handle(); });
    if (found == _timers.end())
        return false;
    std::ignore = found->parked.Take();
    // Erased and re-heaped rather than popped: this entry is somewhere in the
    // middle of the heap, not at its root.
    _timers.erase(found);
    std::ranges::make_heap(_timers, EntryGreater);
    return true;
}

bool EpollReactor::Attach(EpollFdHandler* handler) const noexcept
{
    if (!handler || handler->fd < 0 || _epollFd < 0)
        return false;
    epoll_event ev {};
    ev.events = 0; // start with no interest
    ev.data.ptr = handler;
    return ::epoll_ctl(_epollFd, EPOLL_CTL_ADD, handler->fd, &ev) == 0;
}

bool EpollReactor::UpdateInterest(EpollFdHandler* handler, bool read, bool write) const noexcept
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

void EpollReactor::Detach(EpollFdHandler* handler) const noexcept
{
    if (!handler)
        return;

    if (handler->fd >= 0 && _epollFd >= 0)
    {
        epoll_event ev {};
        ::epoll_ctl(_epollFd, EPOLL_CTL_DEL, handler->fd, &ev);
    }

    // Withdraw this handler from the batch Run() is walking. EPOLL_CTL_DEL above
    // stops future reports and does nothing about entries epoll_wait has already
    // written, which is the whole of #475. The handler is still alive here, so
    // the comparison is against a live pointer and needs no generation counter.
    //
    // The whole array is scanned, including entries already dispatched: nulling
    // one of those is a no-op, and it keeps this free of an off-by-one against
    // the loop's cursor.
    for (int i = 0; i < _batch.count; ++i)
    {
        if (_batch.events[i].data.ptr == handler)
            _batch.events[i].data.ptr = WithdrawnBatchEntry();
    }
}

void EpollReactor::Submit(std::coroutine_handle<> handle)
{
    Submit(ParkedWork { .resume = handle });
}

void EpollReactor::Submit(ParkedWork work)
{
    if (!work.resume)
        return;
    {
        std::scoped_lock const lock { _submitMutex };
        _pendingSubmits.emplace_back(work);
    }
    std::uint64_t one = 1;
    std::ignore = ::write(_wakeFd, &one, sizeof(one));
}

void EpollReactor::Schedule(TimePoint deadline, std::coroutine_handle<> handle)
{
    Schedule(deadline, ParkedWork { .resume = handle });
}

void EpollReactor::Schedule(TimePoint deadline, ParkedWork work)
{
    if (!work.resume)
        return;
    {
        std::scoped_lock const lock { _timerMutex };
        _timers.push_back(
            TimerEntry { .deadline = deadline, .sequence = _nextSequence++, .parked = Detail::Parked { work } });
        std::ranges::push_heap(_timers, EntryGreater);
    }
    std::uint64_t one = 1;
    std::ignore = ::write(_wakeFd, &one, sizeof(one));
}

void EpollReactor::Stop() noexcept
{
    _stopped.store(true, std::memory_order_release);
    std::uint64_t one = 1;
    std::ignore = ::write(_wakeFd, &one, sizeof(one));
}

void EpollReactor::FireExpiredTimers()
{
    auto const now = _clock.Now();
    std::vector<Detail::Parked> due;
    {
        std::scoped_lock const lock { _timerMutex };
        while (!_timers.empty() && _timers.front().deadline <= now)
        {
            std::ranges::pop_heap(_timers, EntryGreater);
            due.push_back(std::move(_timers.back().parked));
            _timers.pop_back();
        }
    }
    // `Resume()` disowns and resumes in one expression, so a timer that fires normally
    // is never also freed by the entry going out of scope here.
    for (auto& parked: due)
        parked.Resume();
}

void EpollReactor::DrainPendingSubmits()
{
    std::deque<Detail::Parked> drained;
    {
        std::scoped_lock const lock { _submitMutex };
        drained.swap(_pendingSubmits);
    }
    while (!drained.empty())
    {
        drained.front().Resume();
        drained.pop_front();
    }
}

void EpollReactor::RunLoop()
{

    constexpr int Batch = 32;
    epoll_event events[Batch];

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
        auto const timeout = DeadlineToMs(nextDeadline, _clock.Now());

        auto const n = ::epoll_wait(_epollFd, events, Batch, timeout);

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

        // Published so `Detach` can withdraw an entry from this array. A callback
        // below can free the object another entry's handler lives in, and
        // EPOLL_CTL_DEL does not retract what epoll_wait already wrote here --
        // #475, reproduced under ASan. Cleared before leaving the batch so a
        // Detach outside the loop scans nothing.
        _batch = DequeuedBatch { .events = events, .count = n };

        for (int i = 0; i < n; ++i)
        {
            auto const& ev = events[i];
            if (ev.data.ptr == WithdrawnBatchEntry())
            {
                // `Detach` withdrew this entry after the batch was dequeued: the
                // handler it named is being torn down and must not be dispatched
                // on. Its fd is already out of the epoll set.
                continue;
            }
            if (ev.data.ptr == nullptr)
            {
                // Wake event — drain the eventfd counter and move on.
                std::uint64_t buf {};
                std::ignore = ::read(_wakeFd, &buf, sizeof(buf));
                continue;
            }
            auto* handler = static_cast<EpollFdHandler*>(ev.data.ptr);

            // Exactly one callback, chosen by SelectEpollCallback -- see the rules
            // recorded there. In short: an error must be routed somewhere or a
            // level-triggered fd is re-reported forever and the loop spins, and a
            // second callback must not run because the first may have resumed a
            // coroutine that freed the object `handler` lives in.
            //
            // That covers ONE handler reported twice in this batch. It does NOT
            // cover handler i freeing the owner of handler j -- for years this
            // comment read as though it did, which is how #475 stayed invisible.
            // What covers that is `Detach` nulling the entry above, not anything
            // here.
            if (auto* const callback = SelectEpollCallback(*handler, ev.events); callback != nullptr)
                callback(handler);
        }

        _batch = DequeuedBatch {};

        DrainPendingSubmits();
        FireExpiredTimers();
    }
}

} // namespace FastCache

#endif // __linux__
