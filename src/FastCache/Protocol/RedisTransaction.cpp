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
    _snapshots.insert_or_assign(std::string { key }, cas);
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

void WatchRegistry::Register(std::shared_ptr<WatchHandle> const& handle, std::string_view key, CasToken cas)
{
    if (!handle)
        return;
    handle->Remember(key, cas);
    std::scoped_lock const lock { _mu };
    _index[std::string { key }].insert_or_assign(handle.get(), std::weak_ptr<WatchHandle> { handle });
}

void WatchRegistry::UnregisterAll(WatchHandle* handle)
{
    if (handle == nullptr)
        return;
    std::scoped_lock const lock { _mu };
    for (auto it = _index.begin(); it != _index.end();)
    {
        it->second.erase(handle);
        if (it->second.empty())
            it = _index.erase(it);
        else
            ++it;
    }
    // The handle's snapshot map is also cleared so a future MULTI on the
    // same connection starts with no watched keys.
    handle->Clear();
}

std::size_t WatchRegistry::Touched(std::string_view key)
{
    // Snapshot the live handles under the lock so we can fire MarkDirty
    // outside it — MarkDirty is a single atomic store, but holding the lock
    // across cross-handle work courts deadlocks if the dirty flag is ever
    // observed under a different mutex on the receive side.
    std::vector<std::shared_ptr<WatchHandle>> targets;
    {
        std::scoped_lock const lock { _mu };
        auto const it = _index.find(std::string { key });
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

} // namespace FastCache
