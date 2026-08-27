// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorageMutationObserver.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache
{

class KeyspaceNotifier;
class WatchRegistry;

/// Concrete `IStorageMutationObserver` that fans every storage-layer
/// mutation out to the WATCH dirty-flag registry, and publishes the
/// keyspace events no protocol handler is in a position to publish.
///
/// Installed in main.cpp by wrapping the production storage chain in a
/// `NotifyingStorage` configured with one of these observers. Because
/// the observer sits BELOW the protocol handler, EVERY protocol (Redis,
/// memcached text, memcached binary, memcached meta) and every
/// storage-internal mutation (FlushDb, TTL expiry, LRU eviction) routes
/// through the same fan-out site — closing the cross-protocol WATCH
/// bypass and the missing-FLUSHDB-event bugs without per-handler
/// duplication.
///
/// **Only two kinds publish a keyspace event here**, and the reason is worth
/// keeping in view: `RedisResp.cpp` already fires verb-specific events for
/// everything a client asked for, so a second publish would put two
/// `__keyevent@0__:*` frames on the wire per write. `Expire` and `Evict` are
/// the two nobody asked for — there is no verb executing to notice them — so
/// this is the only layer that can. `EventTable` in the .cpp is where that
/// lives; it is a row per kind, not a condition.
///
/// The observer's pointers may individually be null:
///   - A null `watches` disables WATCH dirty signalling (useful in
///     memcached-only deployments).
///   - A null `keyspace` disables keyspace publication.
///
/// `HasObservers` returns true when EITHER channel has potential
/// observers, so the storage decorator's fast-path probe is precise.
class RedisMutationObserver final: public IStorageMutationObserver
{
  public:
    /// Construct with an optional WATCH sink and an optional keyspace notifier.
    /// @param watches  WATCH registry; null disables WATCH dirty fan-out.
    /// @param keyspace Keyspace notifier; null disables event publication.
    explicit RedisMutationObserver(WatchRegistry* watches, KeyspaceNotifier* keyspace = nullptr) noexcept;

    void OnMutation(MutationKind kind, std::string_view key) noexcept override;
    [[nodiscard]] bool HasObservers() const noexcept override;

    /// Map a storage-layer MutationKind to the Redis keyspace event name
    /// + class pair. Looked up from a single descriptor table so a new
    /// mutation kind is one row, not a per-handler scatter. An empty
    /// `name` means "publish nothing for this kind", which is most of them.
    ///
    /// Exposed for unit-test coverage of the table; production callers
    /// use OnMutation.
    struct EventDescriptor
    {
        MutationKind kind {};       ///< The kind this row describes; its index in the table.
        std::string_view name {};   ///< Redis keyspace event name; empty means do not publish.
        std::uint32_t classFlag {}; ///< KeyspaceEvents::* bit.
    };

    /// Resolve the keyspace event for `kind`. Returns empty `name` when
    /// no event is defined for this mutation kind.
    /// @param kind Storage mutation kind.
    /// @return Event descriptor; `name.empty()` means "do not publish".
    [[nodiscard]] static EventDescriptor DescriptorFor(MutationKind kind) noexcept;

  private:
    /// Iterate every WATCH-registered key and trip its dirty flag.
    /// Used for FlushDb where no single key identifies the event.
    /// @return The number of handles dirtied (mainly for tests/metrics).
    [[nodiscard]] std::size_t TouchAllForFlush() const noexcept;

    WatchRegistry* _watches;
    KeyspaceNotifier* _keyspace;
};

} // namespace FastCache
