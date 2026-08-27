// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorageMutationObserver.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// One entry a storage tier reclaimed without a client asking it to.
struct ReclaimedKey
{
    MutationKind kind {}; ///< `MutationKind::Expire` or `MutationKind::Evict`.
    std::string key {};   ///< The key that went away.
};

/// Where a storage tier reports the entries it reclaimed on its own
/// initiative — a TTL that lapsed, or an LRU tail dropped to stay under the
/// byte budget. Everything else a client can observe is already reported by
/// `NotifyingStorage`, which sees the call that caused it; these two are the
/// mutations no caller asked for, so nothing above the tier knows the key.
///
/// **This is a buffer and not a callback, and that is the whole design.** A
/// tier reclaims from inside its locked region — `InMemoryLruStorage::EvictToFit`
/// runs under the `ShardedStorage` shard lock — while an observer publishes into
/// other connections' output buffers and takes its own locks to do it.
/// `NotifyingStorage::Update` already documents why the second must not happen
/// inside the first. So the two sides run under different rules:
///
///   * `Record` is called from **inside** the tier's lock. It appends and
///     nothing else: it must not publish, must not block, and must not re-enter
///     storage.
///   * `Drain` is called by `NotifyingStorage` **outside** every storage lock,
///     once the inner call has returned, and that is where events are fired.
///
/// Recording is gated by `IsRecording()` so a daemon nobody is watching pays one
/// relaxed atomic load per reclaimed entry rather than a string copy — the same
/// fast-probe shape as `IStorageMutationObserver::HasObservers`.
class IReclaimLog
{
  public:
    IReclaimLog() = default;
    IReclaimLog(IReclaimLog const&) = delete;
    IReclaimLog(IReclaimLog&&) = delete;
    IReclaimLog& operator=(IReclaimLog const&) = delete;
    IReclaimLog& operator=(IReclaimLog&&) = delete;
    virtual ~IReclaimLog() = default;

    /// Append one reclaimed key. Called from inside a storage tier's locked
    /// region — see the class comment for what that forbids.
    /// @param kind `MutationKind::Expire` or `MutationKind::Evict`.
    /// @param key  The reclaimed key; copied, since the caller is about to
    ///             erase the node that owns it.
    virtual void Record(MutationKind kind, std::string_view key) noexcept = 0;

    /// Hand over everything recorded so far and leave the log empty.
    ///
    /// `out` is emptied first, so a caller may reuse one buffer across calls;
    /// its storage is handed to the log to refill, which keeps a steady-state
    /// drain allocation-free.
    /// @param out Receives the pending entries.
    virtual void Drain(std::vector<ReclaimedKey>& out) = 0;

    /// Lock-free probe: is there anything to `Drain`? Lets the drain site skip
    /// taking the log's lock on the overwhelmingly common empty call.
    /// @return True if at least one entry is pending.
    [[nodiscard]] virtual bool HasPending() const noexcept = 0;

    /// Lock-free probe: would a `Record` be kept? False when nothing is
    /// listening, which is when the tiers skip the call entirely.
    /// @return True if reclaimed keys are worth recording.
    [[nodiscard]] virtual bool IsRecording() const noexcept = 0;
};

} // namespace FastCache
