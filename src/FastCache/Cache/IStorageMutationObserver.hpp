// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Kind of mutation that produced a key-touching event. Bound to a fixed
/// data table (see Protocol/KeyspaceNotifier) so adding a new mutation
/// (e.g. RENAME, COPY) is one new enumerator + one row, not a scatter
/// across every storage decorator.
///
/// The set is intentionally small and storage-layer-agnostic: the
/// decorator that owns this enum does not know about Redis verbs vs
/// memcached commands. Protocol handlers (RedisResp / MemcachedText /
/// MemcachedBinary) translate the wire verb to one of these kinds when
/// dispatching to the storage layer; the decorator then fires the same
/// hook regardless of which protocol caused the mutation.
enum class MutationKind : std::uint8_t
{
    Set,         ///< SET / set — full overwrite or new insert.
    Delete,      ///< DEL / delete — explicit removal.
    Append,      ///< APPEND / append — value extended at the tail.
    Prepend,     ///< PREPEND / prepend — value extended at the head.
    Incr,        ///< INCR / DECR / INCRBY family — numeric counter mutation.
    Touch,       ///< TOUCH / EXPIRE / PEXPIRE / EXPIREAT family — TTL refresh.
    Persist,     ///< PERSIST — TTL clear.
    MarkStale,   ///< meta `md I` / `ms I` — entry marked stale.
    Cas,         ///< Compare-and-swap success.
    Update,      ///< Update primitive (SADD/SREM/SPOP/INCRBYFLOAT/...).
    Expire,      ///< Lazy or scheduled TTL expiry consumed the entry.
    Evict,       ///< LRU eviction reclaimed the entry under memory pressure.
    FlushDb,     ///< FLUSHDB / flush_all — entire database wiped.
};

/// Notification sink consumed by `NotifyingStorage` on every mutation that
/// changes the durable state of a key (or the entire database for
/// FlushDb). Implementations are responsible for fan-out: the bundled
/// Redis observer fires WATCH dirty signals AND keyspace notifications;
/// a future memcached observer might add invalidation callbacks, etc.
///
/// `OnMutation` is invoked synchronously from inside the storage call
/// that produced the mutation, after the inner storage has committed.
/// Implementations MUST NOT block (they run on the reactor thread and
/// every concurrent caller of the same storage shard waits on them).
class IStorageMutationObserver
{
  public:
    IStorageMutationObserver() = default;
    IStorageMutationObserver(IStorageMutationObserver const&) = delete;
    IStorageMutationObserver(IStorageMutationObserver&&) = delete;
    IStorageMutationObserver& operator=(IStorageMutationObserver const&) = delete;
    IStorageMutationObserver& operator=(IStorageMutationObserver&&) = delete;
    virtual ~IStorageMutationObserver() = default;

    /// Called after a successful key-level mutation. `key` is the affected
    /// key. For `FlushDb`, `key` is empty (the event is whole-database).
    /// @param kind What kind of mutation occurred.
    /// @param key  Affected key (empty for FlushDb).
    virtual void OnMutation(MutationKind kind, std::string_view key) noexcept = 0;

    /// Lock-free fast probe: true iff this observer would do meaningful
    /// work for `OnMutation`. Storage decorators consult this once per
    /// command to skip the per-key fan-out when nothing is watching and
    /// no subscriber is listening. The atomic-check shape is documented
    /// in `WatchRegistry::HasAnyWatchers` and
    /// `KeyspaceNotifier::HasAnyPublishers`.
    /// @return True if OnMutation could produce an externally visible
    ///         side effect (a WATCH dirty bit flip, a published event).
    [[nodiscard]] virtual bool HasObservers() const noexcept = 0;
};

} // namespace FastCache
