// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>

#include <atomic>
#include <cstddef>
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

    /// Forget every snapshot (UNWATCH / DISCARD / EXEC tail).
    void Clear() noexcept;

    /// Currently-watched keys, for the registry to walk on UnregisterAll.
    [[nodiscard]] std::vector<std::string> WatchedKeys() const;

    /// Mark this handle as having observed a mutation on a watched key. Called
    /// from any reactor thread by the registry's mutation hook; reads on the
    /// connection thread are guarded by the same atomic.
    void MarkDirty() noexcept;

    /// True iff at least one Touched(key) since the last Clear().
    [[nodiscard]] bool IsDirty() const noexcept;

  private:
    mutable std::mutex _mu;
    std::unordered_map<std::string, CasToken> _snapshots;
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
    void Register(std::shared_ptr<WatchHandle> const& handle, std::string_view key);

    /// Drop every key-index entry referencing `handle`. Called on UNWATCH,
    /// EXEC, DISCARD, and connection teardown.
    void UnregisterAll(WatchHandle* handle);

    /// Mutation hook: every Redis write verb calls this after its engine
    /// mutation succeeds. Flips MarkDirty on every handle subscribed to `key`.
    /// Returns the number of handles dirtied (mainly useful for tests).
    std::size_t Touched(std::string_view key);

    /// Lock-free fast path: true iff at least one handle is currently
    /// registered for some key. Callers in hot paths (MSET/DEL multi-key
    /// loops) probe this once per command to skip the per-key Touched
    /// fan-out when nothing is watching.
    /// @return True iff `Touched` could plausibly dirty a handle.
    [[nodiscard]] bool HasAnyWatchers() const noexcept;

  private:
    mutable std::mutex _mu;
    std::unordered_map<std::string, std::unordered_map<WatchHandle*, std::weak_ptr<WatchHandle>>> _index;
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
