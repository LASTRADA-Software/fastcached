// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FastCache::Distributed
{

/// One outstanding authorization to compile a key on a worker.
struct Lease
{
    std::string token;    ///< Presented by the client to the worker.
    std::string workerId; ///< The worker the job was assigned to.
    std::string key;      ///< The object key being compiled.
};

/// One outstanding lease, as a diagnostic rather than as an authorization.
///
/// Deliberately not a `Lease`: that type carries the **token**, which is what a
/// client presents to a worker, and a report of it is read back out by an
/// operator, printed on a page and served as JSON. The two facts an operator acts
/// on are which key is held and by whom, and the token is neither.
///
/// The age is a duration rather than the `TimePoint` it came from, and that is the
/// injected clock defending itself -- the same rule `WorkerReport` states. Handed
/// an instant, the obvious thing for a consumer to do is subtract
/// `steady_clock::now()` from it, which is right in production and silently wrong
/// under every `ManualClock` test, because the two clocks agree about nothing.
struct LeaseReport
{
    std::string key;                  ///< The object key being compiled.
    std::string workerId;             ///< The worker it was leased to.
    std::chrono::milliseconds age {}; ///< Since the lease was taken.
};

/// What is outstanding, as one answer.
///
/// The bounded listing and the total travel together because they are **one
/// sample under one lock**. Asked separately they can disagree -- a lease resolved
/// between the two calls makes a window look like the whole set, or the whole set
/// look like a window -- and the truncation notice a reader relies on is then
/// wrong in exactly the situation it exists for.
struct LeaseListing
{
    /// The oldest live leases, ordered by descending age, at most as many as asked.
    std::vector<LeaseReport> oldest;
    /// How many are live in total, whether or not they are listed above.
    std::size_t total { 0 };
};

/// The set of leases the scheduler has issued and not yet seen resolved.
///
/// **Pure with respect to I/O**, over an injected `IClock`, for the reason
/// `WorkerRegistry` is: expiry is the whole behaviour here and testing it against
/// wall-clock sleeps would be both slow and flaky.
///
/// ## What a lease is for
///
/// Two things, and they are worth separating because only the first is obvious.
///
/// **It authorizes a job.** A worker must not compile whatever arrives on its
/// port; it compiles what the scheduler said to. The token is the scheduler's
/// signature on one job, which is what lets a worker refuse a client that reached
/// it directly without going through scheduling at all.
///
/// **It suppresses duplicate work.** When sixty parallel clients miss the same key
/// after a header change — which is the ordinary shape of a miss on a shared cache,
/// not an exotic one — only the first should be dispatched. The other fifty-nine
/// are told `AlreadyInFlight` and compile locally. That is strictly better than
/// dispatching sixty identical jobs, and it is something neither distcc nor
/// sccache-dist can do, because neither is also the cache.
///
/// ## Why leases expire
///
/// A client can die between taking a lease and sending the job — `Ctrl-C` on a
/// build is the common case, not a rare one. Without expiry that key would be
/// marked in-flight forever and every later compile of it would be refused, so a
/// single interrupted build would permanently un-distribute one translation unit.
/// Expiry is what makes the suppression safe to have at all.
class LeaseTable
{
  public:
    /// @param clock Time source; must outlive the table.
    /// @param leaseTimeout How long a lease may go unresolved before it is
    ///        reclaimed. Must comfortably exceed the slowest compile in the fleet:
    ///        reclaiming early does not abort the running job, it merely lets a
    ///        second client dispatch the same work.
    explicit LeaseTable(IClock& clock, std::chrono::milliseconds leaseTimeout = DefaultLeaseTimeout) noexcept;

    /// Default lease lifetime.
    ///
    /// Derived rather than written, because the launcher's dispatch deadline is the
    /// same number seen from the other end and the two were a pair of literals
    /// coupled only by prose (#249). `CompileCacheWire.hpp` is where it lives: it is
    /// the one header both ends include, since `fastcache-cc` does not link this
    /// library. The reasoning for the value is there too, stated once.
    static constexpr std::chrono::milliseconds DefaultLeaseTimeout = CompileCacheWire::DefaultCompileLeaseTimeout;

    /// How long a lease this table issues stays live.
    ///
    /// Exposed so a signed grant's expiry can be derived from it rather than
    /// written beside it. The two would otherwise be a pair of literals that have
    /// to agree forever, and the way they fail to is silent: a token outliving its
    /// lease is a capability nothing has any record of, and one dying first is a
    /// worker refusing work the scheduler is still suppressing the key for.
    /// @return The lifetime this table was constructed with.
    [[nodiscard]] std::chrono::milliseconds Timeout() const noexcept
    {
        return _leaseTimeout;
    }

    /// Result of asking for a lease.
    enum class Outcome : std::uint8_t
    {
        Granted,        ///< The lease is yours; `Acquire` returned a token.
        AlreadyInFlight ///< Another client holds a live lease for this key.
    };

    /// Why a release resolved nothing.
    ///
    /// Three enumerators because `Release` has three distinct ways of resolving
    /// nothing, and they used to be one `nullopt`. The scheduler above answered all
    /// three with one wire code and no counter at all, so the single condition worth
    /// telling an operator about -- a `leaseTimeout` shorter than the slowest
    /// translation unit, which this type's own `Release` documentation already named
    /// as such -- had nowhere to be observed
    /// ([#1074](https://github.com/LASTRADA-Software/fastcached/issues/1074)).
    ///
    /// **Every enumerator names what was OBSERVED, never the cause inferred from
    /// it**, and that is deliberate rather than fussy. The mapping from observation
    /// to cause is not one-to-one: a token from a scheduler instance that no longer
    /// exists arrives as `UnknownToken` when this instance has not yet reissued its
    /// number and as `KeyMismatch` when it has, so an enumerator called
    /// `SchedulerRestarted` would be a claim this table cannot make. That is the
    /// `KnownSchedulerTerm` lesson from the same subsystem: a monotonic term standing
    /// in for replay protection reported a legitimate reset as an attack, and the
    /// correction was to report the observation and name the causes beside it. A
    /// confident wrong signal is worse than a vague right one.
    ///
    /// The consequence is what makes the counter policy above decidable at all: only
    /// `Expired` has a single cause, so only `Expired` is worth counting.
    enum class ReleaseRefusal : std::uint8_t
    {
        /// The caller's own lease, past its lifetime.
        ///
        /// Present under this token, naming this key, and outside the timeout. The
        /// one unambiguous outcome of the three: whatever the client was doing, it
        /// held the lease longer than the fleet's bound allows, which is exactly the
        /// evidence a site needs before it can argue that bound is too short. Nothing
        /// else reaches here -- a second release found no entry, and another
        /// instance's token names another key.
        Expired,

        /// No entry under this token.
        ///
        /// Four causes share it, which is why it is not counted: an ordinary second
        /// release of one token (the first erased the entry), a token from a
        /// scheduler instance that no longer exists whose number this one has not yet
        /// reissued, a number that was never issued at all -- on a fleet with no
        /// cluster key, where the token is the bare serial -- and a lease
        /// `ReleaseWorker` reclaimed out from under a client that was still
        /// compiling.
        ///
        /// **That last one is live today and is the reason this row must stay
        /// uncounted even if the others were somehow separated.** `ExpireStale` drops
        /// a worker that has stopped heartbeating and reclaims what was held against
        /// it, so its client resolves a token naming a lease nobody has any more --
        /// through no fault of its timing. Counting it beside `Expired` would move a
        /// number an operator reads as *the lease bound is too short* in exactly the
        /// direction that claim points, for an event that says nothing of the kind.
        /// It cannot happen by construction rather than by anyone remembering:
        /// `ReleaseWorker` ERASES the entry, and `Expired` is reachable only for one
        /// still present and naming this key.
        UnknownToken,

        /// An entry under this token, naming another key.
        ///
        /// `_nextToken` restarts at one with the process while tokens outlive it, so
        /// the reachable cause is a client holding a number from a previous instance
        /// that this one has since reissued for different work. A client naming the
        /// wrong key for its own token reaches here too. Refusing rather than
        /// resolving is what stops the release freeing somebody else's live lease.
        KeyMismatch,

        Last, ///< Not a refusal, and has no row: the length of a table keyed by one.
    };

    /// Take a lease on `key` for `workerId`, unless one is already outstanding.
    /// @param key The object key about to be compiled.
    /// @param workerId The worker the job will go to.
    /// @return The lease when granted; nullopt when another client holds one.
    [[nodiscard]] std::optional<Lease> Acquire(std::string_view key, std::string_view workerId);

    /// Look up a lease by its token, without consuming it.
    ///
    /// Used by a worker to check that the job it was handed was actually scheduled.
    /// Deliberately does not consume: the worker validates before it starts, and the
    /// job is only resolved when it finishes, so consuming here would release the
    /// key while the compile was still running.
    /// @param token The token the client presented.
    /// @return The lease, or nullopt when unknown or expired.
    [[nodiscard]] std::optional<Lease> Find(std::string_view token) const;

    /// Resolve a lease, however the job ended.
    ///
    /// The caller states the **key** as well as the token, and a mismatch resolves
    /// nothing. A token is a small integer minted by this table, and `_nextToken`
    /// starts again at one in a table that has just been constructed -- so a client
    /// reporting a job it began before the scheduler restarted would otherwise
    /// resolve whatever lease the new instance had since issued under the same
    /// number, freeing a key somebody is building. Naming both is what makes a
    /// release resolve the caller's own lease or nothing, and it is checked here
    /// rather than above because this is the layer that decides atomically.
    ///
    /// An **expired** token counts as already gone, and reports so rather than
    /// answering as though it had freed something: the key it named stopped being
    /// suppressed when the lifetime ran out, and the client saying otherwise has a
    /// job that outlived its lease -- which is the one condition worth telling an
    /// operator about, since it means `leaseTimeout` is shorter than their slowest
    /// translation unit. The entry is dropped either way; nothing else ever visits
    /// an expired token but an `Acquire` for the same key.
    ///
    /// **Which of the three it was is part of the answer**, and it did not used to
    /// be. This returned a bare `nullopt` for all three, so the sentence above --
    /// naming one of them as the condition worth reporting -- described something no
    /// caller could act on, and the scheduler above duly left every release refusal
    /// uncounted (#1074). An outcome that has several causes is an enum rather than
    /// an empty optional, which is the same rule `NoUpstream` was fixed under: a
    /// `bool` that has to carry *not attempted* alongside *failed* reports one as
    /// the other.
    /// @param token The token.
    /// @param key The object key the caller believes it holds that token on.
    /// @return The lease that was released, or which of the three ways it resolved
    ///         nothing.
    [[nodiscard]] std::expected<Lease, ReleaseRefusal> Release(std::string_view token, std::string_view key);

    /// Release every lease held against a worker.
    ///
    /// Called when a worker is dropped. Without it, a worker dying mid-job would
    /// leave its keys marked in-flight until each lease expired, and every client
    /// that missed on one would be refused in the meantime — the fleet losing a
    /// machine would quietly stop distributing part of the build.
    /// @param workerId The worker.
    /// @return How many LIVE leases were released. An expired entry held against
    ///         the worker is swept too, and not counted: it had stopped suppressing
    ///         its key already, so reporting it would overstate what this achieved.
    [[nodiscard]] std::size_t ReleaseWorker(std::string_view workerId);

    /// Whether somebody is already compiling `key` right now.
    ///
    /// Exists so a caller can ask about the *key* before it has committed to a
    /// *worker*, which `Acquire` cannot answer because it needs the worker id in
    /// order to issue. That ordering is not a detail: asking for a worker first
    /// means a duplicate request arriving at a full fleet is refused for capacity,
    /// and an operator reads "buy more machines" where the truth is "this build
    /// asked for the same object twice".
    ///
    /// Advisory rather than a reservation, and deliberately so: the answer can go
    /// stale between this call and `Acquire`. `Acquire` is still the one that
    /// decides, atomically, so a race costs the caller a refusal it would have got
    /// anyway -- never a second lease on one key.
    /// @param key The object key.
    /// @return True when a live lease is outstanding for it.
    [[nodiscard]] bool IsInFlight(std::string_view key) const;

    /// What is outstanding: the oldest few of them, and how many there are.
    ///
    /// A count alone is the wrong grain for the moment it matters: a fleet that has
    /// stopped making progress shows a number, and what an operator needs is
    /// *which* keys are held and by whom. Since a lease is resolved by the client
    /// that took it, one still outstanding after minutes is a client that died
    /// mid-build whose worker is still heartbeating -- a specific machine to go and
    /// look at, rather than "something is happening".
    ///
    /// **Oldest first, and that is the diagnostic rather than an ordering
    /// preference.** A fleet at full tilt holds thousands, so any listing is
    /// bounded; the newest fifty of three thousand would answer nothing, while the
    /// oldest are the ones that have stopped moving.
    /// @param limit How many entries to return at most; zero returns the total and
    ///        no entries, which is how a caller asks the cheap question.
    /// @return The listing. Expired entries are excluded from both halves even
    ///         while they are still present in the table.
    [[nodiscard]] LeaseListing LiveLeases(std::size_t limit) const;

  private:
    struct Entry
    {
        Lease lease;
        TimePoint issuedAt {};
    };

    /// Whether `entry` is still within its lease lifetime.
    [[nodiscard]] bool IsLive(Entry const& entry, TimePoint now) const noexcept;

    /// Drop one entry from both maps.
    ///
    /// One implementation rather than one per caller: the key index must be erased
    /// only when it still points at *this* token, and two copies of that guard is
    /// how one of them comes to evict the client that replaced an expired lease.
    /// Caller holds `_mutex`.
    /// @param entry The token entry to remove; invalidated by the call.
    void Forget(std::unordered_map<std::string, Entry>::iterator entry);

    IClock& _clock;
    std::chrono::milliseconds _leaseTimeout;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, Entry> _byToken;          ///< Guarded by _mutex.
    std::unordered_map<std::string, std::string> _tokenByKey; ///< Guarded by _mutex.
    std::uint64_t _nextToken { 1 };                           ///< Guarded by _mutex.
};

} // namespace FastCache::Distributed
