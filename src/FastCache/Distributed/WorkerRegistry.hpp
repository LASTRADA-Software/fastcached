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
};

/// One registered worker, as the registry sees it.
struct WorkerInfo
{
    std::string id;            ///< Assigned at registration; opaque to the worker.
    std::string fingerprint;   ///< Toolchain identity, matched byte-for-byte.
    std::string endpoint;      ///< host:port a client can reach it on.
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

/// What a worker announces about itself.
///
/// A struct rather than four positional parameters: two of them are strings that
/// would be transposable at a call site, and a fifth field is a foreseeable change.
struct WorkerRegistration
{
    std::string_view fingerprint;     ///< Toolchain identity.
    std::string_view endpoint;        ///< host:port clients should use.
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
    /// @param workerId The worker.
    void JobStarted(std::string_view workerId);

    /// Note that a job on a worker has finished, however it finished.
    /// @param workerId The worker.
    void JobFinished(std::string_view workerId);

    /// Drop a worker outright (it said goodbye, or answered nothing).
    /// @param workerId The worker.
    void Remove(std::string_view workerId);

    /// Every live worker, for `/metrics` and diagnostics.
    /// @return A snapshot; expired workers are excluded.
    [[nodiscard]] std::vector<WorkerInfo> LiveWorkers() const;

  private:
    struct Entry
    {
        WorkerInfo info;
        TimePoint lastSeen {};
    };

    /// Whether `entry` has been heard from recently enough to dispatch to.
    [[nodiscard]] bool IsLive(Entry const& entry, TimePoint now) const noexcept;

    IClock& _clock;
    std::chrono::milliseconds _heartbeatTimeout;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, Entry> _workers; ///< Guarded by _mutex.
    std::uint64_t _nextId { 1 };                     ///< Guarded by _mutex.
};

} // namespace FastCache::Distributed
