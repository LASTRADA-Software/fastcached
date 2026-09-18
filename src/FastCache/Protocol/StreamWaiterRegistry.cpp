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
    // does not accumulate dead keys under churn. `std::erase_if` is specified as
    // the erase-while-walking loop this replaced, so the predicate may do the
    // removal and answer whether it left the set empty.
    std::erase_if(_keys, [waiter](auto& waiters) {
        waiters.second.erase(waiter);
        return waiters.second.empty();
    });
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
        // outside the lock. Drop entries whose owner has already disconnected:
        // `std::erase_if` visits each entry once, in order, and erases exactly
        // the ones whose upgrade failed.
        std::erase_if(it->second, [&wake](auto& entry) {
            auto strong = entry.second.lock();
            if (strong == nullptr)
                return true;
            wake.push_back(std::move(strong));
            return false;
        });
        if (it->second.empty())
            _keys.erase(it);
    }
    // Wake outside the lock: a waiter's resume marshals onto its own reactor and
    // must not run with `_mu` held (it may re-enter Unregister on resolution).
    for (auto const& waiter: wake)
        waiter->Wake();
}

} // namespace FastCache
