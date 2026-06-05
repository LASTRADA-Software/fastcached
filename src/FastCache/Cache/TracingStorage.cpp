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
            {
                if (r.error().code == StorageErrorCode::ValueTooLarge)
                    return std::format("VALUE_TOO_LARGE bytes={}", bytes);
                return std::string { ErrorOutcome(r.error()) };
            }
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
            {
                if (r.error().code == StorageErrorCode::ValueTooLarge)
                    return std::format("VALUE_TOO_LARGE bytes={}", bytes);
                return std::string { ErrorOutcome(r.error()) };
            }
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
            {
                if (r.error().code == StorageErrorCode::ValueTooLarge)
                    return std::format("VALUE_TOO_LARGE bytes={}", bytes);
                return std::string { ErrorOutcome(r.error()) };
            }
            return std::format("STORED bytes={}", bytes);
        });
}

std::expected<CasToken, StorageError> TracingStorage::Append(std::string_view key,
                                                             std::span<std::byte const> suffix,
                                                             CasToken expected,
                                                             TimePoint now)
{
    auto const bytes = suffix.size();
    return TraceCall(
        "APPEND",
        key,
        [&] { return _inner.Append(key, suffix, expected, now); },
        [bytes](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
            {
                if (r.error().code == StorageErrorCode::ValueTooLarge)
                    return std::format("VALUE_TOO_LARGE bytes={}", bytes);
                return std::string { ErrorOutcome(r.error()) };
            }
            return "STORED";
        });
}

std::expected<CasToken, StorageError> TracingStorage::Prepend(std::string_view key,
                                                              std::span<std::byte const> prefix,
                                                              CasToken expected,
                                                              TimePoint now)
{
    auto const bytes = prefix.size();
    return TraceCall(
        "PREPEND",
        key,
        [&] { return _inner.Prepend(key, prefix, expected, now); },
        [bytes](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
            {
                if (r.error().code == StorageErrorCode::ValueTooLarge)
                    return std::format("VALUE_TOO_LARGE bytes={}", bytes);
                return std::string { ErrorOutcome(r.error()) };
            }
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
    auto const bytes = value.size();
    return TraceCall(
        "CAS",
        key,
        [&]() mutable { return _inner.CompareAndSwap(key, expected, std::move(value), flags, expiry, now); },
        [bytes](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
            {
                if (r.error().code == StorageErrorCode::ValueTooLarge)
                    return std::format("VALUE_TOO_LARGE bytes={}", bytes);
                return std::string { ErrorOutcome(r.error()) };
            }
            return "STORED";
        });
}

std::expected<IStorage::IncrResult, StorageError> TracingStorage::IncrementOrInitialize(std::string_view key,
                                                                                        std::uint64_t magnitude,
                                                                                        bool decrement,
                                                                                        TimePoint now)
{
    return TraceCall(
        decrement ? "DECR" : "INCR",
        key,
        [&] { return _inner.IncrementOrInitialize(key, magnitude, decrement, now); },
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

std::expected<CasToken, StorageError> TracingStorage::Touch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    return TraceCall(
        "TOUCH",
        key,
        [&] { return _inner.Touch(key, newExpiry, now); },
        [](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return "TOUCHED";
        });
}

std::expected<GetResult, StorageError> TracingStorage::Peek(std::string_view key, TimePoint now)
{
    return TraceCall(
        "PEEK",
        key,
        [&] { return _inner.Peek(key, now); },
        [](std::expected<GetResult, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            if (r->found)
                return std::format("HIT bytes={}", r->entry.value.size());
            return "MISS";
        });
}

std::expected<CasToken, StorageError> TracingStorage::MarkStale(std::string_view key,
                                                                std::optional<TimePoint> newExpiry,
                                                                TimePoint now)
{
    return TraceCall(
        "MARK_STALE",
        key,
        [&] { return _inner.MarkStale(key, newExpiry, now); },
        [](std::expected<CasToken, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            return "STALE";
        });
}

std::expected<GetResult, StorageError> TracingStorage::GetAndTouch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    return TraceCall(
        "GAT",
        key,
        [&] { return _inner.GetAndTouch(key, newExpiry, now); },
        [](std::expected<GetResult, StorageError> const& r) -> std::string {
            if (!r.has_value())
                return std::string { ErrorOutcome(r.error()) };
            if (r->found)
                return std::format("HIT bytes={}", r->entry.value.size());
            return "MISS";
        });
}

std::expected<void, StorageError> TracingStorage::CompareAndDelete(std::string_view key, CasToken expected, TimePoint now)
{
    return TraceCall(
        "CAD",
        key,
        [&] { return _inner.CompareAndDelete(key, expected, now); },
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
