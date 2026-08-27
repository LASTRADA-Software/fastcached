// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace FastCache
{

/// LRU recency policy for the in-memory backend.
///
/// `Strict` promotes an entry to most-recently-used on **every** read, giving
/// exact LRU order — but a read then mutates shared state, so reads on one
/// shard must serialise behind an exclusive lock. `Approximate` skips the
/// per-read promotion on the (lock-free, shared-locked) read path and instead
/// promotes a sampled fraction of reads under a brief exclusive lock; reads run
/// concurrently and eviction stays "good enough" (memcached-style). Approximate
/// is the default — it favours read throughput, the common case for a cache.
enum class LruMode : std::uint8_t
{
    Approximate, ///< Sampled/deferred promotion; concurrent reads (default).
    Strict,      ///< Promote on every read; reads serialise per shard.
};

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

    /// Value writes (SET/ADD/REPLACE/APPEND/PREPEND/CAS/INCR/UPDATE) that
    /// failed to persist because the storage could not accept them: a full
    /// disk or I/O error (`IoError`), an exhausted memory budget
    /// (`OutOfMemory`), on-disk corruption (`Corrupt`), or read-only storage
    /// (`ReadOnly`). Excludes benign conditional-write outcomes (KeyExists,
    /// KeyNotFound, CasMismatch) and client-side rejections (ValueTooLarge).
    /// Populated by `WriteErrorReportingStorage`; 0 for backends not wrapped
    /// by it.
    std::uint64_t writeErrors { 0 };
};

/// One cache's statistics, kept apart by the tier holding them.
///
/// An entry is absent when the cache has no such tier at all, which is a
/// different fact from a tier holding nothing — see `IStorage::SnapshotTiers`.
using TieredStorageStats = EnumTable<StorageTier, std::optional<StorageStats>>;

/// The size-typed fields of `StorageStats`, as member pointers.
///
/// A table so that summing several snapshots is one loop rather than one `+=`
/// per field. `ShardedStorage` wrote that addition out by hand across
/// twenty-three lines, and a hand-written list of every field is the shape where
/// a field added later is silently left out — `writeErrors` already had been,
/// and only failed to matter because the decorator that populates it wraps the
/// sharded backend rather than each shard.
inline constexpr std::array StorageStatsSizeFields {
    &StorageStats::itemCount,
    &StorageStats::bytesUsed,
    &StorageStats::bytesLimit,
};

/// The counter fields of `StorageStats`, as member pointers.
///
/// Separate from the size fields only because the two groups have different
/// types; both are summed the same way.
inline constexpr std::array StorageStatsCounterFields {
    &StorageStats::evictions,    &StorageStats::cmdGet,    &StorageStats::cmdSet,           &StorageStats::cmdTouch,
    &StorageStats::cmdFlush,     &StorageStats::getHits,   &StorageStats::getMisses,        &StorageStats::deleteHits,
    &StorageStats::deleteMisses, &StorageStats::incrHits,  &StorageStats::incrMisses,       &StorageStats::decrHits,
    &StorageStats::decrMisses,   &StorageStats::touchHits, &StorageStats::touchMisses,      &StorageStats::casHits,
    &StorageStats::casMisses,    &StorageStats::casBadval, &StorageStats::evictedUnfetched, &StorageStats::expiredUnfetched,
    &StorageStats::writeErrors,
};

/// The width of the member @p field names.
///
/// Taken from the member's own type rather than assumed, so the guard below
/// stays true whatever a field happens to be declared as.
/// @param field Any pointer-to-member.
/// @return `sizeof` the member's type.
template <typename Member, typename Class>
consteval std::size_t MemberSize(Member Class::* /*field*/) noexcept
{
    return sizeof(Member);
}

// The guard the tables exist to make possible, and the thing whose absence let
// `writeErrors` be added to the struct and to nothing else. Two halves, and the
// runtime duplicate check in `StorageTier_test` is the third: this one catches a
// field that reaches no table, and that one catches a table that names one field
// twice while dropping its neighbour.
//
// The first assert is what makes the second sound. It fails on a platform whose
// `std::size_t` is narrower than its `std::uint64_t` -- a 32-bit build -- because
// `StorageStats` then carries alignment padding and its size stops being the sum
// of its fields. That is a real signal rather than a nuisance: the check has to
// be replaced with one that accounts for the padding, never deleted.
static_assert(std::has_unique_object_representations_v<StorageStats>,
              "StorageStats must have no padding for the field-table completeness check below to hold");
static_assert(sizeof(StorageStats)
                  == (StorageStatsSizeFields.size() * MemberSize(StorageStatsSizeFields.front()))
                         + (StorageStatsCounterFields.size() * MemberSize(StorageStatsCounterFields.front())),
              "every field of StorageStats must appear in exactly one of the field tables above; a field that "
              "reaches neither is silently left out of every sum");

/// Add every field of @p addend into @p total.
/// @param total Accumulator, modified in place.
/// @param addend What to fold in.
constexpr void AddInto(StorageStats& total, StorageStats const& addend) noexcept
{
    for (auto const field: StorageStatsSizeFields)
        total.*field += addend.*field;
    for (auto const field: StorageStatsCounterFields)
        total.*field += addend.*field;
}

/// Fold one tiered snapshot into another, tier by tier.
///
/// A tier the addend does not have leaves the accumulator's entry untouched —
/// **including leaving it absent**, which is what keeps "this cache has no disk
/// tier" from becoming "this cache has a disk tier holding nothing" the moment a
/// second snapshot is merged in.
/// @param total Accumulator, modified in place.
/// @param addend What to fold in.
constexpr void AddInto(TieredStorageStats& total, TieredStorageStats const& addend) noexcept
{
    for (auto const& tierRow: StorageTierTable)
    {
        auto const index = static_cast<std::size_t>(tierRow.tier);
        // Both bound to references before they are tested, rather than subscripted
        // again after. `bugprone-unchecked-optional-access` can follow a guard only
        // on the same expression, and a second `total[index]` is a fresh one it has
        // no guard for -- which with `WarningsAsErrors` is a build failure.
        auto const& from = addend[index];
        if (!from.has_value())
            continue;
        auto& into = total[index];
        if (!into.has_value())
            into = StorageStats {};
        AddInto(*into, *from);
    }
}

/// Declared rather than included: only `SetReclaimLog` mentions it, by pointer,
/// and `Cache/IReclaimLog.hpp` pulls in the mutation-observer vocabulary that
/// every backend would otherwise inherit for a method most of them never define.
class IReclaimLog;

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

    /// Read just the absolute expiry of the entry under `key` without
    /// touching its value, LRU recency, hit/miss stats, or `lastAccess`.
    /// Backs the redis `TTL`/`PTTL` introspection commands, which clients
    /// can poll at high frequency — copying the value buffer or bumping
    /// the LRU on every poll would be wrong twice. Defaults to a `Peek` +
    /// expiry projection (so storages that already implement `Peek`
    /// cheaply need no extra code); decorators may override to short-
    /// circuit further.
    /// @param key Lookup key.
    /// @param now Current clock value (drives the TTL existence check).
    /// @return The entry's absolute expiry, or std::nullopt if the key
    ///         is absent or already expired.
    [[nodiscard]] virtual std::expected<std::optional<TimePoint>, StorageError> PeekExpiry(std::string_view key,
                                                                                           TimePoint now)
    {
        auto const peek = Peek(key, now);
        if (!peek.has_value())
            return std::unexpected(peek.error());
        if (!peek->found)
            return std::optional<TimePoint> {};
        return std::optional<TimePoint> { peek->entry.expiry };
    }

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

    /// What an `Update` callback decides to do with the entry under the key.
    enum class UpdateAction : std::uint8_t
    {
        Store,     ///< Write `value`/`flags` (insert or overwrite).
        Delete,    ///< Remove the entry (e.g. a set became empty).
        Unchanged, ///< Leave the entry as-is (read-only outcome).
    };

    /// Result of an `Update` callback: the new entry state to apply atomically.
    struct UpdateOutcome
    {
        std::vector<std::byte> value; ///< New value bytes (used when action == Store).
        std::uint32_t flags { 0 };    ///< New flags / type tag (used when action == Store).
        UpdateAction action { UpdateAction::Unchanged };
        /// New absolute expiry to apply on Store. `nullopt` (the default)
        /// means "preserve the prior entry's expiry", which is the
        /// behaviour redis mandates for INCR/SADD/INCRBYFLOAT family —
        /// the storage layer therefore forwards the existing
        /// `current->entry.expiry` rather than wiping it. Callbacks that
        /// DO want to change the TTL (a SETEX-like compound op) set
        /// this explicitly; `TimePoint::max()` clears any TTL.
        std::optional<TimePoint> newExpiry {};
    };

    /// Guarded read-modify-write: atomically read the entry under `key`, hand it
    /// to `fn`, and apply the returned outcome — all within the backend's
    /// atomicity boundary for that key. This is the one primitive for
    /// compound mutations (decode → mutate → re-encode → store) such as the
    /// redis set commands and `INCRBYFLOAT`; doing them as a separate
    /// `Get` + `Set` would race under concurrent writers.
    ///
    /// The default composes `Peek` + `Set`/`Delete` (correct for single-threaded
    /// or already-locked backends); lock-owning decorators (`ShardedStorage`)
    /// override it to hold the shard lock across the whole sequence.
    /// @param key Lookup key.
    /// @param fn  Callback given the current GetResult; returns the new state or
    ///            a StorageError to abort without mutating.
    /// @param now Current clock value (drives the existence/expiry check).
    /// @return The new CAS token after a Store, the previous behaviour's token on
    ///         Delete/Unchanged, or the callback's StorageError.
    [[nodiscard]] virtual std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
        TimePoint now)
    {
        auto const current = Peek(key, now);
        if (!current.has_value())
            return std::unexpected(current.error());
        auto outcome = fn(*current);
        if (!outcome.has_value())
            return std::unexpected(outcome.error());
        switch (outcome->action)
        {
            case UpdateAction::Store: {
                // Redis semantics: INCR/SADD/INCRBYFLOAT preserve the
                // prior TTL. The callback opts in to a TTL change via
                // outcome->newExpiry; if it leaves that nullopt we
                // forward the current entry's expiry (or
                // TimePoint::max() for a fresh insert).
                auto const expiry = outcome->newExpiry.value_or(current->found ? current->entry.expiry : TimePoint::max());
                return Set(key, std::move(outcome->value), outcome->flags, expiry);
            }
            case UpdateAction::Delete:
                if (current->found)
                    (void) Delete(key, now);
                return CasToken { 0 };
            case UpdateAction::Unchanged:
                return current->found ? current->entry.cas : CasToken { 0 };
        }
        return CasToken { 0 };
    }

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

    /// Atomically clear the entry's TTL (redis `PERSIST`). Reads the entry
    /// and, if it has a TTL, applies a `TimePoint::max()` expiry in the
    /// same critical section the inner backend uses for `Touch`. The
    /// atomicity boundary is the lock-owning decorator's (ShardedStorage's
    /// per-shard lock); the default-impl decomposition is safe only when
    /// the caller already holds that lock or when no concurrent writer can
    /// race. Returns:
    ///   - true  if a TTL was actually cleared (the entry existed and had
    ///           a non-max expiry — matches redis `:1`),
    ///   - false if the entry existed but had no TTL (redis `:0`),
    ///   - StorageError(KeyNotFound) if absent (also rendered as `:0` by
    ///           the protocol handler — but distinguished here so callers
    ///           that want to observe absence can).
    /// @param key Lookup key.
    /// @param now Current clock value (drives the existence check).
    [[nodiscard]] virtual std::expected<bool, StorageError> ClearExpiry(std::string_view key, TimePoint now)
    {
        auto const peek = Peek(key, now);
        if (!peek.has_value())
            return std::unexpected(peek.error());
        if (!peek->found)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        if (peek->entry.expiry == TimePoint::max())
            return false;
        auto const touched = Touch(key, TimePoint::max(), now);
        if (!touched.has_value())
            return std::unexpected(touched.error());
        return true;
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

    /// The same statistics, kept apart by the tier that holds them.
    ///
    /// `Snapshot()` answers "what does this cache hold", which a composite backend
    /// can only answer by choosing: `LayeredStorage` reports its canonical lower
    /// tier's item count, bytes and budget, so the in-memory half above it leaves no
    /// trace at all. That is the right merge for a `stats` command and the wrong one
    /// for an operator asking whether a node's RAM tier is doing anything.
    ///
    /// **Absent is not zero**, which is why every entry is optional: a memory-only
    /// cache has no disk tier, and a disk tier holding zero bytes is a different
    /// claim that a dashboard renders as "empty" rather than as "there isn't one".
    ///
    /// The default describes a backend that is one in-memory tier — true of
    /// `InMemoryLruStorage` and of every in-memory test double. A backend that keeps
    /// its bytes anywhere else, or that composes several, **must override this**;
    /// the ones in this tree do.
    ///
    /// **These are each tier's own numbers, and nothing here is meant to be summed
    /// across tiers.** A composite's tiers overlap, in both directions.
    /// `LayeredStorage` mirrors into L1 every entry it reads out of L2, so adding
    /// the two item counts counts the mirrored entries twice; and L2 is consulted
    /// only when L1 missed, so adding the two hit counts turns a hundred reads
    /// served entirely from cache into a hundred and sixty requests at 62%. Both
    /// tiers' figures are true, and they answer "is the mirror doing anything" and
    /// "how full is the store" rather than "what does this cache hold" -- which is
    /// what `Snapshot()` is for, and what a consumer wanting a total must call.
    /// @return One entry per tier this backend has, indexed by `StorageTier`.
    [[nodiscard]] virtual TieredStorageStats SnapshotTiers() const noexcept
    {
        TieredStorageStats tiers {};
        tiers[static_cast<std::size_t>(StorageTier::Memory)] = Snapshot();
        return tiers;
    }

    /// Whether this backend's `Get` is safe to call concurrently under a
    /// *shared* (reader) lock — i.e. a read performs no structural mutation of
    /// shared state and uses atomic/last-writer-wins updates for any counters
    /// it touches. A `ShardedStorage` wrapping such a backend takes a shared
    /// lock on `Get`, recovering read parallelism on a single shard; backends
    /// that mutate on read (LRU splice, page touch) return false and are served
    /// under an exclusive lock as before. Defaults to false (conservative).
    /// @return True if concurrent shared-locked `Get` calls are race-free.
    [[nodiscard]] virtual bool SupportsSharedRead() const noexcept
    {
        return false;
    }

    /// Warm the entry under `key` into any in-memory tier this backend
    /// fronts, **without** treating the access as a client read: no LRU
    /// promotion counted as a hit, no hit/miss statistic change, no
    /// `lastAccess`/`fetched` stamping. Used by the compile-cache executor to
    /// pre-load a build prefetch group disk→memory ahead of demand, so the prefetch group's
    /// FETCHes are served from L1.
    ///
    /// The default is correct for single-tier backends (there is no lower tier
    /// to pull from): it Peeks and reports whether the entry exists, warming
    /// nothing. Tiered decorators (`LayeredStorage`) override it to pull the
    /// entry from the lower tier into the in-memory mirror; routing decorators
    /// (`ShardedStorage`, `TracingStorage`, …) forward it.
    /// @param key Key to warm.
    /// @param now Current clock value (drives the TTL existence check).
    /// @return true if a live entry now resides warm (or already existed),
    ///         false on miss, or StorageError on I/O failure.
    [[nodiscard]] virtual std::expected<bool, StorageError> Prefetch(std::string_view key, TimePoint now)
    {
        auto const peek = Peek(key, now);
        if (!peek.has_value())
            return std::unexpected(peek.error());
        return peek->found;
    }

    /// Best-effort LRU promotion + access-time advance for a key just read
    /// under a shared lock. Called by a lock-owning decorator (ShardedStorage)
    /// on a *sampled* fraction of reads, holding an **exclusive** lock — so it
    /// may safely splice the LRU and advance `lastAccess`/`fetched`, which the
    /// shared `Get` deliberately skips. A no-op miss is fine if the key was
    /// evicted in the meantime. The default does nothing (backends that
    /// promote on `Get` itself need no deferred promotion).
    /// @param key Key to promote.
    /// @param now Current clock value (for the access-time advance).
    virtual void PromoteOnRead(std::string_view key, TimePoint now)
    {
        static_cast<void>(key);
        static_cast<void>(now);
    }

    /// Route this backend's reclaim reports to `log` — the entries it drops on
    /// its own initiative, by TTL expiry or by LRU eviction, which no caller
    /// asked for and so nothing above the backend can name.
    ///
    /// One method rather than a victim list threaded through twenty signatures:
    /// the reporting is orthogonal to every operation that can trigger it, and a
    /// backend that reclaims nothing wants nothing to do with it. The default is
    /// therefore right for every backend that does not reclaim, and for every
    /// test double.
    ///
    /// Routing decorators forward to their inner storage. **`LayeredStorage`
    /// forwards to L2 only**: dropping an entry from the L1 mirror is a
    /// demotion, not a reclaim — the key is still in L2 and still served — and
    /// reporting it would publish an `evicted` event for a key that never left
    /// the cache.
    /// @param log Sink for reclaimed keys, or nullptr to stop reporting.
    virtual void SetReclaimLog(IReclaimLog* log)
    {
        static_cast<void>(log);
    }
};

} // namespace FastCache
