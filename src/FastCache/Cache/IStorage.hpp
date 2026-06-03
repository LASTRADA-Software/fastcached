// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Storage statistics surfaced by the `stats` command.
///
/// All counters are monotonic from process start unless the operator
/// issues `stats reset`. Hit/miss counters split each command kind so
/// the operator can see, e.g. how many `touch` requests landed on a key
/// vs. found nothing.
struct StorageStats
{
    std::size_t itemCount { 0 };
    std::size_t bytesUsed { 0 };
    std::size_t bytesLimit { 0 };
    std::uint64_t evictions { 0 };

    std::uint64_t cmdGet { 0 };
    std::uint64_t cmdSet { 0 };
    std::uint64_t cmdTouch { 0 };
    std::uint64_t cmdFlush { 0 };

    std::uint64_t getHits { 0 };
    std::uint64_t getMisses { 0 };
    std::uint64_t deleteHits { 0 };
    std::uint64_t deleteMisses { 0 };
    std::uint64_t incrHits { 0 };
    std::uint64_t incrMisses { 0 };
    std::uint64_t decrHits { 0 };
    std::uint64_t decrMisses { 0 };
    std::uint64_t touchHits { 0 };
    std::uint64_t touchMisses { 0 };
    std::uint64_t casHits { 0 };
    std::uint64_t casMisses { 0 };
    std::uint64_t casBadval { 0 };

    std::uint64_t evictedUnfetched { 0 };
    std::uint64_t expiredUnfetched { 0 };
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
    /// @param key      Lookup key.
    /// @param suffix   Bytes to append.
    /// @param expected CAS precondition; when non-zero the append is applied
    ///                 only if the current entry's CAS equals it (meta
    ///                 `ms ... MA C(token)`), otherwise StorageError(CasMismatch).
    ///                 Pass 0 for an unconditional append.
    /// @param now      Current clock value (drives the existence check).
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Append(std::string_view key,
                                                                       std::span<std::byte const> suffix,
                                                                       CasToken expected,
                                                                       TimePoint now) = 0;

    /// Prepend `prefix` to the existing value at `key`. `expected` is the
    /// optional CAS precondition (0 = unconditional), as for `Append`.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                        std::span<std::byte const> prefix,
                                                                        CasToken expected,
                                                                        TimePoint now) = 0;

    /// Compare expected CAS against the current entry's CAS and replace the
    /// value if they match. Yields StorageError(CasMismatch) on mismatch.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> CompareAndSwap(std::string_view key,
                                                                               CasToken expected,
                                                                               std::vector<std::byte> value,
                                                                               std::uint32_t flags,
                                                                               TimePoint expiry,
                                                                               TimePoint now) = 0;

    /// Treat the existing value as an ASCII unsigned integer and add or
    /// subtract `magnitude`. memcached semantics: increment wraps modulo
    /// 2^64, decrement saturates at 0. The amount is a full `std::uint64_t`
    /// on purpose — a signed delta could not represent magnitudes >= 2^63
    /// (incr would alias to a decrement and `decr` by 2^63 would be UB).
    struct IncrResult
    {
        std::uint64_t value;
        CasToken cas;
    };
    /// @param key       Lookup key.
    /// @param magnitude Unsigned amount to add, or to subtract when `decrement`.
    /// @param decrement When true, subtract (saturating at 0); else add (mod 2^64).
    /// @param now       Current clock value (drives the existence check).
    /// @return New value and CAS token, or StorageError(KeyNotFound).
    [[nodiscard]] virtual std::expected<IncrResult, StorageError> IncrementOrInitialize(std::string_view key,
                                                                                        std::uint64_t magnitude,
                                                                                        bool decrement,
                                                                                        TimePoint now) = 0;

    /// Delete the entry. Returns StorageError(KeyNotFound) if no entry exists.
    [[nodiscard]] virtual std::expected<void, StorageError> Delete(std::string_view key, TimePoint now) = 0;

    /// Refresh the entry's expiry without rewriting its value. Bumps CAS.
    /// Returns StorageError(KeyNotFound) if no live entry exists.
    /// @param key       Lookup key.
    /// @param newExpiry Absolute new expiry deadline; TimePoint::max() = never.
    /// @param now       Current clock value (drives existence check).
    /// @return New CAS token, or StorageError(KeyNotFound).
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Touch(std::string_view key,
                                                                      TimePoint newExpiry,
                                                                      TimePoint now) = 0;

    /// Non-mutating lookup. Like `Get`, but does **not** update the
    /// entry's `lastAccess`, promote it in the LRU, or bump hit/miss
    /// statistics. Used for internal bookkeeping reads (e.g. a write-
    /// through tier refreshing its mirror) where treating the read as a
    /// client access would corrupt observable state.
    /// @param key Lookup key.
    /// @param now Current clock value (drives the TTL existence check).
    /// @return GetResult, or StorageError on I/O failure.
    [[nodiscard]] virtual std::expected<GetResult, StorageError> Peek(std::string_view key, TimePoint now) = 0;

    /// Mark the live entry under `key` stale without removing it (the meta
    /// `md I` / `ms I` flags). The entry remains readable and a reader sees
    /// the `X` response flag; CAS is bumped. Optionally refresh the expiry
    /// at the same time (the `md I T(token)` combination).
    /// @param key       Lookup key.
    /// @param newExpiry New absolute expiry, or `std::nullopt` to leave it.
    /// @param now       Current clock value (drives the existence check).
    /// @return New CAS token, or StorageError(KeyNotFound) if absent.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                          std::optional<TimePoint> newExpiry,
                                                                          TimePoint now) = 0;

    /// Atomically refresh `key`'s expiry and return the resulting entry
    /// (memcached's get-and-touch). Performing the touch and the read as a
    /// single critical section closes the TOCTOU window that composing a
    /// separate `Touch` + `Get` would open under concurrent writers.
    ///
    /// The default implementation composes `Touch` + `Get`; storage
    /// decorators that own the lock (e.g. `ShardedStorage`) override it to
    /// hold the lock across both steps.
    /// @param key       Lookup key.
    /// @param newExpiry New absolute expiry deadline.
    /// @param now       Current clock value.
    /// @return The refreshed GetResult, or StorageError(KeyNotFound) on miss.
    [[nodiscard]] virtual std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                             TimePoint newExpiry,
                                                                             TimePoint now)
    {
        auto const touched = Touch(key, newExpiry, now);
        if (!touched.has_value())
            return std::unexpected(touched.error());
        return Get(key, now);
    }

    /// Atomically delete `key` only if its current CAS equals `expected`
    /// (memcached meta `md C(token)`). Checking and deleting under a single
    /// critical section prevents a concurrent writer from slipping a new
    /// value in between the compare and the delete.
    ///
    /// The default implementation composes `Get` + `Delete`; lock-owning
    /// decorators override it to span both steps atomically.
    /// @param key      Lookup key.
    /// @param expected CAS token the caller believes is current.
    /// @param now      Current clock value.
    /// @return Empty on success, StorageError(KeyNotFound) if absent, or
    ///         StorageError(CasMismatch) if the CAS differs.
    [[nodiscard]] virtual std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                             CasToken expected,
                                                                             TimePoint now)
    {
        auto const got = Peek(key, now);
        if (!got.has_value())
            return std::unexpected(got.error());
        if (!got->found)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        if (got->entry.cas != expected)
            return std::unexpected(MakeStorageError(StorageErrorCode::CasMismatch));
        return Delete(key, now);
    }

    /// Increment the storage's "live" generation so all existing entries
    /// become invisible. Optional `effectiveAt` lets memcached's
    /// `flush_all <delay>` schedule a delayed flush — entries inserted
    /// before effectiveAt are dropped lazily once `now >= effectiveAt`.
    virtual void FlushWithGeneration(TimePoint effectiveAt) = 0;

    /// Purge any entries whose expiry has passed. Returns the number purged.
    virtual std::size_t PurgeExpired(TimePoint now) = 0;

    /// Reconfigure the byte budget at runtime (e.g. on SIGHUP reload).
    /// Budget-owning backends evict to fit; forwarding decorators pass it
    /// on; composite backends split it across their inner storages.
    /// @param newMaxBytes New byte budget. Composite backends split or
    ///        forward this across their inner storages.
    virtual void Resize(std::size_t newMaxBytes) = 0;

    /// @return Current storage statistics.
    [[nodiscard]] virtual StorageStats Snapshot() const noexcept = 0;
};

} // namespace FastCache
