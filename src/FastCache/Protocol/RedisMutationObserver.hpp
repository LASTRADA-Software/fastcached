// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorageMutationObserver.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache
{

class WatchRegistry;

/// Concrete `IStorageMutationObserver` that fans every storage-layer
/// mutation out to the WATCH dirty-flag registry.
///
/// Installed in main.cpp by wrapping the production storage chain in a
/// `NotifyingStorage` configured with one of these observers. Because
/// the observer sits BELOW the protocol handler, EVERY protocol (Redis,
/// memcached text, memcached binary, memcached meta) and every
/// storage-internal mutation (FlushDb, lazy expire, LRU evict) routes
/// through the same fan-out site — closing the cross-protocol WATCH
/// bypass and the missing-FLUSHDB-event bugs without per-handler
/// duplication.
///
/// The observer's pointers may individually be null:
///   - A null `watches` disables WATCH dirty signalling (useful in
///     memcached-only deployments).
///   - A null `keyspaceNotifier` disables keyspace publication.
///
/// `HasObservers` returns true when EITHER channel has potential
/// observers, so the storage decorator's fast-path probe is precise.
class RedisMutationObserver final: public IStorageMutationObserver
{
  public:
    /// Construct with an optional WATCH sink.
    /// @param watches WATCH registry; null disables WATCH dirty fan-out.
    explicit RedisMutationObserver(WatchRegistry* watches) noexcept;

    void OnMutation(MutationKind kind, std::string_view key) noexcept override;
    [[nodiscard]] bool HasObservers() const noexcept override;

    /// Map a storage-layer MutationKind to the Redis keyspace event name
    /// + class pair (`set`, `del`, `expire`, etc.). Looked up from a
    /// single descriptor table so a new mutation kind is one row, not a
    /// per-handler scatter. Returns empty `name` if no keyspace event
    /// should fire for this kind.
    ///
    /// Exposed for unit-test coverage of the table; production callers
    /// use OnMutation.
    struct EventDescriptor
    {
        std::string_view name {};   ///< Redis keyspace event name.
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
};

} // namespace FastCache
