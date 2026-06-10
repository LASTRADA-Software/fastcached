// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StreamCodec.hpp>
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

    /// Snapshot only the CAS token of the entry under `key`, without bumping
    /// LRU recency or `lastAccess`. Used by the Redis `WATCH` handler to
    /// record the entry's version at WATCH time so a later mutation on the
    /// same key (detected via `WatchRegistry::Touched`) can abort the
    /// transaction. Returns 0 when the key is absent — the "no entry"
    /// sentinel mirrors `CacheEntry::cas`'s reserved value.
    /// @param key Lookup key.
    /// @return The current CAS token, or StorageError on I/O failure.
    [[nodiscard]] std::expected<CasToken, StorageError> PeekCas(std::string_view key);

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

    // -- redis stream type (X* command family) ------------------------------
    //
    // A stream is stored as an ordinary value blob tagged with FcTypeStream in
    // the entry flags; the entries, ID watermarks and consumer-group
    // bookkeeping are length-prefixed (see StreamCodec). The mutating ops go
    // through Update so decode-modify-encode is atomic per key, and entry IDs
    // are generated from the injected IWallClock.

    /// A trim directive for XADD .../XTRIM (MAXLEN keeps the newest `threshold`
    /// entries; MINID drops entries with an ID below `minId`).
    struct StreamTrim
    {
        enum class Strategy : std::uint8_t
        {
            MaxLen, ///< Keep at most `threshold` newest entries.
            MinId,  ///< Drop entries with ID < `minId`.
        };
        Strategy strategy { Strategy::MaxLen };
        std::uint64_t threshold { 0 };  ///< Entry count for MaxLen.
        StreamCodec::StreamId minId {}; ///< Lower bound for MinId.
    };

    /// Append an entry to the stream at `key` (creating it unless `noMkStream`).
    /// @param key         Stream key.
    /// @param requestedId Explicit ID, or nullopt to auto-generate (`*`). A
    ///                    requested ID with `seqAuto` true means `<ms>-*`.
    /// @param seqAuto     When `requestedId` carries only a ms (the `<ms>-*`
    ///                    form), auto-assign the sequence within that ms.
    /// @param fields      Ordered field/value pairs for the new entry.
    /// @param trim        Optional trim applied after the append.
    /// @param noMkStream  When true, do not create the stream if it is absent
    ///                    (XADD NOMKSTREAM); returns KeyNotFound instead.
    /// @return The assigned entry ID, or WrongType / KeyNotFound /
    ///         InvalidArgument (ID not greater than the last).
    [[nodiscard]] std::expected<StreamCodec::StreamId, StorageError> StreamAdd(
        std::string_view key,
        std::optional<StreamCodec::StreamId> requestedId,
        bool seqAuto,
        std::span<std::pair<std::string, std::string> const> fields,
        std::optional<StreamTrim> trim,
        bool noMkStream);

    /// @return Entry count of the stream at `key` (0 if absent), or WrongType.
    [[nodiscard]] std::expected<std::int64_t, StorageError> StreamLen(std::string_view key);

    /// Return entries with ID in `[start, end]`, at most `count` of them.
    /// @param key     Stream key.
    /// @param start   Inclusive lower bound (use StreamId::Min for `-`).
    /// @param end     Inclusive upper bound (use StreamId::Max for `+`).
    /// @param count   Maximum entries to return; 0 means unlimited.
    /// @param reverse When true, scan high→low (XREVRANGE); `start`/`end` are
    ///                still the logical low/high bounds.
    /// @return Matching entries in scan order, or WrongType.
    [[nodiscard]] std::expected<std::vector<StreamCodec::StreamEntry>, StorageError> StreamRange(
        std::string_view key, StreamCodec::StreamId start, StreamCodec::StreamId end, std::size_t count, bool reverse);

    /// Return up to `count` entries with ID strictly greater than `after`
    /// (the non-blocking core of XREAD).
    /// @return Matching entries in ascending order, or WrongType.
    [[nodiscard]] std::expected<std::vector<StreamCodec::StreamEntry>, StorageError> StreamRead(std::string_view key,
                                                                                                StreamCodec::StreamId after,
                                                                                                std::size_t count);

    /// Delete the entries with the given `ids` from the stream at `key`.
    /// @return Number of entries actually removed, or WrongType.
    [[nodiscard]] std::expected<std::int64_t, StorageError> StreamDelete(std::string_view key,
                                                                         std::span<StreamCodec::StreamId const> ids);

    /// Trim the stream at `key` per `trim`.
    /// @return Number of entries evicted, or WrongType.
    [[nodiscard]] std::expected<std::int64_t, StorageError> StreamTrimTo(std::string_view key, StreamTrim trim);

    /// Set the stream's last-ID watermark (XSETID), optionally adjusting the
    /// entries-added and max-deleted-id counters.
    /// @return Empty on success, or WrongType / KeyNotFound / InvalidArgument
    ///         (new last-id below an existing entry).
    [[nodiscard]] std::expected<void, StorageError> StreamSetId(std::string_view key,
                                                                StreamCodec::StreamId lastId,
                                                                std::optional<std::uint64_t> entriesAdded,
                                                                std::optional<StreamCodec::StreamId> maxDeletedId);

    /// The highest entry ID currently in the stream at `key` (the `$` target
    /// for XREAD), or StreamId::Min when the stream is absent/empty.
    /// @return The last entry's ID, or WrongType.
    [[nodiscard]] std::expected<StreamCodec::StreamId, StorageError> StreamLastId(std::string_view key);

    // -- consumer groups (XGROUP / XREADGROUP / XACK / XPENDING / XCLAIM) ----

    /// How a freshly created group's read cursor is positioned.
    enum class GroupStart : std::uint8_t
    {
        Beginning, ///< `0` — deliver the whole backlog.
        End,       ///< `$` — deliver only entries added after creation.
        At,        ///< An explicit start ID.
    };

    /// Create consumer group `group` on the stream at `key`.
    /// @param key        Stream key.
    /// @param group      Group name.
    /// @param start      Where the group's cursor begins.
    /// @param at         Explicit start ID when `start == At`.
    /// @param mkStream   Create the stream if absent (XGROUP CREATE MKSTREAM).
    /// @return Empty on success, KeyNotFound when absent and `!mkStream`,
    ///         KeyExists when the group already exists, or WrongType.
    [[nodiscard]] std::expected<void, StorageError> StreamGroupCreate(
        std::string_view key, std::string_view group, GroupStart start, StreamCodec::StreamId at, bool mkStream);

    /// Reposition group `group`'s read cursor (XGROUP SETID).
    /// @return Empty on success, KeyNotFound (stream/group absent), or WrongType.
    [[nodiscard]] std::expected<void, StorageError> StreamGroupSetId(std::string_view key,
                                                                     std::string_view group,
                                                                     GroupStart start,
                                                                     StreamCodec::StreamId at);

    /// Destroy group `group` (XGROUP DESTROY).
    /// @return True if a group was removed, false if it was absent, or WrongType.
    [[nodiscard]] std::expected<bool, StorageError> StreamGroupDestroy(std::string_view key, std::string_view group);

    /// Create consumer `consumer` in group `group` (XGROUP CREATECONSUMER).
    /// @return True if newly created, false if it already existed, or WrongType /
    ///         KeyNotFound (NOGROUP).
    [[nodiscard]] std::expected<bool, StorageError> StreamConsumerCreate(std::string_view key,
                                                                         std::string_view group,
                                                                         std::string_view consumer);

    /// Delete consumer `consumer` from group `group` (XGROUP DELCONSUMER).
    /// @return Number of pending entries the consumer owned, or WrongType /
    ///         KeyNotFound (NOGROUP).
    [[nodiscard]] std::expected<std::int64_t, StorageError> StreamConsumerDelete(std::string_view key,
                                                                                 std::string_view group,
                                                                                 std::string_view consumer);

    /// Read entries for a consumer group (the non-blocking core of XREADGROUP).
    /// @param key      Stream key.
    /// @param group    Group name.
    /// @param consumer Consumer name (created on demand).
    /// @param after    Read cursor: `>` for new entries (use nullopt), or an
    ///                 explicit ID to re-read that consumer's PEL history.
    /// @param count    Maximum entries to return; 0 means unlimited.
    /// @param noAck    When true, deliveries are not added to the PEL.
    /// @return Matching entries, or WrongType / KeyNotFound (NOGROUP).
    [[nodiscard]] std::expected<std::vector<StreamCodec::StreamEntry>, StorageError> StreamReadGroup(
        std::string_view key,
        std::string_view group,
        std::string_view consumer,
        std::optional<StreamCodec::StreamId> after,
        std::size_t count,
        bool noAck);

    /// Acknowledge `ids` for group `group`, removing them from the PEL (XACK).
    /// @return Number of entries actually acknowledged, or WrongType /
    ///         KeyNotFound (NOGROUP).
    [[nodiscard]] std::expected<std::int64_t, StorageError> StreamAck(std::string_view key,
                                                                      std::string_view group,
                                                                      std::span<StreamCodec::StreamId const> ids);

    /// One row of an XPENDING extended reply.
    struct PendingSummary
    {
        StreamCodec::StreamId id {};       ///< Pending entry ID.
        std::string consumer {};           ///< Owning consumer.
        std::uint64_t idleMs { 0 };        ///< Time since last delivery.
        std::uint64_t deliveryCount { 0 }; ///< Delivery count so far.
    };

    /// Summary form of XPENDING: count + min/max IDs + per-consumer tallies.
    struct PendingOverview
    {
        std::uint64_t count { 0 };                                         ///< Total pending entries.
        StreamCodec::StreamId minId {};                                    ///< Smallest pending ID.
        StreamCodec::StreamId maxId {};                                    ///< Largest pending ID.
        std::vector<std::pair<std::string, std::uint64_t>> perConsumer {}; ///< (consumer, count).
    };

    /// XPENDING summary form.
    /// @return The overview (count 0 when none), or WrongType / KeyNotFound.
    [[nodiscard]] std::expected<PendingOverview, StorageError> StreamPendingSummary(std::string_view key,
                                                                                    std::string_view group);

    /// XPENDING extended form: pending entries in `[start, end]` (optionally
    /// filtered to one consumer / a minimum idle time), at most `count`.
    /// @return The matching rows, or WrongType / KeyNotFound.
    [[nodiscard]] std::expected<std::vector<PendingSummary>, StorageError> StreamPendingRange(
        std::string_view key,
        std::string_view group,
        StreamCodec::StreamId start,
        StreamCodec::StreamId end,
        std::size_t count,
        std::optional<std::string_view> consumer,
        std::uint64_t minIdleMs);

    /// Result of an XCLAIM/XAUTOCLAIM: the claimed entries plus, for JUSTID,
    /// just their IDs. `deleted` lists PEL IDs whose entry no longer exists
    /// (XAUTOCLAIM drains these from the PEL).
    struct ClaimResult
    {
        std::vector<StreamCodec::StreamEntry> entries {}; ///< Claimed entries (empty for JUSTID).
        std::vector<StreamCodec::StreamId> ids {};        ///< Claimed IDs (always populated).
        std::vector<StreamCodec::StreamId> deleted {};    ///< PEL IDs dropped because the entry is gone.
        StreamCodec::StreamId cursor {};                  ///< Next scan cursor (XAUTOCLAIM).
    };

    /// XCLAIM: transfer ownership of `ids` in `group` to `consumer` when their
    /// idle time is at least `minIdleMs`.
    /// @param key       Stream key.
    /// @param group     Consumer group.
    /// @param consumer  New owner.
    /// @param minIdleMs Idle threshold a pending entry must meet to be claimed.
    /// @param ids       Entry IDs to claim.
    /// @param justId    Populate only IDs (and do not bump delivery counts).
    /// @param force     Create a PEL entry for a requested ID that exists in the
    ///                  stream but is not currently pending, then claim it.
    /// @return The claimed entries/ids, or WrongType / KeyNotFound (NOGROUP).
    [[nodiscard]] std::expected<ClaimResult, StorageError> StreamClaim(std::string_view key,
                                                                       std::string_view group,
                                                                       std::string_view consumer,
                                                                       std::uint64_t minIdleMs,
                                                                       std::span<StreamCodec::StreamId const> ids,
                                                                       bool justId,
                                                                       bool force = false);

    /// XAUTOCLAIM: scan the PEL from `start`, claiming up to `count` entries
    /// idle at least `minIdleMs` for `consumer`.
    /// @return The claimed entries/ids, the deleted set, and the next cursor.
    [[nodiscard]] std::expected<ClaimResult, StorageError> StreamAutoClaim(std::string_view key,
                                                                           std::string_view group,
                                                                           std::string_view consumer,
                                                                           std::uint64_t minIdleMs,
                                                                           StreamCodec::StreamId start,
                                                                           std::size_t count,
                                                                           bool justId);

    /// Snapshot of a stream for XINFO STREAM.
    struct StreamInfo
    {
        std::uint64_t length { 0 };                       ///< Entry count.
        StreamCodec::StreamId lastId {};                  ///< Last-ID watermark.
        StreamCodec::StreamId maxDeletedId {};            ///< Max deleted ID.
        std::uint64_t entriesAdded { 0 };                 ///< Entries ever added.
        std::uint64_t groupCount { 0 };                   ///< Number of consumer groups.
        std::optional<StreamCodec::StreamEntry> first {}; ///< First entry, if any.
        std::optional<StreamCodec::StreamEntry> last {};  ///< Last entry, if any.
    };

    /// XINFO STREAM.
    /// @return The snapshot, or WrongType / KeyNotFound.
    [[nodiscard]] std::expected<StreamInfo, StorageError> StreamInfoOf(std::string_view key);

    /// Snapshot of one consumer group for XINFO GROUPS.
    struct GroupInfo
    {
        std::string name {};                    ///< Group name.
        std::uint64_t consumers { 0 };          ///< Number of consumers.
        std::uint64_t pending { 0 };            ///< PEL size.
        StreamCodec::StreamId lastDelivered {}; ///< Last-delivered ID.
        std::uint64_t entriesRead { 0 };        ///< Entries read by the group.
        std::uint64_t lag { 0 };                ///< Entries added but not yet read (entriesAdded - entriesRead).
    };

    /// XINFO GROUPS.
    /// @return One row per group, or WrongType / KeyNotFound.
    [[nodiscard]] std::expected<std::vector<GroupInfo>, StorageError> StreamGroupInfo(std::string_view key);

    /// Snapshot of one consumer for XINFO CONSUMERS.
    struct ConsumerInfo
    {
        std::string name {};         ///< Consumer name.
        std::uint64_t pending { 0 }; ///< Entries pending for this consumer.
    };

    /// XINFO CONSUMERS.
    /// @return One row per consumer, or WrongType / KeyNotFound (NOGROUP).
    [[nodiscard]] std::expected<std::vector<ConsumerInfo>, StorageError> StreamConsumerInfo(std::string_view key,
                                                                                            std::string_view group);

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

    /// Current wall-clock time in milliseconds since the UNIX epoch, taken from
    /// the injected `IWallClock`. Drives stream entry-ID timestamps and the
    /// PEL idle-time accounting; deterministic under `ManualWallClock`.
    /// @return Milliseconds since the epoch.
    [[nodiscard]] std::uint64_t WallNowMs() const noexcept;

  private:
    /// Shared implementation of Append/Prepend. Concatenates `extra` onto the
    /// existing value (at the front when `atFront`, else the back) atomically
    /// inside the storage's per-key Update boundary, rejecting non-string
    /// (set/stream) keys with `WrongType` and honouring the CAS precondition.
    /// @param key      Lookup key.
    /// @param extra    Bytes to splice in.
    /// @param expected CAS precondition (0 = unconditional).
    /// @param atFront  `true` for PREPEND, `false` for APPEND.
    /// @return New CAS token, or a StorageError (WrongType / CasMismatch / I/O).
    [[nodiscard]] std::expected<CasToken, StorageError> ConcatGuarded(std::string_view key,
                                                                      std::span<std::byte const> extra,
                                                                      CasToken expected,
                                                                      bool atFront);

    IStorage& _storage;
    IClock& _clock;
    IWallClock& _wallClock;
};

} // namespace FastCache
