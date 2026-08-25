// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Logger.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// IStorage decorator that surfaces *value-write failures* — the class of
/// errors that mean the cache could not persist a write at all: a full disk or
/// I/O error, an exhausted memory budget, on-disk corruption, or read-only
/// storage.
///
/// It exists because those failures were otherwise invisible in production. A
/// failed `SET` propagates as a `StorageError` up to `TracingStorage`, but that
/// decorator (a) logs only at `Trace` and (b) is inserted only when the log
/// level is already `Trace` — so at the default level a disk-full write
/// vanishes: no `storage:` line, no metric, just a store that silently did not
/// happen. This decorator is **always** in the stack. On each value write it
/// forwards to the inner storage and, when the result is a persistence-class
/// failure, logs one `Warn` line (visible at the default level) and increments
/// a counter surfaced through `Snapshot().writeErrors` — which the Prometheus
/// exporter renders as `fastcached_write_errors_total`.
///
/// Benign conditional-write outcomes (`KeyExists`/`KeyNotFound`/`CasMismatch`)
/// and client-side rejections (`ValueTooLarge`) are **not** counted or logged:
/// they are normal control flow, already visible to the client via the protocol
/// reply, and would dilute the disk/persistence signal an operator watches.
///
/// Thread safety: the counter is atomic and the injected logger is thread-safe,
/// so the decorator is safe above a `ShardedStorage` reached from several
/// reactor threads. All calls forward to the inner storage; this class adds no
/// semantic behaviour of its own.
class WriteErrorReportingStorage final: public IStorage
{
  public:
    /// Construct over an inner storage and a logger.
    /// @param inner  Backing storage; non-owning reference, must outlive *this.
    /// @param logger Sink for the `Warn` write-failure lines (the process-wide
    ///        logger, since a persistence failure is a server condition, not a
    ///        per-client one).
    WriteErrorReportingStorage(IStorage& inner, ILogger& logger) noexcept;

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

    /// Forward Prefetch to the inner storage (a prefetch performs no value
    /// write, so there is no write-error to meter). See IStorage::Prefetch.
    [[nodiscard]] std::expected<bool, StorageError> Prefetch(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<std::optional<TimePoint>, StorageError> PeekExpiry(std::string_view key,
                                                                                   TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now) override;

    // Compound primitives are forwarded explicitly (not left to the IStorage
    // base defaults) so the inner backend's atomic implementation is used — the
    // base default would re-decompose them into separate primitives and lose the
    // inner lock-owning decorator's atomicity, exactly as TracingStorage does.
    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<bool, StorageError> ClearExpiry(std::string_view key, TimePoint now) override;

    /// Read-modify-write forwarded to the inner atomic implementation; its
    /// result is inspected because a `Store` outcome is a value write that can
    /// fail to persist (redis set/stream mutations, INCRBYFLOAT).
    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
        TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;
    void Resize(std::size_t newMaxBytes) override;

    /// @return The inner storage's snapshot with `writeErrors` set to the count
    ///         of persistence-class write failures observed by this decorator.
    [[nodiscard]] StorageStats Snapshot() const noexcept override;

    /// @return The inner storage's tiers, unchanged.
    ///
    /// `writeErrors` is deliberately NOT stamped onto them. This decorator sees a
    /// refusal and not which store refused, so any tier it picked would be a
    /// guess -- and stating the same count on every tier would be worse still,
    /// because folding two tiered snapshots together sums them field by field and
    /// would count one decorator's failures once per tier it wraps. The number
    /// belongs to the composite, which is where `Snapshot()` reports it.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept override;

  private:
    /// Classify, count, and log a value write whose result is an error. A
    /// persistence-class failure (see the class comment) increments the counter
    /// and emits one `Warn` line naming the verb, key, error code, OS error
    /// number, and context; any other code is left untouched.
    /// @param verb  Uppercase operation name for the log line (e.g. "SET").
    /// @param key   The key the write targeted.
    /// @param error The storage error the inner backend returned.
    void ReportWriteFailure(std::string_view verb, std::string_view key, StorageError const& error) noexcept;

    IStorage& _inner;
    ILogger& _logger;
    std::atomic<std::uint64_t> _writeErrors { 0 };
};

} // namespace FastCache
