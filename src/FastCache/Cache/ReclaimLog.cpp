// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/ReclaimLog.hpp>

#include <string>
#include <utility>

namespace FastCache
{

ReclaimLog::ReclaimLog(IStorageMutationObserver* observer, std::size_t capacity) noexcept:
    _observer { observer },
    _capacity { capacity }
{
}

bool ReclaimLog::IsRecording() const noexcept
{
    return _observer != nullptr && _observer->HasObservers();
}

bool ReclaimLog::HasPending() const noexcept
{
    return _pendingCount.load(std::memory_order_relaxed) != 0;
}

std::size_t ReclaimLog::Dropped() const noexcept
{
    return _dropped.load(std::memory_order_relaxed);
}

void ReclaimLog::Record(MutationKind kind, std::string_view key) noexcept
{
    // Full is answered without the lock. A mass reclaim — `Resize()` onto a
    // smaller budget evicting until it fits — calls this once per key from
    // inside the tier's shard lock, and taking a second lock per key only to
    // bump a drop counter would serialise every concurrently evicting shard
    // behind this one for the whole sweep.
    if (_pendingCount.load(std::memory_order_relaxed) >= _capacity)
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::scoped_lock const lock { _mu };
    // Re-checked under the lock, because the probe above can be stale by a few
    // entries when several shards record at once. The bound stays exact.
    if (_pending.size() >= _capacity)
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The one allocation on this path, and the reason `IsRecording()` gates the
    // call: the node owning `key` is about to be erased, so the copy has to
    // happen here. A throw would cross a `noexcept` boundary and terminate, so
    // an exhausted allocator is counted as a drop like any other.
    try
    {
        _pending.push_back(ReclaimedKey { .kind = kind, .key = std::string { key } });
    }
    catch (...)
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    _pendingCount.store(_pending.size(), std::memory_order_relaxed);
}

void ReclaimLog::Drain(std::vector<ReclaimedKey>& out) noexcept
{
    std::scoped_lock const lock { _mu };
    out.clear();
    // Swap rather than move: `out` hands its cleared storage back to the log,
    // so a caller reusing one buffer keeps the steady-state drain free of
    // allocation in both directions.
    out.swap(_pending);
    _pendingCount.store(0, std::memory_order_relaxed);
}

} // namespace FastCache
