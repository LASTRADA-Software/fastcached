// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/StreamWaiterRegistry.hpp>

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

void StreamWaiterRegistry::Register(std::weak_ptr<IStreamWaiter> waiter, std::span<std::string const> keys)
{
    auto* const raw = waiter.lock().get();
    if (raw == nullptr)
        return; // the waiter already expired; nothing to register.
    std::scoped_lock const lock { _mu };
    for (auto const& key: keys)
        _keys[key].insert_or_assign(raw, waiter);
}

void StreamWaiterRegistry::Unregister(IStreamWaiter const* waiter)
{
    std::scoped_lock const lock { _mu };
    // Drop the waiter from every key set, pruning any set it empties so the map
    // does not accumulate dead keys under churn.
    for (auto it = _keys.begin(); it != _keys.end();)
    {
        it->second.erase(waiter);
        if (it->second.empty())
            it = _keys.erase(it);
        else
            ++it;
    }
}

void StreamWaiterRegistry::NotifyAppended(std::string_view key)
{
    std::vector<std::shared_ptr<IStreamWaiter>> wake;
    {
        std::scoped_lock const lock { _mu };
        auto const it = _keys.find(std::string { key });
        if (it == _keys.end())
            return;
        // Snapshot-and-upgrade: pin each waiter for the Wake() below, which runs
        // outside the lock. Drop entries whose owner has already disconnected.
        for (auto entry = it->second.begin(); entry != it->second.end();)
        {
            if (auto strong = entry->second.lock())
            {
                wake.push_back(std::move(strong));
                ++entry;
            }
            else
                entry = it->second.erase(entry);
        }
        if (it->second.empty())
            _keys.erase(it);
    }
    // Wake outside the lock: a waiter's resume marshals onto its own reactor and
    // must not run with `_mu` held (it may re-enter Unregister on resolution).
    for (auto const& waiter: wake)
        waiter->Wake();
}

} // namespace FastCache
