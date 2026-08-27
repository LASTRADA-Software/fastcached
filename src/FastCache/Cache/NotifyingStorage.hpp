// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IReclaimLog.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/IStorageMutationObserver.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache
{

/// IStorage decorator that fires `IStorageMutationObserver::OnMutation`
/// after every successful key-level mutation on the inner storage.
///
/// Centralising notification at this layer fixes three correctness
/// bugs that scattered per-handler emission caused:
///
///   1. Memcached writes (`set`, `add`, `replace`, `delete`, `cas`,
///      `incr`/`decr`, `append`/`prepend`, `flush_all`) used to bypass
///      WATCH dirty signals and keyspace notifications entirely. A
///      Redis WATCH'd key mutated by a concurrent memcached client
///      silently committed the watching client's EXEC.
///
///   2. Storage-internal events (TTL expiry inside `Get`/`Peek`/`PurgeExpired`,
///      LRU eviction inside `Set`/`Add`/...) never fired WATCH dirties
///      or `__keyevent@0__:expired` / `:evicted` events. Watched keys
///      that vanished under TTL pressure passed an EXEC the WATCH
///      should have aborted.
///
///   3. `FLUSHDB` / `flush_all` wiped the entire keyspace without
///      firing the canonical `__keyevent@0__:flushdb` event or
///      dirtying any WATCH'd keys.
///
/// Because the decorator sits BELOW the protocol handler in the storage
/// chain (typically wrapped around `LayeredStorage` or `ShardedStorage`),
/// every protocol pays the notification cost exactly once per mutation,
/// regardless of which wire the request came in on.
///
/// The observer pointer may be `nullptr` to disable notifications
/// (cheap: every call short-circuits on the null check before the inner
/// call). When non-null, the observer's own `HasObservers()` fast probe
/// is consulted to skip the OnMutation call when nothing is listening —
/// keeping the steady-state overhead to a single atomic load on the
/// hot write path.
class NotifyingStorage final: public IStorage
{
  public:
    /// Construct over an inner storage and an optional observer.
    /// @param inner    Backing storage; non-owning reference, must outlive *this.
    /// @param observer Notification sink. Pass nullptr to disable.
    NotifyingStorage(IStorage& inner, IStorageMutationObserver* observer) noexcept;

    [[nodiscard]] std::expected<GetResult, StorageError> Get(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Set(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            TimePoint expiry) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Add(
        std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Replace(
        std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Append(std::string_view key,
                                                               std::span<std::byte const> suffix,
                                                               CasToken expected,
                                                               TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                CasToken expected,
                                                                TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> CompareAndSwap(std::string_view key,
                                                                       CasToken expected,
                                                                       std::vector<std::byte> value,
                                                                       std::uint32_t flags,
                                                                       TimePoint expiry,
                                                                       TimePoint now) override;

    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> IncrementOrInitialize(std::string_view key,
                                                                                          std::uint64_t magnitude,
                                                                                          bool decrement,
                                                                                          TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Touch(std::string_view key,
                                                              TimePoint newExpiry,
                                                              TimePoint now) override;

    [[nodiscard]] std::expected<GetResult, StorageError> Peek(std::string_view key, TimePoint now) override;

    /// Forward Prefetch to the inner storage (no notification semantics —
    /// a prefetch is not a client-visible mutation). See IStorage::Prefetch.
    [[nodiscard]] std::expected<bool, StorageError> Prefetch(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<std::optional<TimePoint>, StorageError> PeekExpiry(std::string_view key,
                                                                                   TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now) override;

    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<bool, StorageError> ClearExpiry(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
        TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;

    /// Keep `log` **and** forward it to the inner storage.
    ///
    /// This decorator reclaims nothing itself; it is the drain end of the very
    /// log the tiers below record into. Wiring both halves from one call is
    /// what makes it impossible to connect one without the other.
    /// @param log Sink for reclaimed keys, or nullptr to stop reporting.
    void SetReclaimLog(IReclaimLog* log) override;
    void Resize(std::size_t newMaxBytes) override;
    [[nodiscard]] StorageStats Snapshot() const noexcept override;
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept override;
    [[nodiscard]] bool SupportsSharedRead() const noexcept override;
    void PromoteOnRead(std::string_view key, TimePoint now) override;

  private:
    /// Fire `kind`/`key` on the observer, gated by the lock-free
    /// `HasObservers` probe. The raw emit, with no drain of its own — the
    /// drain itself uses this, which is what keeps the two from recursing.
    /// @param kind Mutation kind.
    /// @param key  Affected key (empty for FlushDb).
    void Emit(MutationKind kind, std::string_view key) const noexcept;

    /// Report `kind`/`key`, preceded by anything the same call reclaimed.
    /// Called from every mutating override after the inner storage has
    /// committed.
    /// @param kind Mutation kind.
    /// @param key  Affected key (empty for FlushDb).
    void Notify(MutationKind kind, std::string_view key) const noexcept;

    /// Drain whatever the tiers reclaimed during the call that is returning,
    /// and fire one `Expire`/`Evict` per key.
    ///
    /// This is the half of the reclaim path that runs **outside** every storage
    /// lock — see `IReclaimLog`. The tiers only appended; publishing happens
    /// here, where taking the observer's locks is safe.
    void FlushReclaimed() const noexcept;

    /// Fires the tiers' reclaim events when the enclosing storage call returns.
    ///
    /// A guard rather than a line before each `return`, because a reclaim is
    /// not tied to the call *succeeding*, nor to it notifying anything. A
    /// `Delete` of a key whose TTL has passed reclaims it and then reports
    /// KeyNotFound; a `Get` on the strict-LRU path reclaims and notifies
    /// nothing at all. Anchoring the drain to scope exit is what makes those
    /// cases impossible to leave out, and it covers every early return.
    ///
    /// It is the backstop, not the usual path: `Notify` drains before emitting,
    /// so a call that reports something has already flushed by the time this
    /// runs and finds nothing left. That ordering matters — a reclaim and the
    /// call that caused it can name the SAME key, as `ADD k` on a lapsed TTL
    /// does, and emitting `set k` before `expired k` tells a subscriber a live
    /// key is gone.
    class ReclaimDrain
    {
      public:
        /// @param owner The decorator whose log to drain on scope exit.
        explicit ReclaimDrain(NotifyingStorage const& owner) noexcept:
            _owner { &owner }
        {
        }

        ReclaimDrain(ReclaimDrain const&) = delete;
        ReclaimDrain(ReclaimDrain&&) = delete;
        ReclaimDrain& operator=(ReclaimDrain const&) = delete;
        ReclaimDrain& operator=(ReclaimDrain&&) = delete;

        ~ReclaimDrain()
        {
            _owner->FlushReclaimed();
        }

      private:
        NotifyingStorage const* _owner;
    };

    IStorage& _inner;
    IStorageMutationObserver* _observer;
    /// The drain end of the log the tiers below record into. Null until
    /// `SetReclaimLog` wires it, which is every existing construction site.
    IReclaimLog* _reclaim { nullptr };
};

} // namespace FastCache
