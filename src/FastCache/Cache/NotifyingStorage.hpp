// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
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
    void Resize(std::size_t newMaxBytes) override;
    [[nodiscard]] StorageStats Snapshot() const noexcept override;
    [[nodiscard]] bool SupportsSharedRead() const noexcept override;
    void PromoteOnRead(std::string_view key, TimePoint now) override;

  private:
    /// Fire `kind`/`key` on the observer, gated by the lock-free
    /// `HasObservers` probe. Called from every mutating override after
    /// the inner storage has committed.
    /// @param kind Mutation kind.
    /// @param key  Affected key (empty for FlushDb).
    void Notify(MutationKind kind, std::string_view key) const noexcept;

    IStorage& _inner;
    IStorageMutationObserver* _observer;
};

} // namespace FastCache
