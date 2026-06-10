// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/IStreamWaiterRegistry.hpp>

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace FastCache
{

/// In-memory IStreamWaiterRegistry: a mutex-guarded key → waiter-set map. One
/// instance is shared process-wide (owned by the daemon body) and injected into
/// every connection's SessionContext, so an XADD on any reactor thread can wake
/// a client blocked in XREAD/XREADGROUP on another reactor thread.
///
/// Mirrors `PubSubRegistry`'s snapshot-and-upgrade-weak_ptr discipline: a key's
/// waiters are stored by weak_ptr keyed on the raw pointer; `NotifyAppended`
/// snapshots and upgrades them under the lock, then calls `Wake()` outside the
/// lock so a waiter's resumption (which marshals onto its own reactor) can never
/// deadlock against an append in progress on a different reactor.
class StreamWaiterRegistry final: public IStreamWaiterRegistry
{
  public:
    void Register(std::weak_ptr<IStreamWaiter> waiter, std::span<std::string const> keys) override;
    void Unregister(IStreamWaiter const* waiter) override;
    void NotifyAppended(std::string_view key) override;

  private:
    /// Per-key waiter set. The raw pointer is the lookup/erase key (stable for
    /// the waiter's lifetime); the weak_ptr lets `NotifyAppended` upgrade to a
    /// shared_ptr under the lock so the `Wake()` call outside the lock cannot
    /// land on a freed waiter.
    using WaiterSet = std::unordered_map<IStreamWaiter const*, std::weak_ptr<IStreamWaiter>>;

    mutable std::mutex _mu;
    /// stream key -> waiters blocked on it.
    std::unordered_map<std::string, WaiterSet> _keys;
};

} // namespace FastCache
