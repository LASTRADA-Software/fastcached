// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Storage statistics surfaced by the `stats` command.
struct StorageStats
{
    std::size_t itemCount { 0 };
    std::size_t bytesUsed { 0 };
    std::size_t bytesLimit { 0 };
    std::uint64_t evictions { 0 };
    std::uint64_t getHits { 0 };
    std::uint64_t getMisses { 0 };
    std::uint64_t cmdGet { 0 };
    std::uint64_t cmdSet { 0 };
};

/// Storage backend abstraction. The cache engine routes every command
/// through these primitives. Implementations are responsible for honouring
/// the atomicity contract — each call is the atomicity boundary for the
/// keys it touches.
class IStorage
{
  public:
    IStorage() = default;
    IStorage(IStorage const&) = delete;
    IStorage(IStorage&&) = delete;
    IStorage& operator=(IStorage const&) = delete;
    IStorage& operator=(IStorage&&) = delete;
    virtual ~IStorage() = default;

    /// Look up the entry under `key`. Returns a found=true/false GetResult.
    /// Lazily purges expired entries (consults `now`).
    /// @param key Lookup key.
    /// @param now Current clock value (drives TTL expiry).
    /// @return GetResult, or StorageError on I/O failure.
    [[nodiscard]] virtual std::expected<GetResult, StorageError> Get(std::string_view key, TimePoint now) = 0;

    /// Unconditionally store `value` under `key`. Overwrites any existing
    /// entry. Issues a new CAS token and returns it.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Set(std::string_view key,
                                                                    std::vector<std::byte> value,
                                                                    std::uint32_t flags,
                                                                    TimePoint expiry) = 0;

    /// Store only if no value currently exists under `key`. Returns the new
    /// CAS token, or StorageError(KeyExists) if the key was present.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Add(
        std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now) = 0;

    /// Store only if a value currently exists under `key`. Returns the new
    /// CAS token, or StorageError(KeyNotFound).
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Replace(
        std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now) = 0;

    /// Append `suffix` to the existing value at `key`. Flags and expiry are
    /// preserved. CAS bumps.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Append(std::string_view key,
                                                                       std::span<std::byte const> suffix,
                                                                       TimePoint now) = 0;

    /// Prepend `prefix` to the existing value at `key`.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                        std::span<std::byte const> prefix,
                                                                        TimePoint now) = 0;

    /// Compare expected CAS against the current entry's CAS and replace the
    /// value if they match. Yields StorageError(CasMismatch) on mismatch.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> CompareAndSwap(std::string_view key,
                                                                               CasToken expected,
                                                                               std::vector<std::byte> value,
                                                                               std::uint32_t flags,
                                                                               TimePoint expiry,
                                                                               TimePoint now) = 0;

    /// Treat the existing value as an ASCII unsigned integer and add `delta`
    /// (which may be negative for decrement; semantics follow memcached:
    /// underflow saturates at 0). Returns the new value and CAS token.
    struct IncrResult
    {
        std::uint64_t value;
        CasToken cas;
    };
    [[nodiscard]] virtual std::expected<IncrResult, StorageError> IncrementOrInitialize(std::string_view key,
                                                                                        std::int64_t delta,
                                                                                        TimePoint now) = 0;

    /// Delete the entry. Returns StorageError(KeyNotFound) if no entry exists.
    [[nodiscard]] virtual std::expected<void, StorageError> Delete(std::string_view key, TimePoint now) = 0;

    /// Increment the storage's "live" generation so all existing entries
    /// become invisible. Optional `effectiveAt` lets memcached's
    /// `flush_all <delay>` schedule a delayed flush — entries inserted
    /// before effectiveAt are dropped lazily once `now >= effectiveAt`.
    virtual void FlushWithGeneration(TimePoint effectiveAt) = 0;

    /// Purge any entries whose expiry has passed. Returns the number purged.
    virtual std::size_t PurgeExpired(TimePoint now) = 0;

    /// @return Current storage statistics.
    [[nodiscard]] virtual StorageStats Snapshot() const noexcept = 0;
};

} // namespace FastCache
