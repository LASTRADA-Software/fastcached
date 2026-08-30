// SPDX-License-Identifier: Apache-2.0
#include "ParallelFor.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace FastCache::Cc
{

bool SerialParallelFor::Run(std::size_t count, std::function<void(std::size_t)> const& slice)
{
    auto ok = true;
    for (std::size_t index = 0; index < count; ++index)
    {
        try
        {
            slice(index);
        }
        catch (...)
        {
            // Recorded and carried on, exactly as the threaded implementation does.
            // The two must agree about a throwing slice or a test that substitutes
            // this one proves nothing about the other.
            ok = false;
        }
    }
    return ok;
}

std::size_t DefaultParallelWidth()
{
    auto const cores = static_cast<std::size_t>(std::max(1U, std::thread::hardware_concurrency()));
    return std::clamp(cores * 4, static_cast<std::size_t>(4), static_cast<std::size_t>(32));
}

ThreadedParallelFor::ThreadedParallelFor(std::size_t width): _width { std::max<std::size_t>(1, width) }
{
}

bool ThreadedParallelFor::Run(std::size_t count, std::function<void(std::size_t)> const& slice)
{
    if (count == 0)
        return true;

    // One atomic cursor rather than a per-thread range: the slices are include
    // roots and their sizes differ by orders of magnitude, so a static split would
    // leave one thread holding the SDK while the rest finished. Work stealing by
    // the simplest available means.
    std::atomic<std::size_t> next { 0 };

    // `std::atomic<bool>` and not a plain one: every worker may write it and the
    // calling thread reads it after the join. The join is a synchronisation point
    // that would make a plain bool safe in practice, and the atomic is what makes
    // it safe by the language rather than by argument -- which is the difference
    // ThreadSanitizer is asked to check.
    std::atomic<bool> ok { true };

    auto const width = std::min(_width, count);
    std::vector<std::thread> workers;
    workers.reserve(width);

    auto const worker = [&] {
        for (auto index = next.fetch_add(1, std::memory_order_relaxed); index < count;
             index = next.fetch_add(1, std::memory_order_relaxed))
        {
            try
            {
                slice(index);
            }
            catch (...)
            {
                // Never propagated out of a thread -- an escaping exception here is
                // `std::terminate`, not a failed build. Recorded, and the loop
                // continues so the remaining slices still run: a partial result is a
                // partial result rather than an unknown one.
                ok.store(false, std::memory_order_relaxed);
            }
        }
    };

    for (std::size_t spawned = 0; spawned < width; ++spawned)
        workers.emplace_back(worker);
    for (auto& thread: workers)
        thread.join();

    return ok.load(std::memory_order_relaxed);
}

} // namespace FastCache::Cc
