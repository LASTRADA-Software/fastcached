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
#include <functional>
#include <optional>
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
    /// An optional `IWallClock&` injects wall-time access for the
    /// EXPIREAT/PEXPIREAT family and the memcached absolute-exptime
    /// translation. Production callers omit the third argument (defaults
    /// to `DefaultSystemWallClock()`); tests pass a ManualWallClock so
    /// the wire path through HandleExpire's absolute branch is
    /// deterministic.
    /// @param storage   Backing storage.
    /// @param clock     Monotonic clock for TTL/timeout semantics.
    /// @param wallClock Wall clock for absolute-UNIX-time translations.
    CacheEngine(IStorage& storage, IClock& clock, IWallClock& wallClock = DefaultSystemWallClock()) noexcept;

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

    /// Non-mutating lookup: like `Get` but does not promote the LRU, advance
    /// `lastAccess`, or bump hit/miss stats. Used by the meta handlers to read
    /// metadata (size / TTL / CAS) and to honour the `u` (no-bump) flag
    /// without recording a client access.
    /// @param key Lookup key.
    /// @return GetResult, or StorageError on I/O failure.
    [[nodiscard]] std::expected<GetResult, StorageError> Peek(std::string_view key);

    [[nodiscard]] std::expected<CasToken, StorageError> Set(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            std::uint32_t exptime);

    /// Set with an explicit absolute deadline — the seam Redis SET ... PX /
    /// PSETEX takes to preserve sub-second TTL precision (which the
    /// `uint32_t exptime` overload above cannot represent, since it
    /// translates milliseconds to whole seconds before reaching the
    /// storage). `TimePoint::max()` clears any TTL.
    /// @param key       Lookup key.
    /// @param value     Value bytes.
    /// @param flags     Memcached "flags" word.
    /// @param deadline  Absolute steady-clock deadline.
    /// @return New CAS token, or StorageError on failure.
    [[nodiscard]] std::expected<CasToken, StorageError> SetWithDeadline(std::string_view key,
                                                                        std::vector<std::byte> value,
                                                                        std::uint32_t flags,
                                                                        TimePoint deadline);

    [[nodiscard]] std::expected<CasToken, StorageError> Add(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            std::uint32_t exptime);

    /// Like Add but with an explicit absolute deadline (Redis SET NX PX path).
    [[nodiscard]] std::expected<CasToken, StorageError> AddWithDeadline(std::string_view key,
                                                                        std::vector<std::byte> value,
                                                                        std::uint32_t flags,
                                                                        TimePoint deadline);

    [[nodiscard]] std::expected<CasToken, StorageError> Replace(std::string_view key,
                                                                std::vector<std::byte> value,
                                                                std::uint32_t flags,
                                                                std::uint32_t exptime);

    /// Like Replace but with an explicit absolute deadline (Redis SET XX PX path).
    [[nodiscard]] std::expected<CasToken, StorageError> ReplaceWithDeadline(std::string_view key,
                                                                            std::vector<std::byte> value,
                                                                            std::uint32_t flags,
                                                                            TimePoint deadline);

    /// Append `suffix` to the existing value. `expected` is an optional CAS
    /// precondition (0 = unconditional; drives meta `ms ... MA C(token)`).
    [[nodiscard]] std::expected<CasToken, StorageError> Append(std::string_view key,
                                                               std::span<std::byte const> suffix,
                                                               CasToken expected = 0);
    /// Prepend `prefix` to the existing value. `expected` is an optional CAS
    /// precondition (0 = unconditional; drives meta `ms ... MP C(token)`).
    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                CasToken expected = 0);

    [[nodiscard]] std::expected<CasToken, StorageError> CompareAndSwap(
        std::string_view key, CasToken expected, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime);

    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> Increment(std::string_view key, std::uint64_t delta);
    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> Decrement(std::string_view key, std::uint64_t delta);

    /// Guarded atomic read-modify-write over the entry under `key`. Forwards to
    /// `IStorage::Update`, supplying the current clock; the backend runs `fn`
    /// within its per-key atomicity boundary. Used for compound mutations such
    /// as the redis set commands and `INCRBYFLOAT`.
    /// @param key Lookup key.
    /// @param fn  Callback given the current GetResult; returns the new outcome
    ///            or a StorageError to abort without mutating.
    /// @return New CAS token, or the callback's/storage's StorageError.
    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<IStorage::UpdateOutcome, StorageError>(GetResult const&)> const& fn);

    // -- redis set type (SADD/SREM/SMEMBERS/SISMEMBER/SMISMEMBER/SCARD/SPOP) --
    //
    // A set is stored as an ordinary value blob tagged with FcTypeSet in the
    // entry flags; the members are sorted and length-prefixed (see SetCodec).
    // The mutating ops go through Update so decode-modify-encode is atomic.

    /// Add `members` to the set at `key` (creating it if absent).
    /// @return Number of members newly added, or WrongType if `key` is a string.
    [[nodiscard]] std::expected<std::int64_t, StorageError> SetAdd(std::string_view key,
                                                                   std::span<std::string const> members);

    /// Remove `members` from the set at `key`; deletes the key if it empties.
    /// @return Number of members actually removed, or WrongType on a non-set key.
    [[nodiscard]] std::expected<std::int64_t, StorageError> SetRemove(std::string_view key,
                                                                      std::span<std::string const> members);

    /// @return All members of the set at `key` (empty if absent), or WrongType.
    [[nodiscard]] std::expected<std::vector<std::string>, StorageError> SetMembers(std::string_view key);

    /// @return True if `member` is in the set at `key`, or WrongType.
    [[nodiscard]] std::expected<bool, StorageError> SetIsMember(std::string_view key, std::string_view member);

    /// @return One bool per `members` entry (membership), or WrongType.
    [[nodiscard]] std::expected<std::vector<bool>, StorageError> SetMIsMember(std::string_view key,
                                                                              std::span<std::string const> members);

    /// @return Cardinality of the set at `key` (0 if absent), or WrongType.
    [[nodiscard]] std::expected<std::int64_t, StorageError> SetCard(std::string_view key);

    /// Remove and return up to `count` members from the set at `key`.
    /// @return The popped members (empty if absent), or WrongType.
    [[nodiscard]] std::expected<std::vector<std::string>, StorageError> SetPop(std::string_view key, std::size_t count);

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key);

    /// Refresh the entry's expiry without rewriting its value. Wraps
    /// `IStorage::Touch` and translates `exptime` via `ExpiryFromExptime`.
    /// @param key     Lookup key.
    /// @param exptime Wire-format expiry word (see `ExpiryFromExptime`).
    /// @return New CAS token, or StorageError(KeyNotFound).
    [[nodiscard]] std::expected<CasToken, StorageError> Touch(std::string_view key, std::uint32_t exptime);

    /// Apply an absolute-deadline TTL to the live entry under `key`.
    /// Used by the redis `EXPIREAT`/`PEXPIREAT` family, which carry a
    /// fully resolved wall-clock instant rather than a relative offset
    /// (so passing them through `Touch(uint32 exptime)` would lose
    /// precision and the absolute-vs-relative distinction).
    /// `TimePoint::max()` clears the expiry (the `PERSIST` semantics).
    /// @param key       Lookup key.
    /// @param newExpiry Absolute deadline on the steady clock.
    /// @return New CAS token, or StorageError(KeyNotFound).
    [[nodiscard]] std::expected<CasToken, StorageError> TouchAt(std::string_view key, TimePoint newExpiry);

    /// Read the remaining time-to-live on the entry under `key`.
    /// `std::nullopt` means "key absent or already expired" (the redis
    /// `-2` return); a non-zero duration is the remaining time; a
    /// zero duration is returned when the entry exists with no TTL
    /// (`TimePoint::max()`), which the redis handler then renders as
    /// `-1`. The distinction between "no TTL" and "has TTL of N" is
    /// signalled via the returned variant in the protocol layer.
    /// @param key Lookup key.
    /// @return An optional pair {hasExpiry, remaining}. `hasExpiry` is
    ///         false when the entry exists with no TTL.
    struct TtlResult
    {
        bool hasExpiry { false };
        Duration remaining { 0 };
    };
    [[nodiscard]] std::expected<std::optional<TtlResult>, StorageError> Ttl(std::string_view key);

    /// Atomically clear the entry's TTL (redis `PERSIST`). Forwards to
    /// `IStorage::ClearExpiry`, which holds the lock-owning decorator's
    /// critical section across the Peek + Touch sequence. Returns:
    ///   - true              if a TTL was actually cleared,
    ///   - false             if the key existed but had no TTL,
    ///   - KeyNotFound error if the key was absent.
    /// The protocol layer collapses both `false` and `KeyNotFound` to
    /// redis `:0`, but the distinction is preserved here so callers
    /// that want to observe absence specifically can.
    [[nodiscard]] std::expected<bool, StorageError> ClearExpiry(std::string_view key);

    /// Atomic get-and-touch: refresh the entry's expiry and return the
    /// refreshed entry in one critical section (memcached `gat`/`gats` and
    /// meta `mg ... T`). Closes the TOCTOU window of a separate Touch+Get.
    /// @param key     Lookup key.
    /// @param exptime Wire-format expiry word (see `ExpiryFromExptime`).
    /// @return The refreshed GetResult, or StorageError(KeyNotFound) on miss.
    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key, std::uint32_t exptime);

    /// Atomic compare-and-delete: delete `key` only if its CAS equals
    /// `expected`, as a single critical section (meta `md C(token)`).
    /// @param key      Lookup key.
    /// @param expected CAS token the caller believes is current.
    /// @return Empty on success, KeyNotFound if absent, CasMismatch on CAS mismatch.
    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key, CasToken expected);

    /// Mark the entry stale without removing it (meta `md I` / `ms I`),
    /// optionally refreshing its TTL at the same time (`md I T(token)`).
    /// @param key        Lookup key.
    /// @param newExptime Wire-format expiry word to apply, or std::nullopt
    ///                   to leave the existing expiry untouched.
    /// @return New CAS token, or StorageError(KeyNotFound) if absent.
    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<std::uint32_t> newExptime);

    /// Flush all entries; `delaySeconds == 0` is immediate.
    void FlushAll(std::uint32_t delaySeconds);

    /// Reconfigure the storage byte budget at runtime. Wraps
    /// `IStorage::Resize` for use by `cache_memlimit`.
    /// @param newMaxBytes Total byte budget across all shards.
    void Resize(std::size_t newMaxBytes)
    {
        _storage.Resize(newMaxBytes);
    }

    [[nodiscard]] StorageStats Snapshot() const noexcept
    {
        return _storage.Snapshot();
    }

    [[nodiscard]] IClock& Clock() noexcept
    {
        return _clock;
    }

    [[nodiscard]] IWallClock& WallClock() noexcept
    {
        return _wallClock;
    }

  private:
    IStorage& _storage;
    IClock& _clock;
    IWallClock& _wallClock;
};

} // namespace FastCache
