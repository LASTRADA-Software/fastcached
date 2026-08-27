// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/WriteErrorReportingStorage.hpp>

#include <atomic>
#include <cstddef>
#include <utility>

namespace FastCache
{

namespace
{

    /// Whether a storage error code represents a failure to *persist* a write —
    /// the class of outcome an operator needs to see — as opposed to a benign
    /// conditional-write result or a client-side rejection.
    ///
    /// Persistence failures: `IoError` (full disk / device failure — the disk's
    /// `systemCode` distinguishes ENOSPC), `OutOfMemory` (budget exhausted after
    /// eviction), `Corrupt` (on-disk record failed verification), `ReadOnly`
    /// (storage refuses writes). Everything else — KeyExists/KeyNotFound/
    /// CasMismatch (normal conditional-write control flow), ValueTooLarge (the
    /// client sent an over-cap value), and the numeric/type codes — is not a
    /// persistence failure.
    /// @param code The storage error code.
    /// @return True when the code means the write could not be persisted.
    [[nodiscard]] constexpr bool IsPersistenceFailure(StorageErrorCode code) noexcept
    {
        switch (code)
        {
            case StorageErrorCode::IoError:
            case StorageErrorCode::OutOfMemory:
            case StorageErrorCode::Corrupt:
            case StorageErrorCode::ReadOnly:
                return true;
            default:
                return false;
        }
    }

} // namespace

WriteErrorReportingStorage::WriteErrorReportingStorage(IStorage& inner, ILogger& logger) noexcept:
    _inner { inner },
    _logger { logger }
{
}

void WriteErrorReportingStorage::ReportWriteFailure(std::string_view verb,
                                                    std::string_view key,
                                                    StorageError const& error) noexcept
{
    if (!IsPersistenceFailure(error.code))
        return;
    _writeErrors.fetch_add(1, std::memory_order_relaxed);
    // Warn (not Trace) so a disk-full write is visible at the default log level,
    // and (not Error) because the daemon keeps serving — the single write is
    // lost but the process recovers. systemCode carries the OS errno (e.g. 28 =
    // ENOSPC), which is the actionable detail for a disk-full diagnosis.
    _logger.Logf(LogLevel::Warn,
                 "storage write failed: {} key={} error={} system={} ({})",
                 verb,
                 key,
                 ToStringView(error.code),
                 error.systemCode,
                 error.context);
}

std::expected<GetResult, StorageError> WriteErrorReportingStorage::Get(std::string_view key, TimePoint now)
{
    return _inner.Get(key, now);
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::Set(std::string_view key,
                                                                      std::vector<std::byte> value,
                                                                      std::uint32_t flags,
                                                                      TimePoint expiry)
{
    auto result = _inner.Set(key, std::move(value), flags, expiry);
    if (!result.has_value())
        ReportWriteFailure("SET", key, result.error());
    return result;
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto result = _inner.Add(key, std::move(value), flags, expiry, now);
    if (!result.has_value())
        ReportWriteFailure("ADD", key, result.error());
    return result;
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto result = _inner.Replace(key, std::move(value), flags, expiry, now);
    if (!result.has_value())
        ReportWriteFailure("REPLACE", key, result.error());
    return result;
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::Append(std::string_view key,
                                                                         std::span<std::byte const> suffix,
                                                                         CasToken expected,
                                                                         TimePoint now)
{
    auto result = _inner.Append(key, suffix, expected, now);
    if (!result.has_value())
        ReportWriteFailure("APPEND", key, result.error());
    return result;
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::Prepend(std::string_view key,
                                                                          std::span<std::byte const> prefix,
                                                                          CasToken expected,
                                                                          TimePoint now)
{
    auto result = _inner.Prepend(key, prefix, expected, now);
    if (!result.has_value())
        ReportWriteFailure("PREPEND", key, result.error());
    return result;
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::CompareAndSwap(std::string_view key,
                                                                                 CasToken expected,
                                                                                 std::vector<std::byte> value,
                                                                                 std::uint32_t flags,
                                                                                 TimePoint expiry,
                                                                                 TimePoint now)
{
    auto result = _inner.CompareAndSwap(key, expected, std::move(value), flags, expiry, now);
    if (!result.has_value())
        ReportWriteFailure("CAS", key, result.error());
    return result;
}

std::expected<IStorage::IncrResult, StorageError> WriteErrorReportingStorage::IncrementOrInitialize(std::string_view key,
                                                                                                    std::uint64_t magnitude,
                                                                                                    bool decrement,
                                                                                                    TimePoint now)
{
    auto result = _inner.IncrementOrInitialize(key, magnitude, decrement, now);
    if (!result.has_value())
        ReportWriteFailure(decrement ? "DECR" : "INCR", key, result.error());
    return result;
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::Update(
    std::string_view key,
    std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
    TimePoint now)
{
    auto result = _inner.Update(key, fn, now);
    if (!result.has_value())
        ReportWriteFailure("UPDATE", key, result.error());
    return result;
}

std::expected<void, StorageError> WriteErrorReportingStorage::Delete(std::string_view key, TimePoint now)
{
    return _inner.Delete(key, now);
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::Touch(std::string_view key,
                                                                        TimePoint newExpiry,
                                                                        TimePoint now)
{
    return _inner.Touch(key, newExpiry, now);
}

std::expected<GetResult, StorageError> WriteErrorReportingStorage::Peek(std::string_view key, TimePoint now)
{
    return _inner.Peek(key, now);
}

std::expected<std::optional<TimePoint>, StorageError> WriteErrorReportingStorage::PeekExpiry(std::string_view key,
                                                                                             TimePoint now)
{
    return _inner.PeekExpiry(key, now);
}
std::expected<bool, StorageError> WriteErrorReportingStorage::Prefetch(std::string_view key, TimePoint now)
{
    return _inner.Prefetch(key, now);
}

std::expected<CasToken, StorageError> WriteErrorReportingStorage::MarkStale(std::string_view key,
                                                                            std::optional<TimePoint> newExpiry,
                                                                            TimePoint now)
{
    return _inner.MarkStale(key, newExpiry, now);
}

std::expected<GetResult, StorageError> WriteErrorReportingStorage::GetAndTouch(std::string_view key,
                                                                               TimePoint newExpiry,
                                                                               TimePoint now)
{
    return _inner.GetAndTouch(key, newExpiry, now);
}

std::expected<void, StorageError> WriteErrorReportingStorage::CompareAndDelete(std::string_view key,
                                                                               CasToken expected,
                                                                               TimePoint now)
{
    return _inner.CompareAndDelete(key, expected, now);
}

std::expected<bool, StorageError> WriteErrorReportingStorage::ClearExpiry(std::string_view key, TimePoint now)
{
    return _inner.ClearExpiry(key, now);
}

void WriteErrorReportingStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    _inner.FlushWithGeneration(effectiveAt);
}

PurgeOutcome WriteErrorReportingStorage::PurgeExpired(TimePoint now, PurgeBudget budget)
{
    return _inner.PurgeExpired(now, budget);
}

void WriteErrorReportingStorage::SetReclaimLog(IReclaimLog* log)
{
    _inner.SetReclaimLog(log);
}

void WriteErrorReportingStorage::Resize(std::size_t newMaxBytes)
{
    _inner.Resize(newMaxBytes);
}

StorageStats WriteErrorReportingStorage::Snapshot() const noexcept
{
    auto stats = _inner.Snapshot();
    stats.writeErrors = _writeErrors.load(std::memory_order_relaxed);
    return stats;
}

TieredStorageStats WriteErrorReportingStorage::SnapshotTiers() const noexcept
{
    return _inner.SnapshotTiers();
}

} // namespace FastCache
