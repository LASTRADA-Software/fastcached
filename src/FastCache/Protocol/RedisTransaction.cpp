// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/RedisTransaction.hpp>

#include <mutex>
#include <string>
#include <utility>

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
    std::scoped_lock const lock { _mu };
    _snapshots.clear();
    _dirty.store(false, std::memory_order_release);
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

void WatchRegistry::Register(std::shared_ptr<WatchHandle> const& handle, std::string_view key)
{
    if (!handle)
        return;
    std::scoped_lock const lock { _mu };
    // Transparent lookup first so the common re-Register path on an
    // existing bucket re-uses its allocation; only allocate a fresh
    // std::string when the bucket is brand new.
    auto it = _index.find(key);
    if (it == _index.end())
        it = _index.emplace(std::string { key }, decltype(_index)::mapped_type {}).first;
    // insert_or_assign returns {iterator, true} only on a fresh insert; on
    // re-Register of the same handle for the same key we must NOT bump
    // the counter (the entry was already counted).
    auto const [_, inserted] = it->second.insert_or_assign(handle.get(), std::weak_ptr<WatchHandle> { handle });
    if (inserted)
        _entryCount.fetch_add(1, std::memory_order_release);
}

void WatchRegistry::UnregisterAll(WatchHandle* handle)
{
    if (handle == nullptr)
        return;
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
    // The handle's snapshot map is also cleared so a future MULTI on the
    // same connection starts with no watched keys.
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

bool WatchRegistry::HasAnyWatchers() const noexcept
{
    return _entryCount.load(std::memory_order_acquire) > 0;
}

} // namespace FastCache
