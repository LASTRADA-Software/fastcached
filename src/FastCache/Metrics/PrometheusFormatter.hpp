// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace FastCache
{

/// Strongly-typed process uptime. A thin box around `std::chrono::seconds` so
/// call sites read `Uptime { 42s }` rather than passing a bare, unit-ambiguous
/// integer that needs a `/* uptimeSeconds */` comment to be legible.
struct Uptime
{
    std::chrono::seconds value { 0 };
};

/// The size of the machine, as a scrape reports it.
///
/// A struct rather than four `IMetricsSink` counters, because none of these is a
/// count of anything: they are gauges, and this interface is counter-only by
/// design. Bundled with the storage snapshot for the same reason that one is —
/// the renderer takes one value per scrape and the server needs no collaborators
/// of its own.
struct HostCapacity
{
    std::size_t logicalCores { 0 };       ///< Schedulable hardware threads.
    std::size_t totalMemoryBytes { 0 };   ///< Physical memory, or the container's ceiling.
    std::uintmax_t diskCapacityBytes { 0 }; ///< Size of the filesystem the work happens on.
    std::uintmax_t diskFreeBytes { 0 };     ///< What an unprivileged process may still write.
    std::size_t configuredSlots { 0 };      ///< Concurrent compiles this node advertises.

    /// Compiles running right now.
    ///
    /// A gauge, and the one number here that moves. Sampled at scrape time rather
    /// than derived from `WorkerJobsStarted - WorkerJobsCompleted`, because those
    /// two counters are incremented by different components and a scrape landing
    /// between them would report a phantom job — the difference is right on
    /// average and wrong at exactly the moment somebody is looking.
    std::size_t busySlots { 0 };
};

/// Everything a `/metrics` scrape needs that varies per call: the storage
/// snapshot plus the process uptime. Produced by the admin server's snapshot
/// provider so the renderer takes a single bundle and the server itself needs
/// no clock.
struct MetricsSnapshot
{
    /// The cache's own statistics, absent in a process that has no cache.
    ///
    /// Optional rather than a default-constructed `StorageStats`, because a
    /// worker has no storage and zeroes are not the truth about it: a scrape
    /// reporting `fastcached_items 0` and `fastcached_bytes_limit 0` says the
    /// cache is empty and unbounded, which an alert or a dashboard will read as
    /// a fact rather than as an absence. `fastcache-compile-node` runs the same
    /// `AdminHttpServer` and shares this renderer, so the alternative was a
    /// second renderer that drifts from this one.
    std::optional<StorageStats> storage;

    /// What this machine is and how much room it has, absent when the process has
    /// no reason to say.
    ///
    /// A worker's capacity is the thing a fleet operator most wants off a scrape —
    /// "is this node pulling its weight" is unanswerable without knowing how big
    /// it is — and it is what PR 8's resource-aware scheduling will weigh. The
    /// daemon leaves it absent: it is not a compile node, and reporting cores it
    /// does not schedule against would be noise.
    std::optional<HostCapacity> host;

    Uptime uptime {};
};

/// Render the Prometheus text exposition format (version 0.0.4) for the given
/// connection-level counters and storage statistics.
///
/// Pure and free of I/O so it can be unit-tested directly. Command-level
/// counters (gets, sets, hit/miss splits, evictions, capacity) come from the
/// storage snapshot — the authoritative source — while connection-level
/// counters come from the metrics sink. Each metric is emitted with its
/// `# HELP` / `# TYPE` lines followed by the `fastcached_<name> <value>` sample.
///
/// @param metrics Connection-level counter sink.
/// @param snapshot Per-scrape storage stats and process uptime.
/// @return A complete metrics body in Prometheus text exposition format.
[[nodiscard]] std::string RenderPrometheus(IMetricsSink const& metrics, MetricsSnapshot const& snapshot);

} // namespace FastCache
