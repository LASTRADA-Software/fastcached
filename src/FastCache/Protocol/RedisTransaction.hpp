// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/StringHash.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FastCache
{

/// Per-connection state shared between WatchRegistry (which flips the dirty
/// flag from any reactor thread on a Touched(key) hit) and the connection's
/// command loop (which reads it on EXEC). Held by shared_ptr by the
/// connection; the registry indexes weak_ptr copies for upgrade-under-lock
/// inside Touched(), so a publisher snapshotting `weak_ptr<WatchHandle>`
/// cannot race a disconnection — the upgrade either yields a live owner or
/// the weak has expired. (No enable_shared_from_this: nothing inside the
/// class ever calls shared_from_this(); the ownership story is driven
/// externally by the connection's `state.watch` shared_ptr.)
class WatchHandle
{
  public:
    /// Snapshot a key's current CAS at WATCH time. Called from the connection's
    /// own thread; no locking needed (the map is owned by this handle).
    /// @param key Lookup key.
    /// @param cas Current CAS token (0 = key absent at snapshot time).
    void Remember(std::string_view key, CasToken cas);

    /// Forget every snapshot (UNWATCH / DISCARD / EXEC tail). Does NOT
    /// reset the dirty flag — that is the exclusive responsibility of
    /// `ClaimAndClearDirty`. If `Clear` also stored `_dirty=false`, a
    /// racing `Touched` whose `MarkDirty` happens BETWEEN the registry
    /// index erase and `Clear` would be silently wiped (the EXEC race).
    /// Callers that need to drop the dirty bit too must do so explicitly
    /// via `ClaimAndClearDirty` (which atomically exchanges, so no racing
    /// MarkDirty is lost across the read).
    void Clear() noexcept;

    /// Currently-watched keys, for the registry to walk on UnregisterAll.
    [[nodiscard]] std::vector<std::string> WatchedKeys() const;

    /// Mark this handle as having observed a mutation on a watched key. Called
    /// from any reactor thread by the registry's mutation hook; reads on the
    /// connection thread are guarded by the same atomic.
    void MarkDirty() noexcept;

    /// True iff at least one Touched(key) since the last Clear().
    [[nodiscard]] bool IsDirty() const noexcept;

    /// Atomically read-and-clear the dirty flag, also wiping `_snapshots`.
    /// Used by EXEC after the handle has been removed from the registry
    /// index so no further Touched can target it. Returns the pre-clear
    /// dirty state — the caller uses it to decide EXEC commit vs *-1 abort.
    /// @return True iff the dirty flag was set before the clear.
    [[nodiscard]] bool ClaimAndClearDirty() noexcept;

  private:
    mutable std::mutex _mu;
    /// Transparent hashing lets `Remember`'s lookup-by-string_view avoid
    /// the per-call `std::string` allocation that the default
    /// `std::hash<std::string>` map would force.
    std::unordered_map<std::string, CasToken, TransparentStringHash, std::equal_to<>> _snapshots;
    std::atomic<bool> _dirty { false };
};

/// Process-wide registry of WATCH subscriptions. Every Redis write verb calls
/// `Touched(key)` after its engine mutation succeeds; this flips `_dirty` on
/// every WatchHandle that snapshotted that key.
///
/// Thread-safe: every method is safe to call concurrently from any reactor
/// thread. Subscribers (WatchHandles) are held by weak_ptr so a connection
/// teardown cannot strand a stale pointer in the index — upgrade-under-lock
/// during Touched yields a live owner or skips.
class WatchRegistry
{
  public:
    /// Register `handle` as a watcher of `key`. Inserts the index entry
    /// only; the snapshot CAS is stored separately by the caller via
    /// `handle->Remember`. The split ordering closes a race that the
    /// previous "insert + remember in one call" shape had: when WATCH ran
    /// (a) `PeekCas` → (b) `Register` (which inserted the index AND stored
    /// the snapshot), a concurrent `SET` between (a) and (b) called
    /// `Touched` against an empty index and dirtied no handle — `EXEC`
    /// later read `IsDirty() == false` and committed over the racing
    /// write. The fix inverts the WATCH order: insert the index FIRST,
    /// then `PeekCas`, then `Remember`. Any concurrent `Touched` that
    /// lands in the window now finds the index entry and dirties the
    /// handle.
    /// @param handle The connection's handle.
    /// @param key    Lookup key.
    /// @return True iff a fresh (handle, key) entry was inserted; false on
    ///         idempotent re-register (already present from an earlier
    ///         WATCH call). Callers driving partial-failure rollback use
    ///         this to avoid Unregister-ing the prior registration.
    [[nodiscard]] bool Register(std::shared_ptr<WatchHandle> const& handle, std::string_view key);

    /// Drop a single (handle, key) index entry. Used by `HandleWatch`'s
    /// partial-failure rollback so a failing later WATCH does NOT wipe keys
    /// that earlier WATCH calls successfully registered. Returns the number
    /// of entries removed (0 if absent, 1 on success).
    /// Does NOT touch `handle->_snapshots` or the dirty flag — partial
    /// rollback keeps the earlier snapshots and any racing dirty bit live.
    /// @param handle The connection's handle.
    /// @param key    Lookup key registered earlier in this same WATCH call.
    /// @return Number of (handle, key) entries removed.
    std::size_t Unregister(WatchHandle* handle, std::string_view key);

    /// Drop every key-index entry referencing `handle`. Called on UNWATCH,
    /// EXEC, DISCARD, and connection teardown.
    ///
    /// The handle's `Clear()` is invoked AFTER the registry mutex has been
    /// released so the registry→handle lock-order edge does not exist —
    /// any future code path that wants to take `handle->_mu` before the
    /// registry mutex cannot deadlock against this call.
    void UnregisterAll(WatchHandle* handle);

    /// Mutation hook: every Redis write verb calls this after its engine
    /// mutation succeeds. Flips MarkDirty on every handle subscribed to `key`.
    /// Returns the number of handles dirtied (mainly useful for tests).
    std::size_t Touched(std::string_view key);

    /// Whole-database mutation hook for FLUSHDB / flush_all. Flips
    /// MarkDirty on every registered handle, regardless of which keys
    /// the handle had been watching — the database-wide wipe
    /// invalidates every WATCH snapshot. Returns the number of handles
    /// dirtied. Steady-state cost when nothing is watched is a single
    /// atomic load (the same lock-free fast path used by `Touched`).
    /// @return Number of WatchHandles dirtied by this call.
    std::size_t TouchedAll();

    /// Lock-free fast path: true iff at least one handle is currently
    /// registered for some key. Callers in hot paths (MSET/DEL multi-key
    /// loops) probe this once per command to skip the per-key Touched
    /// fan-out when nothing is watching.
    /// @return True iff `Touched` could plausibly dirty a handle.
    [[nodiscard]] bool HasAnyWatchers() const noexcept;

  private:
    mutable std::mutex _mu;
    /// Transparent hashing — every `Touched(key)` call previously
    /// allocated a `std::string` to look up `_index`. With
    /// `TransparentStringHash` + `std::equal_to<>` the lookup accepts the
    /// caller's `std::string_view` directly, zero-alloc, on the hot
    /// write path.
    std::unordered_map<std::string,
                       std::unordered_map<WatchHandle*, std::weak_ptr<WatchHandle>>,
                       TransparentStringHash,
                       std::equal_to<>>
        _index;
    /// Total number of (handle, key) entries across all _index buckets.
    /// Updated under `_mu` so it stays consistent with `_index`, but read
    /// lock-free in `Touched` so the steady-state "nothing watching"
    /// fast path is a single atomic load on the hot write path. Without
    /// it every successful cache mutation paid a global mutex
    /// acquisition AND a `std::string` heap allocation just to find an
    /// empty index bucket.
    std::atomic<std::size_t> _entryCount { 0 };
};

} // namespace FastCache
