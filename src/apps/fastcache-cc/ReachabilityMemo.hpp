// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"
#include "Dispatch.hpp"
#include "Stats.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Where a reachability memo is kept between launcher processes.
///
/// A seam rather than a path, so the memo's decisions are tested against an in-memory
/// store two memos share the way two launcher PROCESSES share a file, and only
/// `FileMemoStore` touches the filesystem.
class IMemoStore
{
  public:
    IMemoStore() = default;
    IMemoStore(IMemoStore const&) = delete;
    IMemoStore(IMemoStore&&) = delete;
    IMemoStore& operator=(IMemoStore const&) = delete;
    IMemoStore& operator=(IMemoStore&&) = delete;
    virtual ~IMemoStore() = default;

    /// Read the stored memo.
    /// @return Its text, or nothing when there is none or it cannot be read -- both of
    ///         which the memo reads as *nothing remembered*.
    [[nodiscard]] virtual std::optional<std::string> Read() const = 0;

    /// Replace the stored memo, whole.
    /// @param text The serialized memo.
    /// @return True when it was stored. A failure is not an error the launcher acts on:
    ///         it costs the next launcher one dial, which is what it did without a memo.
    virtual bool Write(std::string_view text) = 0;
};

/// A memo kept in one file, replaced atomically (temp file plus rename).
///
/// An EMPTY path reads nothing and writes nothing, which is how `StateDirectory`'s
/// "do not persist" answer reaches the memo without a second code path.
class FileMemoStore final: public IMemoStore
{
  public:
    /// @param file Where the memo lives; empty for a store that keeps nothing.
    explicit FileMemoStore(std::filesystem::path file);

    /// @return The file's contents, or nothing when it is absent or unreadable.
    [[nodiscard]] std::optional<std::string> Read() const override;

    /// @param text The serialized memo.
    /// @return True when the file now holds exactly @p text.
    bool Write(std::string_view text) override;

  private:
    std::filesystem::path _file;
};

/// What a memo entry says could not be reached.
///
/// Persisted by its TOKEN, never by value: private enum, no explicit values.
enum class MemoKind : std::uint8_t
{
    /// The CONFIGURED scheduler: a lease dial that made no connection at all.
    SchedulerUnreached,
    /// A worker a lease named, by the endpoint it ADVERTISES, that no dial reached.
    WorkerUnreached,
    Last
};

/// How one kind of entry is written, believed and bounded.
struct MemoKindRow
{
    MemoKind kind;                 ///< The kind this row describes.
    std::string_view token;        ///< How the kind is spelled in the memo file.
    std::chrono::milliseconds ttl; ///< How long an entry is believed after it was stamped.
    std::size_t capacity;          ///< How many entries of this kind a memo keeps, newest first.
};

/// One row per `MemoKind`: the token it is written under, how long it is believed, and
/// how many a memo keeps.
inline constexpr EnumTable<MemoKind, MemoKindRow> MemoKinds { {
    { .kind = MemoKind::SchedulerUnreached,
      .token = "scheduler-unreached",
      .ttl = std::chrono::seconds { 15 },
      .capacity = 4 },
    { .kind = MemoKind::WorkerUnreached,
      .token = "worker-unreached",
      .ttl = std::chrono::seconds { 60 },
      .capacity = CompileCacheWire::MaxLeaseExclusions },
} };

static_assert(RowsInEnumeratorOrder(MemoKinds, &MemoKindRow::kind), "MemoKinds must hold one row per MemoKind, in order");
static_assert(MemoKinds[static_cast<std::size_t>(MemoKind::WorkerUnreached)].capacity
                  == CompileCacheWire::MaxLeaseExclusions,
              "the memo keeps exactly as many unreachable workers as one LEASE may name");

/// How old an entry is, as three outcomes rather than a signed number.
enum class MemoAge : std::uint8_t
{
    Fresh,         ///< Stamped within its kind's TTL: believed.
    Expired,       ///< Stamped at least one TTL ago: ignored, and dropped by the next `Note` of its kind.
    FromTheFuture, ///< Stamped AFTER now -- the wall clock stepped back. Treated as stale.
};

/// One remembered failure to reach an endpoint.
struct MemoEntry
{
    MemoKind kind;              ///< What could not be reached.
    std::int64_t stampedUnixMs; ///< When, in WALL-clock unix milliseconds.
    std::string endpoint;       ///< The endpoint as it was dialled (the scheduler) or advertised (a worker).
};

/// A per-user memo of the schedulers and workers a recent launcher could not reach.
///
/// `fastcache-cc` is one process per translation unit, so with the scheduler down every
/// miss of a parallel build pays the whole connect budget -- after a second preprocess
/// and a fingerprint -- to learn what the previous process learned a moment ago. The memo
/// is how one process tells the next, turning that into one dial per TTL.
///
/// **Safe to cache, and decided before anything else.** Both staleness directions fail
/// CLOSED:
/// - a STALE "unreachable" entry costs a local compile, for at most one TTL, and then
///   heals on its own -- never a wrong object, only a slower build for that window. For
///   a WORKER entry it costs less: the scheduler grants ANOTHER worker, and the compile
///   goes local only when every matching worker is excluded (`AllExcluded`);
/// - a MISSING or lost entry costs one extra dial, which is exactly what the launcher did
///   before the memo existed.
///
/// Neither direction produces a wrong answer that looks right. And the memo errs NARROW
/// about what it believes: an entry is written only for `TransportFailure::Unreached`,
/// where no connection was ever made. `PeerLost`, `Expired` and `Silent` each reached a
/// live peer, which is not a machine that is down, so they write nothing.
///
/// **Stamps are WALL clock, in unix milliseconds**, because a steady clock is not
/// comparable across processes once the machine reboots or sleeps. So an age is a
/// subtraction that can come out NEGATIVE when the clock steps back, and that is its own
/// outcome, `MemoAge::FromTheFuture`, decided in `AgeOf` and nowhere else -- never a
/// clamp, which would make it read as the freshest entry there is. A future stamp is
/// treated as stale. `now` is always the caller's, read from an injected
/// `core::platform::IWallClock`; the memo reads no clock of its own.
///
/// **Concurrency is tolerated rather than locked.** Two launchers doing a
/// read-modify-write at once lose at most one entry, which costs one extra dial. The
/// write itself is whole (see `FileMemoStore`), so a reader never sees a torn file, and a
/// file that does not parse is read as an empty memo.
class ReachabilityMemo
{
  public:
    /// Read a serialized memo.
    /// @param text What `Serialize` wrote, or anything else.
    /// @return The entries it holds. Empty for any text without this build's header; a
    ///         line naming a kind this build does not know is skipped on its own.
    [[nodiscard]] static ReachabilityMemo Parse(std::string_view text);

    /// Read a memo from a store.
    /// @param store Where it is kept.
    /// @return What the store holds, or an empty memo when it holds nothing readable.
    [[nodiscard]] static ReachabilityMemo Load(IMemoStore const& store);

    /// @return The memo as text: a header line, then one `<token> <unix-ms> <endpoint>`
    ///         line per entry.
    [[nodiscard]] std::string Serialize() const;

    /// Write the memo to @p store, only when something was noted or forgotten since it
    /// was loaded -- a launcher that learned nothing writes nothing.
    /// @param store Where it is kept.
    void SaveIfChanged(IMemoStore& store);

    /// How old an entry is. The one place its age is computed.
    /// @param entry The entry.
    /// @param now The current wall-clock time.
    /// @return `FromTheFuture` for a stamp after @p now, `Expired` at or past the kind's
    ///         TTL, `Fresh` otherwise.
    [[nodiscard]] static MemoAge AgeOf(MemoEntry const& entry, std::chrono::system_clock::time_point now) noexcept;

    /// Whether @p endpoint is remembered as unreachable right now.
    /// @param kind Which kind of entry.
    /// @param endpoint The endpoint, byte for byte.
    /// @param now The current wall-clock time.
    /// @return True only for a `Fresh` entry.
    [[nodiscard]] bool Remembers(MemoKind kind, std::string_view endpoint, std::chrono::system_clock::time_point now) const;

    /// Every endpoint of one kind that is remembered right now.
    /// @param kind Which kind of entry.
    /// @param now The current wall-clock time.
    /// @return The `Fresh` endpoints, newest first.
    [[nodiscard]] std::vector<std::string> Fresh(MemoKind kind, std::chrono::system_clock::time_point now) const;

    /// Remember that @p endpoint could not be reached at @p now.
    ///
    /// Drops that kind's entries that are no longer fresh, moves an existing entry for the
    /// same endpoint rather than duplicating it, and keeps at most the kind's capacity,
    /// evicting the oldest stamp first.
    /// @param kind Which kind of entry.
    /// @param endpoint The endpoint that was not reached.
    /// @param now The current wall-clock time.
    void Note(MemoKind kind, std::string_view endpoint, std::chrono::system_clock::time_point now);

    /// Stop remembering @p endpoint, because it was reached.
    /// @param kind Which kind of entry.
    /// @param endpoint The endpoint that answered.
    void Forget(MemoKind kind, std::string_view endpoint);

    /// Fold what one dispatch observed into the memo.
    ///
    /// Records the scheduler only when the lease dial reached NOTHING and it was the
    /// CONFIGURED scheduler that was dialled -- a leader that one redirected to is not the
    /// configured one, and the configured one answered. Forgets it once a lease exchange
    /// completes. Records a worker nothing reached, and forgets one that compiled.
    /// @param result What the dispatch returned.
    /// @param configuredScheduler The scheduler this launcher is configured with.
    /// @param now The current wall-clock time.
    void Absorb(DispatchResult const& result,
                std::string_view configuredScheduler,
                std::chrono::system_clock::time_point now);

    /// @return True when something was noted or forgotten since the memo was loaded.
    [[nodiscard]] bool Changed() const noexcept;

    /// @return Every entry, fresh or not, in no promised order.
    [[nodiscard]] std::span<MemoEntry const> Entries() const noexcept;

  private:
    std::vector<MemoEntry> _entries;
    bool _changed { false };
};

/// Where this user's reachability memo lives: `reachability.memo` in the launcher's
/// per-user state directory (`StateDirectory()`, `%LOCALAPPDATA%\fastcache-cc` on
/// Windows), or an empty path when there is none -- which `FileMemoStore` reads as
/// "keep nothing".
///
/// **The state directory rather than `TEMP`, deliberately.** On POSIX `/tmp` is shared
/// between users, and a memo another user can write is a memo another user can switch
/// this user's distribution off with: one planted `scheduler-unreached` line, refreshed,
/// and every compile of this user's goes local. The state directory is this user's own.
/// It is also the location the test fixtures already isolate per run --
/// `scripts/dist-compile-e2e.sh` and `scripts/cluster-e2e.sh` export `XDG_STATE_HOME`,
/// and `scripts/dist-compile-e2e.ps1` sets `LOCALAPPDATA`, to a fresh directory while
/// every run shares `TEMP` -- so two runs never read each other's memo.
/// @return The memo file's path, or empty when nothing may be persisted.
[[nodiscard]] std::filesystem::path ReachabilityMemoPath();

/// What a launcher reports when it compiles locally because the memo remembers the
/// configured scheduler as unreachable, rather than dialling it again.
inline constexpr std::string_view RememberedUnreachableReason = "the scheduler was unreachable moments ago";

/// What a dispatch the memo skipped is recorded as: the fleet unreachable, under a fixed
/// reason naming the memo, so `--show-stats` can tell a remembered outage from a fresh one.
/// @return The recording.
[[nodiscard]] constexpr DispatchRecording RememberedUnreachableRecording() noexcept
{
    return DispatchRecording { .reason = RememberedUnreachableReason, .outcome = DispatchOutcome::Unreachable };
}

} // namespace FastCache::Cc
