// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/TestReactor.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>

namespace FastCache
{

namespace
{

    /// Min-heap comparator: earlier deadlines win, FIFO on ties.
    constexpr auto EntryGreater = [](TestReactor::ScheduledEntry const& a,
                                     TestReactor::ScheduledEntry const& b) noexcept
    {
        if (a.deadline != b.deadline)
            return a.deadline > b.deadline;
        return a.sequence > b.sequence;
    };

} // namespace

TestReactor::TestReactor(IClock& clock) noexcept: _clock { clock } {}

void TestReactor::Run()
{
    while (!_stopped)
    {
        if (Tick() == 0)
            break;
    }
}

void TestReactor::Stop() noexcept
{
    _stopped = true;
}

void TestReactor::Submit(std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    _ready.push_back(handle);
}

void TestReactor::Schedule(TimePoint deadline, std::coroutine_handle<> handle)
{
    if (!handle)
        return;
    _timers.push_back(ScheduledEntry { deadline, _nextSequence++, handle });
    std::ranges::push_heap(_timers, EntryGreater);
}

IClock& TestReactor::Clock() noexcept
{
    return _clock;
}

void TestReactor::FireExpiredTimers()
{
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
    FireExpiredTimers();

    auto const drained = _ready.size();
    for (std::size_t i = 0; i < drained; ++i)
    {
        auto handle = _ready.front();
        _ready.pop_front();
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
    return _ready.size();
}

std::size_t TestReactor::PendingTimers() const noexcept
{
    return _timers.size();
}

} // namespace FastCache
