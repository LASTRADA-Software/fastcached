// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Command-level facade over IStorage. Owns the TTL semantics (translating
/// memcached's seconds-or-unix-timestamp `exptime` into absolute TimePoints
/// via IClock) and the encoding-specific conversions. The protocol handlers
/// call methods on CacheEngine; they never call IStorage directly.
class CacheEngine
{
  public:
    /// Construct over an IStorage and IClock; both must outlive the engine.
    /// @param storage Backing storage.
    /// @param clock Time provider.
    CacheEngine(IStorage& storage, IClock& clock) noexcept;

    // -- memcached-flavoured operations -------------------------------------

    /// memcached `exptime` semantics:
    ///   0           -> never expires (TimePoint::max())
    ///   1..2592000  -> relative seconds from now
    ///   >2592000    -> absolute UNIX timestamp (we approximate by using
    ///                 monotonic now + (timestamp - approx-wall-now), good
    ///                 enough for sccache cache TTLs)
    /// @param exptime Wire-format expiry word.
    /// @return Absolute steady-clock deadline.
    [[nodiscard]] TimePoint ExpiryFromExptime(std::uint32_t exptime) const noexcept;

    [[nodiscard]] std::expected<GetResult, StorageError> Get(std::string_view key);

    [[nodiscard]] std::expected<CasToken, StorageError>
    Set(std::string_view key, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime);

    [[nodiscard]] std::expected<CasToken, StorageError>
    Add(std::string_view key, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime);

    [[nodiscard]] std::expected<CasToken, StorageError>
    Replace(std::string_view key, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime);

    [[nodiscard]] std::expected<CasToken, StorageError> Append(std::string_view key, std::span<std::byte const> suffix);
    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key, std::span<std::byte const> prefix);

    [[nodiscard]] std::expected<CasToken, StorageError>
    CompareAndSwap(std::string_view key,
                   CasToken expected,
                   std::vector<std::byte> value,
                   std::uint32_t flags,
                   std::uint32_t exptime);

    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> Increment(std::string_view key, std::uint64_t delta);
    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> Decrement(std::string_view key, std::uint64_t delta);

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key);

    /// Flush all entries; `delaySeconds == 0` is immediate.
    void FlushAll(std::uint32_t delaySeconds);

    [[nodiscard]] StorageStats Snapshot() const noexcept { return _storage.Snapshot(); }

    [[nodiscard]] IClock& Clock() noexcept { return _clock; }

  private:
    IStorage& _storage;
    IClock& _clock;
};

} // namespace FastCache
