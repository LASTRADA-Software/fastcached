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
#include <string>
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
inline constexpr WireVersion CurrentVersion = 2;

/// The oldest version this build still accepts. Equal to `CurrentVersion` while
/// only one version exists; widen the range when a second one ships and this
/// build can still decode the older shape.
inline constexpr WireVersion MinSupportedVersion = 2;

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

/// How far past a surface's own request cap an oversize declaration is still read
/// and discarded, so the refusal can be a reply rather than a close.
///
/// A frame declares its own length, so a server that refuses one can step over the
/// body and stay in sync -- and it must, or the peer never sees the typed refusal it
/// was answered with. Refusing without draining is what made an over-cap STORE break
/// builds: the client is mid-send when the server stops reading, so it sees its own
/// write fail (and, before issue #68, died of SIGPIPE doing so) instead of the one
/// message that would have told an operator to raise `--storage-max-value`. Draining
/// costs bandwidth the peer was going to spend anyway rather than footprint, since
/// the body is discarded in chunks and never materialised.
///
/// Expressed as a multiple of the cap rather than a byte count of its own: the cap is
/// already the operator's statement of the largest thing a surface will handle, so
/// being willing to discard a few times that much needs no second knob and scales
/// when they retune the first one. Past the bound the connection ends -- a peer
/// declaring gigabytes it was never going to be allowed to send has stopped being one
/// worth resynchronizing with.
///
/// It lives HERE rather than beside either implementation because both the daemon's
/// handler and the node's frame endpoint serve this same wire, and a peer must not
/// have to know which one it reached to know how a refusal behaves. Two copies of a
/// policy comment claiming to be one policy is how they drift.
inline constexpr std::uint64_t OversizeDrainFactor = 4;

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

    /// Client tells the scheduler the job it leased has ended, however it ended.
    ///
    /// The third transition of a lease's life, and for a long time the missing one:
    /// a lease was acquired and could only ever expire, so a key stayed marked
    /// in-flight for the full lease timeout and every later compile of it was
    /// refused `AlreadyInFlight` (#212). Expiry is the safety net for a client that
    /// DIED -- `Ctrl-C` on a build -- and this is the ordinary path.
    ///
    /// Sent by the CLIENT rather than the worker, because the client is who the
    /// lease was issued to and is the only party that knows every way a job can
    /// end: the worker never sees a job whose dispatch failed to reach it.
    ///
    /// It names the KEY as well as the token, so a release resolves the client's own
    /// lease or nothing -- see `EncodeRelease`.
    ///
    /// Numbered after the cluster verbs rather than beside `Lease`, because the
    /// byte is the contract and the ones already spoken cannot move.
    Release = 0x0C,
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
    /// Opcode not in `OpTable`, OR a verb this surface does not implement -- see
    /// `UnimplementedVerb`, which is the name a refusal table spells.
    UnknownOpcode = 0x02,
    MalformedFrame = 0x03,  ///< Fields do not exactly fill the declared payload.
    PayloadTooLarge = 0x04, ///< Declared payload exceeds the session's cap.
    MalformedValue = 0x05,  ///< STORE payload is not a decodable compile-value.

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

    /// A worker announced itself in bytes that are not text.
    ///
    /// Everything a peer states about itself -- its toolchain fingerprint, the
    /// endpoint clients are sent to, the version it is running -- is copied into
    /// the leader's view of the fleet and read back out of it by an operator: a
    /// page, a JSON document somebody's script parses, `--cluster-status`, the
    /// logs. RFC 8259 requires UTF-8 of JSON exchanged between systems, so one
    /// worker registering a byte that belongs to no valid sequence makes the
    /// whole fleet's answer a document a strict parser may reject -- not merely
    /// that worker's row in it.
    ///
    /// Refused rather than repaired, and that is the asymmetry that decides it: a
    /// fingerprint is matched BYTE FOR BYTE, so a worker admitted under a
    /// cleaned-up name would match no client's toolchain and would sit in the
    /// fleet forever, registered and never picked, with nothing anywhere saying
    /// why. A refusal is loud; a repair is a worker that quietly does nothing.
    MalformedRegistration = 0x18,

    /// The lease presented is not one this cluster issued.
    ///
    /// Its MAC does not verify under the shared key, or it is not a lease token at
    /// all -- and those two are deliberately ONE code, because from the receiving
    /// end they are indistinguishable: a forged token and a random string differ
    /// only in how much effort went into them. It is also what an old launcher's
    /// bare serial arrives as, which is the right answer for it too: the client
    /// answers every refusal here by compiling locally, so a mixed-version fleet
    /// degrades to local compiles rather than to a broken build.
    ///
    /// Emphatically NOT `UnknownLease`, which is the scheduler saying it has no
    /// record of a token it may well have issued -- an expired or already-resolved
    /// lease, a fleet condition. This one says the token was never issued by
    /// anybody holding the key, which is a security event and reads as one.
    ///
    /// Nothing the token CLAIMS is reported alongside it. A caller that cannot
    /// authenticate a token has established no fact about it, and echoing its
    /// contents back would turn the refusal into an oracle.
    LeaseUnauthorized = 0x19,

    /// An authentic lease, presented to a worker it was not issued for.
    ///
    /// Only ever reported once the MAC has verified, which is what makes it a
    /// diagnostic rather than a hint to an attacker: the endpoint the scheduler
    /// granted is stated in the message, because the overwhelmingly common cause is
    /// not a replay at all but a worker whose advertised endpoint and actual one
    /// disagree -- a NAT, or a hostname registered where clients resolve an address.
    ///
    /// The replay it also closes is the reason the endpoint is inside the MAC:
    /// without it, a lease minted for one worker is a lease on every worker in the
    /// fleet.
    LeaseEndpointMismatch = 0x1A,

    /// An authentic lease, presented after it expired.
    ///
    /// The expiry is a bound on how long a CAPTURED token stays useful, and nothing
    /// else -- a worker's own slot accounting is what bounds concurrency, so this
    /// code never means "the fleet is full". Reported by name rather than by closing
    /// the connection, because a close is indistinguishable from a network fault and
    /// sends a client into silent local fallback with nothing to report.
    ///
    /// Clock skew is real across a fleet, so the check carries slack; a rise here on
    /// one machine and nowhere else is that machine's clock, not the fleet's leases.
    LeaseExpired = 0x1B,
};

/// Bit for `status` within an `OpDescriptor::legalStatuses` mask.
/// @param status The status to encode.
/// @return A single-bit mask.
[[nodiscard]] constexpr std::uint8_t StatusBit(Status status) noexcept
{
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(status));
}

/// Whether a verb may be served before a credential has been accepted.
///
/// **A type rather than a `bool`, so that a row which does not state its
/// classification fails to COMPILE** ([#289](https://github.com/LASTRADA-Software/fastcached/issues/289)).
/// The default constructor is deleted, which makes an aggregate row that omits
/// `.preAuth` ill-formed: designated initializers value-initialize an omitted
/// member, and a class with no default constructor cannot be value-initialized.
///
/// A defaulted `bool` was not enough, and the distinction is the whole point. It
/// defaulted to `false` -- closed, which is the right direction -- but "the author
/// did not think about it" and "the author decided this verb needs a credential"
/// then produce identical text, so the table can no longer be read as a record of
/// decisions. That is the same failure this codebase records as reopening a hole by
/// omission, and the reason the pre-auth set is a table column at all: what an
/// unauthenticated peer can reach must be readable off the table, and a silent
/// default is not readable.
///
/// Spelled at each row as `OpenBeforeAuth` or `RequiresAuth` rather than as a
/// boolean, because `.preAuth = RequiresAuth` reads as an absence and `.preAuth =
/// RequiresAuth` reads as a decision.
class PreAuth
{
  public:
    /// Deleted on purpose: a row must state its classification. See the class
    /// comment -- this deletion IS the acceptance criterion of #289.
    PreAuth() = delete;

    /// @param allowed True when an unauthenticated peer may reach this verb.
    constexpr explicit PreAuth(bool allowed) noexcept:
        _allowed { allowed }
    {
    }

    /// @return True when an unauthenticated peer may reach this verb.
    [[nodiscard]] constexpr bool Allowed() const noexcept
    {
        return _allowed;
    }

  private:
    bool _allowed;
};

/// This verb is reachable by a peer that has not presented a credential.
///
/// Every one of these is a hole held deliberately open, so each must also declare a
/// `maxPayload` -- `PreAuthVerbsAreBounded` refuses the table otherwise.
inline constexpr PreAuth OpenBeforeAuth { true };

/// This verb is refused until a credential has been accepted.
inline constexpr PreAuth RequiresAuth { false };

/// The largest payload a verb may declare, stated rather than defaulted.
///
/// A plain `std::size_t` here had the hole `PreAuth` had: a designated initializer
/// that omits the member value-initializes it to `0`, and `0` is a *meaningful*
/// value -- "the operator's session cap governs". So a control verb added later
/// would silently inherit a listener-wide 256 MiB ceiling, which is the failure
/// [#284](https://github.com/LASTRADA-Software/fastcached/issues/284) is about, and
/// it would compile and pass every test.
///
/// Spelling it as a type with no default constructor makes the omission
/// ill-formed. The ticket suggested a `ControlVerbsAreBounded()` assertion instead;
/// that would need a second column saying which verbs are "control", and two
/// classifications of the same rows are exactly what this table's rule exists to
/// avoid. Making the cap unomittable removes the failure mode for every verb at
/// once rather than for one category.
class PayloadCap
{
  public:
    /// Deleted on purpose: a row must state its ceiling. See the class comment.
    PayloadCap() = delete;

    /// @param bytes The ceiling, or 0 for "the session cap governs".
    constexpr explicit PayloadCap(std::size_t bytes) noexcept:
        _bytes { bytes }
    {
    }

    /// @return The declared ceiling in bytes; 0 means the session cap governs.
    [[nodiscard]] constexpr std::size_t Bytes() const noexcept
    {
        return _bytes;
    }

    /// @return True when this verb declares a bound of its own.
    [[nodiscard]] constexpr bool IsBounded() const noexcept
    {
        return _bytes != 0;
    }

  private:
    std::size_t _bytes;
};

/// This verb carries whatever the operator's session cap allows.
///
/// Legitimate for the three payload-bearing verbs -- STORE and a COMPILE reply carry
/// an object file, COMPILE carries a preprocessed translation unit -- and never for a
/// verb reachable before authentication, which `PreAuthVerbsAreBounded` refuses.
inline constexpr PayloadCap SessionCapGoverns { 0 };

/// This verb declares a ceiling of its own, tighter than the session's.
/// @param bytes The ceiling.
/// @return The cap to put in the row.
[[nodiscard]] constexpr PayloadCap BoundedTo(std::size_t bytes) noexcept
{
    return PayloadCap { bytes };
}

/// Which of this protocol's verb families a verb belongs to.
///
/// The `Op` enum has always carried this grouping -- as three comment blocks saying
/// "Distributed execution", "Cluster administration" and nothing at all for the first
/// three. A comment is enough while each family has a listener to itself, and stops
/// being enough the moment one listener serves several
/// ([#290](https://github.com/LASTRADA-Software/fastcached/issues/290)): a merged
/// `0xFC` surface routes each frame to the component that owns its family, and asks
/// that component -- rather than the port -- whether the peer is admitted, whether a
/// credential is required, and which counter a refusal belongs to.
///
/// So the grouping becomes a column. It says which family a verb BELONGS to, never
/// which process serves it: `fastcached` and a node both answer `Cache` verbs, and
/// what differs is which of them has a component for the family, not the taxonomy.
enum class VerbFamily : std::uint8_t
{
    /// Not a family. Zero is deliberately unusable, so a row added without a family
    /// fails `EveryVerbHasAFamily` instead of silently joining whichever family
    /// happened to be first -- the same failure mode `PreAuth` and `PayloadCap` are
    /// spelled as unconstructible-by-default types to prevent. A wrapper class would
    /// work here too; the table-wide assertion is the lighter of the two and this
    /// column, unlike those, has no valid value that a `{}` could be confused for.
    Unset = 0,
    /// Establishes a credential for the connection, and belongs to no one surface:
    /// on a merged listener it is answered by whichever component owns the
    /// credential, which is the scheduler.
    Session,
    Cache,     ///< Reads and writes compile results.
    Scheduler, ///< Spends the fleet's capacity, or changes what the cluster agrees.
    Compile,   ///< Causes a compiler to run on this machine.
};

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
    /// itself. A row that does not state it does not compile -- see `PreAuth`.
    PreAuth preAuth;
    /// Largest payload this verb may declare.
    ///
    /// `SessionCapGoverns` or `BoundedTo(n)`, never a bare number and never omitted
    /// -- see `PayloadCap`, whose deleted default constructor is what makes a row
    /// that says nothing a build failure rather than a 256 MiB surprise (#284).
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
    PayloadCap maxPayload;

    /// Which verb family this belongs to; never `Unset`.
    ///
    /// Read by a merged listener to pick the component that answers, admits, gates
    /// and counts for this verb. `EveryVerbHasAFamily` refuses a row that omits it.
    VerbFamily family;
};

/// What an endpoint answers for a verb it does not implement.
///
/// **A wire contract between binaries that do not link each other**, and the reason
/// it is one name rather than a value each surface picks: `fastcache-cc` compiles
/// this header in and links none of `FastCache`, so an enumerator named on both
/// sides is the only thing holding the two ends together.
///
/// `Cc::CacheProtocol::Exchange` steps over exactly this code for a verb an endpoint
/// does not implement and proceeds unauthenticated -- correct against a surface with
/// no credential to check -- while treating every *other* refusal as being about the
/// credential and returning it in place of the answer to the request the caller
/// actually sent. So a surface answering AUTH with anything else gives every
/// `FASTCACHE_TOKEN`-configured client a permanent failure that presents as an
/// endlessly cold cache or a fleet that distributes nothing, with no signal saying
/// which.
///
/// `DispatchNotPermitted` is the tempting alternative and is a different sentence:
/// it says *this endpoint does not do that job*, which is a routing fact a client
/// acts on. This says *I do not implement this verb*, which is what an absent
/// capability is. Three surfaces answered the question three hand-written ways and
/// drifted apart exactly as far as that distinction (#283, #340).
///
/// It is deliberately NOT a statement about AUTH. Any verb a surface does not serve
/// takes this code; AUTH is merely the one whose absence a client is built to walk
/// past.
///
/// Two production users spell it: `FindRefusal`'s tables, which is every server on
/// this wire, and `Cc::CacheProtocol`'s tolerance, which is the client. Those two are
/// the contract.
inline constexpr ErrorCode UnimplementedVerb = ErrorCode::UnknownOpcode;

// **The VALUE is pinned, not just the name.** Naming it once stops the surfaces in
// THIS tree disagreeing with each other; it does nothing about the launchers already
// deployed, which tolerate `0x02` and nothing else. Changing this alias -- even
// consistently, so that every surface and the in-tree client move together -- is a
// silent compatibility break with every binary in the field, and it is silent
// precisely because the in-tree tests would all still agree with one another.
//
// Found by flipping the alias during #340 and watching the surface tests stay green:
// they assert `== UnimplementedVerb` on both ends, which is a tautology under a
// consistent change. This is the assertion that is not.
//
// Written against the BYTE rather than against `ErrorCode::UnknownOpcode`, because
// the enumerator is a name this tree chose and `0x02` is what is on the wire: an
// assertion naming the enumerator survives a renumbering of it, which is the other
// way to break every deployed launcher without a single test going red.
static_assert(static_cast<std::uint8_t>(UnimplementedVerb) == 0x02,
              "deployed launchers step over 0x02 and nothing else; changing this breaks them silently");

/// One verb a surface answers with something other than its generic refusal.
///
/// **The single home for this shape, and the single home for why it exists.** Which
/// verbs a surface declines, and what it tells an operator, is the surface's own
/// business and stays in its own table; that a refusal is `(op, code, why)` and is
/// looked up *before* the catch-all is the wire's, and belongs here.
///
/// It lived in four translation units under two names -- `CacheProxy::RefusedVerbs`,
/// `CompileCacheHandler::RelocatedVerbs`, and one added to each of the scheduler and
/// worker by the change that exists to stop these surfaces drifting apart. Four
/// copies of a struct is the answer to "if a sixth case showed up tomorrow, how many
/// places would I edit".
///
/// **Why a table rather than a `switch` arm**, stated once here rather than in each
/// caller: the moment there are two answers, the next verb added has to *state* which
/// of them it is instead of inheriting whichever the catch-all happens to give. Three
/// surfaces answering that question three hand-written ways is how #283 fixed one of
/// them and left the other two broken for a fortnight (#340).
///
/// Here rather than in a library header because `fastcache-cc` compiles this file in
/// and links none of `FastCache`. It needs only `Op`, `ErrorCode` and
/// `std::string_view`, so the header-only, dependency-free rule holds.
struct RefusedVerb
{
    Op op;                ///< The verb.
    ErrorCode code;       ///< What the client acts on.
    std::string_view why; ///< Why, in words a person can follow.
};

/// The row describing @p op, or null when the surface's generic refusal applies.
///
/// A `span` rather than a template over the extent, so one instantiation serves every
/// surface and a table's length is never part of a caller's type.
/// @param table That surface's rows.
/// @param op The verb, already resolved against `OpTable`.
/// @return Its row, or nullptr.
[[nodiscard]] constexpr RefusedVerb const* FindRefusal(std::span<RefusedVerb const> table, Op op) noexcept
{
    auto const found = std::ranges::find(table, op, &RefusedVerb::op);
    return found == table.end() ? nullptr : &*found;
}

/// Payload ceiling for AUTH: a username and a shared secret, and nothing that
/// grows with a build artefact. Generous by orders of magnitude against any real
/// credential, and small enough that a peer which has proved nothing cannot use
/// it to make the server take memory.
inline constexpr std::size_t MaxAuthPayload = 4096;

/// Payload ceiling for the scheduler's control verbs (`Register`, `Heartbeat`,
/// `Lease`, `Release`).
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
                   .preAuth = RequiresAuth,
                   .maxPayload = SessionCapGoverns, // an object file; bounded by the operator's cap
                   .family = VerbFamily::Cache },
    OpDescriptor { .code = Op::Fetch,
                   .name = "fetch",
                   .fieldCount = 1, // key
                   .legalStatuses =
                       static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Miss) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = SessionCapGoverns,
                   .family = VerbFamily::Cache },
    OpDescriptor { .code = Op::Auth,
                   .name = "auth",
                   .fieldCount = 2, // username, secret
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = OpenBeforeAuth,
                   .maxPayload = BoundedTo(MaxAuthPayload),
                   .family = VerbFamily::Session },

    // Distributed execution. None is `preAuth`: causing a compiler to run on
    // another machine is the last thing an unauthenticated peer should reach.
    OpDescriptor { .code = Op::Register,
                   .name = "register",
                   .fieldCount = 5, // fingerprint, endpoint, u32 slots, accepted codecs, capacity
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::Heartbeat,
                   .name = "heartbeat",
                   .fieldCount = 3, // workerId, u32 inFlight, load
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::Lease,
                   .name = "lease",
                   .fieldCount = 3, // fingerprint, key, accepted codecs
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::Release,
                   .name = "release",
                   .fieldCount = 2, // leaseToken, key
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::ClusterStatus,
                   .name = "cluster-status",
                   .fieldCount = 0, // nothing to ask with
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::ClusterSet,
                   .name = "cluster-set",
                   .fieldCount = 2, // setting name, value
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::ClusterForget,
                   .name = "cluster-forget",
                   .fieldCount = 1, // member id
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::ClusterAdmit,
                   .name = "cluster-admit",
                   .fieldCount = 2, // member id, consensus endpoint
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .family = VerbFamily::Scheduler },
    OpDescriptor { .code = Op::Compile,
                   .name = "compile",
                   .fieldCount = 6, // leaseToken, fingerprint, args, preprocessed, accepted codecs, sourceName
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = SessionCapGoverns, // carries a preprocessed TU; the operator's cap governs
                   .family = VerbFamily::Compile },
};

/// Whether every verb reachable before authentication declares a payload bound.
///
/// A `static_assert`ed invariant rather than a comment, because getting it wrong
/// is not a bug in the new verb — it is the pre-auth allocation gate silently
/// ceasing to hold for the whole protocol.
/// @return True when no `preAuth` row leaves `maxPayload` at 0.
[[nodiscard]] constexpr bool PreAuthVerbsAreBounded() noexcept
{
    return std::ranges::all_of(OpTable,
                               [](OpDescriptor const& row) { return !row.preAuth.Allowed() || row.maxPayload.IsBounded(); });
}

static_assert(PreAuthVerbsAreBounded(), "a verb reachable before AUTH must declare its own payload ceiling");

/// Whether every verb states which family it belongs to.
///
/// The same shape as `PreAuthVerbsAreBounded` and for the same reason: an omitted
/// `family` value-initializes to `Unset`, and a merged listener asking an `Unset`
/// verb's owner to answer would find none and refuse a verb this build implements.
/// That reads to a client as a daemon too OLD to know the verb, which is the one
/// misdiagnosis `UnimplementedVerb` exists to avoid handing out falsely.
/// @return True when no row leaves `family` at `Unset`.
[[nodiscard]] constexpr bool EveryVerbHasAFamily() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) { return row.family != VerbFamily::Unset; });
}

static_assert(EveryVerbHasAFamily(), "a verb belongs to a family -- see VerbFamily");

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
    ErrorDescriptor { .code = ErrorCode::MalformedRegistration,
                      .name = "malformed-registration",
                      .defaultMessage = "a worker must name itself in UTF-8" },
    ErrorDescriptor { .code = ErrorCode::LeaseUnauthorized,
                      .name = "lease-unauthorized",
                      .defaultMessage = "this lease was not issued by this cluster" },
    ErrorDescriptor { .code = ErrorCode::LeaseEndpointMismatch,
                      .name = "lease-endpoint-mismatch",
                      .defaultMessage = "this lease was issued for another worker" },
    ErrorDescriptor { .code = ErrorCode::LeaseExpired, .name = "lease-expired", .defaultMessage = "this lease has expired" },
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

/// Which family a verb belongs to.
///
/// Takes the raw byte rather than an `Op`, because every caller has one: a byte off
/// the wire is not yet known to be a verb at all, and a lookup that demanded an `Op`
/// would push the "is this even a verb" question onto each call site separately.
/// @param opRaw The third header byte, as received.
/// @return The family, or `Unset` when the byte names no verb in this build.
[[nodiscard]] constexpr VerbFamily FamilyOf(std::uint8_t opRaw) noexcept
{
    auto const* const row = FindOp(opRaw);
    return row != nullptr ? row->family : VerbFamily::Unset;
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
    if (row == nullptr || !row->maxPayload.IsBounded())
        return sessionCap;
    return row->maxPayload.Bytes() < sessionCap ? row->maxPayload.Bytes() : sessionCap;
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
    return row != nullptr && row->preAuth.Allowed();
}

/// What a surface must do with a frame, decided from its HEADER alone.
///
/// One value per outcome rather than a `bool`, because "serve it", "it declared too
/// much" and "it has not authenticated" are three different answers that a caller
/// reports differently, and collapsing them is how a refusal comes to be counted as
/// the wrong thing.
enum class PrePayloadDecision : std::uint8_t
{
    Serve,           ///< Read the declared payload and answer the verb.
    UnknownOpcode,   ///< No row in `OpTable`; nothing can be known about it.
    PayloadTooLarge, ///< More than this verb may carry, whoever is asking.
    Unauthenticated, ///< Not reachable until a credential has been accepted.
};

/// Everything the pre-payload decision depends on.
///
/// A struct rather than five positional parameters: `authRequired` and
/// `credentialAccepted` are both `bool` and adjacent, so at a call site they are one
/// transposition away from a gate that admits exactly the peers it should refuse --
/// and that transposition compiles and passes any test whose surface has no
/// credential configured.
struct PrePayloadRequest
{
    std::uint8_t opRaw {};           ///< Third header byte. MUST already resolve via `FindOp`.
    std::uint32_t declaredLength {}; ///< What the header says the payload is.
    std::size_t sessionCap {};       ///< The operator's configured per-frame maximum.
    bool authRequired {};            ///< Whether this surface has a credential configured at all.
    bool credentialAccepted {};      ///< Whether THIS connection has presented it.
};

/// Decide a frame's fate before a payload byte is read.
///
/// **The one place both surfaces spell this rule.** The daemon's `0xFC` handler and
/// the compile node's frame server each have a read loop, each has to answer the
/// same question in the same order, and each used to answer it in its own code. That
/// is the arrangement that produced three refusal tables which drifted (#283, #340);
/// this is one predicate with two callers instead
/// ([#289](https://github.com/LASTRADA-Software/fastcached/issues/289)).
///
/// **The ceiling is checked BEFORE the credential, and the order is load-bearing.**
/// `Op::Auth` is deliberately reachable unauthenticated, so if the credential gate
/// ran first a peer could declare the whole session cap on the one verb the gate
/// holds open and get exactly the allocation the gate exists to deny. Bounding first
/// means a pre-auth verb is bounded whoever is asking.
///
/// The two are not otherwise ordered by preference: telling an unauthenticated peer
/// that its declared length exceeded a published constant discloses nothing, because
/// the constant is in this header and this header ships inside `fastcache-cc`.
///
/// **Total, so an unknown verb is refused rather than buffered.** An earlier draft
/// took a resolved opcode as a precondition, which left the node's loop -- the one
/// surface that does not resolve opcodes before reading -- free to buffer the whole
/// request cap for opcode `0xFF` from an unauthenticated peer, reconstructing the
/// exact hole this gate closes. Answering the question for every byte value is what
/// removes that, and it costs one enumerator.
///
/// @param request The header's facts and the connection's auth state.
/// @return What to do. `Serve` is the only value that permits reading the payload.
[[nodiscard]] constexpr PrePayloadDecision DecidePrePayload(PrePayloadRequest const& request) noexcept
{
    if (FindOp(request.opRaw) == nullptr)
        return PrePayloadDecision::UnknownOpcode;
    if (request.declaredLength > OpPayloadCap(request.opRaw, request.sessionCap))
        return PrePayloadDecision::PayloadTooLarge;
    if (request.authRequired && !request.credentialAccepted && !IsPreAuthAllowed(request.opRaw))
        return PrePayloadDecision::Unauthenticated;
    return PrePayloadDecision::Serve;
}

/// The wire code a refusing decision is reported with.
///
/// A mapping in one place rather than each surface naming an enumerator, for the
/// reason `UnimplementedVerb` is one constant: two surfaces spelling the same
/// refusal separately is precisely how they came to disagree twice.
/// @param decision A decision other than `Serve`.
/// @return The code to answer with. `Serve` has no code and yields `Unauthenticated`,
///         which is unreachable by contract and closed if it ever were not.
[[nodiscard]] constexpr ErrorCode ErrorCodeFor(PrePayloadDecision decision) noexcept
{
    switch (decision)
    {
        case PrePayloadDecision::UnknownOpcode:
            return ErrorCode::UnknownOpcode;
        case PrePayloadDecision::PayloadTooLarge:
            return ErrorCode::PayloadTooLarge;
        case PrePayloadDecision::Unauthenticated:
            return ErrorCode::Unauthenticated;
        case PrePayloadDecision::Serve:
            break;
    }
    return ErrorCode::Unauthenticated;
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
///
/// **`View` because `bytes` borrows the field it was decoded from**, and this is
/// returned by value from `DecodeCodecEnvelope`. The name is the warning the type
/// used to lack ([#366](https://github.com/LASTRADA-Software/fastcached/issues/366)):
/// a caller that lets the decoded buffer die before reading `bytes` has a
/// use-after-free.
///
/// **It borrows rather than owning, and that is measured rather than assumed.**
/// Its one production consumer is `OpenAs` in the launcher's `CodecEnvelope.cpp`,
/// which reads it immediately and in the same scope -- and whose `Identity` branch
/// hands `bytes` straight to the caller's container with a comment saying that an
/// intermediate `std::vector<std::byte>` there "would be a second full copy of a
/// preprocessed translation unit, on the path a build with compression configured
/// out takes for every payload -- the one least able to afford it". Owning here
/// would reinstate exactly that copy, to buy safety that consumer does not need.
///
/// Its sibling `CompileResultFields` owns for the opposite reason: `Dispatch` holds
/// a decoded reply across statements and hands the object onward. The question is
/// per type -- does anything depend on this not copying, and does the result outlive
/// the buffer in practice -- not a preference for one shape.
struct CodecEnvelopeView
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
[[nodiscard]] inline std::optional<CodecEnvelopeView> DecodeCodecEnvelope(std::span<std::byte const> field)
{
    if (field.size() < CodecHeaderSize)
        return std::nullopt;
    return CodecEnvelopeView { .codec = static_cast<std::uint8_t>(field[0]),
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

/// Cache facts that travel one field per tier, positionally.
///
/// **Position is the tier**, because this header is compiled into
/// `fastcache-cc`, which does not link `FastCache` and therefore cannot see
/// `FastCache::StorageTier` — the same reason `nodeClassRaw` is a byte here and
/// an enumerator one layer up. Index 0 is the first tier that enum names, index
/// 1 the second, and so on; the mapping is done in
/// `Distributed/SchedulerProtocol.cpp`, which can see both.
///
/// That makes the enum's ORDER a wire contract: enumerators are appended, never
/// reordered, or a build reads a peer's disk tier as its memory tier and nothing
/// anywhere reports a fault. `StorageTier`'s own header says so.
///
/// An entry is absent when the sender has no such tier at all, which is a
/// different fact from a tier holding nothing — a dashboard draws a zero as
/// "empty" and an absence as "there isn't one". A list SHORTER than this build
/// expects leaves the remaining tiers absent, which is exactly right for a peer
/// that predates them.
template <typename T>
using PerTier = std::vector<std::optional<T>>;

/// What one cache tier is, stable for the life of the sending process.
///
/// Split from `CacheTierUsage` below exactly as `CapacityFields` is split from
/// `LoadFields`, and for the same reason: a budget captured per heartbeat is a
/// number the receiver would keep re-reading for nothing, and a usage figure
/// captured at registration is one it would keep believing.
struct CacheTierBudget
{
    /// Bytes this tier may hold. 0 means unbounded, which is a real
    /// configuration and not an absence — the tier's presence is carried by the
    /// optional around this struct.
    std::uint64_t bytesLimit { 0 };
};

/// A node's cache as it is configured, travelling inside REGISTER.
struct CacheCapacityFields
{
    PerTier<CacheTierBudget> tiers; ///< One entry per tier; absent means "no such tier".
};

/// Frame a cache-capacity record as one nested field list.
///
/// Nested inside the capacity record rather than added to REGISTER, for the
/// reason `EncodeCapacity` is nested inside REGISTER: `SplitFields` is exact by
/// design, so a fact added at any fixed-arity level makes two builds of a fleet
/// unable to speak at all.
/// @param cache The facts to encode.
/// @return The nested record's bytes.
[[nodiscard]] inline std::vector<std::byte> EncodeCacheCapacity(CacheCapacityFields const& cache)
{
    // The per-tier list is itself one field, so a later cache-wide fact that is
    // not per-tier can be the record's second field without moving the tiers.
    std::vector<std::vector<std::byte>> owned;
    std::vector<std::span<std::byte const>> tierFields;
    owned.reserve(cache.tiers.size());
    tierFields.reserve(cache.tiers.size());
    for (auto const& tier: cache.tiers)
    {
        owned.push_back(tier.has_value() ? WireFields::Encode({ std::span<std::byte const> {
                                               WireFields::ToBigEndian<std::uint64_t>(tier->bytesLimit) } })
                                         : std::vector<std::byte> {});
        tierFields.emplace_back(owned.back());
    }
    auto const tiers = WireFields::Encode(WireFields::FieldList { tierFields });
    return WireFields::Encode({ std::span<std::byte const> { tiers } });
}

/// Read a cache-capacity record back.
/// @param field The nested record's bytes.
/// @return The facts, or nullopt when the record is malformed.
[[nodiscard]] inline std::optional<CacheCapacityFields> DecodeCacheCapacity(std::span<std::byte const> field)
{
    // An absent record is not a malformed one: a peer that predates this field,
    // or one with no cache at all, is answered rather than refused.
    if (field.empty())
        return CacheCapacityFields {};

    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value())
        return std::nullopt;
    if (parts->empty() || parts->front().empty())
        return CacheCapacityFields {};

    auto const tiers = WireFields::SplitAll(parts->front());
    if (!tiers.has_value())
        return std::nullopt;

    CacheCapacityFields out {};
    out.tiers.reserve(tiers->size());
    for (auto const& tier: *tiers)
    {
        if (tier.empty())
        {
            out.tiers.emplace_back();
            continue;
        }
        auto const values = WireFields::SplitAll(tier);
        if (!values.has_value() || values->empty())
            return std::nullopt;
        auto const limit = WireFields::FromBigEndian<std::uint64_t>(values->front());
        if (!limit.has_value())
            return std::nullopt;
        out.tiers.emplace_back(CacheTierBudget { .bytesLimit = *limit });
    }
    return out;
}

/// What one cache tier holds right now.
struct CacheTierUsage
{
    std::uint64_t itemCount { 0 }; ///< Live entries.
    std::uint64_t bytesUsed { 0 }; ///< Bytes held.
    std::uint64_t evictions { 0 }; ///< Entries dropped to stay within the budget.

    /// Resident bytes the tier spends on its own key index (#175).
    ///
    /// Appended LAST, which is what makes it compatible in both directions: this
    /// record's arity is variable by design -- the decoder below stops at whichever
    /// of the two sides has fewer fields and leaves the rest at zero -- so an older
    /// peer sends three and is read as three, while a newer one sends four and an
    /// older reader ignores the fourth. Inserting it anywhere else would have
    /// renumbered the fields either side of it and made two builds disagree about
    /// what `bytesUsed` means.
    std::uint64_t indexBytes { 0 };
};

/// A node's cache as it stands right now, travelling inside HEARTBEAT.
struct CacheLoadFields
{
    PerTier<CacheTierUsage> tiers; ///< One entry per tier; absent means "no such tier".

    /// Reads the node's cache served, and reads it could not.
    ///
    /// Node-wide rather than per tier, and deliberately: a lower tier is
    /// consulted only when the one above it missed, so per-tier figures do not
    /// add up to the node's and a consumer summing them would report a cache
    /// serving every read at well under 100%.
    ///
    /// Optional because absent is not zero here too: a node that cannot say is
    /// not a node with no hits.
    std::optional<std::uint64_t> hits;
    std::optional<std::uint64_t> misses; ///< @see hits.
};

/// Frame a cache-load record as one nested field list.
/// @param cache What the cache holds.
/// @return The nested record's bytes.
[[nodiscard]] inline std::vector<std::byte> EncodeCacheLoad(CacheLoadFields const& cache)
{
    std::vector<std::vector<std::byte>> owned;
    std::vector<std::span<std::byte const>> tierFields;
    owned.reserve(cache.tiers.size());
    tierFields.reserve(cache.tiers.size());
    for (auto const& tier: cache.tiers)
    {
        owned.push_back(
            tier.has_value()
                ? WireFields::Encode(
                      { std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(tier->itemCount) },
                        std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(tier->bytesUsed) },
                        std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(tier->evictions) },
                        std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(tier->indexBytes) } })
                : std::vector<std::byte> {});
        tierFields.emplace_back(owned.back());
    }
    auto const tiers = WireFields::Encode(WireFields::FieldList { tierFields });
    auto const hits = WireFields::ToBigEndian<std::uint64_t>(cache.hits.value_or(0));
    auto const misses = WireFields::ToBigEndian<std::uint64_t>(cache.misses.value_or(0));
    return WireFields::Encode(
        { std::span<std::byte const> { tiers },
          cache.hits.has_value() ? std::span<std::byte const> { hits } : std::span<std::byte const> {},
          cache.misses.has_value() ? std::span<std::byte const> { misses } : std::span<std::byte const> {} });
}

/// Read a cache-load record back.
/// @param field The nested record's bytes.
/// @return The facts, or nullopt when the record is malformed.
[[nodiscard]] inline std::optional<CacheLoadFields> DecodeCacheLoad(std::span<std::byte const> field)
{
    if (field.empty())
        return CacheLoadFields {};

    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value())
        return std::nullopt;
    auto const at = [&](std::size_t index) {
        return index < parts->size() ? (*parts)[index] : std::span<std::byte const> {};
    };

    CacheLoadFields out {};
    if (auto const packed = at(0); !packed.empty())
    {
        auto const tiers = WireFields::SplitAll(packed);
        if (!tiers.has_value())
            return std::nullopt;
        out.tiers.reserve(tiers->size());
        for (auto const& tier: *tiers)
        {
            if (tier.empty())
            {
                out.tiers.emplace_back();
                continue;
            }
            auto const values = WireFields::SplitAll(tier);
            // Four fields expected and fewer tolerated, exactly as the records above
            // tolerate a short peer: what is absent stays at zero. That tolerance is
            // what let `indexBytes` be appended without a version bump -- and it only
            // holds while new fields go on the END.
            if (!values.has_value())
                return std::nullopt;
            CacheTierUsage usage {};
            constexpr std::array Members { &CacheTierUsage::itemCount,
                                           &CacheTierUsage::bytesUsed,
                                           &CacheTierUsage::evictions,
                                           &CacheTierUsage::indexBytes };
            for (std::size_t index = 0; index < Members.size() && index < values->size(); ++index)
            {
                auto const& raw = (*values)[index];
                if (raw.empty())
                    continue;
                auto const value = WireFields::FromBigEndian<std::uint64_t>(raw);
                if (!value.has_value())
                    return std::nullopt;
                usage.*Members[index] = *value;
            }
            out.tiers.emplace_back(usage);
        }
    }
    if (auto const hits = at(1); !hits.empty())
    {
        out.hits = WireFields::FromBigEndian<std::uint64_t>(hits);
        if (!out.hits.has_value())
            return std::nullopt;
    }
    if (auto const misses = at(2); !misses.empty())
    {
        out.misses = WireFields::FromBigEndian<std::uint64_t>(misses);
        if (!out.misses.has_value())
            return std::nullopt;
    }
    return out;
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

    /// The node's cache, as it was configured.
    ///
    /// Every member of this fleet is a cache, and the leader could say nothing at
    /// all about any of them. It rides here rather than as a REGISTER field for
    /// the reason the whole record is nested — `SplitFields` is exact, so a
    /// top-level addition makes two builds unable to speak — and it is a
    /// *registration* fact because a budget does not move while the process runs.
    CacheCapacityFields cache {};

    /// What software this node is running, as `FastCache::VersionString` spells it.
    ///
    /// Empty means **did not say**, and on this field that is a fact rather than an
    /// omission: a node built before this field existed cannot report a version, and
    /// a fleet mid-upgrade is exactly when somebody is reading this page. Rendering
    /// it as "unknown" or as a blank would make the one node that is too old to
    /// answer look like the one node with nothing interesting about it.
    ///
    /// A *registration* fact, because a running process does not change version —
    /// and here rather than at REGISTER's top level for the same arity reason as
    /// everything else in this record.
    ///
    /// **Owned, not a view, and that is load-bearing.** `DecodeCapacity` returns
    /// this struct *by value*, so `DecodeCapacity(EncodeCapacity(x))` is the obvious
    /// spelling — and with a `string_view` here it is a use-after-free the moment
    /// the temporary dies. Nothing in the name warns anyone: `RegisterView` says
    /// "View" precisely because it borrows, while this type is used for both
    /// directions and reads as a value. It cost a macOS-only CI failure to learn
    /// that, on the one standard library whose allocator reuses the block quickly
    /// enough to notice. A registration is once per node, so the copy is free.
    std::string version {};

    /// Memory the node holds for itself, and so cannot lend to a compile.
    ///
    /// Travels because slot derivation may happen at either end: a node normally
    /// sizes itself and sends the answer, but `slots = 0` asks the scheduler to do
    /// it -- and a scheduler budgeting jobs against RAM the node has already spent
    /// on its own cache would over-commit exactly the machines that report it.
    /// Zero from a peer too old to say, which is the arithmetic this had before.
    std::uint64_t reservedMemoryBytes { 0 };

    /// What a person calls the toolchain this registration is for, e.g.
    /// `cl 19.44.35207`. Empty means "did not say".
    ///
    /// The one field of this record that is NOT node-wide, and it rides here anyway
    /// for the reason everything else does: `SplitFields` is exact at REGISTER's top
    /// level, so an addition there makes two builds unable to speak, while this
    /// record tolerates a short peer. A machine with two toolsets sends two
    /// registrations describing one machine and two different compilers, so the
    /// per-registration copy is the whole point rather than a wart (#194).
    ///
    /// **Display only, and never an identity.** The fingerprint decides every match;
    /// nothing keys on, compares or routes by this. Both are reported because they
    /// answer different questions -- and a worker that derived its identity from a
    /// second string would register cleanly and never be matched, with nothing
    /// anywhere reporting why.
    ///
    /// Empty for an operator's `<fingerprint>=<compiler>` override, which is never
    /// probed. Absent renders as absent, never as a blank or as "unknown", for the
    /// reason `version` above gives at length.
    ///
    /// **Owned, not a view**, and for exactly the reason `version` documents: this
    /// struct is returned by value, so a `string_view` here would dangle the moment
    /// the encoded temporary died.
    std::string toolchainLabel {};
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
    auto const cache = EncodeCacheCapacity(capacity.cache);
    auto const reservedMemory = WireFields::ToBigEndian<std::uint64_t>(capacity.reservedMemoryBytes);
    return WireFields::Encode({ std::span<std::byte const> { cores },
                                std::span<std::byte const> { memory },
                                std::span<std::byte const> { nodeClass },
                                reserve,
                                std::span<std::byte const> { cache },
                                AsBytes(capacity.version),
                                std::span<std::byte const> { reservedMemory },
                                AsBytes(capacity.toolchainLabel) });
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
    if (auto const cache = DecodeCacheCapacity(at(4)); cache.has_value())
        out.cache = *cache;
    else
        return std::nullopt;
    // Free-form, and deliberately not validated: a version is a string an operator
    // reads, not one this code branches on, so a shape it does not recognise is a
    // peer to report rather than a peer to refuse. A record from a build that
    // predates the field simply has no fifth index, which `at` answers as empty.
    out.version = std::string { AsStringView(at(5)) };
    if (auto const reservedMemory = at(6); !reservedMemory.empty())
    {
        auto const value = WireFields::FromBigEndian<std::uint64_t>(reservedMemory);
        if (!value.has_value())
            return std::nullopt;
        out.reservedMemoryBytes = *value;
    }
    // Free-form and unvalidated here for the reason `version` is: it is a string an
    // operator reads rather than one this code branches on. It is not unchecked,
    // though -- `SchedulerService::Register` refuses text that is not UTF-8, where it
    // enters, because one bad byte makes `/fleet.json` unparseable for the whole
    // fleet. A build predating the field has no eighth index, which `at` answers as
    // empty.
    out.toolchainLabel = std::string { AsStringView(at(7)) };
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

/// How many readings one history bucket carries.
///
/// A WIRE CONTRACT, and the same kind `StorageTier`'s enumerator order already is:
/// the slots travel POSITIONALLY, so this number and `FleetMetric`'s enumerator
/// count are one fact spelled in two places that cannot include each other -- this
/// header stays dependency-free because `fastcache-cc` compiles it without linking
/// `FastCache`. The assertion tying them together lives on the side that can see
/// both.
///
/// Every slot travels, including the ones only a scheduler can answer for, which
/// come across as zero. Sending just the node-scoped ones would make the arity
/// depend on a table the two peers might disagree about, and a misaligned reading is
/// worse than five wasted words.
inline constexpr std::size_t HistorySlotCount = 9;

/// One closed bucket of a node's own history.
///
/// Two instants and the readings: nothing a receiver can recompute travels. A
/// bucket's fold and its coverage are both derived by replaying these readings at
/// these instants, which the leader does anyway to fold them into its coarser rings
/// -- so carrying either would be a second answer to a question already answered,
/// and the one a decoder trusted would be the one nothing kept correct. Nor would a
/// fold help: the leader merges these into a FLEET-wide series whose peak is the
/// peak of the sums, which is not the sum of the peaks. The nested record is
/// variable-arity, so a build that finds a use can add a field without a version.
struct HistoryBucketFields
{
    std::uint64_t startMillis { 0 };                       ///< Wall-clock start of the bucket.
    std::uint64_t sampleMillis { 0 };                      ///< When the reading in `values` was taken.
    std::array<std::uint64_t, HistorySlotCount> values {}; ///< The readings, positionally.
};

/// The most closed buckets one heartbeat may carry.
///
/// A node absent for a day has 1440 minute-buckets to hand over and a heartbeat is
/// bounded at `MaxControlPayload`, so this is a real ceiling rather than defensive
/// padding: what does not fit waits for the next round, oldest first. At a 20-second
/// heartbeat that clears a full day inside four minutes, while steady state is one
/// bucket every three rounds and never approaches it.
inline constexpr std::size_t MaxHistoryBucketsPerHeartbeat = 128;

/// What one encoded bucket costs at most, framing included.
///
/// Three fields, each a length-prefixed word or array, plus the prefix on the record
/// itself. Written out rather than measured so the ceiling below is a compile-time
/// fact, and in terms of `FieldPrefixSize` rather than a literal 4, which would
/// restate the framing contract beside the one place it is defined.
inline constexpr std::size_t MaxHistoryBucketBytes =
    (2 * (sizeof(std::uint64_t) + WireFields::FieldPrefixSize))
    + ((HistorySlotCount * sizeof(std::uint64_t)) + WireFields::FieldPrefixSize) + WireFields::FieldPrefixSize;

/// The batch fits, with the rest of a heartbeat still to fit beside it.
///
/// A ceiling nobody checked is a ceiling that becomes a frame a peer refuses -- and
/// a refused heartbeat is a worker the fleet stops seeing, which is the failure this
/// whole subsystem exists to make visible rather than to cause. Half the payload is
/// left for everything else, which is two orders of magnitude more than the rest of
/// a heartbeat needs.
static_assert(MaxHistoryBucketsPerHeartbeat * MaxHistoryBucketBytes <= MaxControlPayload / 2,
              "a history batch must leave room for the heartbeat carrying it");

/// Frame a run of closed buckets as one nested field list.
///
/// @param buckets What to encode; at most `MaxHistoryBucketsPerHeartbeat` are taken,
///                oldest first, and the rest wait for the next round.
/// @return The nested record's bytes, to be carried as a single load field.
[[nodiscard]] inline std::vector<std::byte> EncodeHistoryBuckets(std::span<HistoryBucketFields const> buckets)
{
    auto const count = std::min(buckets.size(), MaxHistoryBucketsPerHeartbeat);
    std::vector<std::vector<std::byte>> owned;
    std::vector<std::span<std::byte const>> fields;
    owned.reserve(count);
    fields.reserve(count);
    for (auto const& bucket: buckets.subspan(0, count))
    {
        std::vector<std::byte> packed;
        packed.reserve(HistorySlotCount * sizeof(std::uint64_t));
        for (auto const value: bucket.values)
        {
            auto const word = WireFields::ToBigEndian<std::uint64_t>(value);
            packed.insert(packed.end(), word.begin(), word.end());
        }
        owned.push_back(
            WireFields::Encode({ std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(bucket.startMillis) },
                                 std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(bucket.sampleMillis) },
                                 std::span<std::byte const> { packed } }));
        fields.emplace_back(owned.back());
    }
    return WireFields::Encode(WireFields::FieldList { fields });
}

/// Read a run of closed buckets back.
///
/// Tolerant of a peer carrying more slots than this build knows and of one carrying
/// fewer, exactly as `DecodeLoad` is: the extra are ignored and the missing stay
/// zero. Strict about WIDTH, because a slot at the wrong width is a number nobody
/// can interpret rather than one somebody can ignore.
///
/// @param field The nested record's bytes.
/// @return The buckets, or nullopt when a field is present at the wrong width.
[[nodiscard]] inline std::optional<std::vector<HistoryBucketFields>> DecodeHistoryBuckets(std::span<std::byte const> field)
{
    std::vector<HistoryBucketFields> out;
    if (field.empty())
        return out;

    auto const entries = WireFields::SplitAll(field);
    if (!entries.has_value())
        return std::nullopt;
    // Refused rather than truncated: a batch above the ceiling is a peer that did not
    // honour it, and quietly keeping the first 128 would leave the rest looking
    // delivered.
    if (entries->size() > MaxHistoryBucketsPerHeartbeat)
        return std::nullopt;

    out.reserve(entries->size());
    for (auto const& entry: *entries)
    {
        auto const parts = WireFields::SplitAll(entry);
        if (!parts.has_value() || parts->size() < 3)
            return std::nullopt;

        HistoryBucketFields bucket;
        auto const start = WireFields::FromBigEndian<std::uint64_t>((*parts)[0]);
        auto const sampled = WireFields::FromBigEndian<std::uint64_t>((*parts)[1]);
        if (!start.has_value() || !sampled.has_value())
            return std::nullopt;
        bucket.startMillis = *start;
        bucket.sampleMillis = *sampled;

        auto const packed = (*parts)[2];
        if (packed.size() % sizeof(std::uint64_t) != 0)
            return std::nullopt;
        auto const carried = packed.size() / sizeof(std::uint64_t);
        for (auto const slot: std::views::iota(std::size_t { 0 }, std::min(carried, HistorySlotCount)))
        {
            auto const read = WireFields::FromBigEndian<std::uint64_t>(
                packed.subspan(slot * sizeof(std::uint64_t), sizeof(std::uint64_t)));
            if (!read.has_value())
                return std::nullopt;
            bucket.values[slot] = *read;
        }
        out.push_back(bucket);
    }
    return out;
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

    /// What the node's cache holds right now.
    ///
    /// A heartbeat fact rather than a registration one, and the split is the same
    /// `NodeCapacity`/`NodeLoad` draw: item count, bytes and evictions move while
    /// the process runs, so a copy taken at registration is a number the scheduler
    /// would keep believing long after it stopped being true.
    ///
    /// **It is per NODE, not per registry entry.** A worker with two `--toolchain`
    /// flags registers twice against one machine and both entries heartbeat these
    /// same numbers, so anything summing them across entries counts one cache
    /// twice. `WorkerRegistry::NodeCaches()` is what dedupes.
    CacheLoadFields cache {};

    /// Closed history buckets this node has not had acknowledged.
    ///
    /// Carried HERE rather than as a fourth HEARTBEAT field, and that is forced
    /// rather than chosen: `SplitFields` reads a heartbeat with `SplitExactly`, so
    /// its top-level arity is three forever and a fourth field would be a frame every
    /// existing peer refuses. This record is the variable-arity one -- which is the
    /// whole reason it is nested, as the comment on `EncodeLoad` says.
    ///
    /// Empty on almost every heartbeat: a bucket closes once a minute and a heartbeat
    /// goes every twenty seconds.
    std::vector<HistoryBucketFields> history;
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
    auto const cache = EncodeCacheLoad(load.cache);
    auto const history = EncodeHistoryBuckets(load.history);
    return WireFields::Encode(
        { load.cpuBusyPermille.has_value() ? std::span<std::byte const> { cpu } : std::span<std::byte const> {},
          load.availableMemoryBytes.has_value() ? std::span<std::byte const> { memory } : std::span<std::byte const> {},
          load.freeScratchBytes.has_value() ? std::span<std::byte const> { scratch } : std::span<std::byte const> {},
          std::span<std::byte const> { cache },
          std::span<std::byte const> { history } });
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
    if (auto const cache = DecodeCacheLoad(at(3)); cache.has_value())
        out.cache = *cache;
    else
        return std::nullopt;
    // Absent on every heartbeat from a peer older than this field, which `at()`
    // already answers as an empty span -- and an empty span decodes to no buckets
    // rather than to a refusal, which is what keeps this additive.
    auto history = DecodeHistoryBuckets(at(4));
    if (!history.has_value())
        return std::nullopt;
    out.history = std::move(*history);
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

/// A client reporting that the job it leased has ended.
struct ReleaseRequest
{
    std::string_view leaseToken; ///< The token the scheduler granted.
    std::string_view key;        ///< The object key that lease was taken on.
};

/// The same, as views into a received payload.
struct ReleaseView
{
    std::span<std::byte const> leaseToken;
    std::span<std::byte const> key;
};

/// Frame a RELEASE request.
///
/// The key travels beside the token, and it is not redundant: a token is a small
/// integer minted by whichever `LeaseTable` is alive, and that counter starts again
/// at one in a scheduler that has just restarted. Without the key, a client
/// reporting a job it started before the restart would resolve whatever lease the
/// new instance had since issued under the same number -- freeing a key somebody is
/// building, and decrementing a worker that is busy. Naming both makes a release
/// resolve *the client's own* lease or nothing, which is also what stops one member
/// resolving another's by guessing a number.
///
/// Nothing about how the job went, though: the scheduler resolves a lease the same
/// way whatever the outcome -- `LeaseTable::Release` is documented as "however the
/// job ended" -- and a field nothing reads is a field two builds can disagree
/// about. The top-level arity of a verb is exact and fixed forever, so an outcome
/// added later would arrive as a nested record, the way `Register` carries capacity.
/// @param request The token and the key it was taken on.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeRelease(ReleaseRequest const& request,
                                                          WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::Release, { AsBytes(request.leaseToken), AsBytes(request.key) });
}

/// Split a RELEASE payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<ReleaseView> DecodeReleasePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Release));
    if (!fields.has_value())
        return std::nullopt;
    return ReleaseView { .leaseToken = (*fields)[0], .key = (*fields)[1] };
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
    /// What the worker says it actually compiled, tying this reply to its request.
    ///
    /// Nothing else did. The cache key covers the inputs, the fingerprint the
    /// toolchain and the lease the authorization -- every one of them upstream of the
    /// reply -- so a client sent a job and accepted whatever object came back on that
    /// connection ([#280](https://github.com/LASTRADA-Software/fastcached/issues/280)).
    ///
    /// Produced by `Cc::CompileCorrelation` in the RUNNER, from what it actually
    /// spawned and wrote, never recomputed at this layer from the decoded request: at
    /// this layer two crossed requests are both still pristine, so a digest taken here
    /// agrees with whatever it is compared against.
    std::span<std::byte const> correlation;
};

/// Frame the payload of a COMPILE reply.
/// @param result The outcome.
/// @return The reply payload (not a whole frame).
[[nodiscard]] inline std::vector<std::byte> EncodeCompileResult(CompileResult const& result)
{
    auto const code = EncodeU32Field(result.exitCode);
    return WireFields::Encode(
        { std::span<std::byte const> { code }, result.object, result.stdoutText, result.stderrText, result.correlation });
}

/// A decoded COMPILE reply, owning every byte of it.
///
/// **The decode-side twin of `CompileResult`, and it OWNS where that one borrows.**
/// `CompileResult` is an encoder INPUT: its spans are read inside the
/// `EncodeCompileResult` call and never outlive it, which is what every encode-side
/// struct in this header does (`LeaseGrant`, `CompileRequest`, `StoreRequest`).
/// A decoder's result is different in kind -- it is returned **by value**, so a
/// borrowing member outlives the call and dangles the moment the buffer goes
/// ([#366](https://github.com/LASTRADA-Software/fastcached/issues/366)).
///
/// `*Fields` rather than `*View` because the name has to say which it is, and this
/// header already uses that suffix for exactly this: `CapacityFields`, `LoadFields`
/// and `CacheLoadFields` are all owning records a decoder returns, while the ten
/// `*View` types borrow and say so.
struct CompileResultFields
{
    std::uint32_t exitCode { 0 };
    std::vector<std::byte> object; ///< Codec envelope; empty on a failed compile.
    std::vector<std::byte> stdoutText;
    std::vector<std::byte> stderrText;
    std::vector<std::byte> correlation; ///< @see `CompileResult::correlation`.
};

/// Split a COMPILE reply payload.
/// @param payload The reply body.
/// @return The result, or nullopt when malformed.
[[nodiscard]] inline std::optional<CompileResultFields> DecodeCompileResult(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, 5);
    if (!fields.has_value())
        return std::nullopt;
    auto const code = DecodeU32Field((*fields)[0]);
    if (!code.has_value())
        return std::nullopt;
    auto const own = [](std::span<std::byte const> f) {
        return std::vector<std::byte> { f.begin(), f.end() };
    };
    return CompileResultFields { .exitCode = *code,
                                 .object = own((*fields)[1]),
                                 .stdoutText = own((*fields)[2]),
                                 .stderrText = own((*fields)[3]),
                                 .correlation = own((*fields)[4]) };
}

} // namespace FastCache::CompileCacheWire
