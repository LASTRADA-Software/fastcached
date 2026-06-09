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
/// command loop (which reads it on EXEC). Held by shared_ptr so a publisher
/// snapshotting `weak_ptr<WatchHandle>` cannot race a disconnection — the
/// upgrade-under-lock either yields a live owner or the weak has expired.
class WatchHandle: public std::enable_shared_from_this<WatchHandle>
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
    /// Register `handle` as watching `key` and remember the snapshot. Caller
    /// has already read the current CAS via `CacheEngine::PeekCas` on its own
    /// thread; the registry only stores the index entry.
    /// @param handle The connection's handle.
    /// @param key    Lookup key.
    /// @param cas    Snapshotted CAS at WATCH time (0 = key absent).
    void Register(std::shared_ptr<WatchHandle> const& handle, std::string_view key, CasToken cas);

    /// Drop every key-index entry referencing `handle`. Called on UNWATCH,
    /// EXEC, DISCARD, and connection teardown.
    void UnregisterAll(WatchHandle* handle);

    /// Mutation hook: every Redis write verb calls this after its engine
    /// mutation succeeds. Flips MarkDirty on every handle subscribed to `key`.
    /// Returns the number of handles dirtied (mainly useful for tests).
    std::size_t Touched(std::string_view key);

  private:
    mutable std::mutex _mu;
    std::unordered_map<std::string, std::unordered_map<WatchHandle*, std::weak_ptr<WatchHandle>>> _index;
};

} // namespace FastCache
