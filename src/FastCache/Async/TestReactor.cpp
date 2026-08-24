// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/TestReactor.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>

namespace FastCache
{

namespace
{

    /// Min-heap comparator: earlier deadlines win, FIFO on ties.
    constexpr auto EntryGreater = [](TestReactor::ScheduledEntry const& a, TestReactor::ScheduledEntry const& b) noexcept {
        if (a.deadline != b.deadline)
            return a.deadline > b.deadline;
        return a.sequence > b.sequence;
    };

} // namespace

TestReactor::TestReactor(IClock& clock) noexcept:
    _clock { clock }
{
}

void TestReactor::Run()
{
    while (!_stopped.load(std::memory_order_acquire))
    {
        if (Tick() == 0)
            break;
    }
}

void TestReactor::Stop() noexcept
{
    _stopped.store(true, std::memory_order_release);
}

void TestReactor::Submit(std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    std::scoped_lock const guard { _mutex };
    _ready.push_back(handle);
}

void TestReactor::Schedule(TimePoint deadline, std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    std::scoped_lock const guard { _mutex };
    _timers.push_back(ScheduledEntry { .deadline = deadline, .sequence = _nextSequence++, .handle = handle });
    std::ranges::push_heap(_timers, EntryGreater);
}

IClock& TestReactor::Clock() noexcept
{
    return _clock;
}

void TestReactor::FireExpiredTimers()
{
    // Caller holds `_mutex`.
    auto const now = _clock.Now();
    while (!_timers.empty() && _timers.front().deadline <= now)
    {
        std::ranges::pop_heap(_timers, EntryGreater);
        auto entry = _timers.back();
        _timers.pop_back();
        _ready.push_back(entry.handle);
    }
}

std::size_t TestReactor::Tick()
{
    // Take the whole ready batch out under the lock, then resume outside it. A
    // resumed coroutine is entitled to call Submit -- and on this reactor it
    // routinely does, since that is how a woken consumer asks to run again --
    // which would deadlock against a lock held across the resume. The platform
    // reactors drain into a local for the same reason.
    std::deque<std::coroutine_handle<>> batch;
    {
        std::scoped_lock const guard { _mutex };
        FireExpiredTimers();
        batch.swap(_ready);
    }

    auto const drained = batch.size();
    while (!batch.empty())
    {
        auto handle = batch.front();
        batch.pop_front();
        if (handle && !handle.done())
            handle.resume();
    }
    return drained;
}

std::size_t TestReactor::Drain()
{
    std::size_t total = 0;
    while (true)
    {
        auto const advanced = Tick();
        total += advanced;
        if (advanced == 0)
            break;
    }
    return total;
}

std::size_t TestReactor::PendingSubmissions() const noexcept
{
    std::scoped_lock const guard { _mutex };
    return _ready.size();
}

std::size_t TestReactor::PendingTimers() const noexcept
{
    std::scoped_lock const guard { _mutex };
    return _timers.size();
}

} // namespace FastCache
