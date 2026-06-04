// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace FastCache
{

namespace
{

    /// memcached's threshold: any exptime above 30 days is interpreted as a
    /// UNIX timestamp.
    constexpr std::uint32_t ExptimeAbsoluteThreshold = 60U * 60U * 24U * 30U;

} // namespace

CacheEngine::CacheEngine(IStorage& storage, IClock& clock) noexcept:
    _storage { storage },
    _clock { clock }
{
}

TimePoint CacheEngine::ExpiryFromExptime(std::uint32_t exptime) const noexcept
{
    if (exptime == 0)
        return TimePoint::max();

    auto const now = _clock.Now();
    if (exptime <= ExptimeAbsoluteThreshold)
        return now + std::chrono::seconds { exptime };

    // Absolute UNIX timestamp. We approximate "now in unix seconds" by
    // using the system clock once at the call site — this drifts vs. the
    // steady clock but is acceptable for the sccache use case where TTLs
    // are typically short. (For accurate absolute-time TTLs we'd need an
    // IWallClock injected alongside IClock.)
    auto const sysNow = std::chrono::system_clock::now();
    auto const sysSeconds = std::chrono::duration_cast<std::chrono::seconds>(sysNow.time_since_epoch()).count();
    auto const delta = static_cast<std::int64_t>(exptime) - sysSeconds;
    if (delta <= 0)
        return now;
    return now + std::chrono::seconds { delta };
}

std::expected<GetResult, StorageError> CacheEngine::Get(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::Get");
    return _storage.Get(key, _clock.Now());
}

std::expected<GetResult, StorageError> CacheEngine::Peek(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::Peek");
    return _storage.Peek(key, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Set(std::string_view key,
                                                       std::vector<std::byte> value,
                                                       std::uint32_t flags,
                                                       std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Set");
    return _storage.Set(key, std::move(value), flags, ExpiryFromExptime(exptime));
}

std::expected<CasToken, StorageError> CacheEngine::Add(std::string_view key,
                                                       std::vector<std::byte> value,
                                                       std::uint32_t flags,
                                                       std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Add");
    return _storage.Add(key, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Replace(std::string_view key,
                                                           std::vector<std::byte> value,
                                                           std::uint32_t flags,
                                                           std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Replace");
    return _storage.Replace(key, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Append(std::string_view key,
                                                          std::span<std::byte const> suffix,
                                                          CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::Append");
    return _storage.Append(key, suffix, expected, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Prepend(std::string_view key,
                                                           std::span<std::byte const> prefix,
                                                           CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::Prepend");
    return _storage.Prepend(key, prefix, expected, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::CompareAndSwap(
    std::string_view key, CasToken expected, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::CompareAndSwap");
    return _storage.CompareAndSwap(key, expected, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<IStorage::IncrResult, StorageError> CacheEngine::Increment(std::string_view key, std::uint64_t delta)
{
    FC_ZONE_SCOPED_N("CacheEngine::Increment");
    // The full uint64 delta is forwarded verbatim — memcached increments wrap
    // modulo 2^64, and a signed cast would alias deltas >= 2^63 to decrements.
    return _storage.IncrementOrInitialize(key, delta, /*decrement=*/false, _clock.Now());
}

std::expected<IStorage::IncrResult, StorageError> CacheEngine::Decrement(std::string_view key, std::uint64_t delta)
{
    FC_ZONE_SCOPED_N("CacheEngine::Decrement");
    // Pass the magnitude + direction rather than a negated signed delta:
    // negating INT64_MIN (delta == 2^63) would be signed-overflow UB.
    return _storage.IncrementOrInitialize(key, delta, /*decrement=*/true, _clock.Now());
}

std::expected<void, StorageError> CacheEngine::Delete(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::Delete");
    return _storage.Delete(key, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Touch(std::string_view key, std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Touch");
    return _storage.Touch(key, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<GetResult, StorageError> CacheEngine::GetAndTouch(std::string_view key, std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::GetAndTouch");
    return _storage.GetAndTouch(key, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<void, StorageError> CacheEngine::CompareAndDelete(std::string_view key, CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::CompareAndDelete");
    return _storage.CompareAndDelete(key, expected, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::MarkStale(std::string_view key, std::optional<std::uint32_t> newExptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::MarkStale");
    std::optional<TimePoint> newExpiry;
    if (newExptime.has_value())
        newExpiry = ExpiryFromExptime(*newExptime);
    return _storage.MarkStale(key, newExpiry, _clock.Now());
}

void CacheEngine::FlushAll(std::uint32_t delaySeconds)
{
    auto const effectiveAt = delaySeconds == 0 ? _clock.Now() : _clock.Now() + std::chrono::seconds { delaySeconds };
    _storage.FlushWithGeneration(effectiveAt);
}

} // namespace FastCache
