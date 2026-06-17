// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Logger.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

/// IStorage decorator that emits one Trace log line per call describing
/// the verb, key, outcome, and latency.
///
/// All calls are forwarded to an inner IStorage; this class adds no
/// semantic behavior of its own. When the logger's `MinLevel()` is
/// above Trace, the only overhead is one atomic load per call plus the
/// (already cheap) clock read. The boilerplate that would otherwise
/// have to be copy-pasted across all eleven IStorage methods is
/// confined to a single templated `TraceCall` helper.
class TracingStorage final: public IStorage
{
  public:
    /// Construct over an inner storage, logger, and clock.
    /// @param inner  Backing storage; non-owning reference, must outlive *this.
    /// @param logger Sink for trace lines.
    /// @param clock  Source for the latency anchor (injected for tests).
    TracingStorage(IStorage& inner, ILogger& logger, IClock& clock) noexcept;

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

    /// PeekExpiry override so trace lines distinguish TTL polling traffic
    /// from genuine value Peek traffic; without this override the
    /// inherited default would re-enter Peek (emitting a misleading
    /// "PEEK" verb on every TTL/PTTL request).
    [[nodiscard]] std::expected<std::optional<TimePoint>, StorageError> PeekExpiry(std::string_view key,
                                                                                   TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now) override;

    // Forwarded explicitly (not left to the IStorage base default) so the
    // inner storage's *atomic* compound implementation is used — the base
    // default would re-decompose these into separate Touch/Get/Delete
    // calls and lose the inner lock-owning decorator's atomicity.
    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     TimePoint now) override;

    /// PERSIST primitive override so the inner storage's atomic
    /// implementation is used and the trace surfaces TTL-clear operations.
    [[nodiscard]] std::expected<bool, StorageError> ClearExpiry(std::string_view key, TimePoint now) override;

    /// Update override so the inner ShardedStorage's per-shard atomicity
    /// is preserved end-to-end and the trace surfaces RMW operations
    /// (INCR/DECR/SADD/SREM/INCRBYFLOAT) under their own verb. The base
    /// default would decompose into Peek + Set, losing both atomicity
    /// and (until UpdateOutcome::newExpiry was added) the entry's TTL.
    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
        TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;
    void Resize(std::size_t newMaxBytes) override;
    [[nodiscard]] StorageStats Snapshot() const noexcept override;

  private:
    IStorage& _inner;
    ILogger& _logger;
    IClock& _clock;

    /// The client-source prefix for the current trace line: "[203.0.113.7] "
    /// when a handler published `Detail::StorageSourceTag` for this call, else
    /// "". Only evaluated on the trace-enabled path, so the small allocation is
    /// off the hot path.
    /// @return The bracketed-source-plus-space prefix, or an empty string.
    [[nodiscard]] static std::string SourcePrefix()
    {
        auto const tag = Detail::StorageSourceTag;
        if (tag.empty())
            return {};
        return std::string { tag } + ' ';
    }

    /// Single point of truth for the trace-line shape.
    ///
    /// Invokes `op`, then (when trace is on) computes latency and emits
    /// a log line `storage: <verb> key=<key> result=<fmt(result)> took=<us>us`.
    /// Returns whatever `op` returned unchanged.
    template <class Op, class OutcomeFmt>
    auto TraceCall(std::string_view verb, std::string_view key, Op&& op, OutcomeFmt&& fmt) -> decltype(op())
    {
        bool const traceOn = _logger.MinLevel() <= LogLevel::Trace;
        auto const startedAt = _clock.Now();
        auto result = std::forward<Op>(op)();
        if (traceOn)
        {
            auto const took = _clock.Now() - startedAt;
            // Prefix the client source (e.g. "[203.0.113.7] ") when a handler
            // published one for this call — see Detail::StorageSourceTag. Empty
            // tag => the line is unprefixed, exactly as before --log-source.
            _logger.Logf(LogLevel::Trace,
                         "{}storage: {} key={} result={} took={}us",
                         SourcePrefix(),
                         verb,
                         key,
                         std::forward<OutcomeFmt>(fmt)(result),
                         std::chrono::duration_cast<std::chrono::microseconds>(took).count());
        }
        return result;
    }
};

} // namespace FastCache
