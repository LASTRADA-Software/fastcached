// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/TracingStorage.hpp>

#include <format>
#include <string>
#include <utility>

namespace FastCache
{

namespace
{

    /// Convert a storage-error code to the wire-shaped outcome string
    /// used by the trace log.
    [[nodiscard]] std::string_view ErrorOutcome(StorageError const& err) noexcept
    {
        switch (err.code)
        {
            case StorageErrorCode::KeyNotFound:
                return "NOT_FOUND";
            case StorageErrorCode::KeyExists:
                return "NOT_STORED";
            case StorageErrorCode::CasMismatch:
                return "EXISTS";
            case StorageErrorCode::ValueTooLarge:
                return "VALUE_TOO_LARGE";
            case StorageErrorCode::OutOfMemory:
                return "OUT_OF_MEMORY";
            case StorageErrorCode::Corrupt:
                return "CORRUPT";
            case StorageErrorCode::IoError:
                return "IO_ERROR";
            case StorageErrorCode::ReadOnly:
                return "READ_ONLY";
            case StorageErrorCode::InvalidArgument:
                return "INVALID_ARGUMENT";
            default:
                return "ERROR";
        }
    }

} // namespace

TracingStorage::TracingStorage(IStorage& inner, ILogger& logger, IClock& clock) noexcept:
    _inner { inner },
    _logger { logger },
    _clock { clock }
{
}

std::expected<GetResult, StorageError> TracingStorage::Get(std::string_view key, TimePoint now)
{
    return TraceCall(
        "GET",
        key,
        [&] { return _inner.Get(key, now); },
        [](std::expected<GetResult, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            if (r->found)
                return std::format("HIT bytes={}", r->entry.value.size());
            return "MISS";
        });
}

std::expected<CasToken, StorageError> TracingStorage::Set(std::string_view key,
                                                          std::vector<std::byte> value,
                                                          std::uint32_t flags,
                                                          TimePoint expiry)
{
    auto const bytes = value.size();
    return TraceCall(
        "SET",
        key,
        [&]() mutable { return _inner.Set(key, std::move(value), flags, expiry); },
        [bytes](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return std::format("STORED bytes={}", bytes);
        });
}

std::expected<CasToken, StorageError> TracingStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto const bytes = value.size();
    return TraceCall(
        "ADD",
        key,
        [&]() mutable { return _inner.Add(key, std::move(value), flags, expiry, now); },
        [bytes](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return std::format("STORED bytes={}", bytes);
        });
}

std::expected<CasToken, StorageError> TracingStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto const bytes = value.size();
    return TraceCall(
        "REPLACE",
        key,
        [&]() mutable { return _inner.Replace(key, std::move(value), flags, expiry, now); },
        [bytes](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return std::format("STORED bytes={}", bytes);
        });
}

std::expected<CasToken, StorageError> TracingStorage::Append(std::string_view key,
                                                             std::span<std::byte const> suffix,
                                                             TimePoint now)
{
    return TraceCall(
        "APPEND",
        key,
        [&] { return _inner.Append(key, suffix, now); },
        [](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return "STORED";
        });
}

std::expected<CasToken, StorageError> TracingStorage::Prepend(std::string_view key,
                                                              std::span<std::byte const> prefix,
                                                              TimePoint now)
{
    return TraceCall(
        "PREPEND",
        key,
        [&] { return _inner.Prepend(key, prefix, now); },
        [](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return "STORED";
        });
}

std::expected<CasToken, StorageError> TracingStorage::CompareAndSwap(std::string_view key,
                                                                     CasToken expected,
                                                                     std::vector<std::byte> value,
                                                                     std::uint32_t flags,
                                                                     TimePoint expiry,
                                                                     TimePoint now)
{
    return TraceCall(
        "CAS",
        key,
        [&]() mutable { return _inner.CompareAndSwap(key, expected, std::move(value), flags, expiry, now); },
        [](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return "STORED";
        });
}

std::expected<IStorage::IncrResult, StorageError> TracingStorage::IncrementOrInitialize(std::string_view key,
                                                                                        std::int64_t delta,
                                                                                        TimePoint now)
{
    return TraceCall(
        delta >= 0 ? "INCR" : "DECR",
        key,
        [&] { return _inner.IncrementOrInitialize(key, delta, now); },
        [](std::expected<IStorage::IncrResult, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return std::format("OK value={}", r->value);
        });
}

std::expected<void, StorageError> TracingStorage::Delete(std::string_view key, TimePoint now)
{
    return TraceCall(
        "DELETE",
        key,
        [&] { return _inner.Delete(key, now); },
        [](std::expected<void, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return "DELETED";
        });
}

void TracingStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    bool const traceOn = _logger.MinLevel() <= LogLevel::Trace;
    auto const startedAt = _clock.Now();
    _inner.FlushWithGeneration(effectiveAt);
    if (traceOn)
    {
        auto const took = _clock.Now() - startedAt;
        _logger.Logf(LogLevel::Trace,
                     "storage: FLUSH result=OK took={}us",
                     std::chrono::duration_cast<std::chrono::microseconds>(took).count());
    }
}

std::size_t TracingStorage::PurgeExpired(TimePoint now)
{
    return _inner.PurgeExpired(now);
}

void TracingStorage::Resize(std::size_t newMaxBytes)
{
    _inner.Resize(newMaxBytes);
}

StorageStats TracingStorage::Snapshot() const noexcept
{
    return _inner.Snapshot();
}

} // namespace FastCache
