// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

/// The `0xFC` compile-cache wire format — the single source of truth shared by
/// the daemon's handler, the `fastcache-cc` launcher, and the protocol test
/// client.
///
/// **This header must stay dependency-free** (the standard library plus the
/// header-only `Core/Endian.hpp`, and nothing else). `fastcache-cc` deliberately
/// does not link the `FastCache` library — it compiles a handful of leaf sources
/// in directly so it pulls no vcpkg dependency — so an include of anything from
/// `Net/`, `Cache/`, `Async/` or `Config/` here breaks the launcher's **link**,
/// not merely its build. This is the same constraint `Cli/UsageDoc` carries, and
/// for the same reason. Being header-only is also what keeps it free: it costs no
/// row in `_fc_cc_core`.
///
/// **The module performs no I/O and holds no state.** Every function here is a
/// pure transform between bytes and structs. That is a deliberate exception to
/// the project's inject-every-dependency rule rather than an oversight: there is
/// no clock, socket, filesystem or environment behind any of it, so there is
/// nothing to inject. Reading and writing stay at the two call sites, which have
/// very different concurrency models — a coroutine reader on the server, a
/// blocking socket on the client — and share these builders and parsers verbatim.
///
/// ## Frame layout
///
/// A request is a fixed 7-byte header followed by exactly `payloadLength` bytes:
///
/// ```
/// [u8 magic=0xFC][u8 version][u8 op][u32 payloadLength]
/// payload := field*        where field := [u32 len][len bytes]
/// ```
///
/// A reply is a fixed 5-byte header followed by exactly `payloadLength` bytes,
/// **uniformly for every status** — including a miss, which carries a zero-length
/// payload rather than no payload at all:
///
/// ```
/// [u8 status][u32 payloadLength][payload]
/// ```
///
/// The declared lengths are what make the protocol extensible. A receiver that
/// does not recognise an opcode can skip exactly `payloadLength` bytes, answer
/// with a typed error, and stay in sync — so adding a verb is not a breaking
/// change, and a rejection can be a *reply* instead of a dropped connection.
/// Without them (as in the pre-1 format) the only possible response to anything
/// unrecognised was to close, which is indistinguishable from a dead peer.
///
/// All multi-byte integers are big-endian.
///
/// ## Authentication, and why it costs no round trip
///
/// When the server requires a credential, every verb whose `OpDescriptor::preAuth`
/// is false is refused with `Unauthenticated` until an `Auth` frame has been
/// accepted on that connection. That is per-connection state, and the launcher
/// opens a fresh connection *per operation* — so the obvious spelling (send AUTH,
/// await its reply, then send the real command) would double the round trips on
/// exactly the path the "no handshake" decision above exists to protect.
///
/// It does not have to be spelled that way. Replies are strictly ordered and
/// one-per-request, so a client may **pipeline**: write `AUTH` and the real
/// command in a single write, then read the two replies in order. The credential
/// costs a few dozen bytes in a segment that was going to be sent anyway, and the
/// round-trip count is unchanged. `Cc::CacheFetch`/`Cc::CacheStore` do exactly
/// this. A server must therefore never coalesce, reorder, or skip a reply — which
/// the framing already guarantees, since every request is answered exactly once.
namespace FastCache::CompileCacheWire
{

/// First byte of every compile-cache frame. Distinct from the memcached binary
/// magic (0x80) and from every RESP first byte, so `ProtocolAutodetect` can route
/// a connection on it. Lives here rather than in `ProtocolAutodetect.hpp` because
/// the clients need it and cannot include that header.
inline constexpr std::byte Magic { 0xFC };

/// Protocol version, carried in every request header.
///
/// A plain integer alias rather than an `enum class`: this is an ordered quantity
/// compared against a supported *range*, and an enumeration would need a cast at
/// every comparison. The named constants below are the vocabulary.
using WireVersion = std::uint8_t;

/// The version this build speaks and emits.
///
/// Bump this whenever the framing changes shape. It is deliberately **not**
/// derived from the release version: the wire format changes far more rarely than
/// the product does, and tying them would force a flag day on every release.
inline constexpr WireVersion CurrentVersion = 1;

/// The oldest version this build still accepts. Equal to `CurrentVersion` while
/// only one version exists; widen the range when a second one ships and this
/// build can still decode the older shape.
inline constexpr WireVersion MinSupportedVersion = 1;

/// Size of the fixed request header: magic, version, op, payload length.
inline constexpr std::size_t RequestHeaderSize = WireFrame::HeaderSize;

/// Size of the fixed reply header: status, payload length.
inline constexpr std::size_t ReplyHeaderSize = 5;

/// Largest payload a frame can describe, since the length field is a u32.
///
/// Enforced by the encoders rather than assumed. Casting an over-large size down
/// to `std::uint32_t` would quietly emit a frame whose declared length disagrees
/// with its contents — the exact desynchronisation the declared length exists to
/// prevent, and undetectable by the peer.
///
/// A caller that exceeds it has misused the contract, so the encoders throw
/// `std::length_error` rather than returning a sentinel. That keeps the
/// post-condition simple — an encoder either returns a well-formed frame or does
/// not return — which is what lets every caller index the result without first
/// proving it non-empty. It is also unreachable in practice: the daemon caps
/// values at `--storage-max-value` (256 MiB by default), far below this.
inline constexpr std::uint64_t MaxFramePayload = WireFields::MaxPayload;

/// Wire opcodes. One byte, third in the request header.
enum class Op : std::uint8_t
{
    Store = 0x01, ///< Canonicalize and store a compile result.
    Fetch = 0x02, ///< Retrieve a compile result in canonical form.
    Auth = 0x03,  ///< Present a credential; gates every other verb when auth is on.

    // Distributed execution. These are answered only on a listener whose role
    // mask carries `Dispatch` — a cache-only listener refuses them with
    // `DispatchNotPermitted` rather than serving them, because the surface that
    // causes a compiler to RUN somewhere else has a different trust posture from
    // the one that reads and writes a cache. See `BindConfig::roles`.
    Register = 0x04,  ///< Worker announces its toolchain, endpoint and capacity.
    Heartbeat = 0x05, ///< Worker reports liveness and its current load.
    Lease = 0x06,     ///< Client asks the scheduler for a worker to compile on.
    Compile = 0x07,   ///< Client hands a worker one preprocessed translation unit.

    // Cluster administration. Answered on the scheduler's port by the LEADER
    // only, and only to a member, through the same gate the dispatch verbs go
    // through -- these change what every node in the fleet believes, so the two
    // questions the gate asks are exactly the two that matter.
    ClusterStatus = 0x08, ///< Operator asks what the cluster has agreed.
    ClusterSet = 0x09,    ///< Operator changes a replicated setting.
    ClusterForget = 0x0A, ///< Operator removes a member.
    ClusterAdmit = 0x0B,  ///< Operator adds a member, or moves one.
};

/// Reply status, the first byte of every reply.
///
/// `Miss` and `Ok` keep the byte values the pre-version format used. A miss is a
/// legitimate negative answer, not a failure, and is therefore distinct from
/// `Error` — conflating the two (as the pre-1 format did, where both were `0x00`)
/// makes a rejected client see an endlessly cold cache with no diagnostic.
enum class Status : std::uint8_t
{
    Miss = 0x00,  ///< FETCH found nothing. Payload is empty.
    Ok = 0x01,    ///< Command succeeded. Payload is the result, if any.
    Error = 0x02, ///< Command refused. Payload is `[u8 ErrorCode][message]`.
};

/// Why a command was refused. Travels as the first payload byte of an `Error`
/// reply, ahead of a human-readable message.
///
/// A numeric code rather than the bare string the pre-1 format sent: a client has
/// to *act* differently on a version rejection (stop trying, tell the user the
/// install is mismatched) than on a value the server could not store (carry on,
/// this build is simply uncached), and parsing English to decide is not a
/// contract.
enum class ErrorCode : std::uint8_t
{
    UnsupportedVersion = 0x01, ///< Request version outside this build's range.
    UnknownOpcode = 0x02,      ///< Opcode not in `OpTable`.
    MalformedFrame = 0x03,     ///< Fields do not exactly fill the declared payload.
    PayloadTooLarge = 0x04,    ///< Declared payload exceeds the session's cap.
    MalformedValue = 0x05,     ///< STORE payload is not a decodable compile-value.

    // 0x06 is RETIRED and must never be reassigned -- see `RetiredErrorCodes`
    // below, which makes that a build failure rather than a hope. It was
    // `CanonicalizationFailed`, answering a `PathCanon` failure that no code path
    // could produce: a documented status no server could ever send, removed with
    // `CanonError` itself (issues #59, #69).

    StorageWriteFailed = 0x07, ///< The cache engine refused the write.
    Unauthenticated = 0x08,    ///< A credential is required and has not been accepted.

    // Distributed execution. Every one of these is a REFUSAL the client answers by
    // compiling locally, never by failing: the client is holding the source and has
    // a working fallback, so distribution must be incapable of breaking a build.
    // They are distinct codes rather than one "no" because they mean different
    // things to an operator — no matching toolchain in the fleet is a
    // configuration problem, no free slot is a capacity problem, and a duplicate
    // is neither.
    NoWorker = 0x09,             ///< No registered worker matches the requested toolchain.
    NoCapacity = 0x0A,           ///< Every matching worker is full of this fleet's own work.
    AlreadyInFlight = 0x0B,      ///< Another client already holds a lease for this key.
    DispatchNotPermitted = 0x0C, ///< This listener's role mask does not carry `Dispatch`.
    UnknownLease = 0x0D,         ///< The lease token is unknown, expired, or already spent.
    FingerprintMismatch = 0x0E,  ///< The worker's toolchain is not the one the lease named.
    UnsupportedCodec = 0x0F,     ///< No codec in common between the peers.
    /// The worker could not prepare a scratch directory, or could not write the
    /// translation unit into it. Distinct from the next one because they call for
    /// different operator action -- a full or unwritable disk versus a toolchain
    /// that is configured but not runnable.
    WorkerScratchUnavailable = 0x10,
    /// The worker could not START the compiler. Emphatically NOT "the compiler ran
    /// and rejected the code": that is a successful exchange carrying a non-zero
    /// exit code, and the client retries it locally to get real diagnostics. This
    /// means the program named by the worker's own --toolchain could not be
    /// executed at all.
    WorkerSpawnFailed = 0x11,
    /// This node is not the cluster's leader. The message carries the leader's
    /// endpoint when one is known, so a client can redirect rather than give up --
    /// and is empty during an election, which is a *different* fact and one the
    /// client answers the same way it answers every refusal here: compile locally.
    NotLeader = 0x12,
    /// The caller is not a member of this cluster, so it may not spend the fleet's
    /// capacity. Distinct from `Unauthenticated`, which is about a credential this
    /// endpoint requires; this is about contribution, and a non-member is still
    /// served the cache.
    NotAMember = 0x13,
    /// Matching workers exist and have withdrawn their capacity: their machines are
    /// busy with something other than this fleet, or out of scratch space.
    ///
    /// Distinct from `NoCapacity`, which means the fleet is full of this build's own
    /// work. Both make the client compile locally, so the split is entirely for
    /// whoever has to act on it -- one says "somebody is using your machines" and
    /// the other says "buy more machines", and summing them would send an operator
    /// shopping for hardware they already own.
    Withdrawn = 0x14,
    /// A change the cluster could not accept.
    ///
    /// A setting nobody has heard of, a member named with no address, a field a
    /// verb ignores. Refused at the PROPOSER because that is the only place a
    /// change can be refused at all -- an entry is applied after it is committed,
    /// when there is nobody left to report to. The message carries which, because
    /// the reader is a person and "no such cluster setting: upsteam" is what they
    /// can act on.
    InvalidClusterChange = 0x16,

    /// This endpoint is already serving as much as it will serve at once.
    ///
    /// Emphatically NOT `NoCapacity`, which is a statement about the FLEET -- every
    /// matching worker being full of this build's own work. This one is about a
    /// single node's own front door: it has reached its concurrent-request cap or
    /// its in-flight byte budget, and the same client asking again in a moment will
    /// very likely be served. Reporting one under the other's code would send an
    /// operator shopping for machines when the answer is to raise a local bound, or
    /// the reverse -- the same argument `Withdrawn` makes for splitting off its own
    /// case.
    ///
    /// A refusal a client answers by compiling locally, like every other code in
    /// this range.
    EndpointBusy = 0x17,

    /// This node runs no cluster, so there is nothing to administer.
    ///
    /// Distinct from `NotLeader`, and the difference is what an operator does
    /// next: `NotLeader` names somewhere else to ask, while this says the
    /// question does not apply here at all -- a single node started without
    /// `--node-id` leads itself and has no replicated state to change.
    NoCluster = 0x15,
};

/// Bit for `status` within an `OpDescriptor::legalStatuses` mask.
/// @param status The status to encode.
/// @return A single-bit mask.
[[nodiscard]] constexpr std::uint8_t StatusBit(Status status) noexcept
{
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(status));
}

/// One row of the opcode table: everything the framing layer knows about a verb.
struct OpDescriptor
{
    Op code;                    ///< The wire byte.
    std::string_view name;      ///< Stable lower-case name, for logs and diagnostics.
    std::size_t fieldCount;     ///< Exact number of length-prefixed request fields.
    std::uint8_t legalStatuses; ///< Mask of the statuses this op may be answered with.
    /// Whether this verb may be served before a credential has been accepted.
    ///
    /// A column rather than a predicate with its own `switch`: "which verbs are
    /// reachable by an unauthenticated peer" is the security-relevant property of
    /// the whole table, and a reviewer must be able to read it off the table
    /// itself. A verb added without thinking about it defaults to `false` —
    /// closed, which is the direction a mistake here has to fail in.
    bool preAuth;
    /// Largest payload this verb may declare, or 0 for "the session cap".
    ///
    /// A verb reachable *before* authentication MUST declare a real bound, and
    /// `PreAuthVerbsAreBounded` asserts that it does. The pre-auth gate exists so
    /// an unauthenticated peer cannot make the server allocate `maxPayloadBytes`
    /// (256 MiB by default) per frame — and a verb waved through that gate is read
    /// with exactly that allocation unless it says otherwise, which would defeat
    /// the gate through the one door it deliberately holds open.
    ///
    /// A gated verb may legitimately leave this 0: STORE carries a whole object
    /// file, and by the time it is read the peer has authenticated, so the
    /// operator's own cap is the right bound.
    std::size_t maxPayload;
};

/// Payload ceiling for AUTH: a username and a shared secret, and nothing that
/// grows with a build artefact. Generous by orders of magnitude against any real
/// credential, and small enough that a peer which has proved nothing cannot use
/// it to make the server take memory.
inline constexpr std::size_t MaxAuthPayload = 4096;

/// Payload ceiling for the scheduler's control verbs (`Register`, `Heartbeat`,
/// `Lease`).
///
/// These carry identifiers, an endpoint, a fingerprint and small integers — never
/// anything that scales with a build artefact. `Compile` is the deliberate
/// exception and leaves its ceiling to the operator's cap, because it carries a
/// preprocessed translation unit, which is measured in megabytes.
///
/// Bounding them matters for a reason `MaxAuthPayload` does not share: these verbs
/// are answered on a listener a whole fleet is meant to reach, and a scheduler
/// that can be made to allocate 256 MiB per frame by anything that authenticated
/// once is a scheduler that stops scheduling.
inline constexpr std::size_t MaxControlPayload = 64 * 1024;

/// Every opcode this build understands.
///
/// `fieldCount` is the single source of the request arity — parsers take it from
/// here rather than spelling a literal, so an op's shape lives in exactly one
/// place. `legalStatuses` makes "a FETCH may miss but a STORE may not" a property
/// of the data instead of a convention, and is assertable in tests.
///
/// The array size is deduced rather than spelled: a count that has to be edited
/// alongside the rows is a second place to change when a verb is added, which is
/// exactly what this table exists to avoid.
inline constexpr std::array OpTable {
    OpDescriptor { .code = Op::Store,
                   .name = "store",
                   .fieldCount = 5, // key, prefetchGroup, srcRoot, buildTree, value
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = 0 }, // an object file; bounded by the operator's cap
    OpDescriptor { .code = Op::Fetch,
                   .name = "fetch",
                   .fieldCount = 1, // key
                   .legalStatuses =
                       static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Miss) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = 0 },
    OpDescriptor { .code = Op::Auth,
                   .name = "auth",
                   .fieldCount = 2, // username, secret
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = true,
                   .maxPayload = MaxAuthPayload },

    // Distributed execution. None is `preAuth`: causing a compiler to run on
    // another machine is the last thing an unauthenticated peer should reach.
    OpDescriptor { .code = Op::Register,
                   .name = "register",
                   .fieldCount = 5, // fingerprint, endpoint, u32 slots, accepted codecs, capacity
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = MaxControlPayload },
    OpDescriptor { .code = Op::Heartbeat,
                   .name = "heartbeat",
                   .fieldCount = 3, // workerId, u32 inFlight, load
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = MaxControlPayload },
    OpDescriptor { .code = Op::Lease,
                   .name = "lease",
                   .fieldCount = 3, // fingerprint, key, accepted codecs
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = MaxControlPayload },
    OpDescriptor { .code = Op::ClusterStatus,
                   .name = "cluster-status",
                   .fieldCount = 0, // nothing to ask with
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = MaxControlPayload },
    OpDescriptor { .code = Op::ClusterSet,
                   .name = "cluster-set",
                   .fieldCount = 2, // setting name, value
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = MaxControlPayload },
    OpDescriptor { .code = Op::ClusterForget,
                   .name = "cluster-forget",
                   .fieldCount = 1, // member id
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = MaxControlPayload },
    OpDescriptor { .code = Op::ClusterAdmit,
                   .name = "cluster-admit",
                   .fieldCount = 2, // member id, consensus endpoint
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = MaxControlPayload },
    OpDescriptor { .code = Op::Compile,
                   .name = "compile",
                   .fieldCount = 6, // leaseToken, fingerprint, args, preprocessed, accepted codecs, sourceName
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = 0 }, // carries a preprocessed TU; the operator's cap governs
};

/// Whether every verb reachable before authentication declares a payload bound.
///
/// A `static_assert`ed invariant rather than a comment, because getting it wrong
/// is not a bug in the new verb — it is the pre-auth allocation gate silently
/// ceasing to hold for the whole protocol.
/// @return True when no `preAuth` row leaves `maxPayload` at 0.
[[nodiscard]] constexpr bool PreAuthVerbsAreBounded() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) { return !row.preAuth || row.maxPayload != 0; });
}

static_assert(PreAuthVerbsAreBounded(), "a verb reachable before AUTH must declare its own payload ceiling");

/// One row of the error table: the code, its stable name, and the message sent
/// when the caller has nothing more specific to say.
struct ErrorDescriptor
{
    ErrorCode code;                  ///< The wire byte.
    std::string_view name;           ///< Stable lower-case name, for logs and tests.
    std::string_view defaultMessage; ///< Human-readable text sent when no detail is supplied.
};

/// Every error code this build emits. Size deduced, for the reason `OpTable`'s is.
inline constexpr std::array ErrorTable {
    ErrorDescriptor {
        .code = ErrorCode::UnsupportedVersion, .name = "unsupported-version", .defaultMessage = "unsupported wire version" },
    ErrorDescriptor { .code = ErrorCode::UnknownOpcode, .name = "unknown-opcode", .defaultMessage = "unknown opcode" },
    ErrorDescriptor { .code = ErrorCode::MalformedFrame, .name = "malformed-frame", .defaultMessage = "malformed frame" },
    ErrorDescriptor {
        .code = ErrorCode::PayloadTooLarge, .name = "payload-too-large", .defaultMessage = "payload too large" },
    ErrorDescriptor {
        .code = ErrorCode::MalformedValue, .name = "malformed-value", .defaultMessage = "malformed compile-value frame" },
    ErrorDescriptor {
        .code = ErrorCode::StorageWriteFailed, .name = "storage-write-failed", .defaultMessage = "storage write failed" },
    ErrorDescriptor {
        .code = ErrorCode::Unauthenticated, .name = "unauthenticated", .defaultMessage = "authentication required" },
    ErrorDescriptor {
        .code = ErrorCode::NoWorker, .name = "no-worker", .defaultMessage = "no worker matches this toolchain" },
    ErrorDescriptor {
        .code = ErrorCode::NoCapacity, .name = "no-capacity", .defaultMessage = "every matching worker is busy" },
    ErrorDescriptor { .code = ErrorCode::AlreadyInFlight,
                      .name = "already-in-flight",
                      .defaultMessage = "another client is already compiling this key" },
    ErrorDescriptor { .code = ErrorCode::DispatchNotPermitted,
                      .name = "dispatch-not-permitted",
                      .defaultMessage = "this endpoint does not serve distributed execution" },
    ErrorDescriptor {
        .code = ErrorCode::UnknownLease, .name = "unknown-lease", .defaultMessage = "unknown or expired lease" },
    ErrorDescriptor { .code = ErrorCode::FingerprintMismatch,
                      .name = "fingerprint-mismatch",
                      .defaultMessage = "this worker's toolchain is not the one the lease named" },
    ErrorDescriptor {
        .code = ErrorCode::UnsupportedCodec, .name = "unsupported-codec", .defaultMessage = "no codec in common" },
    ErrorDescriptor { .code = ErrorCode::WorkerScratchUnavailable,
                      .name = "worker-scratch-unavailable",
                      .defaultMessage = "the worker could not prepare a scratch directory" },
    ErrorDescriptor { .code = ErrorCode::WorkerSpawnFailed,
                      .name = "worker-spawn-failed",
                      .defaultMessage = "the worker could not start the compiler" },
    ErrorDescriptor {
        .code = ErrorCode::NotLeader, .name = "not-leader", .defaultMessage = "this node does not lead the cluster" },
    ErrorDescriptor {
        .code = ErrorCode::NotAMember, .name = "not-a-member", .defaultMessage = "not a member of this cluster" },
    ErrorDescriptor { .code = ErrorCode::Withdrawn,
                      .name = "withdrawn",
                      .defaultMessage = "every matching worker has withdrawn its capacity" },
    ErrorDescriptor { .code = ErrorCode::NoCluster, .name = "no-cluster", .defaultMessage = "this node runs no cluster" },
    ErrorDescriptor { .code = ErrorCode::InvalidClusterChange,
                      .name = "invalid-cluster-change",
                      .defaultMessage = "the cluster cannot accept that change" },
    ErrorDescriptor { .code = ErrorCode::EndpointBusy,
                      .name = "endpoint-busy",
                      .defaultMessage = "this endpoint is serving all it will serve at once" },
};

/// Wire bytes that once meant something and must never mean anything again.
///
/// A retired code cannot simply be freed. Peers are built against different
/// revisions of this header, so a launcher compiled before the retirement still
/// maps the byte to its old name -- and a NEW meaning assigned to it would be
/// reported by that peer as the old one, which is the single worst way for a
/// refusal to be wrong. `ErrorCode` is already non-dense (`NoCluster = 0x15` is
/// declared after `EndpointBusy = 0x17`), so an author picking the next free
/// number by scanning the enum has no reason to suspect this one is different.
///
/// A row here rather than a comment for the reason `PreAuthVerbsAreBounded` is a
/// `static_assert`: getting it wrong is silent everywhere it matters and visible
/// nowhere, so the build is the only place it can be caught.
inline constexpr std::array<std::uint8_t, 1> RetiredErrorCodes { 0x06 };

/// Whether the error table has kept clear of every retired byte.
///
/// Checks the TABLE rather than the enum, because the table is what a reuse must
/// touch to be usable: `Describe` answers an untabled code with nullptr, so an
/// enumerator alone renders as "unknown" and cannot impersonate the retired
/// meaning. Nothing in C++23 can enumerate an enum's values anyway.
///
/// @return True when no `ErrorTable` row claims a retired byte.
[[nodiscard]] consteval bool NoRetiredErrorCodeIsReused() noexcept
{
    return std::ranges::none_of(ErrorTable, [](ErrorDescriptor const& row) {
        return std::ranges::contains(RetiredErrorCodes, static_cast<std::uint8_t>(row.code));
    });
}

static_assert(NoRetiredErrorCodeIsReused(),
              "a retired wire code must never be reassigned -- a peer built against an older header still reports it "
              "under its old name (0x06 was canonicalization-failed; see issues #59, #69)");

/// Verbs that legitimately carry no fields at all.
///
/// Zero is also what a row that forgot its `fieldCount` would hold, and such a row
/// would then demand an empty payload and silently refuse every well-formed request.
/// So "this verb asks nothing" is stated here rather than left looking like an
/// omission, and `FieldCountsAgree` checks the two in both directions -- which is
/// what makes this a check rather than a second place to be wrong.
inline constexpr std::array FieldlessOps { Op::ClusterStatus };

/// Whether `op` legitimately carries no fields.
/// @param op The verb.
/// @return True when it is listed as fieldless.
[[nodiscard]] constexpr bool CarriesNoFields(Op op) noexcept
{
    return std::ranges::contains(FieldlessOps, op);
}

/// Whether every zero field count is a deliberate one, and every deliberate one is
/// zero.
/// @return True when the two tables agree.
[[nodiscard]] consteval bool FieldCountsAgree() noexcept
{
    return std::ranges::all_of(OpTable,
                               [](OpDescriptor const& row) { return (row.fieldCount == 0) == CarriesNoFields(row.code); });
}

static_assert(FieldCountsAgree(), "a verb carries fields, or is listed as carrying none -- never neither, never both");

/// Look up the descriptor for a raw opcode byte.
/// @param opRaw The third header byte, as received.
/// @return The descriptor, or nullptr when this build does not know the opcode.
[[nodiscard]] constexpr OpDescriptor const* FindOp(std::uint8_t opRaw) noexcept
{
    for (auto const& row: OpTable)
        if (static_cast<std::uint8_t>(row.code) == opRaw)
            return &row;
    return nullptr;
}

/// Look up the descriptor for an error code.
/// @param code The code to describe.
/// @return The descriptor, or nullptr when the code is not in the table.
[[nodiscard]] constexpr ErrorDescriptor const* Describe(ErrorCode code) noexcept
{
    for (auto const& row: ErrorTable)
        if (row.code == code)
            return &row;
    return nullptr;
}

/// Request field count for a known opcode, from the table.
/// @param op The opcode.
/// @return The field count, or 0 when the op is not in the table.
[[nodiscard]] constexpr std::size_t OpFieldCount(Op op) noexcept
{
    auto const* row = FindOp(static_cast<std::uint8_t>(op));
    return row != nullptr ? row->fieldCount : 0;
}

/// The payload ceiling a raw opcode declares, bounded by the session's own cap.
///
/// Takes the raw byte and the session cap together so a caller cannot apply one
/// without the other: the per-op bound is an *additional* restriction, never a
/// licence to exceed what the operator configured.
/// @param opRaw The third header byte, as received.
/// @param sessionCap The session's configured maximum payload.
/// @return The effective ceiling for this frame.
[[nodiscard]] constexpr std::size_t OpPayloadCap(std::uint8_t opRaw, std::size_t sessionCap) noexcept
{
    auto const* row = FindOp(opRaw);
    if (row == nullptr || row->maxPayload == 0)
        return sessionCap;
    return row->maxPayload < sessionCap ? row->maxPayload : sessionCap;
}

/// Whether `op` may legally be answered with `status`, per the table.
/// @param op The opcode.
/// @param status The status under consideration.
/// @return True when the pairing is allowed.
[[nodiscard]] constexpr bool IsLegalStatus(Op op, Status status) noexcept
{
    auto const* row = FindOp(static_cast<std::uint8_t>(op));
    return row != nullptr && (row->legalStatuses & StatusBit(status)) != 0;
}

/// Whether `op` may be served to a peer that has not authenticated, per the table.
///
/// Takes the raw opcode byte, not an `Op`: the gate has to answer for whatever
/// arrived on the wire, and an unknown opcode must read as "not allowed" rather
/// than force the caller to resolve the descriptor first and decide what a null
/// row means. An unknown verb is refused on its own grounds anyway, but a gate
/// that fails open for anything is the wrong shape regardless of who calls it.
/// @param opRaw The third header byte, as received.
/// @return True only for a known verb whose row permits it.
[[nodiscard]] constexpr bool IsPreAuthAllowed(std::uint8_t opRaw) noexcept
{
    auto const* row = FindOp(opRaw);
    return row != nullptr && row->preAuth;
}

/// Whether this build can decode a request at `version`.
/// @param version The version byte from a request header.
/// @return True when within [MinSupportedVersion, CurrentVersion].
[[nodiscard]] constexpr bool IsSupported(WireVersion version) noexcept
{
    return WireFrame::IsSupported(version, MinSupportedVersion, CurrentVersion);
}

// The byte/text reinterpretation and the length-prefixed field grammar below are
// protocol-agnostic and live in `Core/WireFields.hpp`, so this format and the
// Raft one cannot drift apart. Re-exported under the names call sites here
// already use.
using WireFields::AsBytes;
using WireFields::AsStringView;

/// The decoded fixed part of a request. The opcode is kept **raw** because an
/// unrecognised one is a recoverable condition the caller answers with a typed
/// error, not a decode failure.
struct RequestHeader
{
    WireVersion version {};         ///< Protocol version the sender used.
    std::uint8_t opRaw {};          ///< Opcode byte, not yet validated against OpTable.
    std::uint32_t payloadLength {}; ///< Exact byte count following the header.
};

/// The decoded fixed part of a reply.
struct ReplyHeader
{
    Status status {};               ///< Outcome of the command.
    std::uint32_t payloadLength {}; ///< Exact byte count following the header.
};

/// The five fields of a STORE request, as spans into the caller's payload buffer.
///
/// Views, not copies: the value is routinely a multi-megabyte object file and
/// must not be duplicated on the way in. The referenced buffer must outlive the
/// view — on the server that buffer is a coroutine local, which lives in the
/// coroutine frame and so survives suspension.
struct StoreView
{
    std::span<std::byte const> key;           ///< Cache key.
    std::span<std::byte const> prefetchGroup; ///< Prefetch group id, may be empty.
    std::span<std::byte const> srcRoot;       ///< Producer's source root.
    std::span<std::byte const> buildTree;     ///< Producer's build tree.
    std::span<std::byte const> value;         ///< Encoded compile-value.
};

/// The two fields of an AUTH request, as spans into the caller's payload buffer.
struct AuthView
{
    std::span<std::byte const> username; ///< Username; empty selects the default user.
    std::span<std::byte const> secret;   ///< Shared secret.
};

/// The fields of an AUTH request, for encoding.
///
/// The username may be empty, which asks to be verified against the secret alone
/// — the redis one-argument `AUTH <pass>` / `requirepass` form. Carrying it as an
/// always-present (possibly empty) field rather than a separate one-field opcode
/// keeps the arity fixed, so the frame shape does not depend on which credential
/// style the client happens to use.
struct AuthRequest
{
    std::string_view username; ///< Username, or empty for the default user.
    std::string_view secret;   ///< Shared secret.
};

/// The fields of a STORE request, as owning views for encoding.
struct StoreRequest
{
    std::string_view key;             ///< Cache key.
    std::string_view prefetchGroup;   ///< Prefetch group id, may be empty.
    std::string_view srcRoot;         ///< This machine's source root.
    std::string_view buildTree;       ///< This machine's build tree.
    std::span<std::byte const> value; ///< Encoded compile-value.
};

namespace Detail
{

    /// Build a complete request frame. **The only place a request header is
    /// written** — every encoder funnels through here, so the layout has exactly
    /// one author.
    ///
    /// The frame is sized exactly once and then filled in place, header and
    /// payload together. Encoding the payload into a buffer of its own and then
    /// prepending the header would be the obvious spelling and is what
    /// `WireFields::EncodeInto` exists to avoid: a STORE frame carries a whole
    /// object file, so the extra copy would raise peak footprint from about twice
    /// the object to three times it, on the hot path of a parallel build.
    /// @param version Version to advertise.
    /// @param op The opcode.
    /// @param fields The length-prefixed fields, in wire order.
    /// @return The framed request.
    [[nodiscard]] inline std::vector<std::byte> EncodeRequest(WireVersion version,
                                                              Op op,
                                                              std::initializer_list<std::span<std::byte const>> fields)
    {
        auto const view = WireFields::AsFields(fields);
        auto const payloadSize = WireFields::RequireEncodable(view);

        std::vector<std::byte> frame(RequestHeaderSize + payloadSize);
        std::span<std::byte> const out { frame };
        WireFrame::PutHeader(out, Magic, version, static_cast<std::uint8_t>(op), static_cast<std::uint32_t>(payloadSize));
        WireFields::EncodeInto(out, RequestHeaderSize, view);
        return frame;
    }

} // namespace Detail

/// Frame a STORE request.
/// @param request The fields to send.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeStore(StoreRequest const& request, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version,
                                 Op::Store,
                                 { AsBytes(request.key),
                                   AsBytes(request.prefetchGroup),
                                   AsBytes(request.srcRoot),
                                   AsBytes(request.buildTree),
                                   request.value });
}

/// Frame a FETCH request.
/// @param key The key to look up.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeFetch(std::string_view key, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::Fetch, { AsBytes(key) });
}

/// Frame an AUTH request.
/// @param request The credential to present.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeAuth(AuthRequest const& request, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::Auth, { AsBytes(request.username), AsBytes(request.secret) });
}

/// Frame a reply. **The only place a reply header is written.**
///
/// A `Miss` is this function with an empty payload — there is no separate
/// miss encoder, because a second spelling of one thing is how the two drift.
/// @param status The outcome.
/// @param payload The reply body; empty for a miss.
/// @return The framed reply.
[[nodiscard]] inline std::vector<std::byte> EncodeReply(Status status, std::span<std::byte const> payload)
{
    if (payload.size() > MaxFramePayload)
        throw std::length_error("compile-cache reply payload exceeds the u32 wire length");

    std::vector<std::byte> frame(ReplyHeaderSize + payload.size());
    std::span<std::byte> const out { frame };
    out[0] = static_cast<std::byte>(status);
    WireFields::PutBigEndian<std::uint32_t>(out, 1, static_cast<std::uint32_t>(payload.size()));
    std::ranges::copy(payload, out.subspan(ReplyHeaderSize).begin());
    return frame;
}

/// Frame an `Error` reply carrying a code and a message.
/// @param code The refusal reason.
/// @param message Detail for a human; falls back to the table's default when empty.
/// @return The framed reply.
[[nodiscard]] inline std::vector<std::byte> EncodeErrorReply(ErrorCode code, std::string_view message = {})
{
    if (message.empty())
        if (auto const* row = Describe(code); row != nullptr)
            message = row->defaultMessage;
    if (message.size() >= MaxFramePayload)
        throw std::length_error("compile-cache error message exceeds the u32 wire length");

    std::vector<std::byte> payload(1 + message.size());
    payload[0] = static_cast<std::byte>(code);
    std::ranges::copy(AsBytes(message), std::next(payload.begin()));
    return EncodeReply(Status::Error, payload);
}

/// Decode the fixed request header.
///
/// Validates **only the magic** — a wrong magic means the peer is not speaking
/// this protocol at all, the one condition that still has to close the
/// connection. The version and the opcode are checked separately by `IsSupported`
/// and `FindOp`, because each maps to a different error code and a different
/// recovery, and the *order* of those checks belongs to the handler.
/// @param bytes Exactly `RequestHeaderSize` bytes.
/// @return The header, or nullopt when short or not this protocol.
[[nodiscard]] inline std::optional<RequestHeader> DecodeRequestHeader(std::span<std::byte const> bytes)
{
    auto const header = WireFrame::DecodeHeader(bytes, Magic);
    if (!header.has_value())
        return std::nullopt;

    // Rebuilt into this protocol's own struct rather than aliased, unlike
    // `RaftWire::FrameHeader`. The field is named `opRaw` at some seventy call
    // sites across the daemon, the launcher and the test client, and renaming
    // them to share a struct would be a large diff whose only effect is that two
    // protocols spell one byte the same way. The *layout* is what had to stop
    // being duplicated, and it has.
    return RequestHeader { .version = header->version, .opRaw = header->kindRaw, .payloadLength = header->payloadLength };
}

/// Decode the fixed reply header.
/// @param bytes Exactly `ReplyHeaderSize` bytes.
/// @return The header, or nullopt when short or the status byte is unknown.
[[nodiscard]] inline std::optional<ReplyHeader> DecodeReplyHeader(std::span<std::byte const> bytes)
{
    if (bytes.size() < ReplyHeaderSize)
        return std::nullopt;
    auto const raw = static_cast<std::uint8_t>(bytes[0]);
    if (raw != static_cast<std::uint8_t>(Status::Miss) && raw != static_cast<std::uint8_t>(Status::Ok)
        && raw != static_cast<std::uint8_t>(Status::Error))
        return std::nullopt;
    return ReplyHeader { .status = static_cast<Status>(raw),
                         .payloadLength = ReadBigEndian<std::uint32_t>(bytes.subspan(1, sizeof(std::uint32_t))) };
}

/// Split a request payload into exactly `expectedCount` length-prefixed fields.
///
/// Strict in both directions: a field length that overruns the payload and any
/// trailing byte after the last field are both rejected. The declared payload
/// length and the per-field lengths are redundant by design, and disagreement
/// between them is a typed, recoverable `MalformedFrame` rather than the silent
/// desynchronisation it would have been without a declared total.
///
/// Takes the count as a runtime argument rather than a template parameter so the
/// arity has one home — `OpDescriptor::fieldCount` — instead of being spelled
/// again at every call site.
///
/// @param payload The bytes following the request header.
/// @param expectedCount Field count, from the op's descriptor.
/// @return The fields as spans into `payload`, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::vector<std::span<std::byte const>>> SplitFields(std::span<std::byte const> payload,
                                                                                        std::size_t expectedCount)
{
    return WireFields::SplitExactly(payload, expectedCount);
}

/// Split a STORE payload into its five named fields.
/// @param payload The bytes following the request header.
/// @return The field views, or nullopt when malformed.
[[nodiscard]] inline std::optional<StoreView> DecodeStorePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Store));
    if (!fields.has_value())
        return std::nullopt;
    return StoreView { .key = (*fields)[0],
                       .prefetchGroup = (*fields)[1],
                       .srcRoot = (*fields)[2],
                       .buildTree = (*fields)[3],
                       .value = (*fields)[4] };
}

/// Split a FETCH payload into its single key field.
/// @param payload The bytes following the request header.
/// @return The key, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::span<std::byte const>> DecodeFetchPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Fetch));
    if (!fields.has_value())
        return std::nullopt;
    return (*fields)[0];
}

/// Split an AUTH payload into its two named fields.
/// @param payload The bytes following the request header.
/// @return The field views, or nullopt when malformed.
[[nodiscard]] inline std::optional<AuthView> DecodeAuthPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Auth));
    if (!fields.has_value())
        return std::nullopt;
    return AuthView { .username = (*fields)[0], .secret = (*fields)[1] };
}

/// Split an `Error` reply payload into its code and message.
/// @param payload The bytes following the reply header.
/// @return The code and message, or nullopt when the payload is empty.
[[nodiscard]] inline std::optional<std::pair<ErrorCode, std::string_view>> DecodeErrorPayload(
    std::span<std::byte const> payload)
{
    if (payload.empty())
        return std::nullopt;
    return std::pair { static_cast<ErrorCode>(payload[0]), AsStringView(payload.subspan(1)) };
}

// --- distributed execution ---------------------------------------------------
//
// Everything below frames the four dispatch verbs. Two conventions are shared by
// all of them and are worth stating once.
//
// **Integers travel big-endian in a length-prefixed field of their own**, the way
// every other multi-byte quantity here does. A field holding a `u32` is exactly
// four bytes; a decoder that finds any other length rejects the frame rather than
// reading what it can, because a short integer field is a sender this build does
// not understand rather than a value to guess at.
//
// **Bulk payloads travel in a codec envelope**, `[u8 codec][u32 rawLen][bytes]`.
// The codec byte is the id from `Core/Compression.hpp`'s `CompressionCodec`, and
// `Identity` (0) is the uncompressed form, always available even in a build
// configured without compression. The id is NOT re-declared here: this header must
// stay dependency-free (see the file header), so it frames the byte and leaves its
// meaning to the one enum that owns it. Both ends static_assert the agreement in a
// translation unit that includes both, so drift is a build failure rather than a
// wire incompatibility discovered in production.
//
// `rawLen` is the size BEFORE compression and is what a receiver sizes its output
// buffer from. Carrying it is what lets a decoder reject a payload whose declared
// expansion exceeds its cap before decompressing a byte — a compressed frame is
// otherwise an unbounded allocation wearing a small size on the wire.

/// Size of a codec envelope's fixed header: the codec byte plus the raw length.
inline constexpr std::size_t CodecHeaderSize = 1 + sizeof(std::uint32_t);

/// The codec id meaning "stored verbatim". Mirrors `CompressionCodec::Identity`,
/// which is asserted equal wherever both headers are visible.
inline constexpr std::uint8_t IdentityCodec = 0;

/// A bulk payload as it travels: a codec tag, the pre-compression size, and the
/// (possibly compressed) bytes.
struct CodecEnvelope
{
    std::uint8_t codec { IdentityCodec }; ///< `CompressionCodec` id.
    std::uint32_t rawLength { 0 };        ///< Size before compression.
    std::span<std::byte const> bytes;     ///< The payload as it travels.
};

/// Frame a bulk payload into a codec envelope.
/// @param codec The `CompressionCodec` id the bytes are encoded with.
/// @param rawLength Size before compression; equal to `bytes.size()` for Identity.
/// @param bytes The encoded bytes.
/// @return The envelope, ready to be used as one length-prefixed field.
[[nodiscard]] inline std::vector<std::byte> EncodeCodecEnvelope(std::uint8_t codec,
                                                                std::uint32_t rawLength,
                                                                std::span<std::byte const> bytes)
{
    if (bytes.size() > MaxFramePayload - CodecHeaderSize)
        throw std::length_error("compile-cache codec envelope exceeds the u32 wire length");

    std::vector<std::byte> envelope(CodecHeaderSize + bytes.size());
    std::span<std::byte> const out { envelope };
    out[0] = static_cast<std::byte>(codec);
    WireFields::PutBigEndian<std::uint32_t>(out, 1, rawLength);
    std::ranges::copy(bytes, out.subspan(CodecHeaderSize).begin());
    return envelope;
}

/// Split a codec envelope into its tag, declared raw size, and bytes.
/// @param field One length-prefixed field holding an envelope.
/// @return The envelope, or nullopt when the field is too short to hold a header.
[[nodiscard]] inline std::optional<CodecEnvelope> DecodeCodecEnvelope(std::span<std::byte const> field)
{
    if (field.size() < CodecHeaderSize)
        return std::nullopt;
    return CodecEnvelope { .codec = static_cast<std::uint8_t>(field[0]),
                           .rawLength = ReadBigEndian<std::uint32_t>(field.subspan(1, sizeof(std::uint32_t))),
                           .bytes = field.subspan(CodecHeaderSize) };
}

/// Encode a `u32` as its own length-prefixed field's contents.
/// @param value The value.
/// @return Exactly four big-endian bytes.
[[nodiscard]] inline std::array<std::byte, sizeof(std::uint32_t)> EncodeU32Field(std::uint32_t value)
{
    return WireFields::ToBigEndian<std::uint32_t>(value);
}

/// Read a `u32` from a field that must hold exactly four bytes.
///
/// Strict about the width for the reason `SplitFields` is strict about trailing
/// bytes: a field of another length is a sender speaking a shape this build does
/// not know, and reading the first four bytes of it would invent a value.
/// @param field The field.
/// @return The value, or nullopt when the field is not exactly four bytes.
[[nodiscard]] inline std::optional<std::uint32_t> DecodeU32Field(std::span<std::byte const> field)
{
    return WireFields::FromBigEndian<std::uint32_t>(field);
}

/// A codec preference list, most-preferred first.
///
/// Travels as one byte per codec id in a single field. A list rather than a single
/// value because this is how the two ends agree WITHOUT a handshake: every exchange
/// is client-initiated, so the request states what the sender can decode and the
/// reply picks from it. That costs a few bytes in a frame already being sent, where
/// a negotiation round trip would cost what the "no handshake" decision exists to
/// protect.
///
/// `Identity` is always implicitly acceptable and need not be listed — a peer that
/// can speak this protocol at all can read uncompressed bytes. Listing it anyway is
/// harmless and is what a client with compression compiled out does.
using CodecList = std::vector<std::uint8_t>;

/// Encode a codec preference list.
/// @param codecs The ids, most-preferred first.
/// @return One byte per id.
[[nodiscard]] inline std::vector<std::byte> EncodeCodecList(CodecList const& codecs)
{
    std::vector<std::byte> out;
    out.reserve(codecs.size());
    for (auto const id: codecs)
        out.push_back(static_cast<std::byte>(id));
    return out;
}

/// Decode a codec preference list.
/// @param field The field.
/// @return The ids, in the order the sender listed them.
[[nodiscard]] inline CodecList DecodeCodecList(std::span<std::byte const> field)
{
    CodecList out;
    out.reserve(field.size());
    for (auto const byte: field)
        out.push_back(static_cast<std::uint8_t>(byte));
    return out;
}

/// Choose the codec to answer with: the receiver's most-preferred that the sender
/// also accepts, falling back to `Identity`.
///
/// The SENDER's order decides, not the receiver's, because the sender is the one
/// that has to decode the answer and knows what is cheap for it. Falling back to
/// Identity rather than refusing is deliberate: an uncompressed answer is always
/// correct, and a build must never lose its cache because two peers were compiled
/// with different codec sets.
/// @param accepted What the peer said it can decode.
/// @param available What this build can actually produce.
/// @return The chosen id; `IdentityCodec` when nothing else is in common.
[[nodiscard]] inline std::uint8_t ChooseCodec(CodecList const& accepted, CodecList const& available)
{
    for (auto const id: accepted)
        if (std::ranges::find(available, id) != available.end())
            return id;
    return IdentityCodec;
}

/// A worker announcing itself to the scheduler.
/// A worker's static hardware facts, as they travel inside REGISTER.
///
/// Raw values rather than the scheduler's own `Distributed::NodeClass`, for the
/// reason `CodecList` holds raw codec ids: this header is compiled into
/// `fastcache-cc`, which does not link `FastCache`, so it may not know the
/// scheduler's vocabulary. The mapping happens one layer up, where an unknown
/// class can be answered rather than assumed.
struct CapacityFields
{
    std::uint32_t logicalCores { 0 };     ///< Hardware threads; 0 means "did not say".
    std::uint64_t totalMemoryBytes { 0 }; ///< Physical memory; 0 means "did not say".
    std::uint8_t nodeClassRaw { 0 };      ///< How hard the machine may be driven.
    /// Cores held back from the fleet, when the operator named a number.
    ///
    /// Absent is not zero, and conflating them is the bug this optional exists to
    /// prevent: absent means "use whatever the class reserves", while zero means
    /// "the operator said to reserve nothing". A machine that merely failed to
    /// mention a reserve would otherwise be driven to its last core.
    std::optional<std::uint32_t> reservedCores {};
};

/// Frame a capacity record as one nested field list.
///
/// **Nested rather than five more REGISTER fields**, and that is the extensibility
/// decision. `SplitFields` is exact by design — the property that makes a fixed
/// message shape self-describing — so every fact added at the top level would move
/// REGISTER's arity and make two builds of this fleet unable to speak at all. The
/// grammar's own header names the way out: *"a protocol that wants both nests one
/// inside a field of the other rather than giving up either"*. So REGISTER keeps an
/// exact five fields forever, and this record inside it is read with the
/// variable-arity split: a fact this build has not heard of is skipped, and one it
/// expects but was not sent keeps its default, which is "did not say".
/// @param capacity The facts to encode.
/// @return The nested record's bytes, to be carried as a single REGISTER field.
[[nodiscard]] inline std::vector<std::byte> EncodeCapacity(CapacityFields const& capacity)
{
    auto const cores = WireFields::ToBigEndian<std::uint32_t>(capacity.logicalCores);
    auto const memory = WireFields::ToBigEndian<std::uint64_t>(capacity.totalMemoryBytes);
    auto const nodeClass = std::array { static_cast<std::byte>(capacity.nodeClassRaw) };
    // An absent reserve travels as a zero-length field rather than as a zero value.
    // They mean different things -- see the member's own comment -- and a wire that
    // could not tell them apart would put the whole distinction back on the sender.
    auto const reserveBytes = WireFields::ToBigEndian<std::uint32_t>(capacity.reservedCores.value_or(0));
    auto const reserve =
        capacity.reservedCores.has_value() ? std::span<std::byte const> { reserveBytes } : std::span<std::byte const> {};
    return WireFields::Encode({ std::span<std::byte const> { cores },
                                std::span<std::byte const> { memory },
                                std::span<std::byte const> { nodeClass },
                                reserve });
}

/// Read a capacity record back.
///
/// Every field is optional in the sense that matters: a record holding fewer than
/// this build expects is accepted with the rest left at "did not say", and one
/// holding more is accepted with the surplus ignored. What is *not* tolerated is a
/// field of the wrong width — that is a sender speaking a shape this build does not
/// know, and reading its first four bytes would invent a number.
/// @param field The nested record's bytes.
/// @return The facts, or nullopt when the record itself is malformed.
[[nodiscard]] inline std::optional<CapacityFields> DecodeCapacity(std::span<std::byte const> field)
{
    // An absent record is not a malformed one: a peer that predates this field, or
    // one that had nothing to say, is answered rather than refused.
    if (field.empty())
        return CapacityFields {};

    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value())
        return std::nullopt;

    CapacityFields out {};
    auto const at = [&](std::size_t index) {
        return index < parts->size() ? (*parts)[index] : std::span<std::byte const> {};
    };

    if (auto const cores = at(0); !cores.empty())
    {
        auto const value = WireFields::FromBigEndian<std::uint32_t>(cores);
        if (!value.has_value())
            return std::nullopt;
        out.logicalCores = *value;
    }
    if (auto const memory = at(1); !memory.empty())
    {
        auto const value = WireFields::FromBigEndian<std::uint64_t>(memory);
        if (!value.has_value())
            return std::nullopt;
        out.totalMemoryBytes = *value;
    }
    if (auto const nodeClass = at(2); !nodeClass.empty())
    {
        if (nodeClass.size() != 1)
            return std::nullopt;
        out.nodeClassRaw = static_cast<std::uint8_t>(nodeClass[0]);
    }
    if (auto const reserve = at(3); !reserve.empty())
    {
        auto const value = WireFields::FromBigEndian<std::uint32_t>(reserve);
        if (!value.has_value())
            return std::nullopt;
        // Assigned as the optional it already is. Unwrapping and re-wrapping is the
        // obvious spelling and is what `bugprone-optional-value-conversion` exists
        // to catch: it puts a dereference in the path for no gain.
        out.reservedCores = value;
    }
    return out;
}

struct RegisterRequest
{
    std::string_view fingerprint; ///< Opaque toolchain identity; matched byte-for-byte.
    std::string_view endpoint;    ///< host:port a client can reach this worker on.
    /// Jobs this worker will run concurrently, or 0 to let the scheduler size it
    /// from `capacity`. Zero is the spelling a node should prefer: a node that did
    /// that arithmetic itself would be the one place a workstation's reserve could
    /// be got wrong with nothing downstream able to tell.
    std::uint32_t slots { 0 };
    CodecList acceptedCodecs;   ///< What it can decode.
    CapacityFields capacity {}; ///< What the machine is, for slot derivation.
};

/// The same, as views into a received payload.
struct RegisterView
{
    std::span<std::byte const> fingerprint;
    std::span<std::byte const> endpoint;
    std::uint32_t slots { 0 };
    CodecList acceptedCodecs;
    CapacityFields capacity {};
};

/// A client asking the scheduler where to compile.
struct LeaseRequest
{
    std::string_view fingerprint; ///< The toolchain the client is compiling with.
    std::string_view key;         ///< The object key, for duplicate suppression.
    CodecList acceptedCodecs;     ///< What the client can decode.
};

/// The same, as views into a received payload.
struct LeaseView
{
    std::span<std::byte const> fingerprint;
    std::span<std::byte const> key;
    CodecList acceptedCodecs;
};

/// A client handing a worker one translation unit.
struct CompileRequest
{
    std::string_view leaseToken;       ///< Issued by the scheduler; authorizes this job.
    std::string_view fingerprint;      ///< Re-stated so the worker can refuse a mismatch itself.
    std::span<std::byte const> args;   ///< Encoded, allow-listed compile arguments.
    std::span<std::byte const> source; ///< Preprocessed TU, in a codec envelope.
    /// What the CLIENT can decode, so the worker can compress the object it sends
    /// back. Carried here rather than inferred from the lease, because the worker
    /// never sees the lease request -- and asking the scheduler for it would put a
    /// round trip on the one exchange that must not have one.
    CodecList acceptedCodecs;
    /// The BASE NAME of the translation unit, for the worker to name its scratch
    /// file with.
    ///
    /// A compiler records the name of the file it was handed -- clang-cl and gcc in
    /// the COFF/ELF `.file` symbol, MSVC in its compiland record -- so a worker that
    /// invents a name of its own produces an object that differs from a locally
    /// compiled one in that name and nothing else. Measured on clang-cl: seven bytes,
    /// and byte-identical once the names agree.
    ///
    /// The base name only. The worker has no use for the client's directory and no
    /// business learning it, and the worker sanitizes what arrives regardless: this
    /// is a string from the network that becomes a path, so the two checks are the
    /// same pair as the argument filter's.
    std::string_view sourceName;
};

/// The same, as views into a received payload.
struct CompileView
{
    std::span<std::byte const> leaseToken;
    std::span<std::byte const> fingerprint;
    std::span<std::byte const> args;
    std::span<std::byte const> source;     ///< Still enveloped; decode with DecodeCodecEnvelope.
    CodecList acceptedCodecs;              ///< What the client can decode.
    std::span<std::byte const> sourceName; ///< Base name to give the scratch file; sanitize before use.
};

/// Frame a REGISTER request.
/// @param request The worker's announcement.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeRegister(RegisterRequest const& request,
                                                           WireVersion version = CurrentVersion)
{
    auto const slots = EncodeU32Field(request.slots);
    auto const codecs = EncodeCodecList(request.acceptedCodecs);
    auto const capacity = EncodeCapacity(request.capacity);
    return Detail::EncodeRequest(version,
                                 Op::Register,
                                 { AsBytes(request.fingerprint),
                                   AsBytes(request.endpoint),
                                   std::span<std::byte const> { slots },
                                   std::span<std::byte const> { codecs },
                                   std::span<std::byte const> { capacity } });
}

/// Split a REGISTER payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed (including a mis-sized `slots`).
[[nodiscard]] inline std::optional<RegisterView> DecodeRegisterPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Register));
    if (!fields.has_value())
        return std::nullopt;
    auto const slots = DecodeU32Field((*fields)[2]);
    if (!slots.has_value())
        return std::nullopt;
    auto capacity = DecodeCapacity((*fields)[4]);
    if (!capacity.has_value())
        return std::nullopt;
    return RegisterView { .fingerprint = (*fields)[0],
                          .endpoint = (*fields)[1],
                          .slots = *slots,
                          .acceptedCodecs = DecodeCodecList((*fields)[3]),
                          .capacity = *capacity };
}

/// What a worker reports about itself beyond its job count.
///
/// Every field optional in the same sense `CapacityFields::reservedCores` is:
/// absent means "this machine would not say", which is a different fact from a
/// measured zero and leads to the opposite decision. A node whose CPU cannot be
/// read must be scheduled on its other properties; one that reads zero is idle.
struct LoadFields
{
    std::optional<std::uint32_t> cpuBusyPermille;      ///< Host-wide CPU busy, 0..1000.
    std::optional<std::uint64_t> availableMemoryBytes; ///< Memory a new job could get.
    std::optional<std::uint64_t> freeScratchBytes;     ///< Room where jobs are compiled.
};

/// Frame a live-load record as one nested field list.
///
/// Nested for the reason `EncodeCapacity` is, and it is the same decision made a
/// second time rather than a coincidence: a heartbeat is the message most likely to
/// grow a field, since every new thing a scheduler learns to weigh is something a
/// worker has to start reporting. Keeping HEARTBEAT at an exact three fields means
/// none of those ever costs a fleet the ability to speak to itself.
/// @param load What to encode; an absent value travels as a zero-length field.
/// @return The nested record's bytes, to be carried as a single HEARTBEAT field.
[[nodiscard]] inline std::vector<std::byte> EncodeLoad(LoadFields const& load)
{
    auto const cpu = WireFields::ToBigEndian<std::uint32_t>(load.cpuBusyPermille.value_or(0));
    auto const memory = WireFields::ToBigEndian<std::uint64_t>(load.availableMemoryBytes.value_or(0));
    auto const scratch = WireFields::ToBigEndian<std::uint64_t>(load.freeScratchBytes.value_or(0));
    return WireFields::Encode(
        { load.cpuBusyPermille.has_value() ? std::span<std::byte const> { cpu } : std::span<std::byte const> {},
          load.availableMemoryBytes.has_value() ? std::span<std::byte const> { memory } : std::span<std::byte const> {},
          load.freeScratchBytes.has_value() ? std::span<std::byte const> { scratch } : std::span<std::byte const> {} });
}

/// Read a live-load record back.
///
/// Tolerant of a peer that reports fewer facts or more, and strict about the width
/// of the ones it does report -- exactly as `DecodeCapacity` is, and for the same
/// reasons.
/// @param field The nested record's bytes.
/// @return The values, or nullopt when a field is present at the wrong width.
[[nodiscard]] inline std::optional<LoadFields> DecodeLoad(std::span<std::byte const> field)
{
    if (field.empty())
        return LoadFields {};

    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value())
        return std::nullopt;

    LoadFields out {};
    auto const at = [&](std::size_t index) {
        return index < parts->size() ? (*parts)[index] : std::span<std::byte const> {};
    };

    if (auto const cpu = at(0); !cpu.empty())
    {
        out.cpuBusyPermille = WireFields::FromBigEndian<std::uint32_t>(cpu);
        if (!out.cpuBusyPermille.has_value())
            return std::nullopt;
    }
    if (auto const memory = at(1); !memory.empty())
    {
        out.availableMemoryBytes = WireFields::FromBigEndian<std::uint64_t>(memory);
        if (!out.availableMemoryBytes.has_value())
            return std::nullopt;
    }
    if (auto const scratch = at(2); !scratch.empty())
    {
        out.freeScratchBytes = WireFields::FromBigEndian<std::uint64_t>(scratch);
        if (!out.freeScratchBytes.has_value())
            return std::nullopt;
    }
    return out;
}

/// Frame a HEARTBEAT request.
/// @param workerId The id the scheduler issued at registration.
/// @param inFlight How many jobs the worker is running right now.
/// @param load What else the worker has to say about itself.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeHeartbeat(std::string_view workerId,
                                                            std::uint32_t inFlight,
                                                            LoadFields const& load = {},
                                                            WireVersion version = CurrentVersion)
{
    auto const jobs = EncodeU32Field(inFlight);
    auto const rest = EncodeLoad(load);
    return Detail::EncodeRequest(
        version,
        Op::Heartbeat,
        { AsBytes(workerId), std::span<std::byte const> { jobs }, std::span<std::byte const> { rest } });
}

/// A worker's periodic liveness report.
struct HeartbeatView
{
    std::span<std::byte const> workerId;
    std::uint32_t inFlight { 0 };
    LoadFields load {};
};

/// Split a HEARTBEAT payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<HeartbeatView> DecodeHeartbeatPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Heartbeat));
    if (!fields.has_value())
        return std::nullopt;
    auto const inFlight = DecodeU32Field((*fields)[1]);
    if (!inFlight.has_value())
        return std::nullopt;
    auto const load = DecodeLoad((*fields)[2]);
    if (!load.has_value())
        return std::nullopt;
    return HeartbeatView { .workerId = (*fields)[0], .inFlight = *inFlight, .load = *load };
}

/// Frame a LEASE request.
/// @param request What the client wants to compile and with what.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeLease(LeaseRequest const& request, WireVersion version = CurrentVersion)
{
    auto const codecs = EncodeCodecList(request.acceptedCodecs);
    return Detail::EncodeRequest(
        version, Op::Lease, { AsBytes(request.fingerprint), AsBytes(request.key), std::span<std::byte const> { codecs } });
}

/// Split a LEASE payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<LeaseView> DecodeLeasePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Lease));
    if (!fields.has_value())
        return std::nullopt;
    return LeaseView { .fingerprint = (*fields)[0], .key = (*fields)[1], .acceptedCodecs = DecodeCodecList((*fields)[2]) };
}

/// An operator changing one replicated cluster setting.
struct ClusterSetRequest
{
    std::string_view name;  ///< A key from `Cluster::SettingTable`.
    std::string_view value; ///< Whatever it is being set to.
};

/// The same, as views into a received payload.
struct ClusterSetView
{
    std::span<std::byte const> name;
    std::span<std::byte const> value;
};

/// Frame a CLUSTER-STATUS request.
///
/// No fields at all, which is a real shape rather than a placeholder: the request
/// carries no question, so a payload that is not empty is a client this build does
/// not understand and `SplitFields` refuses it on the count alone.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeClusterStatus(WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::ClusterStatus, {});
}

/// Frame a CLUSTER-SET request.
/// @param request The setting and its new value.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeClusterSet(ClusterSetRequest const& request,
                                                             WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::ClusterSet, { AsBytes(request.name), AsBytes(request.value) });
}

/// Split a CLUSTER-SET payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<ClusterSetView> DecodeClusterSetPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::ClusterSet));
    if (!fields.has_value())
        return std::nullopt;
    return ClusterSetView { .name = (*fields)[0], .value = (*fields)[1] };
}

/// Frame a CLUSTER-FORGET request.
/// @param memberId Who to remove.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeClusterForget(std::string_view memberId,
                                                                WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::ClusterForget, { AsBytes(memberId) });
}

/// Split a CLUSTER-FORGET payload.
/// @param payload The bytes following the request header.
/// @return The member id, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::span<std::byte const>> DecodeClusterForgetPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::ClusterForget));
    if (!fields.has_value())
        return std::nullopt;
    return (*fields)[0];
}

/// An operator adding a member to the cluster, or moving one.
struct ClusterAdmitRequest
{
    std::string_view memberId;     ///< Stable identity; what consensus counts.
    std::string_view raftEndpoint; ///< host:port its consensus port answers on.
};

/// The same, as views into a received payload.
struct ClusterAdmitView
{
    std::span<std::byte const> memberId;
    std::span<std::byte const> raftEndpoint;
};

/// Frame a CLUSTER-ADMIT request.
///
/// Two fields and not one: an id with no address is a node the cluster counts
/// towards quorum and cannot reach, which is the defect `Cluster::ClusterMember`
/// exists to make unrepresentable. The scheduler endpoint is deliberately absent —
/// a member announces its own, and nothing an operator types about somebody else
/// could supply it.
/// @param request Who to admit, and where it answers.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeClusterAdmit(ClusterAdmitRequest const& request,
                                                               WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::ClusterAdmit, { AsBytes(request.memberId), AsBytes(request.raftEndpoint) });
}

/// Split a CLUSTER-ADMIT payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<ClusterAdmitView> DecodeClusterAdmitPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::ClusterAdmit));
    if (!fields.has_value())
        return std::nullopt;
    return ClusterAdmitView { .memberId = (*fields)[0], .raftEndpoint = (*fields)[1] };
}

/// Frame a COMPILE request.
/// @param request The job.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeCompile(CompileRequest const& request,
                                                          WireVersion version = CurrentVersion)
{
    auto const codecs = EncodeCodecList(request.acceptedCodecs);
    return Detail::EncodeRequest(version,
                                 Op::Compile,
                                 { AsBytes(request.leaseToken),
                                   AsBytes(request.fingerprint),
                                   request.args,
                                   request.source,
                                   std::span<std::byte const> { codecs },
                                   AsBytes(request.sourceName) });
}

/// Split a COMPILE payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<CompileView> DecodeCompilePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Compile));
    if (!fields.has_value())
        return std::nullopt;
    return CompileView { .leaseToken = (*fields)[0],
                         .fingerprint = (*fields)[1],
                         .args = (*fields)[2],
                         .source = (*fields)[3],
                         .acceptedCodecs = DecodeCodecList((*fields)[4]),
                         .sourceName = (*fields)[5] };
}

/// What a scheduler answers a LEASE with, on success.
struct LeaseGrant
{
    std::string_view endpoint;   ///< The worker to send the job to.
    std::string_view leaseToken; ///< Authorizes exactly this job on that worker.
    /// What the chosen worker can DECODE, relayed from its registration.
    ///
    /// Carried here because the client is about to send that worker a multi-megabyte
    /// preprocessed translation unit and has to choose a codec for it. Without this
    /// the client would have to either send `Identity` always -- giving up the
    /// compression on the one payload large enough to care -- or guess, and a guess
    /// the worker cannot decode is a refused job after the whole payload has already
    /// crossed the network. The scheduler already knows the answer; relaying it costs
    /// a few bytes in a reply that is being sent anyway, and keeps the exchange free
    /// of a negotiation round trip.
    CodecList workerCodecs;
};

/// Frame the payload of a successful LEASE reply.
/// @param grant Where to go and what to present.
/// @return The reply payload (not a whole frame).
[[nodiscard]] inline std::vector<std::byte> EncodeLeaseGrant(LeaseGrant const& grant)
{
    auto const codecs = EncodeCodecList(grant.workerCodecs);
    return WireFields::Encode({ AsBytes(grant.endpoint), AsBytes(grant.leaseToken), std::span<std::byte const> { codecs } });
}

/// The fields of a LEASE grant, as views.
struct LeaseGrantView
{
    std::span<std::byte const> endpoint;
    std::span<std::byte const> leaseToken;
    CodecList workerCodecs;
};

/// Split a LEASE reply payload.
/// @param payload The reply body.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<LeaseGrantView> DecodeLeaseGrant(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, 3);
    if (!fields.has_value())
        return std::nullopt;
    return LeaseGrantView { .endpoint = (*fields)[0],
                            .leaseToken = (*fields)[1],
                            .workerCodecs = DecodeCodecList((*fields)[2]) };
}

/// What a worker answers a COMPILE with.
///
/// The exit code travels as a `u32` holding a non-negative code. A spawn failure is
/// NOT expressible here and must not be: the worker answers `Error` for that, so a
/// client can tell "the compiler ran and rejected the code" — which it must report
/// verbatim — from "this worker could not run a compiler at all", which it must
/// answer by compiling locally.
struct CompileResult
{
    std::uint32_t exitCode { 0 };
    std::span<std::byte const> object; ///< Codec envelope; empty on a failed compile.
    std::span<std::byte const> stdoutText;
    std::span<std::byte const> stderrText;
};

/// Frame the payload of a COMPILE reply.
/// @param result The outcome.
/// @return The reply payload (not a whole frame).
[[nodiscard]] inline std::vector<std::byte> EncodeCompileResult(CompileResult const& result)
{
    auto const code = EncodeU32Field(result.exitCode);
    return WireFields::Encode({ std::span<std::byte const> { code }, result.object, result.stdoutText, result.stderrText });
}

/// Split a COMPILE reply payload.
/// @param payload The reply body.
/// @return The result, or nullopt when malformed.
[[nodiscard]] inline std::optional<CompileResult> DecodeCompileResult(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, 4);
    if (!fields.has_value())
        return std::nullopt;
    auto const code = DecodeU32Field((*fields)[0]);
    if (!code.has_value())
        return std::nullopt;
    return CompileResult {
        .exitCode = *code, .object = (*fields)[1], .stdoutText = (*fields)[2], .stderrText = (*fields)[3]
    };
}

} // namespace FastCache::CompileCacheWire
