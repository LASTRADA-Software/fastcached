// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Prometheus metric kind. Counters are monotonic; gauges may go up or down.
enum class MetricType : std::uint8_t
{
    Counter,
    Gauge,
};

/// The kind's name, as the exposition format spells it.
/// @param type The metric kind.
/// @return "counter" or "gauge".
[[nodiscard]] constexpr std::string_view TypeName(MetricType type) noexcept
{
    return type == MetricType::Gauge ? "gauge" : "counter";
}

/// Everything that is true of a counter regardless of its value.
///
/// ## Why this table exists
///
/// A counter used to be described in two unrelated places: the
/// `IMetricsSink::Counter` enumerator, and — only if somebody remembered — a row
/// in `PrometheusFormatter`'s render table giving its exported name, help text
/// and kind. Nothing tied the two together, and a counter present in one but not
/// the other is not a build error. It is a counter that increments and is never
/// exported.
///
/// That is not hypothetical, and it was not a corner case. **Seven of the eleven
/// live counters were missing from that table**: both TLS splits, and all five
/// `dispatch_*` — which `docs/getting-started/distributed-compilation.md`
/// documents as the things to read off `/metrics` when distribution misbehaves,
/// with a table naming each one. An operator working the commonest setup failure
/// ("no worker matches this toolchain") followed that guide to an endpoint that
/// had never carried a single one of those series.
///
/// So the row is the definition. The renderer walks this table, and
/// `CoversEveryCounter()` is `static_assert`ed below — a counter added to the enum
/// without a row here fails to compile, which is the only version of this that
/// cannot drift again.
struct CounterDescriptor
{
    IMetricsSink::Counter counter {};        ///< The enumerator this row describes.
    std::string_view prometheusName;         ///< Fully-qualified exported name.
    std::string_view help;                   ///< One-line `# HELP` text.
    MetricType type { MetricType::Counter }; ///< `# TYPE`.
};

/// One row per `IMetricsSink::Counter`, in enumerator order.
///
/// The order IS load-bearing: `DescriptorOf` indexes, so row *i* has to be the row
/// for enumerator *i*. `RowsInEnumeratorOrder` is what enforces that, and writing
/// the rows out in the enum's own order is what makes a missing one visible to a
/// reader as well.
inline constexpr EnumTable<IMetricsSink::Counter, CounterDescriptor> CounterTable { {
    { .counter = IMetricsSink::Counter::ConnectionsTotal,
      .prometheusName = "fastcached_connections_total",
      .help = "Connections accepted since start.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ConnectionsAdmissionRejected,
      .prometheusName = "fastcached_connections_rejected_total",
      .help = "Connections refused by admission control.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ConnectionsTotalTls,
      .prometheusName = "fastcached_connections_total_tls",
      .help = "Subset of connections_total that arrived on a TLS-flagged bind; "
              "plaintext traffic is the difference between the two.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ConnectionsAdmissionRejectedTls,
      .prometheusName = "fastcached_connections_rejected_tls",
      .help = "Subset of connections_rejected that arrived on a TLS-flagged bind.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesGranted,
      .prometheusName = "fastcached_dispatch_leases_granted_total",
      .help = "Lease requests answered with a worker; work is being distributed.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesReleased,
      .prometheusName = "fastcached_dispatch_leases_released_total",
      .help = "Leases a client resolved when its job ended. Subtracted from the "
              "granted count this is what is outstanding; flat while that one "
              "climbs means clients are dying mid-job.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesNoWorker,
      .prometheusName = "fastcached_dispatch_leases_no_worker_total",
      .help = "Lease requests refused because no registered worker matched the "
              "toolchain: the fleet is misconfigured rather than busy.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesNoCapacity,
      .prometheusName = "fastcached_dispatch_leases_no_capacity_total",
      .help = "Lease requests refused because every matching worker was full of "
              "this fleet's own work: the fleet is too small. Never sum this with "
              "no_worker or withdrawn.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesWithdrawn,
      .prometheusName = "fastcached_dispatch_leases_withdrawn_total",
      .help = "Lease requests refused because matching workers had slots free and "
              "had withdrawn them: their machines are busy elsewhere or out of "
              "scratch space. The fleet is big enough and unavailable.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesDuplicate,
      .prometheusName = "fastcached_dispatch_leases_duplicate_total",
      .help = "Lease requests refused because another client already held a lease "
              "for this key; duplicate-work suppression, not a failure.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchWorkerRegistrations,
      .prometheusName = "fastcached_dispatch_worker_registrations_total",
      .help = "Worker registrations accepted. A steady rise means heartbeats are "
              "not arriving.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed,
      .prometheusName = "fastcached_dispatch_worker_registrations_malformed_total",
      .help = "Worker registrations refused because the worker did not name its "
              "toolchain, endpoint or version in UTF-8. Any rise names a peer that "
              "is not in the fleet and cannot say so itself.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchWorkerEndpointMismatch,
      .prometheusName = "fastcached_dispatch_worker_endpoint_mismatch_total",
      .help = "Worker registrations accepted whose endpoint names a host other than "
              "the one they connected from. Expected wherever workers advertise DNS "
              "names, are multi-homed, or reach the scheduler over NAT or a VPN; a "
              "rise is not by itself a fault.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchWorkersExpired,
      .prometheusName = "fastcached_dispatch_workers_expired_total",
      .help = "Workers dropped for having stopped heartbeating. Rising beside a "
              "rising registration count is a fleet whose heartbeats are not "
              "arriving, not one that is growing.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesReclaimed,
      .prometheusName = "fastcached_dispatch_leases_reclaimed_total",
      .help = "Leases freed because the worker holding them was dropped. Work "
              "nobody will report done; every one of these keys was being refused "
              "as in-flight until it was reclaimed.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesUnauthorized,
      .prometheusName = "fastcached_dispatch_leases_unauthorized_total",
      .help = "Releases refused because the token was not signed by this cluster. "
              "Never sum with unknown-lease refusals: those name a lease this "
              "scheduler issued and has forgotten, this one was never issued.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsStarted,
      .prometheusName = "fastcache_worker_jobs_started_total",
      .help = "Compiles this worker began. Minus jobs_completed_total, the number "
              "running right now.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsCompleted,
      .prometheusName = "fastcache_worker_jobs_completed_total",
      .help = "Compiles that finished, whatever the compiler concluded. Also the "
              "count half of the compile-time sum below.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerCompileMillisTotal,
      .prometheusName = "fastcache_worker_compile_milliseconds_total",
      .help = "Total wall time spent compiling. Divide by jobs_completed_total, or "
              "take rate() of both, for the average compile.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint,
      .prometheusName = "fastcache_worker_jobs_refused_unknown_fingerprint_total",
      .help = "Jobs refused because no compiler here matches the client's "
              "toolchain fingerprint: the fleet is misconfigured.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedRejectedArgument,
      .prometheusName = "fastcache_worker_jobs_refused_rejected_argument_total",
      .help = "Jobs refused over an argument this worker will not pass to a "
              "compiler.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedScratchUnavailable,
      .prometheusName = "fastcache_worker_jobs_refused_scratch_unavailable_total",
      .help = "Jobs refused because the scratch directory could not be prepared: "
              "a full or read-only disk, not a client or fleet problem.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedSpawnFailed,
      .prometheusName = "fastcache_worker_jobs_refused_spawn_failed_total",
      .help = "Jobs refused because the compiler could not be spawned: the "
              "toolchain this worker advertises is not usable here.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedNoSlot,
      .prometheusName = "fastcache_worker_jobs_refused_no_slot_total",
      .help = "Jobs refused because every slot was busy: this worker is too "
              "small, or the fleet is. Never sum with the refusals above.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedNotAMember,
      .prometheusName = "fastcache_worker_jobs_refused_not_a_member_total",
      .help = "Connections refused because the caller is neither on this machine "
              "nor a cluster member. A rise means something is trying to spend a "
              "machine it has no claim on.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy,
      .prometheusName = "fastcache_worker_jobs_refused_endpoint_busy_total",
      .help = "Jobs refused because the payloads already being read fill this "
              "worker's in-flight byte budget. Distinct from no_slot: slots were "
              "free and memory was not, so more slots would not have helped.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeMalformed,
      .prometheusName = "fastcache_worker_jobs_refused_envelope_malformed_total",
      .help = "Jobs refused because the request's codec envelope could not be "
              "parsed, or an uncompressed one disagreed with the bytes beside it: "
              "a version skew, or a peer not speaking this protocol.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeUnsupportedCodec,
      .prometheusName = "fastcache_worker_jobs_refused_envelope_unsupported_codec_total",
      .help = "Jobs refused because the payload is in a codec this build cannot "
              "decode: two honest processes packaged differently. Every one cost a "
              "local compile.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeDeclaredTooLarge,
      .prometheusName = "fastcache_worker_jobs_refused_envelope_declared_too_large_total",
      .help = "Jobs refused because the envelope declared it expands past this "
              "endpoint's ceiling, before a byte was decompressed. Nothing honest "
              "declares that by accident: read it as a probe or a mis-set client.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeCorrupt,
      .prometheusName = "fastcache_worker_jobs_refused_envelope_corrupt_total",
      .help = "Jobs refused because the payload did not expand to its declared "
              "size. The only envelope refusal that implicates the transport: a "
              "codec version skew, or a link damaging payloads.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized,
      .prometheusName = "fastcache_worker_jobs_refused_lease_unauthorized_total",
      .help = "Jobs refused because the lease was not signed by this cluster. A "
              "security signal, not a capacity one -- or a launcher predating "
              "signed leases, which presents a token that cannot authenticate.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseEndpointMismatch,
      .prometheusName = "fastcache_worker_jobs_refused_lease_endpoint_mismatch_total",
      .help = "Jobs refused because an authentic lease named a different worker. "
              "Usually a registered endpoint that is not the one clients dial, not "
              "a replay.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseExpired,
      .prometheusName = "fastcache_worker_jobs_refused_lease_expired_total",
      .help = "Jobs refused because an authentic lease had expired. A rise on one "
              "machine and nowhere else is that machine's clock, not the fleet's "
              "leases.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerScratchRootsReclaimed,
      .prometheusName = "fastcache_worker_scratch_roots_reclaimed_total",
      .help = "Scratch roots taken over from a node that exited without cleaning up. "
              "A rise means nodes are dying rather than stopping; the work itself is "
              "correct, because a root is only reclaimed once its owner's lock is free.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerBytesReceived,
      .prometheusName = "fastcache_worker_bytes_received_total",
      .help = "Request payload bytes read from clients.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerBytesReturned,
      .prometheusName = "fastcache_worker_bytes_returned_total",
      .help = "Reply payload bytes written back to clients.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheHits,
      .prometheusName = "fastcache_node_cache_hits_total",
      .help = "Objects served from this node's own tier, without touching the network.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheMisses,
      .prometheusName = "fastcache_node_cache_misses_total",
      .help = "Objects this node's own tier did not hold.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheUpstreamHits,
      .prometheusName = "fastcache_node_cache_upstream_hits_total",
      .help = "Objects the shared cache answered after a local miss. High against a low "
              "local hit rate means the local tier is too small for this machine's working "
              "set -- a different problem from a fleet that is missing a lot.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheFillFailures,
      .prometheusName = "fastcache_node_cache_fill_failures_total",
      .help = "Values the shared cache supplied that the local tier refused. Costs a future "
              "round trip rather than a build; a sustained rate means the tier is "
              "misconfigured and silently doing nothing.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheStoreFailures,
      .prometheusName = "fastcache_node_cache_store_failures_total",
      .help = "Local writes that failed. Unlike a fill failure this one is reported to the "
              "client: it is the write that must not be lost.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheUpstreamStores,
      .prometheusName = "fastcache_node_cache_upstream_stores_total",
      .help = "Objects this node offered to the shared cache and it accepted.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheUpstreamStoreFailures,
      .prometheusName = "fastcache_node_cache_upstream_store_failures_total",
      .help = "Objects the shared cache would not take. Best-effort by contract -- the local "
              "write already succeeded -- so this says the FLEET is unreachable, not that "
              "this node is broken. Zero on a node with no shared cache at all, which "
              "fastcache_node_upstream_configured is what distinguishes.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal,
      .prometheusName = "fastcache_node_cache_requests_refused_not_local_total",
      .help = "Cache requests refused because the caller is not on this machine. A node's tier "
              "is this machine's build output and is served to this machine only, whatever the "
              "surface is bound to and whatever the member list says. Stays at zero on the "
              "default loopback bind; a rise means something off-box is asking.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::SchedulerRequestsRefusedUnauthenticated,
      .prometheusName = "fastcache_scheduler_requests_refused_unauthenticated_total",
      .help = "Scheduler requests refused because the connection presented no "
              "accepted credential. Zero while no --scheduler-token-file is set, "
              "because there is then nothing to fail -- so zero here on a "
              "non-loopback bind means the port is open, not that it is quiet.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::KeyspaceReclaimEventsDropped,
      .prometheusName = "fastcached_keyspace_reclaim_events_dropped_total",
      .help = "Reclaimed keys whose expired/evicted keyspace event was never published, because "
              "one call reclaimed more at once than the notification buffer holds. Without this, "
              "a subscriber seeing no event cannot tell that from nothing having expired.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ExpiryCycles,
      .prometheusName = "fastcached_expiry_cycles_total",
      .help = "Sweeps the active expiry cycle has run. Flat on a daemon serving traffic means the "
              "cycle is disabled or wedged, which otherwise looks exactly like nothing having expired.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ExpiryKeysReclaimed,
      .prometheusName = "fastcached_expiry_keys_reclaimed_total",
      .help = "Entries the active expiry cycle reclaimed -- keys that lapsed and that nothing would "
              "have touched again, so no other path would ever have freed them.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheMalformedValues,
      .prometheusName = "fastcached_cache_malformed_values_total",
      .help = "Stored values that did not decode as the type their flags claim -- a client wrote "
              "bytes that are not a well-formed set or stream. NOT a disk signal: the store is "
              "intact and every record still verifies. These were reported as Corrupt until #296, "
              "which moved fastcached_write_errors_total and told operators their disk had failed.",
      .type = MetricType::Counter },
} };

// Checked at compile time rather than by a test, because the failure this prevents
// is a counter that exports nothing — which no test asserts the absence of unless
// somebody thinks to write one for the counter they just added, and that is
// precisely the step that was missed.
static_assert(RowsInEnumeratorOrder(CounterTable, &CounterDescriptor::counter),
              "CounterTable must hold one row per IMetricsSink::Counter, in enumerator order");

/// The row describing `counter`.
///
/// `Last` is not a metric and has no row; it yields nullptr rather than a
/// placeholder, so a caller that reached here with it fails visibly.
/// @param counter The counter to look up.
/// @return Its descriptor, or nullptr for `Last` or an out-of-range value.
[[nodiscard]] constexpr CounterDescriptor const* DescriptorOf(IMetricsSink::Counter counter) noexcept
{
    auto const index = static_cast<std::size_t>(counter);
    if (index >= CounterTable.size())
        return nullptr;
    return &CounterTable[index];
}

} // namespace FastCache
