// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IocpReactor.hpp>

#if defined(_WIN32)

    #include <algorithm>
    #include <chrono>
    #include <coroutine>
    #include <cstddef>
    #include <cstdint>
    #include <mutex>
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
        if (millis > static_cast<std::int64_t>(INFINITE - 1))
            return INFINITE - 1;
        return static_cast<DWORD>(millis);
    }

} // namespace

IocpReactor::IocpReactor(IClock& clock):
    _clock { clock }
{
    _iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, /*threads*/ 1);
}

IocpReactor::~IocpReactor()
{
    if (_iocp)
    {
        CloseHandle(static_cast<HANDLE>(_iocp));
        _iocp = nullptr;
    }
}

bool IocpReactor::AttachHandle(void* handle) noexcept
{
    if (!_iocp || !handle)
        return false;
    auto const result = CreateIoCompletionPort(
        static_cast<HANDLE>(handle), static_cast<HANDLE>(_iocp), reinterpret_cast<ULONG_PTR>(handle), 0);
    return result == static_cast<HANDLE>(_iocp);
}

void IocpReactor::Submit(std::coroutine_handle<> handle)
{
    if (!handle || !_iocp)
        return;
    PostQueuedCompletionStatus(
        static_cast<HANDLE>(_iocp), 0, KeyResumeCoroutine, reinterpret_cast<LPOVERLAPPED>(handle.address()));
}

void IocpReactor::Schedule(TimePoint deadline, std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    {
        std::lock_guard const lock { _timerMutex };
        _timers.push_back(TimerEntry { .deadline = deadline, .sequence = _nextSequence++, .handle = handle });
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

void IocpReactor::Run()
{
    constexpr ULONG Batch = 32;
    OVERLAPPED_ENTRY entries[Batch];

    while (!_stopped.load(std::memory_order_acquire))
    {
        TimePoint nextDeadline;
        {
            std::lock_guard const lock { _timerMutex };
            nextDeadline = _timers.empty() ? TimePoint::max() : _timers.front().deadline;
        }
        auto const now = _clock.Now();
        auto const timeout = nextDeadline == TimePoint::max() ? INFINITE : DeadlineToTimeout(nextDeadline, now);

        ULONG removed = 0;
        BOOL const ok = GetQueuedCompletionStatusEx(static_cast<HANDLE>(_iocp), entries, Batch, &removed, timeout, FALSE);

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

        for (ULONG i = 0; i < removed; ++i)
        {
            auto const& entry = entries[i];
            if (entry.lpCompletionKey == KeyStop)
                return;
            if (entry.lpCompletionKey == KeyResumeCoroutine)
            {
                if (entry.lpOverlapped == nullptr)
                    continue; // pure wake-up, no work
                auto handle = std::coroutine_handle<>::from_address(entry.lpOverlapped);
                if (handle && !handle.done())
                    handle.resume();
                continue;
            }
            // Socket / listener completion: lpOverlapped points to an
            // IocpCompletion whose dispatch routes the result back to the
            // awaitable owned by the issuing socket/listener.
            auto* completion = reinterpret_cast<IocpCompletion*>(entry.lpOverlapped);
            if (completion && completion->dispatch)
            {
                auto const err = static_cast<DWORD>(completion->overlapped.Internal);
                completion->dispatch(completion, entry.dwNumberOfBytesTransferred, err);
            }
        }

        FireExpiredTimers();
    }
}

} // namespace FastCache

#endif // _WIN32
