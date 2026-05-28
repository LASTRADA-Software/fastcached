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
                                                               TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> CompareAndSwap(std::string_view key,
                                                                       CasToken expected,
                                                                       std::vector<std::byte> value,
                                                                       std::uint32_t flags,
                                                                       TimePoint expiry,
                                                                       TimePoint now) override;

    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> IncrementOrInitialize(std::string_view key,
                                                                                          std::int64_t delta,
                                                                                          TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key, TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;
    [[nodiscard]] StorageStats Snapshot() const noexcept override;

  private:
    IStorage& _inner;
    ILogger& _logger;
    IClock& _clock;

    /// Single point of truth for the trace-line shape.
    ///
    /// Invokes `op`, then (when trace is on) computes latency and emits
    /// a log line `storage: <verb> key=<key> result=<fmt(result)> took=<us>us`.
    /// Returns whatever `op` returned unchanged.
    template <class Op, class OutcomeFmt>
    auto TraceCall(std::string_view verb,
                   std::string_view key,
                   Op&& op,
                   OutcomeFmt&& fmt) -> decltype(op())
    {
        bool const traceOn = _logger.MinLevel() <= LogLevel::Trace;
        auto const startedAt = _clock.Now();
        auto result = std::forward<Op>(op)();
        if (traceOn)
        {
            auto const took = _clock.Now() - startedAt;
            _logger.Logf(LogLevel::Trace,
                         "storage: {} key={} result={} took={}us",
                         verb,
                         key,
                         std::forward<OutcomeFmt>(fmt)(result),
                         std::chrono::duration_cast<std::chrono::microseconds>(took).count());
        }
        return result;
    }
};

} // namespace FastCache
