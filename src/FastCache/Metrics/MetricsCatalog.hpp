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
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedMalformedCredential,
      .prometheusName = "fastcache_worker_frames_refused_malformed_credential_total",
      .help = "AUTH payloads on a compile surface that would not decode. Never sum "
              "with malformed_payload: they share a wire code and nothing else -- that "
              "one is a request body, this one is a credential, and an operator cannot "
              "tell a client version skew from somebody malforming AUTH at the door if "
              "the two are added up. Flat at zero BY CONSTRUCTION: the Session verb "
              "family is routed to the scheduler, so a compile surface is never asked "
              "for a credential.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedRejectedCredential,
      .prometheusName = "fastcache_worker_frames_refused_rejected_credential_total",
      .help = "AUTH payloads on a compile surface that decoded and did not verify. "
              "Never sum with frames_refused_unauthenticated: they share a wire code "
              "and nothing else -- that one never presented a credential, this one "
              "presented the wrong one. Flat at zero BY CONSTRUCTION: the Session verb "
              "family is routed to the scheduler.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnauthenticated,
      .prometheusName = "fastcache_worker_frames_refused_unauthenticated_total",
      .help = "Frames refused for reaching a compile verb before a credential. Flat at "
              "zero BY CONSTRUCTION, not for want of anybody probing: the compile "
              "surface answers AuthRequired false -- a compile carries its own per-job "
              "lease instead -- so the pre-payload gate never reaches this. Do not read "
              "the flat line as 'nothing is asking'. A rise means that answer changed, "
              "which is a change to who may compile here.",
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
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnregistered,
      .prometheusName = "fastcache_worker_jobs_refused_lease_unregistered_total",
      .help = "Jobs refused because this worker has not registered and so knows no fleet. "
              "A few at startup are ordinary; a rise that does not stop means the scheduler "
              "is unreachable and this node is compiling nothing while looking alive.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseWrongCluster,
      .prometheusName = "fastcache_worker_jobs_refused_lease_wrong_cluster_total",
      .help = "Jobs refused because an authentic lease was issued by a different fleet. "
              "A rise means two clusters are running from the same --cluster-key-file, "
              "which is what copying a working configuration to a second site produces.",
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
    { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal,
      .prometheusName = "fastcache_node_cache_requests_refused_not_local_total",
      .help = "Cache requests refused because the caller is not on this machine. A node's tier "
              "is this machine's build output and is served to this machine only, whatever the "
              "surface is bound to and whatever the member list says. On a worker, whose node "
              "port defaults to loopback, this stays at zero and a rise means something off-box "
              "is asking. On a node running --serve-scheduler the port faces the network by "
              "design, so a rise is ORDINARY -- it counts peers reaching the right host for the "
              "wrong verb -- and what an operator watches there is the shape of the curve rather "
              "than its existence.",
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
    { .counter = IMetricsSink::Counter::SchedulerRequestsRefusedUnauthenticated,
      .prometheusName = "fastcache_scheduler_requests_refused_unauthenticated_total",
      .help = "Scheduler requests refused because the connection presented no "
              "accepted credential. Zero while no --scheduler-token-file is set, "
              "because there is then nothing to fail -- so zero here on a "
              "non-loopback bind means the port is open, not that it is quiet.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::SchedulerCredentialsRejected,
      .prometheusName = "fastcache_scheduler_credentials_rejected_total",
      .help = "AUTH attempts on the scheduler surface that decoded and did not "
              "verify: a rotated key, or somebody guessing. Never sum with "
              "requests_refused_unauthenticated -- that one is a peer that never "
              "authenticated at all, which is a misconfigured member rather than a "
              "probe.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::SchedulerCredentialsMalformed,
      .prometheusName = "fastcache_scheduler_credentials_malformed_total",
      .help = "AUTH payloads on the scheduler surface that would not decode: a "
              "version or client-library mismatch. Kept apart from "
              "credentials_rejected so an old client in the fleet cannot hide a peer "
              "presenting wrong secrets.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::NodeFrameConnectionsRefusedAtCapacity,
      .prometheusName = "fastcache_node_frame_connections_refused_at_capacity_total",
      .help = "Connections turned away because the node's 0xFC listener already holds "
              "every connection it will. Never sum with "
              "worker_jobs_refused_endpoint_busy: they share a wire code and nothing "
              "else -- that one says one request was too big right now, this says the "
              "surface has no room for another conversation.",
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
