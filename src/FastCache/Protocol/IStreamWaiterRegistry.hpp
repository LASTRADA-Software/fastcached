// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// A client blocked in XREAD/XREADGROUP BLOCK, waiting for an entry to be
/// appended to one of the stream keys it registered interest in. The registry
/// owns a weak reference and upgrades it under its lock when notifying, so a
/// waiter that disconnects mid-wait is skipped rather than dereferenced.
class IStreamWaiter
{
  public:
    IStreamWaiter() = default;
    IStreamWaiter(IStreamWaiter const&) = delete;
    IStreamWaiter(IStreamWaiter&&) = delete;
    IStreamWaiter& operator=(IStreamWaiter const&) = delete;
    IStreamWaiter& operator=(IStreamWaiter&&) = delete;
    virtual ~IStreamWaiter() = default;

    /// Called (from any reactor thread) when a stream key the waiter registered
    /// for gains a new entry. The implementation marshals resumption of the
    /// blocked coroutine back to its own reactor; it must be safe to call after
    /// the waiter has already been woken or torn down.
    virtual void Wake() noexcept = 0;
};

/// Process-wide, thread-safe registry coordinating blocking stream reads with
/// XADD-side appends — the stream analogue of `IPubSubRegistry`. An XADD on any
/// connection (hence any reactor thread) calls `NotifyAppended`, which wakes
/// every waiter registered for that key.
///
/// Injected through `SessionContext`; a null pointer means "blocking reads are
/// not wired in", in which case the Redis handler serves XREAD/XREADGROUP
/// non-blockingly (polling once) rather than parking.
class IStreamWaiterRegistry
{
  public:
    IStreamWaiterRegistry() = default;
    IStreamWaiterRegistry(IStreamWaiterRegistry const&) = delete;
    IStreamWaiterRegistry(IStreamWaiterRegistry&&) = delete;
    IStreamWaiterRegistry& operator=(IStreamWaiterRegistry const&) = delete;
    IStreamWaiterRegistry& operator=(IStreamWaiterRegistry&&) = delete;
    virtual ~IStreamWaiterRegistry() = default;

    /// Register `waiter` as interested in each key in `keys`. The registry holds
    /// a weak reference; the caller keeps the owning `shared_ptr` alive for the
    /// duration of the wait.
    /// @param waiter The blocked waiter (weak reference retained).
    /// @param keys   Stream keys the waiter is blocked on.
    virtual void Register(std::weak_ptr<IStreamWaiter> waiter, std::span<std::string const> keys) = 0;

    /// Remove every registration belonging to `waiter` (called once the wait
    /// resolves, by timeout or by data). Safe to call when not registered.
    /// @param waiter The waiter to deregister (compared by address).
    virtual void Unregister(IStreamWaiter const* waiter) = 0;

    /// Wake every waiter currently registered for `key`. Called by XADD after a
    /// successful append; safe to call when there are no waiters.
    /// @param key The stream key that gained an entry.
    virtual void NotifyAppended(std::string_view key) = 0;
};

} // namespace FastCache
