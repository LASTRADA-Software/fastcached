// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Counter-style metrics sink for facts the *connection* layer knows.
///
/// Designed thin on purpose: only counters today; histograms and gauges come
/// later if/when needed. Implementations must be thread-safe.
///
/// ## What belongs here, and what does not
///
/// Command counts, hit/miss splits and evictions do **not**: those are
/// `StorageStats`, produced by the storage that actually performs them, and that
/// is what `stats`, `INFO` and `/metrics` all report. This enum used to carry a
/// second set of enumerators for the same concepts — `CmdGet`, `CmdSet`,
/// `CmdDelete`, `GetHits`, `GetMisses`, `Evictions`, `BytesIn`, `BytesOut` — and
/// not one of them was incremented anywhere in the tree, so each exported a
/// permanent zero under a name an operator would reasonably read as the real
/// count. They are removed rather than wired up: a second source of truth for a
/// number `StorageStats` already owns is the thing to avoid, not to complete.
///
/// Every remaining enumerator needs a row in `Metrics/MetricsCatalog.hpp`, which
/// `static_assert`s that it has one — that is what makes a new counter reach
/// `/metrics` by construction rather than by somebody remembering.
class IMetricsSink
{
  public:
    IMetricsSink() = default;
    IMetricsSink(IMetricsSink const&) = delete;
    IMetricsSink(IMetricsSink&&) = delete;
    IMetricsSink& operator=(IMetricsSink const&) = delete;
    IMetricsSink& operator=(IMetricsSink&&) = delete;
    virtual ~IMetricsSink() = default;

    enum class Counter : std::uint8_t
    {
        ConnectionsTotal = 0,
        ConnectionsAdmissionRejected,
        /// Subset of `ConnectionsTotal` that came in on a TLS-flagged
        /// bind. Lets operators attribute traffic to plaintext vs TLS
        /// without a per-bind label dimension (the IMetricsSink
        /// interface is intentionally counter-only, no labels). Sum:
        ///   connections_total_plaintext = ConnectionsTotal − ConnectionsTotalTls
        ConnectionsTotalTls,
        /// Subset of `ConnectionsAdmissionRejected` that came in on a
        /// TLS-flagged bind. Pairs with ConnectionsTotalTls.
        ConnectionsAdmissionRejectedTls,
        /// Lease requests the scheduler answered with a worker. The numerator of
        /// "is distribution actually happening", and meaningless without the
        /// refusals below it -- a fleet where every lease is granted and a fleet
        /// nobody asks look identical on this counter alone.
        DispatchLeasesGranted,
        /// Leases a client resolved when its job ended, however it ended.
        ///
        /// The pair of `DispatchLeasesGranted`, and the difference between the two
        /// is the outstanding count. Its absence is what let a whole missing
        /// transition go unnoticed: every lease was granted, none was ever
        /// resolved, and the only number saying so was one nobody exported. A
        /// granted count that climbs while this one stays flat means clients are
        /// dying mid-job, or are older than this verb.
        DispatchLeasesReleased,
        /// Lease requests refused because no registered worker matched the
        /// toolchain. The counter that says a fleet is MISCONFIGURED rather than
        /// busy: it rises when workers are up but nobody can use them, which is
        /// the failure mode a fingerprint mismatch produces and the one that is
        /// otherwise invisible from both ends.
        DispatchLeasesNoWorker,
        /// Lease requests refused because every matching worker was full of this
        /// fleet's own work. Rising here means the fleet is too small, which is a
        /// different decision from the line above and must not be summed with it.
        DispatchLeasesNoCapacity,
        /// Lease requests refused because matching workers had slots free on paper
        /// and had withdrawn them -- their machines are busy with something else, or
        /// out of scratch space.
        ///
        /// The third arm of the same split, and the one whose absence would be worst:
        /// folded into `DispatchLeasesNoCapacity` it reads as "the fleet is too
        /// small", so an operator whose build hosts have all filled their scratch
        /// disks would go and buy machines rather than free 200 MB. Rising here with
        /// a flat `NoCapacity` is a fleet that is big enough and unavailable.
        DispatchLeasesWithdrawn,
        /// Lease requests refused because another client already held a lease for
        /// this key. Not a failure: it is duplicate-work suppression doing its
        /// job, and the clients it refuses compile locally.
        DispatchLeasesDuplicate,
        /// Workers currently registered, as a running total of registrations
        /// accepted. A gauge would be the better shape and this interface is
        /// counter-only by design, so this counts events rather than membership:
        /// it rises on every re-registration, which is itself the signal worth
        /// watching -- a fleet that re-registers constantly is a fleet whose
        /// heartbeats are not arriving.
        DispatchWorkerRegistrations,
        /// Registrations refused because the worker did not announce itself in
        /// UTF-8. Beside the accepted count rather than beside the lease refusals,
        /// deliberately: this is not a statement about the fleet's capacity, it is
        /// a peer that cannot be recorded, and it is the ONLY trace such a peer
        /// leaves on the leader -- a node whose `--toolchain` override carries a
        /// stray byte otherwise vanishes from the fleet with nothing saying why.
        DispatchWorkerRegistrationsMalformed,
        /// Registrations **accepted** whose endpoint names a host other than the
        /// one the registration arrived from.
        ///
        /// Not a refusal, and the distinction is the whole point of the counter
        /// (#242). A registration asserts, unchecked, where work for a toolchain
        /// should be sent, and nothing today ties that claim to the connection
        /// carrying it -- but an exact match refuses a great many legitimate
        /// configurations, starting with the fleet this project's own
        /// getting-started page builds. Whether a strict rule is viable depends on
        /// how often real deployments mismatch, which nobody currently knows. This
        /// is the number that answers it.
        ///
        /// A rise is therefore not a fault to be chased. A fleet whose workers
        /// advertise DNS names, sit behind NAT, are multi-homed or reach the
        /// scheduler over a VPN will report every registration here and be working
        /// perfectly.
        DispatchWorkerEndpointMismatch,
        /// Workers dropped for having stopped heartbeating.
        ///
        /// The counterpart of `DispatchWorkerRegistrations`, and the fleet's only
        /// trace of a machine going away: expiry used to be a filter that hid a
        /// worker from `Pick` while leaving its entry in place, so losing a machine
        /// was invisible from every direction. Rising steadily beside a rising
        /// registration count is a fleet whose heartbeats are not arriving rather
        /// than one that is growing.
        DispatchWorkersExpired,
        /// Leases freed because the worker holding them was dropped.
        ///
        /// Distinct from `DispatchLeasesReleased`, which is a client reporting its
        /// own job done. This is work nobody will ever report: the machine went
        /// away mid-job, and every one of these keys was being refused
        /// `AlreadyInFlight` until it was reclaimed. Rising here says a build lost
        /// part of its distribution, which is a different thing to fix from a fleet
        /// that is merely full.
        DispatchLeasesReclaimed,
        /// Releases refused because the token presented was not signed by this
        /// cluster.
        ///
        /// The scheduler's own half of `WorkerJobsRefusedLeaseUnauthorized`, and its
        /// own counter for the same reason the worker's refusals are split: this one
        /// is about **who** asked, not about what the fleet could do. Deliberately
        /// not folded into the uncounted `UnknownLease` refusal beside it, which is
        /// a statement about one client's timing -- a token this cluster issued and
        /// has since forgotten. This one was never issued at all.
        DispatchLeasesUnauthorized,

        /// Frames refused at the FLEET SCHEDULER's port for naming a protocol version
        /// this build does not serve.
        ///
        /// The scheduler's own half of `WorkerFramesRefusedUnsupportedVersion`, and a
        /// separate counter for the reason every split on this surface has one: the
        /// two ports are reached by different peers for different purposes, and a
        /// worker built against another release is a different operator problem from a
        /// client that is. Rising during a staggered upgrade is expected and bounded;
        /// rising afterwards names a node that never came back.
        DispatchFramesRefusedUnsupportedVersion,

        /// Frames naming an opcode this build has no row for, at the scheduler's port.
        ///
        /// Stepped over rather than fatal by design -- the framing exists so a receiver
        /// can skip a verb it does not know, which is what lets a newer client talk to
        /// an older scheduler. So a rise is either that, or somebody walking the opcode
        /// space; both are worth seeing on the surface that carries lease grants.
        DispatchFramesRefusedUnknownOpcode,

        /// Frames naming a verb this scheduler does not serve, at the scheduler's port.
        ///
        /// A CACHE verb arriving here, answered `DispatchNotPermitted` rather than
        /// `UnknownOpcode`: the verb exists and is served on another port, and telling
        /// a client it is unknown would say this daemon is too OLD when it is merely
        /// the wrong port. A rise is a misconfigured client, which is a thing an
        /// operator fixes rather than a thing they wait out.
        DispatchFramesRefusedNotPermitted,

        /// Frames whose declared payload length did not match what arrived, at the
        /// scheduler's port.
        ///
        /// **Not the same refusal as `SchedulerService`'s uncounted `MalformedFrame`,
        /// and that is the point.** The service refuses a payload it could not make
        /// sense of, and deliberately counts nothing for it because a broken client's
        /// noise must stay out of the numbers a fleet is sized from. This one is a
        /// frame whose own header disagrees with itself, before any verb is routed --
        /// a different event with the same wire code. A table keyed on the CODE could
        /// not hold both, which is exactly why the row rather than the code is the
        /// unit (#327, #494).
        DispatchFramesRefusedMalformedPayload,

        /// Compiles a worker began. With `WorkerJobsCompleted` this is also the
        /// in-flight count — two monotone counters rather than a gauge, which this
        /// interface deliberately does not have, and their difference is what
        /// "slots in use" means. Slots *configured* is not here at all: it is
        /// configuration rather than a measurement, and pushing it through a
        /// counter would mean incrementing to its value at startup.
        WorkerJobsStarted,
        /// Compiles that finished, whatever the compiler concluded. A compiler
        /// that ran and rejected the code did its job; that is the client's
        /// answer, not a worker failure, and it is deliberately not a refusal.
        WorkerJobsCompleted,
        /// Total wall time spent compiling, in milliseconds.
        ///
        /// The `_sum` half of a duration, with `WorkerJobsCompleted` as `_count` —
        /// which is exactly how a Prometheus histogram reports one, so
        /// `rate(sum)/rate(count)` is the average compile time and this interface
        /// stays counter-only. A gauge would answer a different and less useful
        /// question: the duration of whichever compile happened to finish last.
        WorkerCompileMillisTotal,

        /// Jobs refused because no compiler here matches the client's fingerprint.
        /// The worker's own half of `DispatchLeasesNoWorker`: rising here means
        /// the fleet is misconfigured, and it is the commonest setup failure.
        WorkerJobsRefusedUnknownFingerprint,
        /// Jobs refused over an argument this worker will not pass to a compiler.
        WorkerJobsRefusedRejectedArgument,
        /// Jobs refused because the scratch directory could not be prepared.
        /// An operational fault — a full or read-only disk — and nothing the
        /// client or the fleet's configuration can fix.
        WorkerJobsRefusedScratchUnavailable,
        /// Jobs refused because the compiler could not be spawned at all.
        /// Distinct from a compiler that ran and failed: this one says the
        /// toolchain this worker advertises is not actually usable here.
        WorkerJobsRefusedSpawnFailed,
        /// Jobs refused because this worker had not finished surveying its
        /// toolchains. Its own counter rather than folded into
        /// `WorkerJobsRefusedUnknownFingerprint`, because the two send an operator
        /// to opposite conclusions: one says the fleet is matching wrongly, the
        /// other says a node is still starting.
        WorkerJobsRefusedSurveyInFlight,
        /// Jobs refused because every slot was busy.
        ///
        /// Not a fault and deliberately its own counter: it is the worker's half
        /// of `DispatchLeasesNoCapacity`, and summing it with the four above would
        /// hide a misconfigured toolchain behind a busy machine — the same reason
        /// the scheduler splits no-worker from no-capacity.
        WorkerJobsRefusedNoSlot,

        /// Connections refused because the caller is neither on this machine nor a
        /// member of this cluster.
        ///
        /// The anti-leeching rule at the compile port, and its own counter because
        /// it is the only refusal here that is about **who** rather than about what:
        /// the four above describe a job this worker could not do, while a rise in
        /// this one means something is trying to spend a machine it has no claim on.
        /// An operator reads it very differently, which is the whole reason the
        /// refusals are split.
        WorkerJobsRefusedNotAMember,
        WorkerJobsRefusedEndpointBusy,

        /// Jobs refused because this worker had begun stopping.
        ///
        /// Its own series and not `no_slot`, because an operator acts on the two
        /// oppositely: `no_slot` says buy machines or lower the fan-out, this says
        /// wait -- the node is draining and a client that retries will find another
        /// worker or this one restarted. Summed together, a rolling restart reads as
        /// a fleet that is permanently too small.
        ///
        /// Only the merged `0xFC` surface can produce it. The dedicated compile port
        /// closes its listener to stop, so a client meets a closed port rather than a
        /// refusal; the merged listener carries three verb families and goes on
        /// serving the other two, so this one has to be refused in words.
        WorkerJobsRefusedStopping,

        /// Frames refused before a job existed at all.
        ///
        /// A family of their own rather than more `WorkerJobsRefused*`, because they
        /// answer a different question: the job refusals describe work this worker
        /// could not do, while these describe a frame it could not read. A rise in
        /// the job family points at the fleet's configuration; a rise here points at
        /// what is on the wire -- a peer built against another version, a broken
        /// framing, or something probing the port.
        ///
        /// Every refusal `WorkerProtocol` can answer with has one, and
        /// `worker-refusals-counted` is what keeps that true: a refusal answered on
        /// the wire while nothing rises is how a port being probed looks, on
        /// `/metrics`, exactly like a port nobody is talking to (#327).

        /// The request named a protocol version outside this build's range.
        WorkerFramesRefusedUnsupportedVersion,
        /// The frame was shorter than the payload length its own header declared.
        ///
        /// Its own counter although it shares `MalformedFrame` on the wire with
        /// `WorkerFramesRefusedMalformedPayload` below, and the split is the point:
        /// a truncated frame is a framing or transport fault, or a peer sending
        /// deliberate nonsense, while a payload that will not decode is a version or
        /// encoding mismatch between two ends that agree on the framing. One code,
        /// because a client acts on both the same way; two counters, because an
        /// operator does not.
        WorkerFramesRefusedTruncated,
        /// The opcode is in no row of `OpTable`.
        WorkerFramesRefusedUnknownOpcode,
        /// A verb this endpoint knowingly does not implement, such as `AUTH` on a
        /// worker that checks no credential. Distinct from the opcode being unknown:
        /// this one is a verb that exists and is not served here.
        WorkerFramesRefusedUnimplementedVerb,
        /// A verb served elsewhere on this node, sent to the compile surface.
        WorkerFramesRefusedNotPermitted,
        /// The payload did not decode into the fields its verb requires.
        WorkerFramesRefusedMalformedPayload,
        /// The frame's own header declared a payload larger than this surface
        /// accepts, so it was refused without being read.
        ///
        /// **The cheapest probe there is**, and the reason it needs its own counter
        /// rather than leaning on the envelope refusals: this check needs a header,
        /// where `WorkerJobsRefusedEnvelopeDeclaredTooLarge` needs a whole frame to
        /// have been sent and read. An operator alerting only on the envelope series
        /// watches a client hammer the node with oversized declarations, sees the
        /// node refuse every one correctly, and concludes it is not being probed
        /// (#326).
        WorkerFramesRefusedPayloadTooLarge,

        /// `AUTH` payloads on a compile surface that would not decode at all.
        ///
        /// Its own row rather than `WorkerFramesRefusedMalformedPayload`, although
        /// both answer `MalformedFrame`: that one is a request body that did not
        /// decode into the fields its verb requires, this one is a credential. An
        /// operator told only "a malformed frame arrived" cannot tell a client version
        /// skew from somebody malforming AUTH at the door, which is the #327 confusion
        /// re-created one refusal further along.
        ///
        /// Zero on every configuration this build ships, exactly as the counter below
        /// is and for the same reason: `MergedResponder` routes the `Session` family
        /// to the scheduler, so a compile surface is never asked about a credential.
        /// A tally's zero is the truth about events that did not happen; a rise means
        /// what may compile here has changed.
        WorkerFramesRefusedMalformedCredential,

        /// `AUTH` payloads on a compile surface that decoded and did not verify.
        ///
        /// Its own row rather than `WorkerFramesRefusedUnauthenticated` below, although
        /// both answer `unauthenticated`: that one is a peer that reached a verb having
        /// never presented a credential, this one is a peer that presented one and got
        /// it wrong. The same split the scheduler surface carries, and for the same
        /// reason -- an operator acts on a misconfigured member and on somebody trying
        /// secrets differently.
        ///
        /// Zero on every configuration this build ships, exactly as its two neighbours
        /// are: the `Session` verb family is routed to the scheduler, so a compile
        /// surface is never asked for a credential at all.
        WorkerFramesRefusedRejectedCredential,

        /// Frames refused for reaching a compile verb before a credential.
        ///
        /// Zero on every configuration this build ships, and that is the point rather
        /// than a reason to leave it out: a compile carries its own per-job credential
        /// -- the lease the scheduler signed -- so `CompileResponder::AuthRequired`
        /// answers false and the pre-payload gate never reaches this. A rise means
        /// that answer changed, which is a change to who may compile here, and a
        /// counter is how an operator would find out. A tally's zero is the truth
        /// about events that did not happen.
        WorkerFramesRefusedUnauthenticated,

        /// Jobs refused while opening the request's codec envelope, one counter per
        /// reason — the same split, and for the same argument, that `EnvelopeError`
        /// makes one layer down.
        ///
        /// These used to be answered on the wire and counted nowhere, so a fleet
        /// being probed with envelope bombs (issue #241, the reason the ceiling
        /// exists) emitted `payload-too-large` replies while `/metrics` stayed
        /// perfectly flat. A refusal a worker returns and does not count is a
        /// refusal an operator can only find by reading a client's log.
        ///
        /// Split rather than summed into one `..._bad_envelope_total`, because the
        /// four name four different things to go and do, and a sum is exactly what
        /// hides the one that matters:
        ///
        /// - `DeclaredTooLarge` is somebody attacking this port, or a client
        ///   configured with a request ceiling larger than this worker's. Nothing
        ///   honest declares an expansion above the cap by accident.
        /// - `UnsupportedCodec` is two honest processes built differently — a
        ///   client compiled with zstd talking to a worker that was not. It is a
        ///   packaging problem, and every one of these cost a local compile.
        /// - `Malformed` is a peer whose framing this build cannot parse: a version
        ///   skew, or something that is not this protocol at all.
        /// - `Corrupt` is framing this build parsed and bytes that then did not
        ///   expand to their declared length — a codec version skew or a link
        ///   damaging payloads, which is a different investigation from all three
        ///   above and the only one that implicates the transport.
        WorkerJobsRefusedEnvelopeMalformed,
        WorkerJobsRefusedEnvelopeUnsupportedCodec,
        WorkerJobsRefusedEnvelopeDeclaredTooLarge,
        WorkerJobsRefusedEnvelopeCorrupt,

        /// Jobs refused because the lease presented was not signed by this cluster.
        ///
        /// The one counter here that is unambiguously a security signal rather than
        /// a capacity or configuration one: membership decided the caller may reach
        /// this port, and the lease decides the scheduler actually sent them. A
        /// sustained rise is somebody probing the compile port, or -- far more often
        /// -- a launcher built before signed leases, which presents a bare serial
        /// that cannot authenticate. The two are indistinguishable here by design;
        /// what tells them apart is whether the rise tracks a rollout.
        WorkerJobsRefusedLeaseUnauthorized,
        /// Jobs refused because an AUTHENTIC lease came from a different fleet.
        ///
        /// Its own counter rather than a share of the one above, because the operator
        /// action is different and specific: two clusters are running from the same
        /// `--cluster-key-file`, which is the ordinary outcome of copying a working
        /// configuration to a second site or cloning staging from production. Nothing
        /// else produces this. Before #322 the token bound no cluster identity, so
        /// those grants verified and the other fleet's workers simply compiled them.
        WorkerJobsRefusedLeaseWrongCluster,
        /// Jobs refused because an AUTHENTIC lease named a superseded scheduler term.
        ///
        /// Again its own, and again because the action differs: this says grants are
        /// outliving elections, which is a replay window rather than a provisioning
        /// mistake. Expected to read zero on a fleet that is not electing, so any
        /// sustained rise is worth looking at even when it is small.
        WorkerJobsRefusedLeaseStaleEpoch,
        /// Jobs refused because an AUTHENTIC lease named a different worker.
        ///
        /// Almost never a replay and almost always a fleet whose registered endpoint
        /// is not the one clients dial -- a NAT, or a hostname registered where an
        /// address resolves. Counted apart from the refusal above precisely because
        /// the actions differ: one is somebody attacking, the other is somebody's
        /// `--scheduler-advertise` being wrong.
        WorkerJobsRefusedLeaseEndpointMismatch,
        /// Jobs refused because an AUTHENTIC lease had expired.
        ///
        /// Not a capacity fact. A lease's expiry bounds how long a captured token is
        /// useful; a worker's slots bound what it will run. A rise on one machine
        /// and nowhere else is that machine's clock drifting, which is why the check
        /// carries skew slack and why this is worth seeing per node.
        WorkerJobsRefusedLeaseExpired,

        /// Scratch roots this worker took over from a node that died without
        /// cleaning up.
        ///
        /// A worker claims its scratch root exclusively at startup and holds the
        /// claim for the life of the process, so a root whose lock is free but whose
        /// contents are not is one whose owner is gone -- including via the
        /// abandoned-drain `_Exit`, which by design bypasses every destructor.
        /// Reclaiming it is correct and needs no reasoning about staleness.
        ///
        /// Counted because it is otherwise invisible. The startup REFUSAL to claim a
        /// root at all is deliberately not counted: that path exits the process, so
        /// nothing would ever scrape the number -- an operator learns about it by the
        /// node not starting, and the log line names which of the two refusals it was.
        WorkerScratchRootsReclaimed,

        /// Bytes of request payload read from clients, and of reply written back.
        /// The pair is what says whether a codec negotiation is doing anything:
        /// preprocessed text in against object bytes out.
        WorkerBytesReceived,
        WorkerBytesReturned,

        /// The node's own cache tier, which exists so a local rebuild on a slow or
        /// bad network does not go to the wire at all.
        ///
        /// Hits and misses are the node's OWN tier, before any upstream is asked;
        /// `NodeCacheUpstreamHits` is how often the shared cache answered what this
        /// node could not. The three together are what say whether the local tier is
        /// earning its disk: a high upstream-hit rate against a low local-hit rate is
        /// a tier too small to hold this machine's working set, which is a different
        /// problem from a fleet that is missing a lot.
        NodeCacheHits,
        NodeCacheMisses,
        NodeCacheUpstreamHits,

        /// A value the shared cache supplied that the local tier then refused. Costs
        /// one future round trip rather than a build, so it is counted rather than
        /// reported -- but a rate that stays high means the tier is misconfigured
        /// (unwritable path, a cap below the objects being stored) and is silently
        /// doing nothing.
        NodeCacheFillFailures,

        /// A local write that failed. Distinct from the fill failure above because
        /// this one IS reported to the client: it is the write that must not be lost.
        NodeCacheStoreFailures,

        /// What the fleet got. Best-effort by contract -- the local write already
        /// succeeded -- so a failure here costs the fleet a shared entry and costs
        /// this machine nothing. Split from the local counters precisely so an
        /// operator can tell "my node is fine, the fleet is unreachable" from "my
        /// node is broken", which are different things to go and fix.
        NodeCacheUpstreamStores,
        NodeCacheUpstreamStoreFailures,

        /// Cache requests refused because the caller is not on this machine (#287).
        ///
        /// The node's tier is this machine's entire build output, so it is served to
        /// this machine and to nothing else -- a rule of the **verb**, independent of
        /// what the surface is bound to and of any member list. Counted because the
        /// tightening withdrew access a fleet peer used to have: an operator whose
        /// peers stopped getting hits needs one number that says so, and without it
        /// the only evidence is a client-side hit rate that fell for no visible
        /// reason. On a loopback bind it stays at zero forever, which is the answer
        /// to "is this happening to me".
        ///
        /// That last sentence stopped being the whole story when the cache and
        /// scheduler surfaces merged (#290). A node running `--serve-scheduler` binds
        /// the wildcard, so peers reach the same port the cache verbs arrive on and a
        /// rise there is ORDINARY rather than a signal -- it is the tightening working.
        /// A **worker** still defaults to loopback and a rise there still means what it
        /// always did. The counter is the same fact either way; which reading applies
        /// is a property of the node, and the help text says so rather than promising
        /// a zero the merge made false.
        NodeCacheRequestsRefusedNotLocal,

        /// A cache verb whose header declared more payload than the surface will
        /// buffer, so nothing was read.
        ///
        /// **The cheapest probe there is**: twenty-four bytes and no body. It is also
        /// the frame ceiling that fires on a real node, which is why it is a row at
        /// all -- the merged `0xFC` listener asks the component owning the verb, and
        /// until #491 the cache's answer moved nothing, so an operator alerting on
        /// `fastcache_worker_frames_refused_payload_too_large_total` watched a flat
        /// graph while the port was being hammered (#326's scenario, one surface
        /// over).
        ///
        /// **Never summed with `WorkerFramesRefusedPayloadTooLarge`.** Both answer
        /// `payload-too-large` and a client does the same thing about both; an
        /// operator does not, because the two name different subsystems on one port
        /// and one of them is what a misconfigured launcher produces.
        NodeCacheRequestsRefusedPayloadTooLarge,

        /// A cache verb that would not fit in the bytes the surface already holds in
        /// flight.
        ///
        /// **The byte-budget refusal a node with a tier actually reaches.**
        /// `MergedResponder::MaxInFlightBytes()` folds to the largest owner's budget,
        /// which in practice is this cache's, so the endpoint's in-flight ceiling is
        /// the cache's ceiling and a `STORE` is what runs into it.
        ///
        /// **Never summed with `WorkerJobsRefusedEndpointBusy` or
        /// `NodeFrameConnectionsRefusedAtCapacity`.** Three refusals answer
        /// `endpoint-busy` on this listener and an operator does three different
        /// things: shrink the objects being stored, add compile capacity, raise a
        /// connection ceiling.
        NodeCacheRequestsRefusedEndpointBusy,

        /// A cache request whose wire version this build cannot decode.
        ///
        /// A client compiled against another release, and every one of its exchanges
        /// fails. Counted because the only other evidence is a cache that looks
        /// permanently cold: the launcher steps over a refused `FETCH` and compiles
        /// locally, so the build stays correct and merely stops being fast, which is
        /// the failure shape this tree has paid for before.
        NodeCacheRequestsRefusedUnsupportedVersion,

        /// A `FETCH` or `STORE` body this build could not decode.
        ///
        /// The frame was well formed -- its declared length arrived in full -- and the
        /// payload inside it was not. That is a client-library or version mismatch
        /// between two ends that agree on the framing, or somebody malforming bodies
        /// on purpose; either way no healthy launcher produces one.
        ///
        /// **Its own row rather than any other `malformed-frame` counter.** The code
        /// is shared by a truncated compile frame, an undecodable compile payload and
        /// two `AUTH` payloads; the row is the refusal and not the code, so a table
        /// keyed on the code could not hold them.
        NodeCacheRequestsRefusedMalformedPayload,

        /// A `STORE` refused because its value names a canonicalization generation
        /// this build does not implement.
        ///
        /// The near-twin of `...RefusedUnsupportedVersion` one enumerator up, and
        /// counted for the same reason: that one is a client at another WIRE version,
        /// this is a value at another VALUE-FORMAT generation, and an operator does
        /// the same thing about both -- find the machine that is out of step and
        /// finish or roll back the upgrade.
        ///
        /// **Its baseline is zero and that is what makes it readable.** A fleet is
        /// permanently mid-upgrade ([#173]), so it might look like ordinary traffic --
        /// but `CompileValueVersion` has never moved, so nothing produces one today
        /// and any rise is a real event with a date on it. That is the opposite of the
        /// `AUTH` refusal this tier deliberately leaves uncounted, which a healthy
        /// launcher emits once per exchange for a whole build.
        ///
        /// It is also what makes #483's decision auditable. Refusing a foreign
        /// generation buys correctness by giving up hits, and without this series
        /// nobody can see what that trade costs -- the refusal is invisible to the
        /// launcher, which reports a miss and compiles locally.
        NodeCacheRequestsRefusedForeignGeneration,

        /// Scheduler requests refused because the connection presented no accepted
        /// credential.
        ///
        /// Counted at the pre-payload gate, so it rises for a frame whose body was
        /// never read. Only a surface with `--scheduler-token-file` configured can
        /// move it: with no credential set there is nothing to fail, and the counter
        /// stays at zero -- which is the honest answer to "is my scheduler port
        /// guarded", because zero here plus a non-loopback bind means it is not.
        ///
        /// Never sum with `WorkerJobsRefusedNotAMember`: that one says a host was not
        /// on the operator's list, this one says a caller held no secret. A fleet peer
        /// with a stale token moves this and not that.
        SchedulerRequestsRefusedUnauthenticated,

        /// `AUTH` attempts on the scheduler surface that decoded and did not verify.
        ///
        /// **The credential-guessing signal**, and the one
        /// `SchedulerRequestsRefusedUnauthenticated` above cannot carry: that one
        /// counts a peer reaching a verb WITHOUT having authenticated, which is a
        /// misconfigured fleet member. This counts a peer that presented a token and
        /// got it wrong -- a rotated key, or somebody trying. An operator does
        /// opposite things about the two, so they are two rows although a client is
        /// told `unauthenticated` either way.
        ///
        /// Until #447 this rose nowhere at all: the endpoint answered the wire code
        /// itself, so a fleet being probed for its scheduler token read a flat zero on
        /// the exact series an operator would go looking at.
        SchedulerCredentialsRejected,

        /// `AUTH` payloads on the scheduler surface that would not decode at all.
        ///
        /// Separate from the rejection above because it says something different: a
        /// peer that cannot form the frame is a version or client-library mismatch,
        /// while one forming it correctly with the wrong secret is the security
        /// question. Summed, the second hides inside the first whenever an old client
        /// is in the fleet.
        SchedulerCredentialsMalformed,

        /// Connections turned away because the `0xFC` listener already holds every
        /// connection it will.
        ///
        /// **Never summed with `WorkerJobsRefusedEndpointBusy`**, which shares its
        /// wire code (`endpoint-busy`) and nothing else. That one says one request was
        /// too big for the bytes currently in flight and a smaller or later one would
        /// be served; this says the surface has no room for another *conversation* at
        /// all. An operator raises a connection ceiling for one and looks at request
        /// sizes for the other.
        ///
        /// Counted by the endpoint rather than by a surface, because it is decided at
        /// accept -- before a header exists -- so it names no verb and no owning
        /// component can be asked for it.
        NodeFrameConnectionsRefusedAtCapacity,

        /// Reclaim reports the buffer between the storage tiers and the keyspace
        /// notifier could not hold, so the `expired` / `evicted` events for those
        /// keys were never published.
        ///
        /// Exported because absent is not zero here either: without it a
        /// subscriber seeing no `expired` frame cannot tell "nothing expired"
        /// from "the buffer overflowed". It rises when one call reclaims more
        /// than the bound at once, which in practice means a `maxmemory` shrink
        /// on a large cache.
        KeyspaceReclaimEventsDropped,

        /// Sweeps the active expiry cycle has run.
        ///
        /// The denominator for the line below, and on its own the answer to "is
        /// the cycle running at all" -- which is otherwise indistinguishable
        /// from "nothing has expired", since both publish no events and reclaim
        /// nothing. A flat count here on a daemon serving traffic means the
        /// cycle is disabled or wedged.
        ExpiryCycles,
        /// Entries the active expiry cycle reclaimed.
        ///
        /// Keys that lapsed and that no client would ever have touched again,
        /// so nothing else would have reclaimed them. Rising here alongside a
        /// rising `KeyspaceReclaimEventsDropped` means the sweep is finding
        /// more per cycle than the notification buffer can carry, and the
        /// `expired` events for the excess are being lost.
        ExpiryKeysReclaimed,

        /// Stored values that did not decode as the type their flags claim (#296).
        ///
        /// **Its own counter because the alternative is a lie or a silence.** These
        /// used to be reported as `StorageErrorCode::Corrupt`, so a client planting a
        /// malformed set moved `fastcached_write_errors_total` and wrote a "storage
        /// write failed" line -- an unprivileged client driving the disk-failure
        /// signal on a healthy store. Taking the code away without adding this would
        /// have traded a wrong signal for no signal at all: the operator would then
        /// see nothing while clients sent nonsense.
        ///
        /// Read it as "clients are sending malformed sets or streams", never as a
        /// statement about the disk -- what says the disk is failing is
        /// `fastcached_write_errors_total` beside a `Corrupt` in the log, and this
        /// counter exists precisely so those two never share a line again.
        ///
        /// Both the read and the write path count here: a malformed value is found by
        /// whichever verb decodes it first, and SMEMBERS reaches it as readily as
        /// SADD.
        CacheMalformedValues,

        /// Connections the frame surface swept before the peer had named a verb.
        ///
        /// The pre-header window (`FrameServer::HeaderTimeout`, five seconds), armed
        /// at accept and again before every header read. A peer counted here told the
        /// surface nothing at all, which is why this is the ENDPOINT's row and not any
        /// responder's: there is no verb to attribute it to, and a row that named one
        /// could sum with a verb-attributed refusal that means something else.
        ///
        /// **Not an ordinary event**, though it looks like it might be. `RunOneExchange`
        /// opens a connection, sends immediately, reads the reply and closes, so no
        /// honest client of this surface idles across the window. A rise is a peer that
        /// connected and did not speak -- a scanner, a health probe pointed at the
        /// wrong port, or a client wedged before its first write.
        ///
        /// **Never summed with `FrameAnswerDeadlineSweeps`.** That one says a request
        /// this surface accepted outran its own budget, which is a fleet problem; this
        /// one says somebody knocked and said nothing, which is background noise on any
        /// reachable port. Sharing a row would bury the first under the second.
        FrameRequestDeadlineSweeps,

        /// Connections the frame surface swept after the peer had named a verb.
        ///
        /// The verb's own window (`IFrameResponder::RequestTimeout`), armed once the
        /// header decodes -- a round trip for a scheduler verb, and
        /// `DefaultCompileLeaseTimeout` for a compile.
        ///
        /// **This is the rare, load-bearing one.** For a compile it means a translation
        /// unit outran the lease it was granted under, so the object it would have
        /// produced is one the scheduler may already have re-granted to somebody else.
        /// Before this existed the connection was closed silently and the only trace was
        /// a Debug log line carrying an undifferentiated total, so a site whose units are
        /// genuinely longer than the grant could only INFER that from builds that got
        /// slower.
        ///
        /// A rise is a question about the lease timeout, never about this worker's
        /// speed. See the issue linked from `FrameServer::CloseOverdue`.
        FrameAnswerDeadlineSweeps,

        Last,
    };

    /// Increment the named counter by 1 (or `by`).
    virtual void Increment(Counter counter, std::uint64_t by = 1) noexcept = 0;

    /// Read the current value of a counter.
    [[nodiscard]] virtual std::uint64_t Read(Counter counter) const noexcept = 0;
};

/// Default atomic-counter sink.
class AtomicMetricsSink final: public IMetricsSink
{
  public:
    void Increment(Counter counter, std::uint64_t by = 1) noexcept override
    {
        auto const idx = static_cast<std::size_t>(counter);
        if (idx >= static_cast<std::size_t>(Counter::Last))
            return;
        _counters[idx].fetch_add(by, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t Read(Counter counter) const noexcept override
    {
        auto const idx = static_cast<std::size_t>(counter);
        if (idx >= static_cast<std::size_t>(Counter::Last))
            return 0;
        return _counters[idx].load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> _counters[static_cast<std::size_t>(Counter::Last)] {};
};

} // namespace FastCache
