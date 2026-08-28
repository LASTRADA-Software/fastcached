// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FastCache::Distributed
{

/// Why a worker could not be picked.
///
/// Three reasons rather than one "no", because they are three different
/// operator problems and the client reports the scheduler's own words: an empty
/// fleet for a toolchain is a configuration mistake, a busy fleet is a capacity
/// one, and a duplicate is neither. All three end the same way at the client —
/// compile locally — so this type never has to express a failure.
enum class PickError : std::uint8_t
{
    NoWorker,   ///< Nothing registered with this fingerprint (or all expired).
    NoCapacity, ///< Every matching worker is full of this fleet's own work.
    /// Matching workers have slots free on paper, and have withdrawn them: their
    /// machines are busy with something else, or out of scratch space.
    ///
    /// Split from `NoCapacity` for the same reason `NoCapacity` is split from
    /// `NoWorker`: they are different operator problems with opposite fixes. This
    /// one says "your machines are doing something else" -- somebody is using them,
    /// or a disk has filled -- while `NoCapacity` says "buy more machines". Folding
    /// this into that one would send an operator shopping for hardware they already
    /// own, and hide a fleet-wide full disk behind a number that looks like growth.
    Withdrawn,
    Last, ///< Not a refusal, and has no row: the length of a table keyed by one.
};

/// One registered worker, as the registry sees it.
struct WorkerInfo
{
    std::string id;          ///< Assigned at registration; opaque to the worker.
    std::string fingerprint; ///< Toolchain identity, matched byte-for-byte.
    std::string endpoint;    ///< host:port a client can reach it on.
    /// What software this node is running, as it stated at registration.
    ///
    /// Beside the other registration strings rather than inside `NodeCapacity`,
    /// and that is not filing: `NodeCapacity` is a **literal type**, exercised by
    /// `constexpr` tests over `OfferableSlots` and the slot ceilings, and a
    /// `std::string` in it would quietly end that for the whole codebase. It is
    /// also not a scheduling input -- nothing weighs it -- which is exactly what
    /// `NodeCapacity` holds.
    ///
    /// Empty means the node did not say, which on this field is a fact rather than
    /// an omission: a peer built before the field existed cannot report a version,
    /// and a fleet part-way through an upgrade is when somebody is reading it.
    std::string version {};
    std::uint32_t slots {};    ///< Concurrent jobs it will accept in general.
    std::uint32_t inFlight {}; ///< Jobs currently outstanding on it.

    /// What the machine is, as it stated at registration.
    NodeCapacity capacity {};

    /// What it said it was doing at its last heartbeat.
    ///
    /// Held so `Pick` can weigh it. Kept beside `inFlight` rather than replacing it
    /// because the two have different authors: the registry maintains `inFlight`
    /// itself as leases are taken and returned, while this arrives whole from the
    /// worker and is only as fresh as the last heartbeat.
    NodeLoad load {};
    /// Compression codec ids this worker can DECODE, most-preferred first.
    ///
    /// Kept here so a lease can relay them: the client is about to send this worker
    /// a multi-megabyte preprocessed translation unit and has to pick a codec for
    /// it. Held as raw ids rather than the wire header's alias, so the registry
    /// keeps no dependency on the protocol layer.
    std::vector<std::uint8_t> codecs;
};

/// One live worker, as a diagnostic rather than as a scheduling input.
///
/// `WorkerInfo` deliberately carries no timestamp: it is what `Pick` returns and
/// what a lease is built from, and an age frozen inside a lease is a number that
/// stops meaning anything the moment it is stored. An age is a property of an
/// *observation*, so it lives on the report of one.
///
/// The age is a duration rather than the `TimePoint` it came from, and that is the
/// injected clock defending itself: handed a `TimePoint`, the obvious thing for a
/// consumer to do is subtract `steady_clock::now()` from it -- which is right in
/// production and wrong under every `ManualClock` test, silently, because the two
/// clocks agree about nothing. Subtracting inside the registry means the answer
/// comes from the clock the registry was given.
struct WorkerReport
{
    WorkerInfo info;                           ///< What the worker said about itself.
    std::chrono::milliseconds heartbeatAge {}; ///< Since it registered or last heartbeat.
};

/// One live MACHINE, grouped as an operator means "node".
///
/// This registry keys on `(fingerprint, endpoint)`, so a node started with two
/// `--toolchain` flags is two entries describing **one machine** -- deliberately,
/// because a lease is per toolchain. Both entries then carry that machine's cores,
/// its memory, its CPU reading and its cache, so anything summing those across
/// `LiveWorkers()` counts one machine once per toolchain it serves: a two-toolchain
/// node doubles the fleet's core count, and a fleet page is exactly the consumer
/// that would.
///
/// Which fields add across sibling entries and which do not is the decision this
/// type exists to make once, rather than in every consumer's memory:
///
///   - **`capacity`, `load` and `cache` do not add.** They describe the machine,
///     and are taken from a single contributing entry, chosen by the rule below.
///   - **`registeredSlots` is the maximum**, not the sum: the siblings describe one
///     machine's offer, and `OfferableSlots` derived each from the same cores.
///   - **`fleetJobsInFlight` is the maximum too**, for the same reason and one
///     more. A node runs ONE `WorkerServer` for every toolchain it serves, so the
///     only job count that exists anywhere is machine-wide: its heartbeat samples
///     `InFlight()` once per round and sends that one number to every registrar.
///     Summing it reported a machine running four jobs as running eight, and the
///     page that read it then had four slots of the operator's own work to account
///     for and blamed them on an external ceiling.
///
/// A per-toolchain job count is not merely unimplemented here; there is nowhere
/// for one to come from. `JobStarted`/`JobFinished` therefore move every sibling
/// entry together, so the speculative count and the authoritative one that
/// corrects it are the same quantity rather than two grains folded by one rule.
struct NodeReport
{
    /// host:port the node answers on — the key an operator means by "node".
    std::string endpoint;
    /// Every toolchain this machine serves, sorted so a snapshot is reproducible.
    std::vector<std::string> fingerprints;
    /// What the machine is, from the contributing entry.
    NodeCapacity capacity {};
    /// What it is doing, from the contributing entry.
    ///
    /// Its machine-wide fields — CPU, memory, scratch, cache — are what this grain
    /// is for. Its `inFlight` is one entry's copy of a figure that is machine-wide
    /// at every writer, so it agrees with `fleetJobsInFlight` below rather than
    /// describing a narrower thing; the two are separate only because this one is
    /// whatever the *contributing* entry last carried, and that one is folded
    /// across all of them.
    NodeLoad load {};
    /// The largest slot count any of this machine's entries registered with.
    std::uint32_t registeredSlots { 0 };
    /// This fleet's jobs running on the machine.
    ///
    /// The maximum across its entries, not the sum: every writer of the underlying
    /// count is machine-wide, so adding siblings reports one machine's work once
    /// per toolchain it serves. See the class doc above.
    std::uint32_t fleetJobsInFlight { 0 };
    /// Since the contributing entry was last heard from.
    std::chrono::milliseconds heartbeatAge {};
    /// What software the machine runs, from the contributing entry. Empty means it
    /// did not say -- see `WorkerInfo::version`.
    std::string version {};
};

/// One node's cache, as `NodeCaches()` reports it.
///
/// A machine rather than a registry entry, which is the whole reason this type
/// exists: a node with two `--toolchain` flags is two entries carrying one
/// cache's figures, so a consumer reading them off `LiveWorkers()` would count
/// that cache once per toolchain.
struct NodeCacheReport
{
    /// host:port the node answers on — the key an operator means by "node".
    std::string endpoint;
    /// What it was configured to hold, as stated at registration.
    NodeCacheCapacity capacity {};
    /// What it holds now, as of its last heartbeat.
    NodeCacheLoad load {};
    /// Since the contributing entry was last heard from.
    ///
    /// So a consumer can tell "this cache is empty" from "this node stopped
    /// answering an hour ago and these are its last figures" -- which look
    /// identical without it, and lead to opposite conclusions.
    std::chrono::milliseconds heartbeatAge {};
};

/// What a worker announces about itself.
///
/// A struct rather than four positional parameters: two of them are strings that
/// would be transposable at a call site, and a fifth field is a foreseeable change.
struct WorkerRegistration
{
    std::string_view fingerprint;     ///< Toolchain identity.
    std::string_view endpoint;        ///< host:port clients should use.
    std::string_view version {};      ///< What software the node runs; empty means it did not say.
    std::uint32_t slots {};           ///< Concurrent job limit it asks for; 0 to derive.
    std::vector<std::uint8_t> codecs; ///< What it can decode.

    /// What the machine is, as far as scheduling onto it is concerned.
    ///
    /// The registry applies `OfferableSlots` to it, so a workstation's reserve is
    /// held back HERE rather than being left to each worker to subtract for itself.
    /// A worker that got that arithmetic wrong would advertise more of a developer's
    /// machine than the operator allowed, and nothing downstream could tell.
    NodeCapacity capacity {};
};

/// The set of live compile workers, grouped by toolchain.
///
/// **Pure with respect to I/O**: it never opens a socket, spawns anything, or
/// reads a clock of its own — time comes from the injected `IClock`, which is
/// what makes expiry testable against `ManualClock` instead of against `sleep`.
/// The whole scheduling policy therefore has unit tests and no fixtures.
///
/// Thread-safe: several reactor threads answer registrations and leases
/// concurrently.
///
/// ## The matching rule, which is the whole point
///
/// A job may only go to a worker whose fingerprint is **byte-identical** to the
/// client's. Not "compatible", not "same major version" — identical. The cache
/// key already depends on the compiler identity, so this is the invariant the
/// cache needs anyway; distribution just makes the consequence of getting it
/// wrong worse. A cache miss from an over-strict match costs a local compile,
/// while an over-loose match produces a **silently wrong object** that is then
/// stored under a key other machines will fetch. There is no symmetry between
/// those two errors, so the match is exact and no configuration can loosen it.
///
/// ## Why most-headroom rather than round-robin, or least-outstanding
///
/// Compile times vary by an order of magnitude within one build — a header-heavy
/// template instantiation against a small C file. Round-robin queues a 40-second
/// translation unit behind another 40-second one while a worker sits idle, because
/// it distributes *arrivals* rather than *load*.
///
/// Least-outstanding fixes that and introduces its own error, which is that it
/// treats every machine as an identical box: a 64-slot server running 8 jobs looks
/// busier than a 4-slot laptop running 2, when the server has 56 slots free and the
/// laptop has none. Across a fleet of mixed machines — the ordinary case in a
/// peer-to-peer fleet, not an exotic one — that sends work to the smallest machines
/// first and leaves the big ones idle. So the comparison is on **free slots**, with
/// utilization breaking the tie, and what counts as a free slot is
/// `AvailableSlots` rather than the registered count: a machine somebody else is
/// using, or one whose disk has filled, has fewer than it registered with.
///
/// It still needs no history and no estimate of how long a job will take, which is
/// fortunate because the scheduler has neither.
class WorkerRegistry
{
  public:
    /// @param clock Time source for heartbeat expiry; must outlive the registry.
    /// @param heartbeatTimeout How long a worker may go unheard-from before it is
    ///        treated as gone. A worker that dies mid-job otherwise holds its slots
    ///        forever, and the fleet quietly shrinks to nothing with no diagnostic.
    explicit WorkerRegistry(IClock& clock, std::chrono::milliseconds heartbeatTimeout = DefaultHeartbeatTimeout) noexcept;

    /// Default grace period before an unheard-from worker is dropped.
    ///
    /// Generous relative to the heartbeat interval a worker should use, because the
    /// cost of the two errors is asymmetric: expiring a live worker early costs a
    /// dispatch that falls back to a local compile, while keeping a dead one costs
    /// every client that leases it a timeout first.
    static constexpr std::chrono::milliseconds DefaultHeartbeatTimeout { 90'000 };

    /// Register a worker, or refresh one that is re-registering.
    ///
    /// Re-registration is keyed on `(fingerprint, endpoint)` rather than on the
    /// issued id: a worker that restarts has lost its id but is the same host and
    /// the same toolchain, and treating it as new would leak the old entry until it
    /// expired — during which half the leases for that toolchain would be sent to a
    /// port nothing is listening on.
    /// @param registration What the worker announced.
    /// @return The worker's id.
    [[nodiscard]] std::string Register(WorkerRegistration const& registration);

    /// Record a heartbeat and the worker's own view of its load.
    ///
    /// The worker's count is authoritative over the registry's, and deliberately:
    /// the registry's counter drifts whenever a client dies between leasing and
    /// compiling, and only the worker knows what it is actually running. A
    /// heartbeat is therefore also a correction.
    /// @param workerId The id from `Register`.
    /// @param load What the worker reports about itself, its job count included.
    /// @return False when the id is unknown (the worker should re-register).
    [[nodiscard]] bool Heartbeat(std::string_view workerId, NodeLoad const& load);

    /// Pick the least-loaded live worker whose fingerprint matches exactly.
    ///
    /// Does **not** reserve the slot: the caller pairs this with a lease, and the
    /// lease is what accounts for the slot. Separating them keeps this function a
    /// pure query over the fleet, and keeps the accounting in the one place that
    /// also knows how to expire it.
    /// @param fingerprint The toolchain the client is compiling with.
    /// @return The chosen worker, or why none could be chosen.
    [[nodiscard]] std::expected<WorkerInfo, PickError> Pick(std::string_view fingerprint) const;

    /// Note that a job has been dispatched to a worker.
    ///
    /// Moves every entry sharing that worker's endpoint, because a job occupies the
    /// **machine** and not one of its toolchains — see `NodeReport`. Counting it
    /// against the leased entry alone let the scheduler offer a host's other
    /// toolchain a full complement of slots it was already using.
    /// @param workerId The worker.
    void JobStarted(std::string_view workerId);

    /// Note that a job on a worker has finished, however it finished.
    ///
    /// Machine-wide, exactly as `JobStarted` is; the pair has to move the same
    /// entries or a node's count drifts by a toolchain per job.
    /// @param workerId The worker.
    void JobFinished(std::string_view workerId);

    /// Drop a worker outright (it said goodbye, or answered nothing).
    /// @param workerId The worker.
    void Remove(std::string_view workerId);

    /// Every live worker, for `/metrics` and diagnostics.
    /// @return A snapshot; expired workers are excluded.
    [[nodiscard]] std::vector<WorkerInfo> LiveWorkers() const;

    /// Every live worker, with how long ago each was heard from.
    ///
    /// The diagnostic counterpart of `LiveWorkers()`, kept apart from it because an
    /// age has no business on the value `Pick` hands to a lease.
    /// @return A snapshot ordered by worker id; expired workers are excluded.
    [[nodiscard]] std::vector<WorkerReport> LiveWorkerReports() const;

    /// Every live NODE, one entry per machine.
    ///
    /// The deduped view of the whole registry, and the one a fleet-wide total must
    /// be computed over: see `NodeReport` for which fields add across a machine's
    /// sibling entries and which do not.
    /// @return One entry per live endpoint, ordered by endpoint.
    [[nodiscard]] std::vector<NodeReport> NodeReports() const;

    /// Every live NODE's cache, one entry per machine.
    ///
    /// `LiveWorkers()` lists registry entries, and this registry keys on
    /// `(fingerprint, endpoint)` — so a node started with two `--toolchain` flags
    /// is two entries describing one machine, deliberately. A cache is per node,
    /// not per toolchain, and both entries heartbeat the *same* cache figures, so
    /// anything summing a cache field across `LiveWorkers()` counts one node's
    /// objects and bytes once per toolchain it serves.
    ///
    /// This is the deduped view, and it exists so that fact lives in one place
    /// rather than in every consumer's memory. Grouping is by endpoint, which is
    /// what an operator means by "node": the entries share it precisely because
    /// they are one machine.
    ///
    /// The entries of one node do not always agree, which is why only one of them
    /// contributes and which one is a real choice: `Register` clears a worker's
    /// load, so a sibling that has just re-registered holds nothing while the
    /// other still holds last round's figures. An entry that has reported a cache
    /// wins over one that has not, and among those the most recently heard from.
    /// @return One entry per live endpoint, ordered by endpoint so a snapshot is
    ///         reproducible — the property `LiveWorkers()` sorts for.
    [[nodiscard]] std::vector<NodeCacheReport> NodeCaches() const;

  private:
    struct Entry
    {
        WorkerInfo info;
        TimePoint lastSeen {};
    };

    /// Whether `entry` has been heard from recently enough to dispatch to.
    [[nodiscard]] bool IsLive(Entry const& entry, TimePoint now) const noexcept;

    /// Which way `AdjustMachineInFlight` moves a machine's count.
    ///
    /// An enumeration rather than a signed delta: only two values are meaningful,
    /// and a parameter typed to admit four billion of them says so at every call
    /// site — the reason this codebase spells a two-way choice as a type.
    enum class JobTransition : std::uint8_t
    {
        Started,  ///< One more job on the machine.
        Finished, ///< One fewer, saturating at zero.
    };

    /// Move the in-flight count of every entry sharing a worker's endpoint.
    ///
    /// One implementation for both directions: `JobStarted` and `JobFinished`
    /// differ by that direction and by nothing else, and two copies of "find the
    /// endpoint, then walk its siblings" is how the pair comes to move different
    /// entries. Caller holds `_mutex`.
    /// @param workerId The worker the job was leased to; its endpoint selects the
    ///        entries to move.
    /// @param transition Which way to move them.
    void AdjustMachineInFlight(std::string_view workerId, JobTransition transition);

    /// How long ago an entry was last heard from, clamped at zero.
    ///
    /// Static, because it reads no member: the clock has already been asked, once,
    /// by the caller holding the lock. Asking it per entry would let one snapshot
    /// report ages measured against different instants.
    /// @param lastSeen When the entry last registered or heartbeat.
    /// @param now The instant the whole snapshot is measured against.
    /// @return The age; never negative, even under a clock set backwards.
    [[nodiscard]] static std::chrono::milliseconds AgeOf(TimePoint lastSeen, TimePoint now) noexcept;

    IClock& _clock;
    std::chrono::milliseconds _heartbeatTimeout;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, Entry> _workers; ///< Guarded by _mutex.
    std::uint64_t _nextId { 1 };                     ///< Guarded by _mutex.
};

} // namespace FastCache::Distributed
