// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>

#include <chrono>
#include <cstdint>
#include <span>
#include <utility>

namespace FastCache
{

namespace
{

    /// memcached's threshold: any exptime above 30 days is interpreted as a
    /// UNIX timestamp.
    constexpr std::uint32_t kExptimeAbsoluteThreshold = 60u * 60u * 24u * 30u;

} // namespace

CacheEngine::CacheEngine(IStorage& storage, IClock& clock) noexcept: _storage { storage }, _clock { clock } {}

TimePoint CacheEngine::ExpiryFromExptime(std::uint32_t exptime) const noexcept
{
    if (exptime == 0)
        return TimePoint::max();

    auto const now = _clock.Now();
    if (exptime <= kExptimeAbsoluteThreshold)
        return now + std::chrono::seconds { exptime };

    // Absolute UNIX timestamp. We approximate "now in unix seconds" by
    // using the system clock once at the call site — this drifts vs. the
    // steady clock but is acceptable for the sccache use case where TTLs
    // are typically short. (For accurate absolute-time TTLs we'd need an
    // IWallClock injected alongside IClock.)
    auto const sysNow = std::chrono::system_clock::now();
    auto const sysSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(sysNow.time_since_epoch()).count();
    auto const delta = static_cast<std::int64_t>(exptime) - sysSeconds;
    if (delta <= 0)
        return now;
    return now + std::chrono::seconds { delta };
}

std::expected<GetResult, StorageError> CacheEngine::Get(std::string_view key)
{
    return _storage.Get(key, _clock.Now());
}

std::expected<CasToken, StorageError>
CacheEngine::Set(std::string_view key, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime)
{
    return _storage.Set(key, std::move(value), flags, ExpiryFromExptime(exptime));
}

std::expected<CasToken, StorageError>
CacheEngine::Add(std::string_view key, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime)
{
    return _storage.Add(key, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<CasToken, StorageError>
CacheEngine::Replace(std::string_view key, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime)
{
    return _storage.Replace(key, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Append(std::string_view key, std::span<std::byte const> suffix)
{
    return _storage.Append(key, suffix, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Prepend(std::string_view key, std::span<std::byte const> prefix)
{
    return _storage.Prepend(key, prefix, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::CompareAndSwap(std::string_view key,
                                                                   CasToken expected,
                                                                   std::vector<std::byte> value,
                                                                   std::uint32_t flags,
                                                                   std::uint32_t exptime)
{
    return _storage.CompareAndSwap(key, expected, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<IStorage::IncrResult, StorageError> CacheEngine::Increment(std::string_view key, std::uint64_t delta)
{
    return _storage.IncrementOrInitialize(key, static_cast<std::int64_t>(delta), _clock.Now());
}

std::expected<IStorage::IncrResult, StorageError> CacheEngine::Decrement(std::string_view key, std::uint64_t delta)
{
    auto const signedDelta = -static_cast<std::int64_t>(delta);
    return _storage.IncrementOrInitialize(key, signedDelta, _clock.Now());
}

std::expected<void, StorageError> CacheEngine::Delete(std::string_view key)
{
    return _storage.Delete(key, _clock.Now());
}

void CacheEngine::FlushAll(std::uint32_t delaySeconds)
{
    auto const effectiveAt =
        delaySeconds == 0 ? _clock.Now() : _clock.Now() + std::chrono::seconds { delaySeconds };
    _storage.FlushWithGeneration(effectiveAt);
}

} // namespace FastCache
