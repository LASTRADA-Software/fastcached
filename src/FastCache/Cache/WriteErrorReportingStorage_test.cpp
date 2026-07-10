// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/WriteErrorReportingStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{

/// Fault-injecting IStorage stub: every value-write method returns `writeError`
/// when set (else success), and `Snapshot()` returns `stats`. The compound
/// primitives (Update/GetAndTouch/...) use the IStorage base defaults, which
/// compose the overridden primitives — enough to drive the decorator.
class StubStorage final: public FastCache::IStorage
{
  public:
    std::optional<FastCache::StorageError> writeError {}; ///< Error every write returns, or success when unset.
    FastCache::StorageStats stats {};                     ///< Value Snapshot() returns.

    [[nodiscard]] std::expected<FastCache::CasToken, FastCache::StorageError> WriteResult() const
    {
        if (writeError.has_value())
            return std::unexpected(*writeError);
        return FastCache::CasToken { 1 };
    }

    std::expected<FastCache::GetResult, FastCache::StorageError> Get(std::string_view /*key*/,
                                                                     FastCache::TimePoint /*now*/) override
    {
        return FastCache::GetResult {};
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Set(std::string_view /*key*/,
                                                                    std::vector<std::byte> /*value*/,
                                                                    std::uint32_t /*flags*/,
                                                                    FastCache::TimePoint /*expiry*/) override
    {
        return WriteResult();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Add(std::string_view /*key*/,
                                                                    std::vector<std::byte> /*value*/,
                                                                    std::uint32_t /*flags*/,
                                                                    FastCache::TimePoint /*expiry*/,
                                                                    FastCache::TimePoint /*now*/) override
    {
        return WriteResult();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Replace(std::string_view /*key*/,
                                                                        std::vector<std::byte> /*value*/,
                                                                        std::uint32_t /*flags*/,
                                                                        FastCache::TimePoint /*expiry*/,
                                                                        FastCache::TimePoint /*now*/) override
    {
        return WriteResult();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Append(std::string_view /*key*/,
                                                                       std::span<std::byte const> /*suffix*/,
                                                                       FastCache::CasToken /*expected*/,
                                                                       FastCache::TimePoint /*now*/) override
    {
        return WriteResult();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Prepend(std::string_view /*key*/,
                                                                        std::span<std::byte const> /*prefix*/,
                                                                        FastCache::CasToken /*expected*/,
                                                                        FastCache::TimePoint /*now*/) override
    {
        return WriteResult();
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> CompareAndSwap(std::string_view /*key*/,
                                                                               FastCache::CasToken /*expected*/,
                                                                               std::vector<std::byte> /*value*/,
                                                                               std::uint32_t /*flags*/,
                                                                               FastCache::TimePoint /*expiry*/,
                                                                               FastCache::TimePoint /*now*/) override
    {
        return WriteResult();
    }
    std::expected<IStorage::IncrResult, FastCache::StorageError> IncrementOrInitialize(std::string_view /*key*/,
                                                                                       std::uint64_t /*magnitude*/,
                                                                                       bool /*decrement*/,
                                                                                       FastCache::TimePoint /*now*/) override
    {
        if (writeError.has_value())
            return std::unexpected(*writeError);
        return IStorage::IncrResult { .value = 0, .cas = FastCache::CasToken { 1 } };
    }
    std::expected<void, FastCache::StorageError> Delete(std::string_view /*key*/, FastCache::TimePoint /*now*/) override
    {
        return {};
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> Touch(std::string_view /*key*/,
                                                                      FastCache::TimePoint /*newExpiry*/,
                                                                      FastCache::TimePoint /*now*/) override
    {
        return FastCache::CasToken { 1 };
    }
    std::expected<FastCache::GetResult, FastCache::StorageError> Peek(std::string_view /*key*/,
                                                                      FastCache::TimePoint /*now*/) override
    {
        return FastCache::GetResult {};
    }
    std::expected<FastCache::CasToken, FastCache::StorageError> MarkStale(std::string_view /*key*/,
                                                                          std::optional<FastCache::TimePoint> /*newExpiry*/,
                                                                          FastCache::TimePoint /*now*/) override
    {
        return FastCache::CasToken { 1 };
    }
    void FlushWithGeneration(FastCache::TimePoint /*effectiveAt*/) override {}
    std::size_t PurgeExpired(FastCache::TimePoint /*now*/) override
    {
        return 0;
    }
    void Resize(std::size_t /*newMaxBytes*/) override {}
    [[nodiscard]] FastCache::StorageStats Snapshot() const noexcept override
    {
        return stats;
    }
};

/// @return True when `records` contains a line at `level` whose message holds
///         every fragment in `needles`.
[[nodiscard]] bool HasLine(std::vector<FastCache::CapturingLogger::Record> const& records,
                           FastCache::LogLevel level,
                           std::initializer_list<std::string_view> needles)
{
    return std::ranges::any_of(records, [&](FastCache::CapturingLogger::Record const& r) {
        return r.level == level && std::ranges::all_of(needles, [&](std::string_view n) { return r.message.contains(n); });
    });
}

} // namespace

TEST_CASE("WriteErrorReportingStorage logs and counts a persistence failure", "[cache][write-errors]")
{
    StubStorage inner;
    FastCache::CapturingLogger logger;
    FastCache::WriteErrorReportingStorage reporter { inner, logger };

    // A full disk surfaces as IoError with the OS errno (28 == ENOSPC).
    inner.writeError = FastCache::StorageError { .code = FastCache::StorageErrorCode::IoError,
                                                 .systemCode = 28,
                                                 .context = "no space left on device" };
    auto const result = reporter.Set("obj", std::vector<std::byte> { std::byte { 'x' } }, 0, FastCache::TimePoint::max());

    REQUIRE_FALSE(result.has_value());
    REQUIRE(reporter.Snapshot().writeErrors == 1);
    auto const records = logger.Snapshot();
    // Visible at the default level (Warn), and carries the actionable detail:
    // verb, key, error code, and the OS errno / context for a disk-full diagnosis.
    REQUIRE(HasLine(records,
                    FastCache::LogLevel::Warn,
                    { "storage write failed", "SET", "key=obj", "IoError", "system=28", "no space left on device" }));
}

TEST_CASE("WriteErrorReportingStorage ignores benign conditional-write outcomes", "[cache][write-errors]")
{
    StubStorage inner;
    FastCache::CapturingLogger logger;
    FastCache::WriteErrorReportingStorage reporter { inner, logger };

    // ADD onto an existing key (KeyExists) and CAS on a stale token (CasMismatch)
    // are normal control flow, not persistence failures: neither counts nor logs.
    inner.writeError = FastCache::MakeStorageError(FastCache::StorageErrorCode::KeyExists);
    REQUIRE_FALSE(reporter.Add("k", {}, 0, FastCache::TimePoint::max(), FastCache::TimePoint {}).has_value());

    inner.writeError = FastCache::MakeStorageError(FastCache::StorageErrorCode::CasMismatch);
    REQUIRE_FALSE(reporter.CompareAndSwap("k", 7, {}, 0, FastCache::TimePoint::max(), FastCache::TimePoint {}).has_value());

    // ValueTooLarge is a client-side rejection, already visible via the reply;
    // it is deliberately not treated as a persistence failure either.
    inner.writeError = FastCache::MakeStorageError(FastCache::StorageErrorCode::ValueTooLarge);
    REQUIRE_FALSE(reporter.Set("k", {}, 0, FastCache::TimePoint::max()).has_value());

    REQUIRE(reporter.Snapshot().writeErrors == 0);
    REQUIRE(logger.Snapshot().empty());
}

TEST_CASE("WriteErrorReportingStorage is silent on a successful write and forwards Snapshot", "[cache][write-errors]")
{
    StubStorage inner;
    inner.stats.itemCount = 42; // sentinel to prove inner fields are forwarded verbatim
    FastCache::CapturingLogger logger;
    FastCache::WriteErrorReportingStorage reporter { inner, logger };

    REQUIRE(reporter.Set("k", std::vector<std::byte> { std::byte { 'v' } }, 0, FastCache::TimePoint::max()).has_value());

    auto const snapshot = reporter.Snapshot();
    REQUIRE(snapshot.writeErrors == 0);
    REQUIRE(snapshot.itemCount == 42);
    REQUIRE(logger.Snapshot().empty());
}

TEST_CASE("WriteErrorReportingStorage counts persistence failures across write verbs", "[cache][write-errors]")
{
    StubStorage inner;
    FastCache::CapturingLogger logger;
    FastCache::WriteErrorReportingStorage reporter { inner, logger };
    inner.writeError = FastCache::MakeStorageError(FastCache::StorageErrorCode::OutOfMemory);

    static_cast<void>(reporter.Set("k", {}, 0, FastCache::TimePoint::max()));
    static_cast<void>(reporter.Append("k", {}, 0, FastCache::TimePoint {}));
    static_cast<void>(reporter.IncrementOrInitialize("k", 1, /*decrement*/ false, FastCache::TimePoint {}));

    // One count per failed write, regardless of verb.
    REQUIRE(reporter.Snapshot().writeErrors == 3);
    auto const records = logger.Snapshot();
    REQUIRE(HasLine(records, FastCache::LogLevel::Warn, { "APPEND", "OutOfMemory" }));
    REQUIRE(HasLine(records, FastCache::LogLevel::Warn, { "INCR", "OutOfMemory" }));
}
