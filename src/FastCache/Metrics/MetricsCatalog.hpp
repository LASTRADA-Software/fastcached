// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <array>
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
    { .counter = IMetricsSink::Counter::DispatchLeasesAllExcluded,
      .prometheusName = "fastcached_dispatch_leases_all_excluded_total",
      .help = "Lease requests refused because every matching worker was on the client's "
              "exclusion list: the machines exist and the clients cannot reach them. "
              "Answered no-worker; never sum this with no_worker.",
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
    { .counter = IMetricsSink::Counter::DispatchWorkersWithdrawn,
      .prometheusName = "fastcached_dispatch_workers_withdrawn_total",
      .help = "Registrations a worker retired deliberately. Distinct from the expiry "
              "counter beside it: a withdrawal is a machine that re-surveyed and said "
              "so, an expiry is one that stopped answering.",
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
    { .counter = IMetricsSink::Counter::DispatchLeasesReleasedLate,
      .prometheusName = "fastcached_dispatch_leases_released_late_total",
      .help = "Releases that arrived after their own lease had expired: a real "
              "compile outran the lease timeout. Not a count of leases that "
              "expired, since a client that never reports back reaches this "
              "nowhere. Read as a fraction of released_total; a steady fraction "
              "means the lease bound is shorter than this site's slowest "
              "translation unit.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchFramesRefusedUnsupportedVersion,
      .prometheusName = "fastcached_dispatch_frames_refused_unsupported_version_total",
      .help = "Frames refused at the fleet scheduler's port for naming a protocol "
              "version this build does not serve: a peer built against another "
              "release.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchFramesRefusedUnknownOpcode,
      .prometheusName = "fastcached_dispatch_frames_refused_unknown_opcode_total",
      .help = "Frames naming an opcode this build has no row for, at the fleet "
              "scheduler's port.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchFramesRefusedNotPermitted,
      .prometheusName = "fastcached_dispatch_frames_refused_not_permitted_total",
      .help = "Frames naming a verb the fleet scheduler does not serve, typically a "
              "cache verb sent to the scheduler's port. Any rise names a client "
              "pointed at the wrong address.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchFramesRefusedTruncated,
      .prometheusName = "fastcached_dispatch_frames_refused_truncated_total",
      .help = "Frames at the fleet scheduler's port whose declared payload length did "
              "not match what arrived: a framing or transport fault, before any verb "
              "is routed. Never sum with malformed_payload -- they share a wire code "
              "and nothing else, and the scheduler's own malformed-frame refusal (a "
              "payload it could not parse) is deliberately uncounted.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsStarted,
      .prometheusName = "fastcache_worker_jobs_started_total",
      .help = "Compiles this worker handed to its runner. NOT the running count when "
              "differenced with jobs_completed_total: a refused job increments this "
              "one only, so the difference drifts up and never returns. Use the "
              "fastcache_node_slots_busy gauge for what is running now.",
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
    { .counter = IMetricsSink::Counter::WorkerJobsAbandonedClientGone,
      .prometheusName = "fastcache_worker_jobs_abandoned_client_gone_total",
      .help = "Compiles whose client had disconnected before the object could be "
              "written back. The compile itself still counts in jobs_completed_total "
              "-- the compiler ran and this machine paid for it; only the delivery "
              "found nobody there. Never a refusal: nothing was declined and no reply "
              "was sent.",
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
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedCompilerUnclassified,
      .prometheusName = "fastcache_worker_jobs_refused_compiler_unclassified_total",
      .help = "Jobs refused because this worker cannot classify the compiler its "
              "own --toolchain names: it ran, and this build does not recognise "
              "which driver it is. Not a path fault -- see "
              "fastcache_worker_jobs_refused_spawn_failed_total for that one.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedSurveyInFlight,
      .prometheusName = "fastcache_worker_jobs_refused_survey_in_flight_total",
      .help = "Jobs refused because this worker had not finished identifying its "
              "toolchains: it is still starting, not misconfigured. Rises only "
              "from clients dialling this port directly -- the scheduler is not "
              "offered this worker until the survey completes.",
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
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedStopping,
      .prometheusName = "fastcache_worker_jobs_refused_stopping_total",
      .help = "Jobs refused because this worker had begun stopping. Never sum with "
              "no_slot: that one says the fleet is too small, this one says a node is "
              "draining and a retry will land somewhere else.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedCordoned,
      .prometheusName = "fastcache_worker_jobs_refused_cordoned_total",
      .help = "Jobs refused because an operator cordoned this worker. Never sum with "
              "stopping: a stop ends by itself, a cordon lasts until somebody lifts it. "
              "A steady rise says the scheduler is not hearing this worker's heartbeat.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerCordonsRefusedNotLocal,
      .prometheusName = "fastcache_worker_cordons_refused_not_local_total",
      .help = "Cordon requests refused because they came from another machine. A "
              "machine is cordoned from itself; a rise means somebody elsewhere is "
              "trying to take it out of the fleet.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnsupportedVersion,
      .prometheusName = "fastcache_worker_frames_refused_unsupported_version_total",
      .help = "Frames refused for naming a protocol version this build does not "
              "serve: a peer built against another release.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedTruncated,
      .prometheusName = "fastcache_worker_frames_refused_truncated_total",
      .help = "Frames shorter than the payload their own header declared: a framing "
              "or transport fault, or a peer sending nonsense. Never sum with "
              "malformed_payload -- they share a wire code and nothing else.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnknownOpcode,
      .prometheusName = "fastcache_worker_frames_refused_unknown_opcode_total",
      .help = "Frames naming an opcode this build has no row for.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnimplementedVerb,
      .prometheusName = "fastcache_worker_frames_refused_unimplemented_verb_total",
      .help = "Frames naming a verb that exists and is not served here, such as AUTH "
              "on a worker that checks no credential.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedNotPermitted,
      .prometheusName = "fastcache_worker_frames_refused_not_permitted_total",
      .help = "Frames naming a verb this node serves on another surface: a client "
              "reached the compile port with a cache or scheduler request.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedMalformedPayload,
      .prometheusName = "fastcache_worker_frames_refused_malformed_payload_total",
      .help = "Frames whose payload did not decode into the fields its verb "
              "requires: a version or encoding mismatch between two ends that agree "
              "on the framing.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedPayloadTooLarge,
      .prometheusName = "fastcache_worker_frames_refused_payload_too_large_total",
      .help = "Frames whose header declared a payload larger than this surface "
              "accepts, refused without being read. The cheapest probe there is: it "
              "needs only a header, where the envelope series needs a whole frame.",
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
      .help = "Jobs refused because the lease was not signed by any voter this worker's "
              "roster names. A security signal, not a capacity one -- or a launcher "
              "predating this lease format, which presents a token that cannot authenticate.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnregistered,
      .prometheusName = "fastcache_worker_jobs_refused_lease_unregistered_total",
      .help = "Jobs refused because this worker has not registered and so knows no fleet. "
              "A few at startup are ordinary; a rise that does not stop means the scheduler "
              "is unreachable and this node is compiling nothing while looking alive.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseWrongCluster,
      .prometheusName = "fastcache_worker_jobs_refused_lease_wrong_cluster_total",
      .help = "Jobs refused because an authentic lease was issued by a different fleet. "
              "Every grant is signed by a voter's own identity key, so a rise means one "
              "key votes in two clusters -- a --cluster-dir copied to a second site, which "
              "copies the node and its key with it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseReplayed,
      .prometheusName = "fastcache_worker_jobs_refused_lease_replayed_total",
      .help = "Jobs refused because an authentic lease had already been spent at this "
              "worker. A lease is single-use, so an honest client never produces this; "
              "any rise is somebody replaying a captured grant.",
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
    { .counter = IMetricsSink::Counter::WorkerSchedulerTermRegressions,
      .prometheusName = "fastcache_worker_scheduler_term_regressions_total",
      .help = "Times this worker adopted a scheduler term that went backwards. Two causes "
              "look identical here: a grant minted before a leadership change and "
              "delivered after one, which is ordinary; or a scheduler that was reset. The "
              "rate tells them apart -- occasional counts tracking elections are the "
              "first, a sustained rise against no election is the second.",
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
    { .counter = IMetricsSink::Counter::NodeSharedCacheHits,
      .prometheusName = "fastcache_node_shared_cache_hits_total",
      .help = "Objects the fleet's shared tier on this machine answered.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheMisses,
      .prometheusName = "fastcache_node_shared_cache_misses_total",
      .help = "Objects the shared tier on this machine did not hold.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheStoreFailures,
      .prometheusName = "fastcache_node_shared_cache_store_failures_total",
      .help = "Objects the shared tier could not keep. A sustained rate is this machine's disk.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember,
      .prometheusName = "fastcache_node_shared_cache_requests_refused_not_a_member_total",
      .help = "Shared-cache requests from a caller that proved no key and presented no ticket "
              "the fleet admits.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotServing,
      .prometheusName = "fastcache_node_shared_cache_requests_refused_not_serving_total",
      .help = "Fleet cache verbs answered not-shared-cache: the setting names another machine, "
              "or this one and its tier is unavailable.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedPayloadTooLarge,
      .prometheusName = "fastcache_node_shared_cache_requests_refused_payload_too_large_total",
      .help = "Shared-cache requests declaring more than one object's ceiling.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedEndpointBusy,
      .prometheusName = "fastcache_node_shared_cache_requests_refused_endpoint_busy_total",
      .help = "Shared-cache requests refused because the surface's in-flight byte budget was "
              "spent.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedUnsupportedVersion,
      .prometheusName = "fastcache_node_shared_cache_requests_refused_unsupported_version_total",
      .help = "Shared-cache requests from a build of another wire version.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedMalformedPayload,
      .prometheusName = "fastcache_node_shared_cache_requests_refused_malformed_payload_total",
      .help = "Shared-cache requests whose payload would not decode.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedForeignGeneration,
      .prometheusName = "fastcache_node_shared_cache_requests_refused_foreign_generation_total",
      .help = "Shared-cache stores of a value generation this build does not implement.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey,
      .prometheusName = "fastcache_node_shared_cache_proofs_refused_wrong_key_total",
      .help = "The machine answering at the shared cache's announced address proved a key that "
              "is not the named machine's; nothing was sent.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheProofsFailed,
      .prometheusName = "fastcache_node_shared_cache_proofs_failed_total",
      .help = "The shared cache could not be reached, or did not complete the handshake; the "
              "build compiled locally.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheUnresolved,
      .prometheusName = "fastcache_node_shared_cache_unresolved_total",
      .help = "Operations skipped because the shared-cache setting names no machine this node "
              "can reach by key; --node-status says which reason.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheStaleHints,
      .prometheusName = "fastcache_node_shared_cache_stale_hints_total",
      .help = "The address this node's last proven session to the shared cache connected to "
              "failed, and the announced name was dialled instead.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSharedCacheSessionsOpened,
      .prometheusName = "fastcache_node_shared_cache_sessions_opened_total",
      .help = "Proven sessions this node opened to the shared cache. One per burst of misses is "
              "healthy; one per miss means the kept session is being lost.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal,
      .prometheusName = "fastcache_node_cache_requests_refused_not_local_total",
      .help = "Cache requests refused because the caller is not on this machine. A node's tier "
              "is this machine's build output and is served to this machine only, whatever the "
              "surface is bound to and whatever the member list says. The node port faces the "
              "network on every node, so this counts other machines: on a node clients are pointed "
              "at for the scheduler a rise is ORDINARY -- peers reaching the right host for the "
              "wrong verb -- and what an operator watches there is the shape of the curve rather "
              "than its existence; on any other node it means something off-box is asking for this "
              "machine's build output.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember,
      .prometheusName = "fastcache_node_status_requests_refused_not_a_member_total",
      .help = "Operator verbs refused because the caller is not a fleet member: node-status, "
              "node-metrics, and explain-admission asked about a MACHINE (asked about the caller's "
              "own connection it is answered to anyone). These report this node's version, "
              "uptime, components, its surface PORT MAP and where machines stand in its roster, "
              "which is why they are gated at all rather than being pre-auth. Unlike the cache "
              "tier's not-local refusal this is not ordinary on any deployment: an operator asking "
              "is either admitted or is being told to be, so a steady rise is fastcache-cli run "
              "from a machine the cluster has not admitted, or has forgotten, and a burst from one "
              "host is somebody scanning. Zero on a node nobody has asked, which is the "
              "common case and is not evidence the gate works.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedPayloadTooLarge,
      .prometheusName = "fastcache_node_status_requests_refused_payload_too_large_total",
      .help = "Operator verbs refused because the header declared more payload than they may "
              "carry, so nothing was read. node-status and node-metrics are FIELDLESS -- there is "
              "nothing to ask them with -- and their wire rows bound them to the control payload "
              "cap, so a header declaring more against either came from no client of this tree at "
              "any version. explain-admission carries one subject, a machine id or a 43-character "
              "key, under its own 512-byte ceiling, and is reachable before admission: a rise from "
              "it is an operand no roster could name, or a stranger probing the port. Never sum with "
              "fastcache_node_cache_requests_refused_payload_too_large_total: that one is what a "
              "launcher storing large objects produces and has an innocent explanation, and this "
              "one does not.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedEndpointBusy,
      .prometheusName = "fastcache_node_status_requests_refused_endpoint_busy_total",
      .help = "Operator verbs refused because the surface had no bytes left in flight. These are "
              "the verbs somebody reaches for when a node is in trouble, so this is the node "
              "answering that it is too busy to say what it is. It names the request that was "
              "refused, not the load that exhausted the budget -- on a node holding a tier that "
              "is the cache's traffic, the listener's ceiling folding to the largest owner's. "
              "Read beside fastcache_node_cache_requests_refused_endpoint_busy_total rather than "
              "summed with it: this says the diagnosis failed, that says why.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeAdmissionExplanationsRefusedMalformed,
      .prometheusName = "fastcache_node_admission_explanations_refused_malformed_total",
      .help = "explain-admission requests refused because the payload was not exactly one field "
              "naming its subject. No shipped client can build one: the CLI encodes this verb through "
              "EncodeExplainAdmissionRequest. So a rise is a client of another build or somebody "
              "probing the port by hand, and the two are told apart by whether anything else on "
              "this surface refuses at the same time. Kept apart from the node-status refusals, "
              "which are a fieldless verb and cannot arise from the same mistake.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedPayloadTooLarge,
      .prometheusName = "fastcache_node_cache_requests_refused_payload_too_large_total",
      .help = "Cache verbs refused because the header declared more payload than the surface "
              "will buffer, so nothing was read. The cheapest probe there is -- 24 bytes and no "
              "body -- and on a node holding a cache tier it is the frame ceiling that actually "
              "fires, because the listener asks the component owning the verb. Never sum with "
              "fastcache_worker_frames_refused_payload_too_large_total: both answer "
              "payload-too-large and they name different subsystems on one port.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedEndpointBusy,
      .prometheusName = "fastcache_node_cache_requests_refused_endpoint_busy_total",
      .help = "Cache verbs refused because the request would not fit in the bytes already in "
              "flight on this surface. A slot was free and the memory was not. This is the "
              "byte-budget refusal a node with a cache tier reaches, the listener's in-flight "
              "ceiling being the largest of the components present and that usually being the "
              "cache's. Never sum with fastcache_worker_jobs_refused_endpoint_busy_total or "
              "fastcache_node_frame_connections_refused_at_capacity_total, which share the "
              "endpoint-busy code and nothing else.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedUnsupportedVersion,
      .prometheusName = "fastcache_node_cache_requests_refused_unsupported_version_total",
      .help = "Cache requests refused because this build cannot decode the wire version they "
              "were sent at -- a client compiled against another release. Worth alerting on "
              "because the only other evidence is a cache that looks permanently cold: a "
              "launcher steps over a refused fetch and compiles locally, so the build stays "
              "correct and merely stops being fast.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedMalformedPayload,
      .prometheusName = "fastcache_node_cache_requests_refused_malformed_payload_total",
      .help = "Fetch or store bodies the cache tier could not decode, in frames whose declared "
              "length arrived in full. A client-library or version mismatch between two ends "
              "that agree on the framing, or somebody malforming bodies deliberately. Its own "
              "series rather than any other malformed-frame counter, which describe a truncated "
              "compile frame, an undecodable compile payload and two AUTH payloads.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedForeignGeneration,
      .prometheusName = "fastcache_node_cache_requests_refused_foreign_generation_total",
      .help = "Stores refused because the value names a canonicalization generation this build "
              "does not implement -- the value-format twin of the unsupported-version series, "
              "and the same operator action: find the machine that is out of step. Zero unless "
              "a fleet is spanning a CompileValueVersion bump, so any rise is a real event. It "
              "is also the only view of what refusing costs: the launcher sees a miss and "
              "compiles locally, so the build stays correct and merely stops being fast.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeFrameConnectionsRefusedAtCapacity,
      .prometheusName = "fastcache_node_frame_connections_refused_at_capacity_total",
      .help = "Connections turned away because the node's 0xFC listener already holds "
              "every connection it will. Never sum with "
              "worker_jobs_refused_endpoint_busy: they share a wire code and nothing "
              "else -- that one says one request was too big right now, this says the "
              "surface has no room for another conversation.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeProofsAccepted,
      .prometheusName = "fastcache_node_proofs_accepted_total",
      .help = "Connections that PROVED which machine they are, under an identity key this cluster holds live, "
              "and are sealed from then on (#178). The positive half: every other node_proofs row counts a "
              "refusal, so a fleet where the proof is never taken reads the same on all of them as one where it "
              "works. Per exchange, never per verb.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeProofsRejected,
      .prometheusName = "fastcache_node_proofs_rejected_total",
      .help = "Node proofs whose signature does not verify under the key they presented: a client of another "
              "build, or somebody forging one. Asked before the roster, so it says nothing about which keys the "
              "cluster holds.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeProofsUnchallenged,
      .prometheusName = "fastcache_node_proofs_unchallenged_total",
      .help = "prove-node frames sent with no challenge outstanding on their connection: "
              "never asked for one, or already spent one. A client that has the exchange "
              "wrong, not a security signal -- kept apart from node_proofs_rejected so an "
              "old client cannot hide a forgery.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeProofsMalformed,
      .prometheusName = "fastcache_node_proofs_malformed_total",
      .help = "node-challenge and prove-node payloads that would not decode into their fixed-width fields: a "
              "version or client-library mismatch, kept apart from node_proofs_rejected so an old client "
              "cannot hide a forgery.",
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
    { .counter = IMetricsSink::Counter::CacheFramesRefusedUnsupportedVersion,
      .prometheusName = "fastcached_cache_frames_refused_unsupported_version_total",
      .help = "Frames refused at the daemon's compile-cache port for naming a protocol "
              "version this build does not serve.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheFramesRefusedUnknownOpcode,
      .prometheusName = "fastcached_cache_frames_refused_unknown_opcode_total",
      .help = "Frames naming an opcode this build has no row for, at the daemon's "
              "compile-cache port.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheFramesRefusedPayloadTooLarge,
      .prometheusName = "fastcached_cache_frames_refused_payload_too_large_total",
      .help = "Frames refused before their payload was read, for declaring more than "
              "the session cap or the verb's own ceiling allows.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheFramesRefusedUnauthenticated,
      .prometheusName = "fastcached_cache_frames_refused_unauthenticated_total",
      .help = "Verbs attempted on a connection that has not authenticated. A client "
              "that never learned it needs a credential, rather than one presenting a "
              "wrong credential.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedPayload,
      .prometheusName = "fastcached_cache_frames_refused_malformed_payload_total",
      .help = "Requests whose payload did not decode as the verb they named.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedCredential,
      .prometheusName = "fastcached_cache_frames_refused_malformed_credential_total",
      .help = "AUTH requests whose payload did not decode. Counted apart from ordinary "
              "malformed payloads because garbage aimed at the credential verb is what "
              "a scanner produces.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheCredentialsRejected,
      .prometheusName = "fastcached_cache_credentials_rejected_total",
      .help = "Credentials presented to the daemon's compile-cache port and refused. "
              "Any sustained rise names somebody guessing.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheStoresRefusedNotACompileValue,
      .prometheusName = "fastcached_cache_stores_refused_not_a_compile_value_total",
      .help = "STOREs refused because the value is not a compile value. Distinct from "
              "fastcached_cache_malformed_values_total, which is the memcached and "
              "Redis path's answer for a set or stream contradicting its own flags.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheStoresRefusedForeignGeneration,
      .prometheusName = "fastcached_cache_stores_refused_foreign_generation_total",
      .help = "STOREs naming a canonicalization generation this build does not "
              "implement. A rise names a rolling upgrade in progress; it stopping "
              "names one that finished.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::CacheStoresFailed,
      .prometheusName = "fastcached_cache_stores_failed_total",
      .help = "STOREs that reached the engine and could not be written: a full disk, a "
              "read-only mount, or a backend refusing writes. The only arm of this "
              "surface that is about this machine rather than its clients.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FrameRequestDeadlineSweeps,
      .prometheusName = "fastcache_frame_request_deadline_sweeps_total",
      .help = "Connections swept before the peer named a verb. A peer counted here "
              "connected and did not speak, so no honest client of this surface "
              "appears in it. Never sum with answer_deadline: that one is a request "
              "this node accepted outrunning its budget, this one is a knock at the "
              "door.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FrameAnswerDeadlineSweeps,
      .prometheusName = "fastcache_frame_answer_deadline_sweeps_total",
      .help = "Connections swept after the peer named a verb, meaning the answer "
              "outran the window that verb allows. For a compile that is a "
              "translation unit outliving its lease grant, so a rise is a question "
              "about the lease timeout rather than about this worker.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FrameDeadlineRefusalsSent,
      .prometheusName = "fastcache_frame_deadline_refusals_sent_total",
      .help = "Swept connections whose peer was sent a frame saying why. A second "
              "event, not a restatement of answer_deadline_sweeps: only a connection "
              "parked inside the surface can be told, because a connection parked on "
              "the socket is ended by the close and the close is the write side gone. "
              "The gap between the two is how many swept peers were left to infer it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FramePeerWatchDepartures,
      .prometheusName = "fastcache_frame_peer_watch_departures_total",
      .help = "Peers the watch saw leave while the connection could still act on it: "
              "the observation, where jobs_abandoned_client_gone is the decision. "
              "Subtract the second from the first for what was suppressed on purpose "
              "-- an empty reply, or a socket this node closed itself. A client that "
              "vanished and was NOT noticed is this row flat while the object was "
              "written anyway. It does not rise for a client that hangs up after "
              "reading its reply, which is every honest one.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FramePeerWatchDeparturesObserved,
      .prometheusName = "fastcache_frame_peer_watch_departures_observed_total",
      .help = "Every peer departure the watch reached, before either suppression: the "
              "denominator peer_watch_departures cannot supply for itself. That row "
              "flat is both the healthy reading and a watch that never ran; this one "
              "rising says the mechanism is live. Subtract to get what was suppressed "
              "on purpose -- an ordinary hang-up, or a socket this node closed. Never "
              "below peer_watch_departures. A watch that ended because the peer sent "
              "BYTES is a pipelined request and is in neither row.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FramePeerWatchDeparturesAbortive,
      .prometheusName = "fastcache_frame_peer_watch_departures_abortive_total",
      .help = "Of peer_watch_departures, the ones where the peer RESET rather than "
              "closing gracefully. A subset under the same suppressions, so it is "
              "never above that row and subtracting gives the graceful half. This is "
              "the half worth alerting on: a graceful mid-answer departure is a "
              "cancelled build or a reclaimed runner and its rate means nothing, while "
              "a rise here is crashing clients, a lost route, or a middlebox resetting "
              "long-lived connections.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedFull,
      .prometheusName = "fastcache_enrollment_requests_refused_full_total",
      .help = "Enrollment requests refused because the pending list was full, so nothing was "
              "recorded. Never sum with any endpoint-busy series: this is not momentary and the "
              "same request will go on being refused until a person decides something. The list "
              "refuses rather than evicting on purpose -- anybody may ask, so eviction would let a "
              "flooder push the real joiner off the list the operator is reading, which is silent "
              "from both ends. A rise is a rollout larger than the bound, fixed by approving in "
              "batches, or many hosts filling it, which --enroll-clear makes room from.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed,
      .prometheusName = "fastcache_enrollment_requests_refused_malformed_total",
      .help = "Enrollment bodies that arrived in full and would not decode. Its own series rather "
              "than any other malformed-frame counter, which describe a truncated compile frame, "
              "an undecodable compile payload, two AUTH payloads and the cache tier's bodies. A "
              "client of this tree sends two length-prefixed fields, so a body that splits into "
              "anything else came from no version of this software -- and the peer had presented "
              "nothing when it sent it, which is what makes this one worth reading. So did one asking "
              "under a small-order or non-canonical key, which no build mints and none may admit.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey,
      .prometheusName = "fastcache_enrollment_requests_refused_revoked_key_total",
      .help = "Enrollment requests refused because they asked under a key the cluster has revoked "
              "-- a machine an operator forgot, asking to come back as itself. Refused at the door "
              "rather than listed, because no approval could admit a revoked key. Expected once "
              "after forgetting a machine that is still running; a steady rate is a removed "
              "machine nobody stopped. It can come back only under a new identity.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedForged,
      .prometheusName = "fastcache_enrollment_requests_refused_forged_total",
      .help = "Enrollment requests refused because their signature does not verify under the key "
              "they ask with -- a request the key's holder did not make. A joiner's id and key are "
              "public, so anybody can poll under them; only the holder can sign. Not ordinary on any "
              "deployment: a rise is somebody polling under another machine's identity, or a joiner "
              "whose key file and stated key disagree.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentControlRefusedNotAMember,
      .prometheusName = "fastcache_enrollment_control_refused_not_a_member_total",
      .help = "Enrollment control verbs refused because the caller is not a fleet member -- "
              "somebody trying to approve themselves. The window admits strangers by design and "
              "the approval is what it withholds, so this refusal carries the whole security "
              "argument for the pair: a peer reaching it has found an open window and gone on to "
              "ask for the decision too. Not ordinary on any deployment. Never sum with "
              "fastcache_node_status_requests_refused_not_a_member_total, which shares the wire "
              "code and describes somebody asking a node what it is.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRostersServed,
      .prometheusName = "fastcache_enrollment_rosters_served_total",
      .help = "Rosters handed to an admitted joiner: a machine an operator approved by name, whose "
              "admission the leader's roster now records, told who else is in the cluster. No secret "
              "crosses with it -- the roster is every member's public key. During a rollout it rises "
              "about once per machine and stops.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey,
      .prometheusName = "fastcache_discovery_proofs_refused_unknown_key_total",
      .help = "Discovery proofs that verified under a key this node's roster does not record for "
              "the id they claimed. Reported by name, key and address in the log, and never desired "
              "onto the cluster: a machine to enroll, or one that was wiped and minted a new key.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DiscoveryProofsRefusedRevokedKey,
      .prometheusName = "fastcache_discovery_proofs_refused_revoked_key_total",
      .help = "Discovery proofs that verified under a key the roster has revoked: a machine that "
              "was removed from the cluster and is still announcing itself on the segment.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DiscoveryProofsRefusedForged,
      .prometheusName = "fastcache_discovery_proofs_refused_forged_total",
      .help = "Discovery proofs whose signature did not verify under the key they carried: whoever "
              "sent them does not hold that key. A healthy segment produces none.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DiscoveryBeaconsOverBound,
      .prometheusName = "fastcache_discovery_beacons_over_bound_total",
      .help = "Discovery beacons that reached a bound: the oldest entry nothing vouches for was displaced "
              "(an unrostered peer, a fleet that proved nothing), or a new fleet "
              "was dropped because every remembered one had proven itself. One series for every bound. A "
              "healthy segment produces none; a rise is somebody flooding the beacon port.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DiscoveryRepliesWithheld,
      .prometheusName = "fastcache_discovery_replies_withheld_total",
      .help = "Discovery challenges and proofs not sent: larger than the datagram that provoked them, or past "
              "the answer budget. A healthy segment produces almost none; a rise is somebody sending challenges "
              "by hand or flooding them.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DiscoveryProofChecksWithheld,
      .prometheusName = "fastcache_discovery_proof_checks_withheld_total",
      .help = "Discovery proofs answering a live challenge whose signature was not checked: past the check budget "
              "(one host's share or everybody's), or against a challenge that already failed as many checks as one "
              "buys. A healthy segment produces none; a rise is somebody sending forgeries against a challenge.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsOpened,
      .prometheusName = "fastcache_live_subscriptions_opened_total",
      .help = "Live-stats subscriptions this process granted. One per dashboard opened against it; a steady climb "
              "with no operator watching is a client re-subscribing in a loop.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSnapshotsRendered,
      .prometheusName = "fastcache_live_snapshots_rendered_total",
      .help = "Live-stats snapshots rendered. Once per subject per tick, however many subscribers are watching "
              "it: this rising with the NUMBER of watchers rather than with time is the render being paid per "
              "watcher, which is the cost a subscription exists to remove.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSnapshotsSkipped,
      .prometheusName = "fastcache_live_snapshots_skipped_total",
      .help = "Live-stats snapshots a subscriber was too slow to receive. The node sends the newest and tells "
              "the subscriber how many it skipped; it never waits for it. A rise is a watcher on a slow link or "
              "a stalled terminal, not a node problem.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsRevoked,
      .prometheusName = "fastcache_live_subscriptions_revoked_total",
      .help = "Live-stats streams ended because the peer no longer passes this node's gate -- a reload or a "
              "replicated removal revoked it. Removal fails open unless something re-checks, and this is that "
              "check acting.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsEndedNotLeader,
      .prometheusName = "fastcache_live_subscriptions_ended_not_leader_total",
      .help = "Live-stats fleet streams ended because this node stopped leading. Each watcher was told the new "
              "leader and follows it; a rise without an election is leadership flapping.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedAtCapacity,
      .prometheusName = "fastcache_live_subscriptions_refused_at_capacity_total",
      .help = "Live-stats subscriptions refused because this process already streams to as many watchers as it "
              "serves. A dashboard left open on every workstation reaches this long before a real team does.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedUnauthenticated,
      .prometheusName = "fastcache_live_subscriptions_refused_unauthenticated_total",
      .help = "Live-stats fleet subscriptions refused for a missing or wrong dashboard credential, or from a "
              "remote peer where no credential is configured. The fleet map is behind the dashboard credential "
              "over 0xFC exactly as it is over HTTP; a burst from one host is somebody guessing.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsStalled,
      .prometheusName = "fastcache_live_subscriptions_stalled_total",
      .help = "Live-stats streams ended because a push stayed unwritten past its bound: the watcher stopped "
              "reading altogether. The node drops the connection rather than hold its buffers.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsEndedByClient,
      .prometheusName = "fastcache_live_subscriptions_ended_by_client_total",
      .help = "Live-stats streams the watching client closed. The ordinary way a dashboard ends; read it "
              "against the opened count, where the difference is the streams still open.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsEndedByReset,
      .prometheusName = "fastcache_live_subscriptions_ended_by_reset_total",
      .help = "Live-stats streams whose watching client reset the connection instead of closing it. A "
              "dashboard killed with pushes still unread does this; counted apart from the orderly close.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember,
      .prometheusName = "fastcache_live_subscriptions_refused_not_a_member_total",
      .help = "Live-stats subscriptions refused because the peer is not a fleet member. Live stats stream to "
              "members only, as NodeStatus answers them; a burst from one host is a stranger probing the port.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedMalformed,
      .prometheusName = "fastcache_live_subscriptions_refused_malformed_total",
      .help = "Live-stats subscriptions whose request did not decode, or named a subject this build does not "
              "serve. No client of this tree sends one; a rise is a client of another build or no client at "
              "all.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedPayloadTooLarge,
      .prometheusName = "fastcache_live_subscriptions_refused_payload_too_large_total",
      .help = "Live-stats subscriptions whose header declared more than the control payload the verb is bounded "
              "to. Refused before a byte of it is read; no client of this tree at any version sends one.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedEndpointBusy,
      .prometheusName = "fastcache_live_subscriptions_refused_endpoint_busy_total",
      .help = "Live-stats subscriptions refused because the 0xFC listener's in-flight byte budget was full of "
              "other requests. The dashboard retries; a rise says the view was lost when the node was busiest.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedNotAMember,
      .prometheusName = "fastcache_fleet_text_requests_refused_not_a_member_total",
      .help = "Fleet document reads over 0xFC refused because the peer is not a fleet member. The fleet is read "
              "by members only, as a subscription to it is; a burst from one host is a stranger probing the port.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedUnauthenticated,
      .prometheusName = "fastcache_fleet_text_requests_refused_unauthenticated_total",
      .help = "Fleet document reads over 0xFC refused for a wrong or missing dashboard credential, or from a "
              "remote peer while no dashboard token file is configured. A burst from one host is somebody guessing.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedMalformed,
      .prometheusName = "fastcache_fleet_text_requests_refused_malformed_total",
      .help = "Fleet document reads whose request did not decode. No client of this tree sends one; a rise is a "
              "client of another build or no client at all.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedPayloadTooLarge,
      .prometheusName = "fastcache_fleet_text_requests_refused_payload_too_large_total",
      .help = "Fleet document reads whose header declared more than the control payload the verb is bounded to. "
              "Refused before a byte of it is read; no client of this tree at any version sends one.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedEndpointBusy,
      .prometheusName = "fastcache_fleet_text_requests_refused_endpoint_busy_total",
      .help = "Fleet document reads refused because the 0xFC listener's in-flight byte budget was full of other "
              "requests. A rise says an operator asked for the fleet when the leader was busiest.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedNoHandshake,
      .prometheusName = "fastcache_raft_peer_connections_refused_no_handshake_total",
      .help = "Raft peer connections closed because the first frame was not a handshake proof this build reads: a "
              "Raft message sent without one (a build from before the handshake), a proof at another wire version, "
              "one over its size ceiling, or bytes that are not this wire at all. The log line beside it names "
              "which and the address it came from. Nothing was read from the connection.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedHandshakeTimeout,
      .prometheusName = "fastcache_raft_peer_connections_refused_handshake_timeout_total",
      .help = "Raft peer connections closed because no proof arrived within the handshake bound. Before the "
              "handshake existed such a connection held a slot for as long as its socket lived; now it is closed "
              "and counted. A few is a slow or stalled peer; a steady rate from one address is something holding "
              "connections open on purpose.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedProof,
      .prometheusName = "fastcache_raft_peer_connections_refused_proof_total",
      .help = "Raft peer connections refused because the proof's signature did not verify under the key this node "
              "holds for the id it claims: another machine claiming that member's id, which only the member's own "
              "private key can prove. No verdict is sent. On a healthy fleet this is flat at zero, so any rise is "
              "an impersonation attempt or a member whose key file was replaced without re-admitting it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedWrongTarget,
      .prometheusName = "fastcache_raft_peer_connections_refused_wrong_target_total",
      .help = "Raft peer connections from a dialler that proved its id but dialled another member at this address: "
              "its record of where that member answers is stale, usually because a node moved or two swapped "
              "addresses. Refused with a signed verdict, so the dialler reports it by name rather than as a key "
              "problem. The log names both ids.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedOwnId,
      .prometheusName = "fastcache_raft_peer_connections_refused_own_id_total",
      .help = "Raft peer connections from a dialler that proved THIS node's own id, which only this node's private "
              "key can do: two machines hold one identity, which is a copied --cluster-dir. Refused with a signed "
              "verdict. Never ordinary; the address in the log is the second machine.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerFramesRefusedTag,
      .prometheusName = "fastcache_raft_peer_frames_refused_tag_total",
      .help = "Raft connections closed because a frame's tag did not verify under the connection's own session key: "
              "a frame changed, injected, replayed, reordered or carried over from another connection. A correct "
              "peer never produces one, so a rise is the network path or something on it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerFramesRefusedSender,
      .prometheusName = "fastcache_raft_peer_frames_refused_sender_total",
      .help = "Raft connections closed because a frame whose tag verified carried a message naming a sender other "
              "than the id its connection proved. Only the proven member can produce one, so a rise is a defect in "
              "a member rather than an attacker.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerFramesRefusedUnreadable,
      .prometheusName = "fastcache_raft_peer_frames_refused_unreadable_total",
      .help = "Raft connections closed on a frame whose tag verified and which this build cannot read: a wire "
              "version other than the one the handshake settled, or a message out of place. A steady count names a "
              "peer running a different build. The dialler counts its own direction as "
              "fastcache_raft_peer_dials_ended_frame_unreadable_total.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerFramesRefusedOverCap,
      .prometheusName = "fastcache_raft_peer_frames_refused_over_cap_total",
      .help = "Raft connections closed on a frame declaring more payload than this node buffers, refused before "
              "any of it was read.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerFramesRefusedBadMagic,
      .prometheusName = "fastcache_raft_peer_frames_refused_bad_magic_total",
      .help = "Raft connections closed on a frame whose header did not begin with this wire's magic, after which "
              "nothing on the connection can be framed.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedFull,
      .prometheusName = "fastcache_raft_peer_connections_refused_full_total",
      .help = "Raft peer connections closed on arrival because the listener already served as many as it holds. A "
              "cluster needs one per peer, so a rise is something opening connections it does not need -- the last "
              "thing a stranger can still do before proving an id, now that a silent connection is closed at the "
              "handshake bound.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedTimeout,
      .prometheusName = "fastcache_raft_peer_dials_refused_timeout_total",
      .help = "Raft dials abandoned because the acceptor sent no challenge, or no verdict, within the handshake "
              "bound. The ordinary cause is a peer running a build from before the handshake, which never sends "
              "one, or an address that is not a Raft port. Read beside the peer's own no_handshake series, which "
              "rises on the other machine for the same connection.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedNoChallenge,
      .prometheusName = "fastcache_raft_peer_dials_refused_no_challenge_total",
      .help = "Raft dials abandoned because the acceptor opened with something other than a challenge this build "
              "reads: another wire version, or a port that is not this protocol. The log names the version seen.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorProof,
      .prometheusName = "fastcache_raft_peer_dials_refused_acceptor_proof_total",
      .help = "Raft dials abandoned because the acceptor's verdict did not verify under the key this node holds for "
              "the member that answered: another machine answering under that id. Nothing was sent to it. On a "
              "healthy fleet flat at zero; a rise names an address that is not the member it claims to be.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedWrongTarget,
      .prometheusName = "fastcache_raft_peer_dials_refused_wrong_target_total",
      .help = "Raft dials refused, by a verified verdict, because the member answering at the address is not the "
              "one this node dialled: this node's record of that member's address is stale. The log names both. It "
              "clears when the replicated state or discovery re-addresses the member.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedOwnId,
      .prometheusName = "fastcache_raft_peer_dials_refused_own_id_total",
      .help = "Raft dials refused, by a verified verdict, because the acceptor proved this node's own id from this "
              "node: two machines hold one private key, a copied --cluster-dir.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsEndedByAcceptor,
      .prometheusName = "fastcache_raft_peer_dials_ended_by_acceptor_total",
      .help = "Raft dials the acceptor closed after this node sent its proof, without a signed verdict. The causes "
              "are the ones an acceptor cannot sign: it holds no key for this node's id, or a different one, or it "
              "refused the proof's shape or ran out of handshake time. Never a stale address, a shared identity or "
              "a revoked key, which arrive signed and have series of their own.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ClusterAdmissionsRefusedMalformedKey,
      .prometheusName = "fastcache_cluster_admissions_refused_malformed_key_total",
      .help = "cluster-admit requests the leader refused because the member's identity key was not one: not 43 "
              "base64url characters naming 32 bytes, or 32 bytes naming a small-order or non-canonical point, "
              "under which a signature proves nothing. Nothing was proposed. This project's clients check the key "
              "where it is typed, so a rise names a client that does not.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedUnknownKey,
      .prometheusName = "fastcache_raft_peer_connections_refused_unknown_key_total",
      .help = "Raft peer connections refused because this node holds no key for the id the proof claims, so "
              "nothing could be verified and nothing was answered. A machine that is not a member at all -- every "
              "member the roster records holds its key. The address is in the log line and the claimed id is not: "
              "nobody proved it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedRevokedKey,
      .prometheusName = "fastcache_raft_peer_connections_refused_revoked_key_total",
      .help = "Raft peer connections refused because the proof verified under a key the cluster has REVOKED: the "
              "removed machine itself, still dialling. Answered with a signed verdict saying so, so the removed "
              "machine reports its own revocation rather than a key problem here. Expected briefly after a "
              "--cluster-forget of a key; a steady rate is a machine nobody stopped.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerConnectionsEndedKeyWithdrawn,
      .prometheusName = "fastcache_raft_peer_connections_ended_key_withdrawn_total",
      .help = "Proven Raft peer connections this node closed because the key the dialler proved them with stopped "
              "being that member's in the cluster's roster: revoked, or replaced by a re-admission. Checked on "
              "every frame, so a revocation reaches every open connection at the next message rather than when "
              "the connection happens to break.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorKeyUnknown,
      .prometheusName = "fastcache_raft_peer_dials_refused_acceptor_key_unknown_total",
      .help = "Raft dials abandoned because this node holds no key for the member that answered, so its verdict "
              "could not be verified. Nothing was sent to it. Admit the member again with its key "
              "(--cluster-admit=<id>=<host>:<port>@<key>), or wait for the cluster to replicate it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorKeyRevoked,
      .prometheusName = "fastcache_raft_peer_dials_refused_acceptor_key_revoked_total",
      .help = "Raft dials abandoned because the member that answered signed with a key the cluster has revoked: a "
              "removed machine still answering at an address this node dials. Nothing was sent to it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedOwnKeyRevoked,
      .prometheusName = "fastcache_raft_peer_dials_refused_own_key_revoked_total",
      .help = "Raft dials refused, by a verified verdict, because the acceptor's roster has revoked THIS node's "
              "key: this machine was removed from the cluster. It never clears by itself; the machine must mint a "
              "new identity -- a fresh --cluster-dir -- and be admitted under it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsEndedKeyWithdrawn,
      .prometheusName = "fastcache_raft_peer_dials_ended_key_withdrawn_total",
      .help = "Proven Raft sessions this node ended before sending a frame, because the key the acceptor proved "
              "the session with stopped being that member's in the cluster's roster: revoked, or replaced. The "
              "redial that follows is judged against the roster as it is now.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsEndedFrameTag,
      .prometheusName = "fastcache_raft_peer_dials_ended_frame_tag_total",
      .help = "Two-way Raft sessions this node dialled -- a learner's, which the acceptor writes back on -- that "
              "it ended because a frame the acceptor wrote failed its tag: changed, injected, replayed or "
              "reordered in flight. A correct peer never produces one. The acceptor counts its own direction as "
              "fastcache_raft_peer_frames_refused_tag_total.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsEndedFrameSender,
      .prometheusName = "fastcache_raft_peer_dials_ended_frame_sender_total",
      .help = "Two-way Raft sessions this node dialled that it ended because a message whose tag verified named a "
              "sender other than the member the acceptor proved. Nothing it said was delivered.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsEndedFrameUnreadable,
      .prometheusName = "fastcache_raft_peer_dials_ended_frame_unreadable_total",
      .help = "Two-way Raft sessions this node dialled that it ended on a frame whose tag verified and which this "
              "build cannot read: a wire version other than the one the handshake settled, or a message out of "
              "place. A steady count names a peer running a different build.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsEndedFrameOverCap,
      .prometheusName = "fastcache_raft_peer_dials_ended_frame_over_cap_total",
      .help = "Two-way Raft sessions this node dialled that it ended on a frame declaring more payload than this "
              "node buffers, refused before any of it was read.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftPeerDialsEndedFrameBadMagic,
      .prometheusName = "fastcache_raft_peer_dials_ended_frame_bad_magic_total",
      .help = "Two-way Raft sessions this node dialled that it ended on a frame whose header did not begin with "
              "this wire's magic, after which nothing on the connection can be framed.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftSendsDroppedNoSession,
      .prometheusName = "fastcache_raft_sends_dropped_no_session_total",
      .help = "Raft messages dropped for a peer that dials in -- a learner, which nobody dials -- while no "
              "session of its is attached: the learner is offline, or has not dialled yet. Raft retransmits, so "
              "a drop costs the leader nothing else.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftSendsDroppedUnknownPeer,
      .prometheusName = "fastcache_raft_sends_dropped_unknown_peer_total",
      .help = "Raft messages dropped for a peer this node can place nowhere: it neither dials it nor was told "
              "it dials in, so nothing here can reach it however long it waits -- a member whose recorded address "
              "this node cannot dial, or an id it was never given an address for.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::RaftInboundSessionsSuperseded,
      .prometheusName = "fastcache_raft_inbound_sessions_superseded_total",
      .help = "Two-way Raft sessions a peer's newer session superseded: the same id proved a second session "
              "while its first was still attached, and the first was closed. One per reconnect is a roaming "
              "learner whose old connection had not yet been seen to end; a steady rate from one peer names two "
              "machines holding one identity key -- a copied --cluster-dir -- taking the session from each other.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseNoRoster,
      .prometheusName = "fastcache_worker_jobs_refused_lease_no_roster_total",
      .help = "Grants refused because the state this worker applied records no voter's key yet, so no grant "
              "from anybody could verify. A few while a node starts or joins are ordinary; a rise that does not "
              "stop means this node never applied its fleet's state.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseIsolated,
      .prometheusName = "fastcache_worker_jobs_refused_lease_isolated_total",
      .help = "Grants refused because this worker has heard from no leader its fleet counts for longer than it "
              "may trust the state it applied (65 minutes), so a voter the fleet forgot meanwhile could still be "
              "in it. Should read zero; a rise means this worker is cut off from its fleet's consensus -- check "
              "its Raft sessions -- while something still hands it grants.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseSignerRevoked,
      .prometheusName = "fastcache_worker_jobs_refused_lease_signer_revoked_total",
      .help = "Grants refused because their signature verifies under a key the cluster has revoked: the removed "
              "machine itself, still leasing out work. Should read zero; a rise names a machine somebody removed and "
              "nobody stopped.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeProofsRefusedUnknownKey,
      .prometheusName = "fastcache_node_proofs_refused_unknown_key_total",
      .help = "Node proofs whose signature verified under a key this cluster does not hold for the id named: a "
              "machine nobody enrolled, or one presenting a key other than "
              "the one admitted under its id. The remedy is an admission, not a key.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeProofsRefusedRevokedKey,
      .prometheusName = "fastcache_node_proofs_refused_revoked_key_total",
      .help = "Node proofs whose signature verified under a key this cluster revoked: the forgotten machine "
              "itself, still dialling in. Should read zero; a rise names a machine somebody removed and nobody "
              "stopped.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked,
      .prometheusName = "fastcache_node_requests_refused_key_revoked_total",
      .help = "Requests refused at any door because their connection proved an identity key the cluster revoked, "
              "or presented a ticket signed by one, whatever address it came from: a machine somebody forgot is "
              "still configured to use this fleet, or somebody holds one of its tickets. Only a PROOF is told why; "
              "a ticket's holder gets the words a stranger gets, so off --fleet-open this counter is where the difference "
              "shows; under --fleet-open the refusal itself shows it, since a stranger is served.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeSealedFramesRefused,
      .prometheusName = "fastcache_node_sealed_frames_refused_total",
      .help = "Proven connections closed because a frame's seal did not verify: something between the two ends "
              "injected, altered, replayed or reordered a frame. Should read zero; the frame is never answered.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::SchedulerRequestsRefusedNodeIdentityRequired,
      .prometheusName = "fastcache_scheduler_requests_refused_node_identity_required_total",
      .help = "register, node-announce, heartbeat and withdraw requests refused because their connection proved no "
              "identity the cluster holds live. An address admits a client, never a machine joining the fleet. A "
              "rise from one host is a node not yet admitted, or one whose proof is being refused.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsExpired,
      .prometheusName = "fastcache_enrollment_requests_expired_total",
      .help = "Enrollment requests the leader forgot because the machine stopped asking for ten minutes before anybody "
              "decided about it: switched off, or given up while waiting for a person. A machine that asks again is "
              "recorded afresh. A rise alongside machines nobody approved is joins that waited for somebody who was not "
              "looking.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentApprovalsManual,
      .prometheusName = "fastcache_enrollment_approvals_manual_total",
      .help = "Joiners an operator admitted by name with --enroll-approve, counted once the cluster agreed to record "
              "them under the key they asked with.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentApprovalsAuto,
      .prometheusName = "fastcache_enrollment_approvals_auto_total",
      .help = "Joiners an armed --enroll-auto-approve deadline admitted on the leader's own authority, under the key "
              "they asked with first. Apart from the manual approvals because the question after a window is who got "
              "in while nobody was looking; --enroll-list marks each such row with when the window was armed.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedHostCap,
      .prometheusName = "fastcache_enrollment_requests_refused_host_cap_total",
      .help = "Enrollment requests refused because their source host already had as many undecided requests on the "
              "list as one host may hold. Apart from the full-list series because the cause is one address asking "
              "a lot rather than many machines waiting: a NAT or a VM host rolling out more than a handful at once, "
              "or a flood.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsCleared,
      .prometheusName = "fastcache_enrollment_requests_cleared_total",
      .help = "Enrollment requests --enroll-clear dropped: rows nobody had decided about, forgotten on an operator's "
              "word. Approved and rejected rows are kept, and a machine still asking is recorded again at its next "
              "poll.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentApprovalsRefusedKeyMismatch,
      .prometheusName = "fastcache_enrollment_approvals_refused_key_mismatch_total",
      .help = "Approvals refused because the key they named is not the key the row under that id holds, so nothing "
              "was admitted. The machine asking under the id is not the one the operator compared -- most often "
              "because that one stopped asking, its row lapsed, and another machine asked under the same id.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedIdTooLong,
      .prometheusName = "fastcache_enrollment_requests_refused_id_too_long_total",
      .help = "Enrollment requests refused because the id they named is longer than every id this fleet carries may "
              "be. Refused where it enters, so the list never shows a row --enroll-reject could not name. A machine "
              "that keeps sending one was not minted by this software.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FormationYields,
      .prometheusName = "fastcache_formation_yields_total",
      .help = "Times this node decided to ask another fleet to admit it: while solitary it proved a fleet it yields "
              "to -- an established one, or an older solitary one -- and recorded the join before asking. More than "
              "one on a machine that should have joined once is a fleet that keeps refusing or stops answering.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FormationJoinsAbandoned,
      .prometheusName = "fastcache_formation_joins_abandoned_total",
      .help = "Joins this node gave up because the fleet it asked gave no answer it signed for ten minutes: silence, or "
              "only what nobody signs -- a full list, a not-leader, a redirect. It stays in its own cluster, keeps "
              "serving, and asks again when it next proves a fleet it yields to.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FormationAdmissionsRefused,
      .prometheusName = "fastcache_formation_admissions_refused_total",
      .help = "Admissions this node did not believe although they were signed by a key it proved for that fleet: "
              "the roster does not record it under its own key, or records no member under the key that proved the "
              "fleet. The fleet answered, wrongly -- an approval of another key for this id, most often. Nothing was "
              "archived; the node stays in its own cluster, and gives the join up if it goes on.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsAccepted,
      .prometheusName = "fastcache_node_tickets_accepted_total",
      .help = "Machine tickets this node verified and spent, so AUTH on that connection speaks for the machine the "
              "ticket names. The positive half of every node_tickets_refused row.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedMalformed,
      .prometheusName = "fastcache_node_tickets_refused_malformed_total",
      .help = "Machine tickets whose bytes are not a ticket: a client of another version, or something that is not one "
              "of ours.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedNotUtf8,
      .prometheusName = "fastcache_node_tickets_refused_not_utf8_total",
      .help = "Genuine machine tickets whose machine id or audience is not UTF-8, refused before it is compared, "
              "logged or rendered. A forged ticket counts as forged whatever its claims say, so a rise names an "
              "admitted machine minting claims that are not text.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedNoRoster,
      .prometheusName = "fastcache_node_tickets_refused_no_roster_total",
      .help = "Machine tickets refused because this node holds no roster, or one past its certification: nothing was "
              "checked. Not unknown_machine -- the remedy is this node reaching a leader, not an admission.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedUnknownMachine,
      .prometheusName = "fastcache_node_tickets_refused_unknown_machine_total",
      .help = "Machine tickets naming a machine this node's current roster holds no live key for, and signed by no "
              "key it revoked: a machine the cluster has not admitted. Distinct from no_roster, which checked "
              "nothing.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedForged,
      .prometheusName = "fastcache_node_tickets_refused_forged_total",
      .help = "Machine tickets naming an admitted machine and not signed by its key. Should read zero; a rise is "
              "somebody presenting tickets in another machine's name.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedRevoked,
      .prometheusName = "fastcache_node_tickets_refused_revoked_total",
      .help = "Machine tickets signed by a key the cluster revoked: the forgotten machine itself, still minting. Should"
              " read zero once it is switched off.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedWrongAudience,
      .prometheusName = "fastcache_node_tickets_refused_wrong_audience_total",
      .help = "Genuine machine tickets minted for an endpoint that is not this node: a stale name or address, or a "
              "ticket captured elsewhere and presented here.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedExpired,
      .prometheusName = "fastcache_node_tickets_refused_expired_total",
      .help = "Genuine machine tickets past their expiry and the clock-skew slack, further ahead than any minter "
              "issues, or expiring no later than a spend this node has already let go of. A steady rise from one "
              "machine is its clock; a burst from every machine just after this node's clock was corrected is the "
              "last case, and clears by itself within ten minutes, or, if this node accepted tickets while its "
              "clock was ahead, once real time reaches that reading; restarting the node clears it at once.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedReplayed,
      .prometheusName = "fastcache_node_tickets_refused_replayed_total",
      .help = "Genuine machine tickets this node had already spent. A ticket is presented once, so a rise is a captured"
              " ticket being replayed.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsRefusedSpentSetFull,
      .prometheusName = "fastcache_node_tickets_refused_spent_set_full_total",
      .help = "Genuine machine tickets refused because this node already remembers its capacity of spent tickets that "
              "are all still acceptable. A rise says resize the set; nothing is admitted while it is full.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketsMinted,
      .prometheusName = "fastcache_node_tickets_minted_total",
      .help = "Machine tickets this node signed for a process on this machine. Every ticket another node accepts was "
              "minted by some node's row here.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketMintsRefusedNotLocal,
      .prometheusName = "fastcache_node_ticket_mints_refused_not_local_total",
      .help = "MINT-TICKET asked from anywhere but loopback and refused before anything was signed: a ticket is minted "
              "for this machine's own processes only. Another machine asking this node to vouch for it, or a local "
              "launcher dialling its node by a LAN address.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketMintsRefusedMalformed,
      .prometheusName = "fastcache_node_ticket_mints_refused_malformed_total",
      .help = "MINT-TICKET whose audience would not decode, was not UTF-8 text, or named no one machine -- loopback, a "
              "wildcard or no host -- so the ticket would be spendable at any node. A client configured with a "
              "loopback endpoint.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketMintsRefusedNoKey,
      .prometheusName = "fastcache_node_ticket_mints_refused_no_key_total",
      .help = "MINT-TICKET on a node that holds no identity key, so it has nothing to sign with. The remedy is this "
              "node's own state directory, not the client.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeTicketMintsRefusedNoRandom,
      .prometheusName = "fastcache_node_ticket_mints_refused_no_random_total",
      .help = "MINT-TICKET this node could not draw a nonce for: its own random source failed, which is a fact about "
              "this machine. Refused rather than signed with a weaker nonce.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::SchedulerRequestsRefusedIdentifiedCallerRequired,
      .prometheusName = "fastcache_scheduler_requests_refused_identified_caller_required_total",
      .help = "Cluster control verbs -- admit, forget, set -- refused because only --fleet-open admitted the caller. "
              "--fleet-open admits a caller to what the fleet serves, never to what decides it; send the verb from "
              "this machine or from one whose node presents a machine ticket. On an open node a rise is somebody "
              "trying to change the fleet anonymously.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::EnrollmentControlRefusedIdentifiedCallerRequired,
      .prometheusName = "fastcache_enrollment_control_refused_identified_caller_required_total",
      .help = "Enrollment control verbs -- approve, reject, auto-approve, clear, list -- refused because only "
              "--fleet-open admitted the caller: an anonymous caller trying to decide who joins, the one this "
              "refusal exists for. Never sum with fastcache_enrollment_control_refused_not_a_member_total, a "
              "caller nothing admitted at all.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesMalformed,
      .prometheusName = "fastcached_dispatch_leases_malformed_total",
      .help = "Lease requests refused because their key, toolchain fingerprint or toolchain label "
              "is not UTF-8. Nothing is recorded for them. This project's launcher checks its label "
              "before sending it, so any rise names a client that does not.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesFieldTooLong,
      .prometheusName = "fastcached_dispatch_leases_field_too_long_total",
      .help = "Lease requests refused because their key, toolchain fingerprint or toolchain label is "
              "longer than a scheduler records. Nothing is recorded for them. This project's launcher "
              "sends none that long, so any rise names a client that does.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsFieldTooLong,
      .prometheusName = "fastcached_dispatch_worker_registrations_field_too_long_total",
      .help = "Worker registrations refused because their fingerprint, endpoint, version, toolchain "
              "label, display name or codec list is longer than a scheduler records. Nothing is recorded "
              "for them. This project's nodes send none that long, so any rise names a peer that does.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchNodeAnnouncementsFieldTooLong,
      .prometheusName = "fastcached_dispatch_node_announcements_field_too_long_total",
      .help = "Machine announcements refused because their endpoint or version is longer than a "
              "scheduler records. Nothing is recorded for them. An overlong condition field is refused "
              "earlier, when the frame is decoded, and is not counted here. This project's nodes send "
              "none that long, so any rise names a peer that does.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FormationAdmissionsUnverified,
      .prometheusName = "fastcache_formation_admissions_unverified_total",
      .help = "Answers to this node's join -- an admission, a refusal or a not-yet -- it refused because nothing "
              "bound them to the fleet it asked: no signature, one that does not verify over this node's own "
              "request and the outcome it states, or a genuine one by a key it never proved for that fleet. Such an "
              "answer counts as no answer: a refusal does not send the node away and a not-yet does not keep it "
              "waiting. A rise is something answering at the endpoint this node polls that is not that fleet. "
              "Nothing was archived; the node stays in its own cluster, and gives the join up if it goes on.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FormationYieldsRefusedPin,
      .prometheusName = "fastcache_formation_yields_refused_pin_total",
      .help = "Proven fleets this node would have asked to admit it and did not, because --fleet-id pins it to "
              "another cluster. Counted per proof: a fleet that keeps beaconing keeps counting. A steady rise is a "
              "fleet on this segment the pin keeps this node out of -- a second office, or somebody proving an older "
              "fleet to be joined; foreign-fleet-visible names it.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::FormationAdmissionsRefusedPin,
      .prometheusName = "fastcache_formation_admissions_refused_pin_total",
      .help = "Moves into another cluster this node refused because --fleet-id pins it elsewhere: an admission from a "
              "fleet the pin does not name, or its own fleet's order to dissolve into one. Nothing changed; the node "
              "stays where it is, and the log names both ids.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::StateFileReplacesFellBack,
      .prometheusName = "fastcache_state_file_replaces_fell_back_total",
      .help = "Serving bodies that found, as they started, that the state files in this node's directory are "
              "replaced by the classic rename because the POSIX-semantics one was refused -- a filesystem "
              "without it, or a path form it will not take. Every replace still lands; on Windows a reader "
              "holding a state file open then makes its replace fail. The warning names the directory and the "
              "refusal.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::StateDirectorySyncsUnsupported,
      .prometheusName = "fastcache_state_directory_syncs_unsupported_total",
      .help = "Serving bodies that found, as they started, that the filesystem holding this node's state directory "
              "cannot sync a directory at all, so a replaced state file is not known to survive a power loss there. "
              "Every replace still lands and is used. The warning names the directory and the answer; a local "
              "volume syncs.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeProofsRefusedRosterNotYetApplied,
      .prometheusName = "fastcache_node_proofs_refused_roster_not_yet_applied_total",
      .help = "Node proofs this node could not judge yet, because its consensus had not applied the log it recovered "
              "at start: answered roster-not-yet-applied, and retried by the prover, rather than node-key-unknown. A "
              "few at every start are the boot order; a steady rise is a node that cannot elect or cannot hear its "
              "leader.",
      .type = MetricType::Counter },
} };

// Checked at compile time rather than by a test, because the failure this prevents
// is a counter that exports nothing — which no test asserts the absence of unless
// somebody thinks to write one for the counter they just added, and that is
// precisely the step that was missed.
static_assert(RowsInEnumeratorOrder(CounterTable, &CounterDescriptor::counter),
              "CounterTable must hold one row per IMetricsSink::Counter, in enumerator order");

/// A fact about the BUILD this process runs, exported as a Prometheus info series:
/// one sample whose value is always 1 and whose label carries the fact.
///
/// A row here rather than a line in `PrometheusFormatter`, for the reason
/// `CounterDescriptor` gives: the renderer walks tables, and a series spelled inside
/// the renderer is one nothing else can enumerate. Not a `CounterDescriptor`, because
/// a version is not a tally the sink holds: it is a fact the READING carries
/// (`StatsReading::version`), so the text and the binary encoding state one fact.
///
/// Why it exists at all (#134): `fastcache-cli live-stats cache` titles its panel with
/// the daemon's version, and the scrape it reads carried none, so the title could only
/// say `-`. The label is the version verbatim; the renderer escapes it, since
/// `-DFASTCACHED_VERSION_STRING` accepts any text.
struct InfoDescriptor
{
    std::string_view prometheusName;   ///< Fully-qualified exported name.
    std::string_view help;             ///< One-line `# HELP` text.
    std::string_view label;            ///< The label the fact is carried in.
    std::string StatsReading::* value; ///< Where the reading holds the fact, unescaped.
};

/// Every info series this build exports, rendered once each by `PrometheusFormatter`.
inline constexpr std::array InfoTable {
    InfoDescriptor { .prometheusName = "fastcached_build_info",
                     .help = "The build this process runs: the version is the label, and the value is always 1.",
                     .label = "version",
                     .value = &StatsReading::version },
};

/// The row describing `counter`.
///
/// `Last` is not a metric and has no row; it yields nullptr rather than a
/// placeholder, so a caller that reached here with it fails visibly.
///
/// **An ordinal PAST `Last` is a different condition from `Last` itself**, and
/// this function answers both with nullptr while they mean opposite things.
/// `Last` is an ordinary caller mistake. Past it is what
/// `IMetricsSink::Carries` names: a build whose catalogue and sink were compiled
/// against different versions of `Counter`, which is inconsistent rather than
/// misused. That contract is stated once on `IMetricsSink` and is not restated
/// here.
///
/// nullptr is what a LOOKUP can honestly answer in both cases -- there is no row
/// to return and inventing a placeholder would be worse -- so this site matches
/// the contract rather than implementing it.
///
/// **`Carries` does not stand in for the null check here**, and the broader claim
/// is the one to avoid: the caller that renders the whole table
/// (`PrometheusFormatter`) walks `CounterTable` itself and asks the SINK's
/// `Carries` per row, so it never reaches this lookup at all. The two predicates
/// also answer for different translation units -- in the skew direction where the
/// SINK saw the longer enum, `Carries` says `true` for an ordinal this catalogue
/// has no row for, which is this nullptr reached past a `Carries` that agreed. A
/// skewed build is unsound in both directions, as `IMetricsSink::Carries` states
/// narrowly.
///
/// So what this site owes a caller is a null rather than a plausible value --
/// whether to CHECK it is the caller's own question, and the one production
/// caller (`FleetView`) dereferences unconditionally on the strength of the
/// `static_assert` above. That is sound for its own translation unit, which is
/// the scope this note declines to generalise past.
/// @param counter The counter to look up.
/// @return Its descriptor, or nullptr for `Last` or an out-of-range value.
[[nodiscard]] constexpr CounterDescriptor const* DescriptorOf(IMetricsSink::Counter counter) noexcept
{
    auto const index = CounterIndex(counter);
    if (!index.has_value())
        return nullptr;
    return &CounterTable[*index];
}

} // namespace FastCache
