// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IocpReactor.hpp>

#if defined(_WIN32)

    #include <algorithm>
    #include <chrono>
    #include <coroutine>
    #include <cstddef>
    #include <cstdint>
    #include <mutex>
    #include <ranges>
    #include <tuple>
    #include <utility>
    #include <vector>

    #include <windows.h>

namespace FastCache
{

namespace
{

    /// Completion-key sentinels distinguishing internal vs. socket
    /// completions on a single IOCP port.
    constexpr ULONG_PTR KeyResumeCoroutine = 1;
    constexpr ULONG_PTR KeyStop = 2;
    // Socket completions use the IocpSocket pointer as the key.

    /// Min-heap comparator: earlier deadlines win; FIFO on ties.
    constexpr auto EntryGreater = [](IocpReactor::TimerEntry const& a, IocpReactor::TimerEntry const& b) noexcept {
        if (a.deadline != b.deadline)
            return a.deadline > b.deadline;
        return a.sequence > b.sequence;
    };

    [[nodiscard]] DWORD DeadlineToTimeout(TimePoint nextDeadline, TimePoint now) noexcept
    {
        if (nextDeadline <= now)
            return 0;
        auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(nextDeadline - now).count();
        if (millis < 0)
            return 0;
        // `std::cmp_greater` rather than a cast: `INFINITE` is an unsigned DWORD and
        // `millis` is a signed count, and a cast only hides the mismatch from the
        // reader while leaving it in the code.
        if (std::cmp_greater(millis, INFINITE - 1))
            return INFINITE - 1;
        return static_cast<DWORD>(millis);
    }

} // namespace

IocpReactor::IocpReactor(IClock& clock):
    _clock { clock },
    // One worker thread per reactor instance — scaling is by running several
    // independent reactors, not by draining one port from many threads.
    _iocp { CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, /*threads*/ 1) }
{
}

IocpReactor::~IocpReactor()
{
    // Before the port closes, because an abandoned connection chain frees its socket on
    // the way down and `IocpSocket` asks this reactor about its own teardown.
    AbandonParkedWork();

    if (_iocp)
    {
        CloseHandle(static_cast<HANDLE>(_iocp));
        _iocp = nullptr;
    }
}

void IocpReactor::AbandonParkedWork() noexcept
{
    // Swapped out under the locks and freed OUTSIDE them, and LOOPED, for
    // `EpollReactor::AbandonParkedWork`'s two reasons: destroying a chain runs arbitrary
    // destructors that reach back into this reactor, and one of them can park again --
    // which a single fixed-order pass would leave for member destruction, after the
    // completion port below is closed.
    while (true)
    {
        std::vector<Detail::Parked> posted;
        std::vector<TimerEntry> timers;
        {
            std::scoped_lock const guard { _postedMutex };
            posted.swap(_posted);
            _postedCount.store(0, std::memory_order_release);
        }
        {
            std::scoped_lock const guard { _timerMutex };
            timers.swap(_timers);
        }
        if (posted.empty() && timers.empty())
            return;
        posted.clear();
        timers.clear();
    }
}

Detail::Parked IocpReactor::TakePosted(std::coroutine_handle<> handle) noexcept
{
    // Checked before the lock is taken, because this runs for EVERY resumption packet
    // and almost none of them is registered: borrowed submissions -- which is what a
    // plain `Submit(handle)` posts -- never enter `_posted` at all. Without this, an
    // owning submission anywhere in the process would put a contended global lock and a
    // linear scan on the reactor's hot resume path. The count is written under the same
    // mutex, and a registration is complete before its packet is posted, so a worker
    // that can see the packet can see the count.
    if (_postedCount.load(std::memory_order_acquire) == 0)
        return Detail::Parked { ParkedWork { .resume = handle } };

    std::scoped_lock const guard { _postedMutex };
    auto const found = std::ranges::find(_posted, handle, &Detail::Parked::Handle);
    if (found == _posted.end())
        return Detail::Parked { ParkedWork { .resume = handle } };
    auto taken = std::move(*found);
    _posted.erase(found);
    _postedCount.store(_posted.size(), std::memory_order_release);
    return taken;
}

bool IocpReactor::CancelPending(std::coroutine_handle<> handle) noexcept
{
    if (!handle)
        return false;

    // Timers only, and the omission is the platform's rather than a shortcut: a
    // submission here is a completion packet already posted to the kernel, and
    // there is no call that takes one back out. So a handle waiting on the port
    // answers false -- the honest answer, since the caller must not resume what
    // the reactor is still going to.
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

bool IocpReactor::AttachHandle(void* handle) noexcept
{
    if (!_iocp || !handle)
        return false;
    auto* const result = CreateIoCompletionPort(
        static_cast<HANDLE>(handle), static_cast<HANDLE>(_iocp), reinterpret_cast<ULONG_PTR>(handle), 0);
    return result == static_cast<HANDLE>(_iocp);
}

void IocpReactor::Submit(std::coroutine_handle<> handle)
{
    Submit(ParkedWork { .resume = handle });
}

void IocpReactor::Submit(ParkedWork work)
{
    if (!work.resume || !_iocp)
        return;

    // Registered BEFORE the post, or the worker thread can dequeue the packet and look
    // for an entry that has not been written yet -- which would resume correctly and
    // leave a stale root behind to be freed at teardown, on a frame that has since gone.
    if (work.abandon)
    {
        std::scoped_lock const guard { _postedMutex };
        _posted.emplace_back(work);
        _postedCount.store(_posted.size(), std::memory_order_release);
    }

    PostQueuedCompletionStatus(
        static_cast<HANDLE>(_iocp), 0, KeyResumeCoroutine, reinterpret_cast<LPOVERLAPPED>(work.resume.address()));
}

void IocpReactor::Schedule(TimePoint deadline, std::coroutine_handle<> handle)
{
    Schedule(deadline, ParkedWork { .resume = handle });
}

void IocpReactor::Schedule(TimePoint deadline, ParkedWork work)
{
    if (!work.resume)
        return;
    {
        std::scoped_lock const lock { _timerMutex };
        _timers.push_back(
            TimerEntry { .deadline = deadline, .sequence = _nextSequence++, .parked = Detail::Parked { work } });
        std::ranges::push_heap(_timers, EntryGreater);
    }
    // Nudge the reactor in case it's blocked waiting on a later deadline.
    PostQueuedCompletionStatus(static_cast<HANDLE>(_iocp), 0, KeyResumeCoroutine, nullptr);
}

void IocpReactor::Stop() noexcept
{
    _stopped.store(true, std::memory_order_release);
    if (_iocp)
        PostQueuedCompletionStatus(static_cast<HANDLE>(_iocp), 0, KeyStop, nullptr);
}

void IocpReactor::FireExpiredTimers()
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

void IocpReactor::RunLoop()
{
    // Publish which thread is dequeuing, so `IsOnWorkerThread()` can answer.
    // `IocpSocket` and `IocpListener` clear a pending awaitable in their
    // destructors and that is only safe against this thread; both assert on it.
    // Cleared on the way out by the guard, including on the early return in the
    // error branch -- a stale id outliving `Run()` would make a destructor on any
    // thread look correct.
    //
    // The guard used to be a `WorkerScope` local to this function, which is the
    // reason the question was IOCP-only: three other reactors could not be asked it
    // at all. `ReactorWorkerIdentity` is that same pair with a name (#668).

    constexpr ULONG Batch = 32;
    OVERLAPPED_ENTRY entries[Batch];

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
        auto const now = _clock.Now();
        auto const timeout = nextDeadline == TimePoint::max() ? INFINITE : DeadlineToTimeout(nextDeadline, now);

        ULONG removed = 0;
        BOOL const ok = GetQueuedCompletionStatusEx(static_cast<HANDLE>(_iocp), entries, Batch, &removed, timeout, FALSE);

        // The wait above may have blocked for an arbitrary time, so this is the
        // point in the loop where a cached clock has to re-sample. It comes
        // before the timeout branch below deliberately: that branch fires
        // timers, which must see the time the wait actually ended at.
        _clock.Refresh();

        if (!ok)
        {
            // Timeout (WAIT_TIMEOUT) is the common case once a deadline
            // elapses; any other error we treat as fatal and exit.
            auto const err = GetLastError();
            if (err == WAIT_TIMEOUT)
            {
                FireExpiredTimers();
                continue;
            }
            return;
        }

        bool stopRequested = false;
        for (ULONG i = 0; i < removed; ++i)
        {
            auto const& entry = entries[i];
            if (entry.lpCompletionKey == KeyStop)
            {
                // Don't bail out mid-batch: finish processing the entries already
                // dequeued alongside this stop, so a coroutine handed off to this
                // reactor concurrently with shutdown still gets resumed (and takes
                // ownership of its socket) instead of being abandoned. Exit after
                // draining the current batch.
                stopRequested = true;
                continue;
            }
            if (entry.lpCompletionKey == KeyResumeCoroutine)
            {
                if (entry.lpOverlapped == nullptr)
                    continue; // pure wake-up, no work
                // Taken and resumed in one expression: `TakePosted` is what hands the
                // chain back to itself, so there is no line for a future edit to forget
                // it on and no window where both this and `_posted` claim the frame.
                TakePosted(std::coroutine_handle<>::from_address(entry.lpOverlapped)).Resume();
                continue;
            }
            // Socket / listener completion: lpOverlapped points to an
            // IocpCompletion whose dispatch routes the result back to the
            // awaitable owned by the issuing socket/listener.
            auto* completion = reinterpret_cast<IocpCompletion*>(entry.lpOverlapped);
            if (completion && completion->dispatch)
            {
                // `Internal` is an NTSTATUS and this reactor cannot translate it --
                // `WSAGetOverlappedResult` needs the SOCKET, which lives one layer up.
                // So it travels as an `IocpStatus` rather than a `DWORD`, and the
                // consumer converts. See the type's own comment for what handing this
                // over as a bare number cost.
                auto const status = IocpStatus { static_cast<LONG>(completion->overlapped.Internal) };
                completion->dispatch(completion, entry.dwNumberOfBytesTransferred, status);
            }
        }

        if (stopRequested)
            return;

        FireExpiredTimers();
    }
}

} // namespace FastCache

#endif // _WIN32
