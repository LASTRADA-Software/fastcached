// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/RedisTransaction.hpp>

#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FastCache
{

void WatchHandle::Remember(std::string_view key, CasToken cas)
{
    std::scoped_lock const lock { _mu };
    // Transparent lookup: try-find by string_view first so we re-use the
    // existing key allocation on the common "re-Remember of the same key"
    // path; only allocate a new std::string on a fresh insert.
    if (auto const it = _snapshots.find(key); it != _snapshots.end())
    {
        it->second = cas;
        return;
    }
    _snapshots.emplace(std::string { key }, cas);
}

void WatchHandle::Clear() noexcept
{
    // Drop the snapshot map only. The `_dirty` atomic is owned by
    // `ClaimAndClearDirty` so a racing `MarkDirty` that lands between the
    // registry index erase and this call is preserved for the next
    // claim-and-clear. Previously this also stored `_dirty=false`, which
    // silently wiped any racing dirty bit set BETWEEN UnregisterAll's
    // index-erase (under _mu) and Clear() (outside _mu) — exactly the
    // EXEC race the comment at HandleExec asserted was closed.
    std::scoped_lock const lock { _mu };
    _snapshots.clear();
}

std::vector<std::string> WatchHandle::WatchedKeys() const
{
    std::scoped_lock const lock { _mu };
    std::vector<std::string> out;
    out.reserve(_snapshots.size());
    for (auto const& [key, _]: _snapshots)
        out.push_back(key);
    return out;
}

void WatchHandle::MarkDirty() noexcept
{
    _dirty.store(true, std::memory_order_release);
}

bool WatchHandle::IsDirty() const noexcept
{
    return _dirty.load(std::memory_order_acquire);
}

bool WatchHandle::ClaimAndClearDirty() noexcept
{
    // exchange returns the previous value AND atomically stores false; acq_rel
    // pairs with the release in MarkDirty and the acquire in IsDirty. Holding
    // _mu while wiping _snapshots ensures a concurrent Remember-on-the-same-
    // handle (impossible in normal use — the owner is single-threaded for the
    // handle — but defensive) sees a consistent map.
    bool const wasDirty = _dirty.exchange(false, std::memory_order_acq_rel);
    std::scoped_lock const lock { _mu };
    _snapshots.clear();
    return wasDirty;
}

bool WatchRegistry::Register(std::shared_ptr<WatchHandle> const& handle, std::string_view key)
{
    if (!handle)
        return false;
    std::scoped_lock const lock { _mu };
    // Transparent lookup first so the common re-Register path on an
    // existing bucket re-uses its allocation; only allocate a fresh
    // std::string when the bucket is brand new.
    auto it = _index.find(key);
    if (it == _index.end())
        it = _index.emplace(std::string { key }, decltype(_index)::mapped_type {}).first;
    // insert_or_assign returns {iterator, true} only on a fresh insert; on
    // re-Register of the same handle for the same key we must NOT bump
    // the counter (the entry was already counted) AND the caller's
    // rollback path must NOT Unregister this key — that would wipe the
    // prior registration from an earlier WATCH call.
    auto const [_, inserted] = it->second.insert_or_assign(handle.get(), std::weak_ptr<WatchHandle> { handle });
    if (inserted)
        _entryCount.fetch_add(1, std::memory_order_release);
    return inserted;
}

std::size_t WatchRegistry::Unregister(WatchHandle* handle, std::string_view key)
{
    if (handle == nullptr)
        return 0;
    std::scoped_lock const lock { _mu };
    auto const it = _index.find(key);
    if (it == _index.end())
        return 0;
    std::size_t const removed = it->second.erase(handle);
    if (it->second.empty())
        _index.erase(it);
    if (removed > 0)
        _entryCount.fetch_sub(removed, std::memory_order_release);
    return removed;
}

void WatchRegistry::UnregisterAll(WatchHandle* handle)
{
    if (handle == nullptr)
        return;
    // Scope the registry lock TIGHTLY around the index walk so the handle's
    // own mutex is never acquired while we hold _mu. This avoids a
    // registry→handle lock-order edge that any future caller taking
    // handle->_mu before registry._mu would deadlock against, AND it closes
    // the EXEC race: a Touched that snapshotted a strong_ptr to the handle
    // before our scoped_lock had finished cannot land its MarkDirty after
    // our Clear(), because the EXEC caller now invokes ClaimAndClearDirty
    // *after* this function returns (the index entry is already gone, so
    // no new Touched can snapshot us).
    {
        std::scoped_lock const lock { _mu };
        std::size_t removed = 0;
        for (auto it = _index.begin(); it != _index.end();)
        {
            removed += it->second.erase(handle);
            if (it->second.empty())
                it = _index.erase(it);
            else
                ++it;
        }
        if (removed > 0)
            _entryCount.fetch_sub(removed, std::memory_order_release);
    }
    // The handle's snapshot map is also cleared so a future MULTI on the
    // same connection starts with no watched keys. Done OUTSIDE the
    // registry lock — Clear acquires handle->_mu.
    handle->Clear();
}

std::size_t WatchRegistry::Touched(std::string_view key)
{
    // Lock-free fast path — the steady state for a daemon with no active
    // WATCHers is "_entryCount == 0", so every successful cache mutation
    // skips the global mutex + heap allocation for the key string before
    // ever finding an empty bucket. Acquire pairs with the release stores
    // in Register / UnregisterAll so the lock-free reader sees a
    // consistent count.
    if (_entryCount.load(std::memory_order_acquire) == 0)
        return 0;
    // Snapshot the live handles under the lock so we can fire MarkDirty
    // outside it — MarkDirty is a single atomic store, but holding the lock
    // across cross-handle work courts deadlocks if the dirty flag is ever
    // observed under a different mutex on the receive side.
    std::vector<std::shared_ptr<WatchHandle>> targets;
    {
        std::scoped_lock const lock { _mu };
        // Transparent find — accept the caller's string_view zero-alloc.
        auto const it = _index.find(key);
        if (it == _index.end())
            return 0;
        targets.reserve(it->second.size());
        for (auto const& [_, weak]: it->second)
            if (auto strong = weak.lock())
                targets.push_back(std::move(strong));
    }
    for (auto const& t: targets)
        t->MarkDirty();
    return targets.size();
}

std::size_t WatchRegistry::TouchedAll()
{
    // Same lock-free fast path as Touched: when nothing is watched, a
    // FLUSHDB pays only a single atomic load.
    if (_entryCount.load(std::memory_order_acquire) == 0)
        return 0;
    // Snapshot every distinct handle under the lock, then fire MarkDirty
    // outside the lock — same pattern as Touched but iterates every
    // bucket instead of one. Use an unordered_set to dedupe: a handle
    // watching N keys appears in N buckets but should be dirtied only once.
    std::vector<std::shared_ptr<WatchHandle>> targets;
    {
        std::scoped_lock const lock { _mu };
        std::unordered_set<WatchHandle*> seen;
        for (auto const& [_, bucket]: _index)
        {
            for (auto const& [raw, weak]: bucket)
            {
                if (!seen.insert(raw).second)
                    continue;
                if (auto strong = weak.lock())
                    targets.push_back(std::move(strong));
            }
        }
    }
    for (auto const& t: targets)
        t->MarkDirty();
    return targets.size();
}

bool WatchRegistry::HasAnyWatchers() const noexcept
{
    return _entryCount.load(std::memory_order_acquire) > 0;
}

} // namespace FastCache
