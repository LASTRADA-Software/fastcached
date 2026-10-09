// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Protocol/NodeConditionWire.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
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
///
/// **3 added `Status::Progress`**, the liveness pulse a worker writes while a
/// dispatched compile runs
/// ([#245](https://github.com/LASTRADA-Software/fastcached/issues/245)).
///
/// **4 gave the REGISTER reply a shape.** It was the worker id as bare bytes; it is
/// now a nested record carrying the id, the fleet identity and the scheduler epoch,
/// so a worker learns which fleet it serves from the one exchange it has with the
/// scheduler it was configured to reach
/// ([#401](https://github.com/LASTRADA-Software/fastcached/issues/401)).
///
/// **6 put the lease's lifetime on the LEASE grant.** The lifetime became a
/// replicated setting, so the number a client must bound its wait by is no longer a
/// constant both ends compile in -- it has to travel
/// ([#522](https://github.com/LASTRADA-Software/fastcached/issues/522)).
///
/// **7 gave the NODE-STATUS reply a runtime record**, so a node with the default
/// configuration -- no `--admin-listen`, therefore no `/metrics` and no dashboard --
/// can say what it is DOING and not only what it was configured as
/// ([#1294](https://github.com/LASTRADA-Software/fastcached/issues/1294),
/// [#1295](https://github.com/LASTRADA-Software/fastcached/issues/1295)).
///
/// It is one new top-level field and it is intended to be the LAST one: what it
/// carries is a nested variable-arity record, for the reason `EncodeCapacity` is
/// nested inside REGISTER. So this arity moves once, here, and every later runtime
/// fact costs no version at all.
///
/// **8 gave the CLUSTER-ADMIT reply a receipt.** It answered an empty success, so the
/// only thing an operator learned was that the message had arrived; the id and the
/// endpoint the leader wrote into the command now come back as bytes they can read
/// against the machine being admitted
/// ([#1296](https://github.com/LASTRADA-Software/fastcached/issues/1296)).
///
/// A RECEIPT and not a confirmation, which is a constraint on the shape as much as on
/// the wording: see `ClusterAdmitReceipt`. Exact arity of two, like every other reply
/// body here.
///
/// **9 added `Status::Push` and `Op::Subscribe`**: a live-stats session subscribes on a
/// connection of its own and the node PUSHES what the dashboard draws, instead of the
/// dashboard polling `/metrics` and `/fleet.txt` over HTTP
/// ([#1399](https://github.com/LASTRADA-Software/fastcached/issues/1399)). A reply carries a
/// status byte and no kind, and `DecodeReplyHeader` refuses a status it does not know, so a
/// new status is a new reply grammar for every reader and the floor moves with it -- the
/// version-3 argument exactly.
///
/// **Every node and every `fastcache-cc` upgrade together.** A launcher is a reader of this
/// wire, and a version-8 launcher is refused by a version-9 node rather than served; on this
/// project's one installation that is one rebuild of both binaries
/// ([#332](https://github.com/LASTRADA-Software/fastcached/issues/332)).
///
/// **10 made the NODE-METRICS reply a `StatsReading`** instead of the counter catalogue as
/// `name value` pairs, so it carries every figure `/metrics` does
/// ([#1406](https://github.com/LASTRADA-Software/fastcached/issues/1406)). The body of an
/// existing verb changed shape, which no reader can step over: a version-9 client would read
/// the reading as malformed rows and report a node whose answer it could not read, and a
/// version-10 client would read a version-9 node's rows as a reading laid out by another build.
/// Both are wrong sentences about the right machine; `UnsupportedVersion` naming the range is
/// the right one.
///
/// **11 gave CLUSTER-ADMIT and CLUSTER-ADMIT-LEARNER a member's identity key** (#178): the
/// request carries the `@<key>` an operator typed, as a third field, and the receipt echoes
/// the key the leader RECORDED, as a third field of its own. Both arities moved, which a
/// reader cannot step over -- the request's is exact, and a version-10 client reading a
/// version-11 receipt would refuse a body with a field it never heard of, while a version-11
/// client reading a version-10 receipt would report a key nobody recorded as absent. A key
/// that reaches the leader and is dropped on the way is the worst of the three: a member
/// admitted with no key while its operator believes it has one.
///
/// **12 took the secret out of ENROLL and put identity in** (#178). The request carries the
/// joiner's ROLE and its identity KEY beside its id and endpoint, so its arity went from two to
/// four; the approved reply carries the cluster's ROSTER where it carried the cluster key; and
/// a pending row reports the joiner's role, its key and the fingerprint of the roster it was
/// last handed. A version-11 joiner would send two fields a version-12 leader refuses, and a
/// version-11 leader would hand a version-12 joiner a secret where it expects a roster -- which
/// a version-12 decoder reads as a malformed roster, but only after the secret crossed the
/// wire. Refused by version, before a byte of the request is read, is the one outcome that
/// hands nothing to anybody.
///
/// **13 made NODE-ANNOUNCE carry a voter's roster endorsement and answer with the certified
/// roster** (#178). The request gained a fourth top-level field, which the exact-arity decoder
/// cannot step over, and the reply gained a body where it had none: a version-12 node would
/// refuse every announcement a version-13 voter sends, and a version-12 worker would never
/// adopt a roster and refuse every grant once its old one lapsed -- both silently, in the
/// words of the refusal that ends it.
///
/// **14 made the node proof an IDENTITY and sealed everything after it** (#178). NODE-CHALLENGE
/// now carries the caller's nonce and ephemeral key and is answered with the server's id, key,
/// nonce, ephemeral key and a signature over all of it; PROVE-NODE carries the caller's id, key
/// and a signature where it carried a MAC under the cluster key; and every frame after an
/// accepted proof, in both directions, carries a tag under a session key only the two ends hold.
/// Three request shapes and a frame grammar moved, which no reader can step over: a version-13
/// node would refuse every version-14 challenge as malformed, and a version-14 node reading a
/// version-13 proof would be asked to verify a MAC under a key it no longer has. The four verbs
/// a joining machine sends -- `Register`, `NodeAnnounce`, `Heartbeat`, `Withdraw` -- also now
/// require that identity (`OpDescriptor::identity`), and a worker-admission verb (0x1D, since
/// retired with principal mode) and three error codes are new.
///
/// **15 is the office-fleet flag day: zero-config formation, machine tickets and signed compile
/// replies** (#178 and the formation lanes). Request arities moved -- AUTH carries its credential
/// kind (2 -> 3 fields), LEASE the workers the client could not reach and its toolchain label
/// (3 -> 5), ENROLL is signed over a challenge (4 -> 7) -- and so did replies: a LEASE grant names
/// a dial hint and the worker's identity key (4 -> 6), a COMPILE reply carries that key's
/// signature, an ENROLL reply carries the leader's challenge, key and signature, NODE-ANNOUNCE
/// carries join memos where it carried a roster endorsement and answers with nothing, the
/// NODE-STATUS runtime record holds 24 positions, its roster record closed up to three, and a
/// FLEET-SUMMARY reply holds its member list to the datagram's cap. MINT-TICKET (0x1E),
/// FLEET-SUMMARY (0x1F), SHARED-FETCH (0x20) and SHARED-STORE (0x21) are new verbs; 0x16, 0x17 and
/// 0x1D are retired; error codes 0x2C..0x33 are new and 0x28 is retired. Every one of those is a
/// shape an exact-arity decoder cannot step over in either direction: a version-14 reader would
/// refuse the new requests as malformed and read the new replies' extra fields as a peer speaking
/// nonsense, and a version-15 reader of a version-14 grant would hold no worker key and trust an
/// unsigned compile.
inline constexpr WireVersion CurrentVersion = 15;

/// The oldest version this build still accepts. Equal to `CurrentVersion` while
/// only one version exists; widen the range when a second one ships and this
/// build can still decode the older shape.
///
/// **The consequence is a flag day, and it is now written down for operators**
/// rather than left to be discovered during a rollout:
/// `docs/operations/upgrading-a-fleet.md` carries the supported stop-upgrade-start
/// procedure and the four counters a mismatch is visible on. That page is the
/// operator-facing half of what this constant decides, so a change here that opens a
/// compatibility window updates it in the same commit — a window nobody is told about
/// buys an operator nothing.
///
/// The argument below rests on #332's position that backwards compatibility is not
/// owed before the production-ready declaration. **That licence expires at the
/// declaration**, and the release that creates fleets is the release that retires the
/// premise a flag day rests on
/// ([#998](https://github.com/LASTRADA-Software/fastcached/issues/998)). Documenting
/// the flag day is what that ticket calls the cheapest honest option; widening the
/// range per verb — `Op::Compile` has exact arity and is the binding constraint, the
/// cache verbs are cheaper — remains open and is a decision rather than a defect.
///
/// **It moved with `CurrentVersion` for version 3, and that is the whole
/// negotiation.** `Status::Progress` is a change to the REPLY, which carries a
/// status byte and a length and no kind — so the "step over what you do not know"
/// property the request framing has does not exist on the way back:
/// `DecodeReplyHeader` refuses a status outside its own set, and a launcher built
/// before this meets a progress frame by abandoning the compile. Keeping the range
/// open would therefore not buy a mixed fleet anything; it would buy a mixed fleet
/// that fails at the worst possible moment, several minutes into a translation unit,
/// as a transport failure naming nothing. Refusing the older request outright is
/// `UnsupportedVersion`, which names the supported range and is answered before a
/// byte of source is sent.
///
/// The second thing it buys is that the capability needs no negotiation at all:
/// every accepted request is version 3, so every client a worker answers understands
/// a progress frame, and nothing has to remember a per-connection flag. Backwards
/// compatibility on this wire is not owed
/// ([#332](https://github.com/LASTRADA-Software/fastcached/issues/332)) — one
/// installation, major version 0 — and a loud, named, immediate refusal is what that
/// budget is best spent on.
///
/// **It moved again for version 4, for the same reason and a sharper one.** The
/// REGISTER reply's payload changed shape, and a reply carries a status byte and a
/// length and no kind -- so a version-3 worker meeting a version-4 reply does not
/// refuse it. It reads the whole payload as its worker id, exactly as it always has,
/// and registers successfully under an id that is really a serialized record. That
/// is silent, it is durable, and it is the shape
/// [`.agent/rules/metrics-and-observability.md`](../../../.agent/rules/metrics-and-observability.md)
/// spends its length on: a wrong answer that looks like a right one. Refusing the
/// older REGISTER outright is `UnsupportedVersion`, which names the range and
/// arrives before the worker believes it has joined anything.
///
/// **And again for version 6, which is version 4's case exactly.** The LEASE reply
/// gained a field, and `SplitFields` is exact about arity -- so a version-5 launcher
/// meeting a version-6 grant does not misread it, it fails to decode it at all, which
/// is the better half of this. The reason the range still cannot stay open is the
/// other direction: a version-6 launcher against a version-5 scheduler decodes
/// nothing either, and the alternative -- accepting a three-field grant and falling
/// back to the constant -- is precisely the silent wrong answer #522 exists to
/// remove, reintroduced as a compatibility shim. A client that cannot learn the
/// fleet's lifetime must not guess it.
///
/// **And again for version 7 -- on a DIFFERENT argument from every one above, which
/// is why it is spelled out rather than filed under "the floor always moves".** The
/// three bumps above all rest on the failure being SILENT: a version-3 worker misreads
/// a version-4 REGISTER reply as a worker id, a pre-#245 launcher abandons a compile
/// minutes in. Version 7's does not. `NodeStatusFields` grew a sixth top-level field
/// and `SplitFields` is exact about arity, so a version-6 client fails to decode the
/// body at all and says so by name -- *"answered node-status with a body this client
/// cannot read"*. On the usual argument that would be a reason to LEAVE the floor at 6
/// and let older clients keep every verb whose shape did not move, which is most of
/// them.
///
/// The floor moves anyway, because of what leaving it would ADVERTISE. A window is
/// only real if this build can still SERVE the older version, and it cannot:
/// `EncodeNodeStatus` emits one shape and takes no version, so a floor of 6 would have
/// this build ACCEPT a version-6 request and answer it with bytes no version-6 client
/// can read. That is a supported version by advertisement and an unsupported one in
/// fact -- the version-4 paragraph's own defect one level up, since
/// `UnsupportedVersion` names the range -- `CompileCacheHandler`'s own
/// *"unsupported wire version {}; this server speaks {}..{}"*, whose numbers are these
/// two constants and are deliberately not restated here -- which
/// `docs/tools/fastcache-cc.md` documents as meaning a mixed install, while a decode
/// failure names no version and reads as a corrupt reply or a protocol bug. Refusing
/// by name is the more actionable of two loud failures.
///
/// Serving both shapes is the only other honest option and buys nothing here:
/// `docs/operations/upgrading-a-fleet.md` already documents the stop-upgrade-start
/// procedure, both binaries ship together, and this node has one installation
/// ([#332](https://github.com/LASTRADA-Software/fastcached/issues/332)), so the cost
/// of the flag day is one local rebuild. Opening a real window is the work that page
/// describes and is not owed yet.
///
/// **And again for version 8, whose argument is version 7's INVERTED.** It is written
/// out for the same reason 7's was: *the floor always moves* is not a reason, and the
/// reason here is not any of the four above. Every one of them rests on the older peer
/// failing -- loudly (7, 6) or silently-and-wrongly (4, 3). A version-7 client meeting
/// a version-8 CLUSTER-ADMIT reply does neither. It **succeeds**, correctly, and
/// quietly loses the change: that client renders its sentence from the verb it asked
/// and reads no reply body for this one at all, so an added payload is neither misread
/// nor refused. It prints *"accepted; the change is replicating"* -- still true -- and
/// drops the receipt.
///
/// That is worse than either loud failure, and worse **in this verb specifically**.
/// The receipt exists so that an operator compares two printed strings, and a string
/// that is missing does not announce itself as missing: there is nothing on the screen
/// to be suspicious of, and the operator concludes the comparison passed because they
/// never knew there was one. #1296 is a ticket about a quiet failure; a floor left at
/// 7 would manufacture a second one inside the fix.
///
/// A window is also only real if this build can SERVE the older version, and it cannot
/// for the reason version 7 could not: `EncodeClusterAdmitReceipt` emits one shape and
/// takes no version, so a floor of 7 would ACCEPT a version-7 request and answer it
/// with a body that version has no reading for. Supported by advertisement,
/// unsupported in fact.
///
/// The mirror direction rules out meeting in the middle. A version-8 client against a
/// version-7 leader asks for a receipt and is handed an empty body: it either refuses
/// -- loud, but misattributed, since an absent body reads as a corrupt reply rather
/// than as an older leader -- or renders nothing, which is the missing string again.
/// `UnsupportedVersion` naming the range -- the same sentence, rendered from these two
/// constants -- is the only one of the three that sends an operator to the right
/// machine.
///
/// The cost is unchanged from every bump above and is stated rather than reasoned from
/// again: one installation
/// ([#332](https://github.com/LASTRADA-Software/fastcached/issues/332)), both binaries
/// shipped together, and the stop-upgrade-start procedure that page already carries.
///
/// Version 9 moves it for version 3's reason: `Status::Push` is a status an older reader
/// refuses, so accepting a version-8 request would answer it in a grammar it cannot read.
///
/// Version 10 moves it for version 7's: `NodeMetrics` answers one shape and takes no version,
/// so a floor of 9 would accept a version-9 request and answer it with a body that version has
/// no reading for.
///
/// Version 11 moves it for the same reason: `EncodeClusterAdmitReceipt` emits one shape, three
/// fields, and a version-10 request answered with it would be answered in a grammar that
/// version has no reading for -- and a version-10 request is two fields, which the exact-arity
/// decoder refuses anyway.
///
/// Version 12 moves it for version 3's reason, and for a sharper one of its own: a version-11
/// ENROLL was answered, when approved, with the cluster KEY, and a leader that still accepted one
/// would have to either refuse it anyway or keep a secret-handing path alive for it.
///
/// Version 13 moves it for version 12's: a version-12 announcement is three fields, which the
/// exact-arity decoder refuses, and a version-12 client has no reading for the roster the reply
/// now carries.
///
/// Version 14 moves it for version 3's reason and a sharper one: a version-13 caller proves with a
/// MAC under a key this build no longer holds, so accepting its request would either refuse the
/// proof as a wrong key -- a confident wrong diagnosis for a version mismatch -- or keep a
/// pre-shared-key path alive for it, which is the path #178 exists to retire.
///
/// Version 15 moves it for version 3's reason and two sharper ones: a version-14 LEASE grant names
/// no worker key, so a version-15 client accepting one would take an UNSIGNED compile reply as
/// served -- the very forgery a signed reply exists to refuse -- and a version-14 AUTH carries no
/// credential kind, so a machine ticket and a password would be one field read two ways. Refused by
/// number, the mismatch names the range before a byte of either is read.
inline constexpr WireVersion MinSupportedVersion = 15;

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
    ClusterForget = 0x0A, ///< Operator forgets a member, revoking its key.
    ClusterAdmit = 0x0B,  ///< Operator adds a member, or moves one.
    // 0x16 and 0x17 are RETIRED and must never be reassigned -- see `RetiredOpcodes`.

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

    /// Worker tells the scheduler it has stopped serving one registration.
    ///
    /// The counterpart `Register` never had: a worker could announce a toolchain and
    /// could not retract one, so a node that re-surveyed and dropped a fingerprint
    /// simply stopped heartbeating that entry and let it age out at
    /// `DefaultHeartbeatTimeout`. For those 90 seconds the scheduler went on picking
    /// it -- `ReapExpiredWorkers` runs BEFORE `Pick`, so an entry inside its window
    /// survives the reap and is handed out, and the client is granted a lease to a
    /// worker certain to refuse it
    /// ([#573](https://github.com/LASTRADA-Software/fastcached/issues/573)).
    ///
    /// **It names the `workerId` and nothing else, and that is a security decision
    /// rather than a shape one.** A withdrawal naming `(fingerprint, endpoint)` would
    /// be an unauthenticated eviction primitive: `SchedulerService::Gate` asks
    /// membership, and membership deliberately admits every laptop and CI runner
    /// the roster holds, by the ticket it presents, so any admitted client could
    /// evict any worker from the fleet. The id is a capability this scheduler minted at `Register` and only
    /// that worker holds -- exactly the model `Heartbeat` already uses, which is the
    /// argument for it: it introduces no new trust question rather than merely
    /// resembling one.
    ///
    /// **Best-effort, and never load-bearing.** Every way of not delivering it --
    /// an old scheduler answering `UnknownOpcode`, a redirect, an unreachable
    /// scheduler -- leaves the pre-existing expiry to close the window exactly as
    /// before. That is what keeps this a latency optimisation rather than a
    /// correctness dependency, and it is why a worker must step over the refusal
    /// rather than treat it as fatal (#283, #340).
    Withdraw = 0x0D,

    // Operator verbs. What a person with a terminal needs to ASK a node, as opposed to
    // what the fleet needs to tell it. They exist because `fastcache-compile-node`
    // speaks this wire and nothing else, so a client pointed at one got a silent close
    // and no way to find out why: measured against the running node, RESP `PING`, RESP
    // `INFO` and memcached `version` each closed having sent nothing, while a `0xFC`
    // header got a proper refusal. `fastcache-cli` is the consumer.

    /// Operator asks what this node IS: its version, what it serves, and where.
    ///
    /// The reply carries, per surface this node actually opened, the surface's
    /// enumerator and its **port** -- never its bound address. A node bound
    /// `0.0.0.0:6675` that reported its bound address would hand a client something it
    /// cannot dial, and one bound to a loopback address would hand a REMOTE client an
    /// address meaning that client's own machine. The client composes the endpoint from
    /// the host it already dialled (known-good, since a frame just came back over it)
    /// plus the reported port. That is the same *opened addresses and dialled addresses
    /// are two tables* rule the config layer already carries.
    ///
    /// A surface this node does not serve is ABSENT from the reply rather than reported
    /// as port 0 -- absent is not zero, and a client must be able to tell *this node
    /// runs no admin surface* from *it runs one whose port I could not read*.
    NodeStatus = 0x0E,

    /// Operator asks this node for its figures.
    ///
    /// The same figures `/metrics` serves, over the wire an operator is already
    /// connected on -- which is what lets `--admin-addr` stop being required, since the
    /// admin surface may also be off entirely while this one is by definition up.
    ///
    /// **The reply is one `EncodeStatsReading`** (`Metrics/StatsReadingCodec.hpp`): the
    /// reading `/metrics` renders -- every counter, and the storage, tier, host and
    /// consensus blocks beside them -- captured once and encoded as a live-stats cache
    /// subject streams it. It was the counter catalogue alone, as `name value` pairs, so a
    /// node with no admin surface showed none of its cache tier's figures
    /// ([#1406](https://github.com/LASTRADA-Software/fastcached/issues/1406)). A client
    /// renders it through the same `RenderPrometheus`, and a client of another layout is
    /// refused by the digest the encoding starts with rather than misreading it.
    NodeMetrics = 0x0F,

    // Runtime enrollment. A fresh install asks the leader to join under the identity key it
    // minted, and is told who else is in the cluster once a person -- or a deadline a person
    // armed -- approves it, instead of every member's key being typed onto forty machines by hand
    // ([#1298](https://github.com/LASTRADA-Software/fastcached/issues/1298), #178). **No
    // secret crosses this exchange in either direction**: the request carries a public key
    // and the approved reply carries the roster, which is public too.
    //
    // Its own family and its own block, deliberately away from the distributed-execution
    // rows: that block's section comment states that none of it is `preAuth`, and `Enroll`
    // IS, so a row placed there would make the one comment a reviewer reads to understand
    // this table's security posture false.

    /// A keyless joiner asks the leader to be let into the cluster.
    ///
    /// **The second `OpenBeforeAuth` verb this protocol has ever had, and the first that
    /// is not the credential handshake.** Until this row, `Op::Auth` was the only one, so
    /// `PreAuthVerbsAreBounded` had never discriminated anything: it was satisfied by a
    /// table that was going to satisfy it anyway. This row is the first that makes it
    /// load-bearing, which is why it declares `BoundedTo(MaxEnrollPayload)` -- a ceiling
    /// far under the control cap, because the whole request is an identity, an endpoint, a
    /// role and a public key, and the peer sending it has presented nothing.
    ///
    /// It has to be pre-auth. The machine asking is by construction the one that holds
    /// no secret of this cluster -- that is the entire problem being solved -- so a
    /// credential gate here would refuse exactly the population the verb exists for.
    /// What replaces the credential is a person: a request is recorded on a bounded list
    /// and admits nobody until an operator approves the named id under the key it asked
    /// with, or asks inside a deadline an operator armed with `EnrollControlVerb::AutoApprove`.
    ///
    /// Answered by the LEADER only. A follower redirects with `NotLeader`, carrying the
    /// leader's endpoint, exactly as every other fleet-changing verb does.
    ///
    /// **Polled, not parked.** The reply is one of `EnrollOutcome`, immediately: a
    /// joiner that is not yet approved is told `Pending` and asks again. Holding the
    /// connection open until a person decided would need `Status::Progress`, which is
    /// `Op::Compile`'s alone and asserted to be, and would park an unauthenticated peer
    /// inside this surface for as long as the operator took to read the list.
    Enroll = 0x10,

    /// An operator inspects the joiners waiting, decides one, or arms or ends an auto-approve
    /// deadline -- one `EnrollControlVerbTable` row each.
    ///
    /// Gated exactly as `ClusterAdmit` is -- leadership, membership, and the credential
    /// when one is configured -- because it decides who joins the fleet. It is the half
    /// of this pair that carries the authority; `Enroll` carries only a request.
    EnrollControl = 0x11,

    /// Subscribe to what a live-stats panel draws, on a connection of its own.
    ///
    /// **The one request answered by a STREAM.** The node answers with `Status::Push` frames --
    /// first `PushKind::Subscribed`, then a `Snapshot` every granted cadence and an `Event`
    /// whenever a discrete fact changes -- and ends the stream with exactly one terminal reply:
    /// `Ok` when it stops for its own orderly reason, `Error` naming why otherwise (`NotLeader`
    /// after a demotion, `NotAMember` after a reload revoked the peer). A client that is done
    /// closes the connection; there is no unsubscribe verb, because EOF already says it.
    ///
    /// **Its own connection, always.** A stream is a writer on the socket for as long as it
    /// lives, and a second request on the same connection would put a second writer beside it
    /// -- the one-writer rule the endpoint is built on. So a subscribed connection carries no
    /// further request, and bytes arriving on one end it.
    ///
    /// Replaces polling for the dashboard only: `/metrics` and `/fleet.txt` stay what browsers
    /// and Prometheus read.
    Subscribe = 0x12,

    /// An operator on this machine cordons its worker, or lifts the cordon (#1303).
    ///
    /// **A cordon refuses new compiles and lets the running ones finish**, so a machine
    /// can be taken out of the fleet before a reboot without abandoning a compile that was
    /// about to succeed -- which is what a bounded shutdown does to any translation unit
    /// longer than `--drain-timeout`. The node keeps serving everything else.
    ///
    /// **A property of the worker PROCESS, and nothing replicates it.** The scheduler
    /// learns it from the worker's own heartbeat (`LoadFields::cordoned`), so a new leader
    /// learns it from the next one, and a restart un-cordons by construction: a cordon
    /// that survived a restart would be a machine that silently never came back.
    ///
    /// Not `Withdraw`, which REMOVES a registration: a cordoned machine stays registered
    /// and visible, draining, and `Pick` skips it through `SlotLimit::Cordoned`.
    ///
    /// Answered from the `Compile` family because it governs exactly that family's
    /// admission; a node running no worker has nothing to cordon and answers the family's
    /// not-served sentence. The reply is `CordonFields`: the state now, and how many
    /// compiles are still running. Setting the state it already has answers the same.
    Cordon = 0x13,

    /// Read the leader's fleet document once: one section, or every section, for one range.
    ///
    /// **The one-shot twin of subscribing to the fleet**, answered with the text `/fleet.txt`
    /// serves -- the same renderer, reached through one function both doors call -- so a terminal
    /// reads a fleet table from a node with no admin surface at all
    /// ([#1391](https://github.com/LASTRADA-Software/fastcached/issues/1391)). A read rather than a
    /// stream: a subscription taken for one frame and dropped is a stream misused as a query, and it
    /// could carry no range.
    ///
    /// Admitted exactly as the fleet subject of `Subscribe` is -- a member, then the dashboard
    /// credential or this machine only, then leadership -- through one decision both answer from. A
    /// follower refuses `NotLeader` naming the leader, which a client follows. A section or range
    /// this build does not serve is `UnknownFleetSelector`, naming the ones it does.
    ///
    /// `Ok | Error` only, so no version moved: step-over is a request-side property, and an older
    /// node answers this opcode `UnknownOpcode` by name, as `Withdraw`, `NodeStatus` and `Enroll`
    /// were added.
    FleetText = 0x14,

    /// An operator removes one key from the cache tier that answers
    /// ([#1276](https://github.com/LASTRADA-Software/fastcached/issues/1276)).
    ///
    /// The case is a stored value somebody has determined is wrong -- `FASTCACHE_VERIFY`
    /// naming a `WRONG OBJECT` and its key is the ordinary way to learn one -- on a
    /// `fastcache-compile-node`, which speaks no RESP and therefore has no `DEL`.
    ///
    /// **`Miss` is an answer, not a failure.** Dropping a key that is not there is what the
    /// second run of a repair does, and an idempotent operation that errors on its second
    /// run looks broken. `Ok` means this call removed it.
    ///
    /// **It reaches the tier that answers and nothing else.** A node never forwards it to
    /// the shared cache it reads through to: a destructive verb reaches exactly the endpoint
    /// its sender named, and a forward would delete from the whole fleet's cache under the
    /// node's credential on the strength of a local command. So a node that reads through
    /// to an upstream holding the key refills it on the next fetch, and the key must be
    /// dropped there as well.
    ///
    /// Gated as each surface gates a write, which is what a drop is: a node's cache serves
    /// its own machine only (#287), and the daemon asks for the credential `Store` needs.
    ///
    /// `0x13` and `0x14` were held by work on parallel branches when this byte was taken, and have
    /// since landed as `Cordon` and `FleetText`. Taking a byte above a gap was safe only because
    /// `EveryOpcodeIsDistinct` refuses a byte two rows claim.
    CacheDrop = 0x15,

    // The node proof. Two verbs with which a caller establishes, on THIS connection, WHICH
    // machine it is -- by signing with the identity key the cluster admitted -- and after which
    // every frame both ways is sealed under a key the two ends agreed
    // ([#178](https://github.com/LASTRADA-Software/fastcached/issues/178),
    // [#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428)).
    //
    // Their own block and their own family, and `VerbFamily::NodeProof` carries the
    // argument. **Neither is `OpenBeforeAuth`**, which is the one thing about this pair
    // a reader of the enrollment block above will expect and not find: the proof
    // establishes IDENTITY, and the credential gate is a different question that a
    // proving caller has to pass for every verb it came to send anyway.

    /// The caller opens the handshake; the server says who IT is, and states its challenge.
    ///
    /// The request is the caller's nonce and ephemeral X25519 key. The reply is the server's
    /// id, identity key, nonce and ephemeral key, and its signature over all of them and over
    /// the caller's two -- so a caller that holds a roster learns, before it proves anything,
    /// whether it is talking to a voter or to a revoked machine still named in its
    /// `--scheduler`. Both nonces are drawn per connection, which is what makes a captured
    /// handshake useless on a second one -- discovery's rule and the Raft handshake's.
    ///
    /// **Asking twice on one connection re-draws it**, and the old one is gone. That is
    /// the only answer that keeps *a challenge is spent whatever the outcome* true
    /// without a second rule: a caller that asks again has abandoned the first, and a
    /// server holding two live challenges would accept a proof over either.
    NodeChallenge = 0x18,

    /// The caller presents its node id, its identity key and its signature over the handshake.
    ///
    /// `Ok` means this connection has proved WHICH machine it is, for every verb sent on it
    /// afterwards. The answer -- `Ok` or a refusal alike, once the handshake has agreed a key -- is
    /// the first SEALED frame: from it on, every frame both ways carries a `SealedFrameTagBytes`
    /// tag after its payload, under a session key derived from the two ephemeral keys. Sealed
    /// whatever the verdict, so the grammar of the answer never depends on it. The seal is what makes the proof worth having
    /// -- without it a machine that relayed a genuine handshake could inject verbs on the proven connection
    /// (`Distributed::NodeProof` carries the argument). A frame whose tag fails closes the
    /// connection, unanswered.
    ///
    /// **The challenge is spent whatever the outcome.** A refusal decided before any key was
    /// agreed -- a malformed proof -- leaves the connection open, un-proved and unsealed, and
    /// the caller may ask for a new challenge. One decided after -- a bad signature, an unknown
    /// or a revoked key -- is sealed like an acceptance, and a sealed connection proves nothing
    /// further: either verb on it closes it, so that caller redials. Neither gives it a nonce to
    /// grind against.
    ///
    /// Refused `NodeProofUnchallenged` when no challenge is outstanding, `NodeProofRejected`
    /// when the signature does not verify under the key presented, `NodeKeyUnknown` when it
    /// does and this cluster holds no such key for that id, and `NodeKeyRevoked` when the key
    /// is one the cluster REVOKED -- which also marks the connection, so every later verb on it
    /// is refused as the forgotten machine's whatever its address says.
    ProveNode = 0x19,

    /// A node tells the scheduler it EXISTS, whatever components it runs.
    ///
    /// Its own verb rather than a `Register` with no fingerprint, and that is the whole
    /// decision of [#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440).
    /// The registry keys on `(fingerprint, endpoint)`, so a fingerprint-less registration
    /// has no key -- and every consumer of `LiveWorkers()` would then walk an entry with no
    /// toolchain and no slots. That pseudo-worker is exactly the workaround #206 deleted, in
    /// which a FAKE toolchain made a machine a registered worker so it would get a Machine
    /// row; spelling the fake as an absent field rather than a made-up string changes how it
    /// reads and not what it is.
    ///
    /// So presence is keyed by the machine's ENDPOINT and held apart from worker entries. It
    /// carries no slots and no fingerprint, nothing may lease against it, and it adds no
    /// capacity: a machine announcing itself is a row on the page and a place for history to
    /// be filed, never a place to send work.
    ///
    /// **Every node sends it, including one that runs a worker.** A node with `--slots=0`
    /// is the reason it exists, but a verb only the workerless send would be a verb that is
    /// exercised exactly where nobody is looking -- and the history cursor rule (*it advances
    /// only on the verb that carried the batch*) then has two answers depending on a
    /// configuration flag.
    ///
    /// **It once carried a certified roster (#178)**: a voter's endorsement in the request, the
    /// roster a majority endorsed in the `Ok`. Both are gone from the grammar -- every node applies
    /// the state its own consensus replicates and verifies grants against that -- so the request is
    /// the endpoint, the capacity, the load and the join memos, and the `Ok` carries nothing.
    NodeAnnounce = 0x1A,

    /// Ask a node which routes admit or refuse a MACHINE, or the caller's own connection (#1471).
    ///
    /// **Its own verb rather than a field of `NodeStatus`, because it takes an ARGUMENT**: the
    /// subject, a machine id or its identity key's text -- or EMPTY, which asks about the caller's
    /// own connection. Folding it into `NodeStatus` would move that verb out of `FieldlessOps` for
    /// every caller, to carry a field all but one of them would leave empty.
    ///
    /// It reports the fold the SURFACES enforce, `ExplainConnection` through the same
    /// `IMembershipOracle`, or the reported answer and the enforced one can disagree -- which is
    /// the defect class #1471 exists to make visible rather than to add to.
    ExplainAdmission = 0x1B,

    /// Operator admits a member as a LEARNER, or moves one into that set (#1449).
    ///
    /// `ClusterAdmit`'s payload and receipt under a byte of its own: which set consensus
    /// counts a member in must not depend on a FIELD a build may not know. A verb rather than a field, so
    /// `CurrentVersion` does not move -- a build without it refuses the byte by name.
    /// Admitting a voter through it DEMOTES that voter, and `ClusterAdmit` on a learner
    /// promotes it: the seat follows the verb, as `Cluster::MemberSeatTable` says.
    ClusterAdmitLearner = 0x1C,

    // 0x1D is RETIRED and never reused -- it was CLUSTER-ADMIT-WORKER, which admitted a worker
    // PRINCIPAL by key. Principal mode is retired: every machine that joins is a learner member
    // holding a key. See `RetiredOpcodes`.

    /// This machine asks its own node for a machine ticket to one audience.
    ///
    /// Served on loopback only, judged per connection from the kernel's peer address, with no cache
    /// and no locality oracle -- a credential cannot tolerate an oracle's stale direction. A caller
    /// anywhere else is refused `NotAMember` by name.
    /// Answered `Ok` with the ticket's bytes, which the caller carries into AUTH's opaque credential
    /// field. The audience must name one machine, as text: a loopback or wildcard host, or bytes that
    /// are not UTF-8, are refused `MalformedFrame`.
    MintTicket = 0x1E,

    /// Ask a node what fleet it is in: its cluster id, whether anybody but its founder was ever
    /// admitted, when it was created, and where its leader takes enrollment. Pre-auth, because a
    /// joiner asks a SEED before it is anybody's member; the reply is signed by the answering node
    /// over the asker's nonce, so it is fresh and attributable, and it carries nothing secret.
    ///
    /// Its own family, `VerbFamily::Formation`, for `Enroll`'s reason: it is `OpenBeforeAuth`, and
    /// no family that holds a gated verb could take it without changing what that family's gate
    /// answers.
    FleetSummary = 0x1F,

    /// Read a compile result from the FLEET's shared cache. `Fetch`'s payload and replies;
    /// a different verb because a different policy: FETCH reads this machine's private tier and
    /// answers this machine alone (#287), this answers any caller the admission fold admits, and only
    /// on the machine the `shared-cache` setting names. Everywhere else it is `NotSharedCache`.
    SharedFetch = 0x20,
    /// Store a compile result in the fleet's shared cache: `Store`'s payload, `SharedFetch`'s policy.
    SharedStore = 0x21,
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

    /// The server is still working on this request. Payload is empty. **Not** an
    /// outcome: zero or more of these precede exactly one of the three above, and
    /// the request is answered only by that one.
    ///
    /// **The one thing on this wire that is not one-reply-per-request**, and the
    /// exception is bounded to exactly this: a progress frame is never the last frame
    /// of an exchange, so a reader that loops until it sees a terminal status still
    /// consumes precisely one answer per request and pipelining stays well defined.
    ///
    /// **It says nothing about the compile, deliberately**
    /// ([#245](https://github.com/LASTRADA-Software/fastcached/issues/245)). A
    /// liveness signal that carried partial diagnostics would be a second, racy
    /// channel for output the result frame already carries in full — two sources for
    /// one fact, differing by however much of the compile had happened when each was
    /// written. So the payload is empty, and this enumerator's whole meaning is *the
    /// process answering you is still there*.
    ///
    /// A receiver still DRAINS `payloadLength` rather than asserting it is zero, for
    /// the reason every other frame here is drained by its declared length: the
    /// framing is what keeps the link synchronised, and a version that one day says
    /// something in this payload must not desynchronise a reader that ignores it.
    ///
    /// **Why it exists at all.** A dispatched compile is bounded by one flat
    /// deadline, and a flat deadline cannot tell *"still working"* from *"gone"*: it
    /// must be as long as the slowest translation unit anybody compiles (ten minutes
    /// here, derived from `DefaultCompileLeaseTimeout`), which is then also how long
    /// a worker whose process has stopped making progress goes unnoticed. Keepalive
    /// (#247) closes the case where the HOST vanishes — no FIN, no RST, a kernel that
    /// has stopped answering — and cannot reach the case where the kernel answers the
    /// probes perfectly while nothing above it does. Silence is what separates those,
    /// and silence is only measurable against something that would otherwise be said.
    Progress = 0x03,

    /// A frame of a subscription's stream. Payload is `[u8 PushKind][fields]`. **Not** an
    /// outcome: zero or more precede exactly one terminal status, like `Progress`.
    ///
    /// **The second exception to one-reply-per-request, and bounded the same way**: legal on
    /// `Op::Subscribe` alone, `static_assert`ed by `PushIsSubscribeOnly`, never the last frame
    /// of an exchange. It is a separate status rather than a `Progress` with a payload because
    /// `Progress` is defined as carrying NOTHING -- a reader that ignores its payload is correct
    /// by contract -- and a stream's frames are the answer itself.
    Push = 0x04,
};

/// Whether @p raw is a status byte this build understands.
///
/// A table walk rather than a chain of `!=` comparisons: the chain is what a fourth
/// status is forgotten from, and it was — the shape it had before this one was added
/// named its three members inline, so nothing but a reader could notice an omission.
/// @param raw The first reply byte, as received.
/// @return True when it names a `Status`.
[[nodiscard]] constexpr bool IsKnownStatus(std::uint8_t raw) noexcept
{
    constexpr std::array Known { Status::Miss, Status::Ok, Status::Error, Status::Progress, Status::Push };
    return std::ranges::any_of(Known, [raw](Status status) { return static_cast<std::uint8_t>(status) == raw; });
}

/// Whether @p status ends an exchange, as against reporting on one still running.
///
/// **The question every reply reader actually asks**, and it is here rather than at
/// each of them because there are four such readers in this tree — the launcher's
/// cache and worker exchanges, the admin CLI, and the protocol test client — and a
/// reader that does not ask it treats the first progress frame as the answer.
/// @param status The reply status.
/// @return True for `Miss`, `Ok` and `Error`; false for `Progress` and `Push`.
[[nodiscard]] constexpr bool IsTerminalStatus(Status status) noexcept
{
    return status != Status::Progress && status != Status::Push;
}

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
    /// STORE payload is not a compile value AT ALL -- it does not decode.
    ///
    /// Narrower than "this build cannot decode it", which would also cover a value
    /// of a generation this build does not implement: that is
    /// `ForeignValueGeneration`, and keeping the two apart is the whole of #544.
    MalformedValue = 0x05,

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
    /// The worker cannot CLASSIFY the program its own `--toolchain` names, so it
    /// cannot build a command line for it. Split from `WorkerSpawnFailed` because
    /// the two send an operator to different places: that one says the program
    /// could not be run (missing, unreadable, wrong architecture) and this one says
    /// it ran fine and this build does not recognise what it is. Numbered 0x21
    /// rather than 0x12 because a code's VALUE is a wire contract and the low bytes
    /// are long since spent; the enum is not dense and does not need to be.
    WorkerCompilerUnclassified = 0x21,
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

    /// One membership change is already uncommitted, so this one must wait.
    ///
    /// Emphatically NOT `InvalidClusterChange`, and the split is #196: that code
    /// says the cluster could never accept what was asked, and this one says it
    /// could and will, shortly. Raft admits one configuration change at a time --
    /// a second built on a configuration a truncation can still roll back would
    /// have its safety argument made against a set that never existed -- so this
    /// is what a healthy cluster answers while a change it already accepted is
    /// replicating. Under the wrong code an operator is sent to correct a record
    /// that is already correct, once per reconcile interval.
    ///
    /// Retriable, and the ONE code in this range that is (`ErrorDescriptor::retry`):
    /// everything else here is a refusal the caller stops asking about.
    ClusterChangeInFlight = 0x1F,

    /// The change asked for is already in force, so nothing was recorded.
    ///
    /// Not a failure at either end. `--cluster-admit` naming a member the cluster
    /// already has is idempotence rather than a mistake, and a reconciler
    /// proposing a quorum equal to the current one has nothing to do -- so the
    /// answer an operator needs is "already", never "invalid". It is a distinct
    /// code rather than a success because no entry was appended and there is
    /// therefore no index to name; `ConsensusErrorCode::MembershipUnchanged`
    /// carries the same argument at the layer that decides it.
    ClusterChangeNotNeeded = 0x20,

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
    /// Its signature does not verify under the key the worker's roster holds for the
    /// voter it names, that key is revoked, or it is not a lease token at all -- and
    /// those are deliberately ONE code, because from the receiving
    /// end they are indistinguishable: a forged token and a random string differ
    /// only in how much effort went into them. It is also what an old launcher's
    /// bare serial arrives as, which is the right answer for it too: the client
    /// answers every refusal here by compiling locally, so a mixed-version fleet
    /// degrades to local compiles rather than to a broken build.
    ///
    /// Emphatically NOT `UnknownLease`, which is the scheduler saying it has no
    /// record of a token it may well have issued -- an expired or already-resolved
    /// lease, a fleet condition. This one says the token was never issued by
    /// any voter the worker trusts, which is a security event and reads as one.
    ///
    /// Nothing the token CLAIMS is reported alongside it. A caller that cannot
    /// authenticate a token has established no fact about it, and echoing its
    /// contents back would turn the refusal into an oracle.
    LeaseUnauthorized = 0x19,

    /// An authentic lease, presented to a worker it was not issued for.
    ///
    /// Only ever reported once the signature has verified, which is what makes it a
    /// diagnostic rather than a hint to an attacker: the endpoint the scheduler
    /// granted is stated in the message, because the overwhelmingly common cause is
    /// not a replay at all but a worker whose advertised endpoint and actual one
    /// disagree -- a NAT, or a hostname registered where clients resolve an address.
    ///
    /// The replay it also closes is the reason the endpoint is inside the signed claims:
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

    /// This worker has not finished identifying its toolchains yet.
    ///
    /// Distinct from `FingerprintMismatch`, and the distinction is the whole point.
    /// Both mean "not compiled here", but they send an operator to different places:
    /// a mismatch says this worker serves a different toolchain and the scheduler
    /// should not have chosen it, while this says the worker is coming up and the
    /// same request will succeed shortly. Collapsed, a node restart reads as a fleet
    /// full of machines on the wrong compiler.
    ///
    /// A node serves its cache tier and its admin surface while it walks its include
    /// trees -- measured at over 300 s on a cold Windows runner (#354) -- and it
    /// registers **nothing** until the walk finishes, because a provisional
    /// fingerprint is a machine advertising a value no other machine will compute
    /// (#225). So a client reaches this only by dialling the compile port directly;
    /// through the scheduler it is simply not offered this worker yet.
    ///
    /// Transient by construction: the walk either completes or the node exits.
    WorkerToolchainSurveyInFlight = 0x1C,

    /// The request outran the window the surface allows for answering it.
    ///
    /// The server gave up, and it says so rather than hanging up. Until #523 this
    /// was a bare TCP close, which a peer cannot tell from a crash, a network drop
    /// or a refusal it never received -- three causes with three different remedies,
    /// arriving as one silence.
    ///
    /// **Emphatically NOT `EndpointBusy`.** That one says this node is momentarily
    /// full and the same request will very likely be served in a moment, so a client
    /// retries and an operator raises a local bound. This one says the request was
    /// admitted, worked on, and abandoned on time -- for a compile, a translation
    /// unit that outlived its lease grant, which is a question about the timeout and
    /// never about this worker's speed. An operator does opposite things about them,
    /// which is what separates every code in this range from its neighbours.
    ///
    /// A refusal a client answers by compiling locally, like the rest of this range.
    /// Deployed launchers need no new arm to do that: they special-case `NotLeader`
    /// and `UnknownLease` and treat every other code as a generic rejection, so this
    /// is additive for clients built before it existed.
    ///
    /// **Only a peer parked in the SURFACE can receive it.** A peer swept while the
    /// connection is parked on the socket -- dribbling a header, dribbling a declared
    /// payload -- is still ended by the close, because the close is the only thing
    /// that ends a parked read and it is the write side gone. See
    /// `FrameServer::CloseOverdue`.
    RequestDeadlineExceeded = 0x1D,

    /// The value a STORE carried is a compile value of a **generation this build
    /// does not implement**.
    ///
    /// **Emphatically NOT `MalformedValue`**, and the split is the one
    /// `.agent/rules/storage.md` already draws on disk between
    /// `UnsupportedFormatVersion` and `Corrupt`. `MalformedValue` says the bytes are
    /// not a compile value at all, which is a statement about a broken or mismatched
    /// CLIENT. This one says the bytes *are* a compile value, well formed, written
    /// under a canonicalization spec that is not this server's -- which during a
    /// rolling upgrade is a **normal and expected** condition and says nothing is
    /// wrong with anybody.
    ///
    /// **Either direction**, and the older one is the case actually in the field.
    /// `DecodeCompileValue` refuses any `version != CompileValueVersion`, so this
    /// fires for a producer behind this server exactly as for one ahead of it; #547
    /// bumped `CompileValueVersion` to 2, so what a gen-2 server meets is gen-1
    /// launchers. An operator sent looking for a *newer* machine would be hunting the
    /// wrong half of the fleet, which is why the message states both numbers rather
    /// than a direction.
    ///
    /// The code is what monitoring sees, and the remedies diverge: a rise in
    /// malformed values sends an operator looking for a broken client, or -- worse,
    /// and the reason the disk rule exists -- to wipe a cache that is perfectly
    /// healthy. The remedy here is to finish the rollout or roll it back. A fleet is
    /// permanently mid-upgrade ([#173](https://github.com/LASTRADA-Software/fastcached/issues/173)),
    /// so this is not an exotic state.
    ///
    /// Refused rather than stored, and neither server on this wire has a choice about
    /// that: storing it would mean storing text this build cannot rewrite -- the
    /// producing checkout's absolute paths, under a key every machine computes, which
    /// is exactly what `CanonicalStoredValue` exists to prevent
    /// ([#483](https://github.com/LASTRADA-Software/fastcached/issues/483)).
    ///
    /// The two surfaces answering it keep **separate counters**, because a shared code
    /// is not a shared event: `SurfaceRefusal`'s row is the refusal, not the code.
    ///
    /// A refusal a client answers by compiling locally, like the rest of this range.
    /// Deployed launchers need no new arm: they special-case `NotLeader` and
    /// `UnknownLease` and treat every other code as a generic rejection, so this is
    /// additive for clients built before it existed -- which is the whole point, since
    /// the clients that meet it are by definition of another generation.
    ForeignValueGeneration = 0x1E,

    // 0x22 was `EnrollmentClosed`. Since zero-config formation a joiner's request is always
    // recorded and the approval is the gate; #178 already made the window guard no secret. The
    // byte is RETIRED and never reused -- see `RetiredErrorCodes`.

    /// The pending list is full, so this request was not recorded.
    ///
    /// A refusal rather than an eviction, and that is the decision the bound exists to
    /// express. Recording is ungated by design -- the explicit approve is the gate --
    /// so anybody who can reach the port can fill the list; evicting to make room would
    /// let a flooder push the real joiner off the list the operator is reading, which
    /// is silent and undetectable from either end. A full list is visible, and this
    /// code plus its counter is how.
    ///
    /// Distinct from `EndpointBusy`, which says this node is momentarily out of bytes
    /// or slots and the same request will very likely be served in a moment. This one
    /// will go on being answered until a person decides something.
    EnrollmentFull = 0x23,

    // 0x24 was `EnrollmentAlreadyCollected`, the refusal that made a key hand-over spendable
    // once. An approved enrollment no longer carries a secret (#178), so there is nothing to
    // spend; the byte is RETIRED and never reused, because a reader that still names it would
    // read a new code under it as a spent grant -- see `RetiredErrorCodes`.

    /// A fleet read named a section or a range this build does not serve.
    ///
    /// **Its own code, and neither `MalformedFrame` nor `InvalidClusterChange`.** The frame was
    /// well-formed and nothing about the cluster was asked to change: the words were a key this
    /// build's tables do not hold, which is a client of another build or an operator's typo. The
    /// message lists the keys this build serves, so the answer is actionable where it is read, and a
    /// client concludes it as a usage mistake rather than as a peer speaking another protocol.
    ///
    /// Uncounted where it is answered: a typo is seen by whoever typed it, and a rise would mean
    /// nothing an operator acts on.
    UnknownFleetSelector = 0x25,

    /// A node proof arrived with no challenge outstanding on this connection.
    ///
    /// Never asked for one, or already spent one -- a challenge answers exactly one proof
    /// whatever that proof's outcome, so a second `ProveNode` after any first one lands here.
    /// **Its own code rather than `NodeProofRejected`**, because the two remedies are opposite:
    /// this says the exchange was got wrong and the caller should ask for a challenge, while a
    /// rejection says the key does not match and no amount of retrying will help. Answering one
    /// for the other sends an operator to check a key that is fine, or leaves a wrong key
    /// looking like a protocol bug.
    NodeProofUnchallenged = 0x26,

    /// A node proof's signature did not verify under the identity key it presented -- or an
    /// `Enroll`'s, under the key it asks to be admitted with (`Cluster::VerifyEnrollRequest`).
    ///
    /// A signature over a different challenge, a different id or a different handshake, or bytes
    /// that are no signature at all -- one answer, deliberately: the verification cannot tell them
    /// apart, and a refusal naming which field was wrong would be an oracle. Asked BEFORE the
    /// roster is consulted, so a caller who cannot sign learns nothing about which ids and keys
    /// this cluster holds.
    ///
    /// Not a diagnosis of the caller's whole posture: a connection that fails this is still
    /// admitted or refused by its ADDRESS exactly as it would have been without ever proving
    /// anything -- which, since #178, admits it to nothing a JOINING machine sends
    /// (`OpDescriptor::identity`).
    NodeProofRejected = 0x27,

    // 0x28 is RETIRED and never reused -- it was `RosterExpired`, named for the certified
    // roster's lapse (#178); `GrantUnverifiable` (0x31) carries what it still meant. See
    // `RetiredErrorCodes`.

    /// A node proof verified under a key this cluster does not hold for the id it named (#178).
    ///
    /// A machine never admitted -- one nobody enrolled -- or
    /// one presenting a key other than the one admitted under its id. **Not `NodeProofRejected`**,
    /// because the remedy is the operator's rather than the caller's: the signature is sound and
    /// the machine is who it says it is, so what is missing is an admission, and a refusal naming
    /// a bad signature would send somebody to regenerate a key that is fine.
    NodeKeyUnknown = 0x29,

    /// A node proof verified under a key this cluster has REVOKED (#178).
    ///
    /// The forgotten machine itself, still holding every byte it ever held. Named rather than
    /// folded into `NodeKeyUnknown` because it is the one outcome an operator must be able to
    /// SEE -- a removed machine still dialling in -- and because it is not merely a refusal: the
    /// connection is kept and marked, and every later verb on it is refused as the forgotten
    /// machine's, from any address, this machine's included.
    NodeKeyRevoked = 0x2A,

    /// A verb only a machine that PROVED its identity may send, sent on a connection that has not
    /// (#178).
    ///
    /// `Register`, `NodeAnnounce`, `Heartbeat` and `Withdraw`: the verbs with which a machine
    /// joins the fleet, and which decide where the fleet's work is sent. An address admits a
    /// CLIENT; it no longer admits a machine into the fleet, loopback included -- a process on the
    /// scheduler's own host could otherwise register an endpoint of its choosing and be leased the
    /// fleet's jobs. `OpDescriptor::identity` is the table the refusal is decided from.
    NodeIdentityRequired = 0x2B,

    /// This source host already has as many enrollment requests waiting as one host may hold.
    ///
    /// **Its own code rather than `EnrollmentFull`**, because the two send an operator to
    /// different places: a full list is many machines waiting for a decision, and this is ONE
    /// address asking for more rows than a handful of machines behind it would -- a NAT or a VM
    /// host at worst, and a flood at best. A joiner reads it as the same wait: its row is
    /// recorded once one of its host's rows is decided.
    EnrollmentHostFull = 0x2C,

    /// An `AUTH` presenting a machine ticket that this node did not accept.
    ///
    /// One code for every way a ticket is refused -- a signature that does not verify, a machine
    /// the roster does not hold, a revoked key, an audience that is not this node, an expiry
    /// passed -- with the reason named in the message, since the remedy differs and the caller
    /// cannot tell them apart otherwise. **Not `Unauthenticated`**, which says a credential is
    /// still owed: this one says the credential was presented and judged.
    TicketRefused = 0x2D,

    /// An operator's CONTROL verb -- a cluster admission, forget or setting, or an enrollment
    /// decision -- from a caller no route IDENTIFIES: admitted by `--fleet-open` alone.
    ///
    /// `--fleet-open` admits a caller to what the fleet SERVES -- a lease, a status, a cache
    /// request -- and never to what decides the fleet: on an open node an anonymous remote caller
    /// is a member, and without this it could approve its own enrollment or arm an auto-approve
    /// window. Loopback, a proven node key or a verified machine ticket satisfy it
    /// (`IdentityRequirement::IdentifiedCaller`). Its own code rather than `NotAMember`, because
    /// the caller IS admitted and the remedy is to identify itself, not to be admitted.
    IdentifiedCallerRequired = 0x2E,

    /// This machine is not the fleet's shared cache right now: the `shared-cache` setting names
    /// another machine or none, or names this one and its tier could not be opened.
    ///
    /// Its own code rather than `UnimplementedVerb` -- every node implements the family -- and
    /// rather than `NotAMember`, which would tell a proven member it is not one. A client treats
    /// it as a miss.
    NotSharedCache = 0x2F,

    /// A worker will not pass one of the job's arguments to its compiler: the argument is not on
    /// its per-driver-family allowlist, or a path-mapping value it was sent cannot be spelled into
    /// a rule. The message names the argument.
    ///
    /// **Its own code rather than `MalformedFrame`, which is what it was answered with until a
    /// cl-debug build met it**: the frame was perfectly formed, and a launcher reading
    /// `malformed-frame` reported every such refusal as *this launcher and the fleet disagree
    /// about the wire* -- a version skew, on a fleet where both ends were one build. The remedy is
    /// the opposite of an upgrade: a flag this fleet does not dispatch, which an operator either
    /// adds with `--allow-compile-arg` on the workers or leaves to compile locally.
    ///
    /// A new byte and no version bump, because the reply GRAMMAR is unchanged -- a status, a code
    /// and a message -- and a launcher that predates it reads it as a refusal it does not know
    /// (`DeclineCause::Unrecognised`), which is true, rather than as anything it would act on
    /// wrongly.
    WorkerRejectedArgument = 0x30,

    /// This worker cannot verify ANY grant right now (#178): the state it applied records no
    /// voter's key yet, or no leader its applied configuration counts has spoken to it for longer
    /// than `Distributed::LeaderSilenceBound`.
    ///
    /// A statement about the WORKER, never about the grant, which is why it is not
    /// `LeaseUnauthorized` or `LeaseExpired`: a client told its lease was bad would ask the same
    /// scheduler for another and be refused again, while this says the machine it was sent to
    /// cannot vouch for anybody's grant until it hears from its fleet. What `RosterExpired`
    /// (0x28, retired) meant once the certified roster it was named for was gone.
    ///
    /// 0x31 rather than the next number on this branch: 0x2D-0x30 are allocated to other work
    /// (`TicketRefused`, `IdentifiedCallerRequired`, `NotSharedCache`, `WorkerRejectedArgument`).
    GrantUnverifiable = 0x31,

    /// A node proof this node cannot judge YET: its consensus has not applied the log it recovered at
    /// start, so the key roster it judges by may lack a key its cluster holds (batch 3's M3).
    ///
    /// Answered in place of `NodeKeyUnknown` and only while that lasts -- a lone voter before its
    /// election commits, a follower before its leader's first `AppendEntries` -- because
    /// `NodeKeyUnknown` there is a confident wrong signal: it tells an operator to ADMIT a machine
    /// the cluster already holds, and the prover waits a whole announce interval to try again.
    /// A statement about the ANSWERING node, never about the caller, so it is retriable: the prover
    /// asks again on a short bounded backoff (`Node::DeferredProofWait`).
    RosterNotYetApplied = 0x32,

    /// An operator's CONTROL verb from a caller that is IDENTIFIED -- a proven node key or a verified
    /// machine ticket -- whose machine holds no voter's seat in the state this node applied.
    ///
    /// A machine ticket proves a fleet MACHINE, never an operator: any process on a machine the
    /// fleet admitted can have its node mint one, so a ticket that opened the control verbs made
    /// every process on every laptop an operator of the whole fleet. What decides the fleet is
    /// answered to this machine (loopback) and to a VOTER's identity alone
    /// (`IdentityRequirement::OperatorStanding`). Its own code rather than `IdentifiedCallerRequired`,
    /// because the caller IS identified and the remedy differs: run the verb on a voter, or have an
    /// operator promote this machine.
    ///
    /// 0x33 rather than the next free byte here because 0x32 is `RosterNotYetApplied`, which the
    /// batch this change lands beside assigns; two builds that each read 0x32 their own way would
    /// disagree about a refusal while both looked correct.
    OperatorStandingRequired = 0x33,
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

/// The largest reply a verb may be ANSWERED with, stated rather than defaulted.
///
/// `PayloadCap`'s mirror on the other direction, and for the reverse population: a request is read
/// by a server from whoever connected, a reply by a client from whoever it dialled -- and a client
/// that dials a seed a DNS answer named, or anything answering at that address, is reading from a
/// stranger just as surely. A reply header declares its length in a `u32`, and a reader that sizes
/// its buffer by that before a byte of the payload arrives lets five bytes take four GiB. So a
/// reader refuses a declared length above its verb's ceiling BEFORE it allocates
/// (`Cc::RecvReply`), and the ceiling is a column so that no verb can be added without one: the
/// default constructor is deleted, as `PayloadCap`'s is.
class ReplyCap
{
  public:
    /// Deleted on purpose: a row must state its reply ceiling. See the class comment.
    ReplyCap() = delete;

    /// @param bytes The ceiling, or 0 for "the frame's own length governs".
    constexpr explicit ReplyCap(std::size_t bytes) noexcept:
        _bytes { bytes }
    {
    }

    /// @return The declared ceiling in bytes; 0 means the frame's own length governs.
    [[nodiscard]] constexpr std::size_t Bytes() const noexcept
    {
        return _bytes;
    }

    /// @return True when this verb's replies declare a bound of their own.
    [[nodiscard]] constexpr bool IsBounded() const noexcept
    {
        return _bytes != 0;
    }

  private:
    std::size_t _bytes;
};

/// This verb's reply carries a build artefact -- a FETCH hit, a COMPILE result -- whose size is the
/// object file's, so nothing below the frame's own length bounds it.
///
/// Legitimate for exactly those two, and asked of a server the caller chose and, for both, one it
/// authenticated to: never for a verb reachable before authentication, which
/// `PreAuthRepliesAreBounded` refuses.
inline constexpr ReplyCap ReplyCarriesArtefact { 0 };

/// This verb's replies are bounded.
/// @param bytes The ceiling.
/// @return The cap to put in the row.
[[nodiscard]] constexpr ReplyCap ReplyBoundedTo(std::size_t bytes) noexcept
{
    return ReplyCap { bytes };
}

/// Who may send a verb: any caller its surface admits, only a caller a route IDENTIFIES, or only a
/// machine that PROVED its identity on this connection (#178).
///
/// **PRIVATE: in-process only**, read off `OpTable` at each end, so only the zero value is spelled.
///
/// A COLUMN rather than a list at the one site that enforces it, for `PreAuth`'s reason: which
/// verbs a joining machine owns is a property of the verb, and a list kept beside the scheduler's
/// gate is one a new verb joins by being remembered. `JoiningVerbsNeedAnIdentity` below pins the set.
enum class IdentityRequirement : std::uint8_t
{
    /// Any caller the surface admits -- by address, by a credential, or by a proof.
    AddressAdmits = 0,

    /// Only a connection whose proved identity key is live in the cluster's roster: the verbs a
    /// machine JOINS the fleet with, which decide where the fleet's work is sent. Loopback is not
    /// exempt -- `ErrorCode::NodeIdentityRequired` says why.
    ProvenNodeOnly,

    /// Only a caller admitted by a route that IDENTIFIES it -- this machine, a proven node key or
    /// a verified machine ticket -- and never by `--fleet-open` alone. No verb names it any more: it
    /// is `OperatorStanding`'s PREREQUISITE, asked first so a caller `--fleet-open` alone admitted is
    /// still told that (`ErrorCode::IdentifiedCallerRequired`) rather than to go and find a voter.
    /// Which routes identify is a column of the route table (`Distributed::MembershipRoutes`).
    IdentifiedCaller,

    /// Only a caller with an operator's STANDING: this machine (loopback), or an identified machine
    /// -- a proven node key or a verified ticket -- whose seat in the state the asked node applied
    /// is a voter's. An operator's CONTROL verbs, which change who is in the fleet or how it is run.
    /// A ticket proves a MACHINE, not an operator, so a learner's is refused
    /// (`ErrorCode::OperatorStandingRequired`). Which routes may confer it is the route table's
    /// `standing` column (`Distributed::MembershipRoutes`).
    OperatorStanding,

    /// The count, for a table over this enum.
    Last,
};

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
///
/// **Private: in-process only.** A verb's family is read off `OpTable` at each end and is
/// never transmitted or persisted, so the enumerator order is free and appending a family
/// shifts nothing on the wire. The one explicit value is `Unset`'s zero, which is
/// load-bearing for the reason stated on it; no other enumerator carries one.
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
    /// Reports what this node IS, rather than spending anything it holds.
    ///
    /// Its own family rather than a corner of `Scheduler`, because every other family
    /// names a COMPONENT a node may or may not run, and these verbs must not depend on
    /// WHICH of them it runs: a worker with no cache tier and no scheduler is an
    /// ordinary deployment, and *what do you serve?* has to work there. Filing them
    /// under a component would make the answer depend on that component being present.
    ///
    /// Not *answerable by a node running nothing* -- `StartNodeSurfaceOrExplain` serves
    /// no port at all in that case, so it is unreachable and claiming it would be a
    /// reason that generalises past the fact it was drawn from.
    Node,

    /// Lets a machine the cluster has never heard of ask to be admitted under the key it minted.
    ///
    /// **Its own family because one of its two verbs is `OpenBeforeAuth`, and no other
    /// family could hold that without changing what it means.** The distributed-execution
    /// block states in its section comment that none of it is pre-auth; `Scheduler` is
    /// most of that block. A component answers for a family -- `MergedResponder::OwnerOf`
    /// routes by it and asks that component whether the peer is admitted and whether a
    /// credential is required -- so folding `Enroll` into `Scheduler` would put a verb
    /// that must admit strangers behind the answer that refuses them, or relax that
    /// answer for the nine verbs beside it.
    ///
    /// Separated, the whole surface is additive: an `EnrollmentResponder` that does not
    /// consult membership for `Enroll` opens exactly one door and leaves every existing
    /// gate answering what it answered before, which is checkable rather than argued.
    Enrollment,

    /// Streams what a live-stats panel draws.
    ///
    /// Its own family because its one verb is answered by a stream, which no component that
    /// answers request/reply verbs owns: a node routes it to the live hub, and a component
    /// routing by family must not be handed a verb whose answer never returns.
    Live,

    /// Reads the leader's fleet document once.
    ///
    /// Its own family rather than a corner of `Live`, whose one verb is a stream, or of
    /// `Scheduler`, whose gate is membership: a fleet read is
    /// answered by a component holding the dashboard credential, which is a different secret, and
    /// with a reply rather than a stream. A node running no scheduler still owns the family and
    /// says the fleet is served elsewhere, as the fleet subject of a subscription does.
    Fleet,

    /// Establishes, for this CONNECTION, WHICH machine the caller is: its node id, proved by a
    /// signature under the identity key the cluster admitted, and a session key both ends then seal
    /// every frame under (#178).
    ///
    /// ## Its own family, and not one of the two that look closer
    ///
    /// **Not `Session`**, although that family's own sentence -- *establishes a credential for
    /// the connection* -- describes this pair exactly. On a node `Session` carries a TICKET, a
    /// statement another machine made about the caller; a proof is the caller's OWN signature over
    /// this connection. Folding them would let one stand in for the other.
    ///
    /// **Not `Scheduler`**, for the mirror reason: a row there would be gated by the very
    /// admission answer this pair exists to establish, so the verb would be refused to exactly the
    /// caller it was written for.
    ///
    /// ## Neither verb is pre-auth, and that is a decision
    ///
    /// The obvious reading of `Enrollment` -- *a verb that admits strangers must be
    /// `OpenBeforeAuth`* -- does not transfer, because the two verbs admit strangers to
    /// different gates. `Enroll` exists for a machine the cluster has not admitted yet, so a
    /// credential gate there refuses the whole population. A node proving its identity is the
    /// opposite case: it came to send `Register`, which requires the credential when one is
    /// configured, so a caller that cannot pass that gate gains nothing from being proved and a
    /// caller that can passes it first. Nobody is refused by requiring it.
    ///
    /// What pre-auth would cost is real and one-sided: an unauthenticated stranger could make
    /// this node draw an ephemeral key and compute a signature per frame, and
    /// `PreAuthVerbsAreBounded` would have two more rows to be right about. So the answer is
    /// `RequiresAuth` on both.
    ///
    /// ## Legitimately absent
    ///
    /// A proof is checked against the cluster's applied roster, which only a node running
    /// consensus holds, so any other node runs no component for this family and refuses it
    /// `NoCluster` -- never `UnimplementedVerb`, which a caller reads as *this node's build is too
    /// old* and acts on by upgrading a machine that is already current.
    NodeProof,

    /// Says which fleet this node is in, to a machine that is nobody's member yet (zero-config
    /// formation).
    ///
    /// Its own family for `Enrollment`'s reason: its one verb is `OpenBeforeAuth`, and a component
    /// answers for a family -- whether the peer is admitted, whether a credential is required -- so
    /// folding it into a gated family would either put a verb that must answer strangers behind
    /// the answer that refuses them, or relax that answer for every verb beside it. Meant for every
    /// built node, whatever components it runs, since a seed is asked which fleet it is in before
    /// anybody knows what it serves; while no component owns it, the merged listener refuses it as
    /// it refuses every family nobody answers (`FamilyRoutes`).
    Formation,

    /// The fleet's shared cache: its own family so `MergedResponder` routes it to the
    /// component every node builds -- dormant unless the applied state names this machine -- and never
    /// to the private tier, whose verbs are `Cache`.
    SharedCache,

    /// The count, not a family: what sizes a table with one row per family
    /// (`Core/EnumTable.hpp`), so appending a family fails the build of every such table
    /// until it has a row. Never in `OpTable`, which `EveryVerbHasAFamily` would not catch
    /// by itself, so `NoVerbIsInTheCountFamily` does.
    Last,
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

    /// Largest reply this verb may be answered with, whatever its status; see `ReplyCap`.
    ///
    /// `ReplyCarriesArtefact` or `ReplyBoundedTo(n)`, never omitted. A verb reachable before
    /// authentication MUST declare a real bound (`PreAuthRepliesAreBounded`): its client is the
    /// one most likely to be talking to a stranger.
    ReplyCap maxReply;

    /// Which verb family this belongs to; never `Unset`.
    ///
    /// Read by a merged listener to pick the component that answers, admits, gates
    /// and counts for this verb. `EveryVerbHasAFamily` refuses a row that omits it.
    VerbFamily family;

    /// Who may send it: any admitted caller, or only a machine that proved its identity on this
    /// connection. Stated on every row, never defaulted, for `preAuth`'s reason.
    IdentityRequirement identity;
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

/// Payload ceiling for AUTH: a kind byte and a username and a shared secret, or
/// a machine ticket, and nothing that grows with a build artefact. Generous by
/// orders of magnitude against any real credential, and small enough that a peer
/// which has proved nothing cannot use it to make the server take memory.
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

/// Payload ceiling for `Enroll`, the one verb here that is both pre-auth and not the
/// credential handshake.
///
/// **Its own constant, tighter than `MaxControlPayload`, because the populations
/// differ and nothing else records that.** A control verb is bounded against a peer
/// that authenticated once; this is bounded against anybody who can route to the port
/// while an operator has a window open, which is the whole point of the verb. The
/// request is an identity and a `host:port` -- two length-prefixed fields whose
/// contents are both bounded by what a human types -- so 1 KiB is generous by an
/// order of magnitude against any real one and is 64 times less memory an
/// unauthenticated peer can make this process take per frame.
///
/// Reusing `MaxControlPayload` would compile, pass `PreAuthVerbsAreBounded`, and
/// quietly state that a stranger and a member may ask this surface for the same
/// allocation. Reusing `MaxAuthPayload` would be the same value by coincidence of
/// magnitude and would couple two ceilings that answer to different verbs, so the next
/// person retuning AUTH's would retune this one without knowing.
inline constexpr std::size_t MaxEnrollPayload = 1024;

// The relation is asserted rather than left to whoever next retunes one of them: the
// whole argument above is that a pre-auth verb is bounded more tightly than a gated
// one, and that argument stops being true silently the moment either number moves past
// the other. `EnrollControl` is gated and takes the control cap like every other
// operator verb, so this says what it says about `Enroll` alone.
static_assert(MaxEnrollPayload < MaxControlPayload,
              "a pre-auth verb must be bounded more tightly than one a member had to authenticate to reach");

/// Bytes in a node's identity key as every verb carrying one spells it: an Ed25519 public key
/// (#178).
///
/// Spelled here rather than taken from `Core/Ed25519.hpp`, which this header may not include --
/// it is dependency-free by rule, and the launcher compiles it without the library. The node
/// `static_assert`s the two equal where it fills the field, so they cannot drift apart silently.
inline constexpr std::size_t IdentityPublicKeyBytes = 32;

/// How many bytes a node-handshake nonce carries on the wire.
///
/// Spelled HERE rather than included from `Core/Nonce.hpp`, because this header is what the
/// launcher compiles in and stays free of everything but the three leaf headers above it. Every
/// width in this block is pinned to the type that fills it with a `static_assert` in
/// `Distributed/NodeProof.hpp`, the one place both are visible -- so a nonce that grew and a wire
/// field that did not is a BUILD failure rather than a field silently truncated, which would make
/// both ends sign different inputs and report a forgery for a version mismatch.
inline constexpr std::size_t NodeChallengeBytes = 32;

/// How many bytes a node-handshake ephemeral key carries: one X25519 public key.
inline constexpr std::size_t NodeEphemeralKeyBytes = 32;

/// How many bytes a node-handshake signature carries: one Ed25519 signature.
inline constexpr std::size_t NodeSignatureBytes = 64;

/// Payload ceiling for `FleetSummary`: one nonce and its length prefix.
///
/// Its own constant, and the tightest on the table, for `MaxEnrollPayload`'s reason: the verb is
/// pre-auth, so this bounds what anybody who can route to the port may make the node read per
/// frame, and the request is one fixed-width field with nothing a person types in it.
inline constexpr std::size_t MaxFleetSummaryPayload = 64;

// The one field has to FIT, or the verb's own cap refuses every well-formed request and the
// refusal names a payload ceiling rather than the encoding that cannot meet it.
static_assert(MaxFleetSummaryPayload >= NodeChallengeBytes + WireFields::FieldPrefixSize,
              "the fleet summary ceiling must hold one nonce and its length prefix");
static_assert(MaxFleetSummaryPayload < MaxEnrollPayload,
              "a request of one fixed-width field must be bounded more tightly than one carrying typed text");

/// How many bytes the tag after every sealed frame carries: one HMAC-SHA256 output.
///
/// NOT counted in the frame's declared length. The header still states the payload alone, so a
/// reader that has not engaged the seal would read a sealed frame as a frame followed by 32 bytes
/// of garbage and close -- which is the right failure for a peer that proved nothing.
inline constexpr std::size_t SealedFrameTagBytes = 32;

/// Payload ceiling for `NodeChallenge` and `ProveNode`.
///
/// Both requests are fixed-width fields plus, for the proof, a node id a `--cluster-dir` minted or
/// an operator typed. Their own constant rather than `MaxControlPayload` for `MaxEnrollPayload`'s
/// reason -- the populations differ, and nothing else in the table records that these verbs are
/// reached by a caller whose identity has not been established yet, even though its credential
/// has.
///
/// 512 rather than the hundred-odd bytes a real proof measures: the point of the bound is that it
/// cannot scale with anything, not that it is snug, and a ceiling somebody has to raise for a
/// longer id is a ceiling that gets raised to the control cap.
inline constexpr std::size_t MaxNodeProofPayload = 512;

// The fixed part has to FIT, or the verb's own cap refuses every well-formed request and the
// refusal names a payload ceiling rather than the encoding that cannot meet it. Three 4-byte field
// prefixes (`WireFields`), the key and the signature, and room left over for the id.
static_assert(MaxNodeProofPayload > (3 * sizeof(std::uint32_t)) + IdentityPublicKeyBytes + NodeSignatureBytes,
              "MaxNodeProofPayload must leave room for the key, the signature, three field prefixes and an id");

/// Payload ceiling for `MintTicket`.
///
/// One endpoint, as an operator would spell it or a config file would carry it -- the same
/// population `MaxNodeProofPayload`'s neighbours bound, and its own constant for the reason
/// theirs are: this verb is reached over loopback by whatever asked for a ticket, not by a
/// caller that has proved a fleet identity, so nothing about that population licenses reusing
/// `MaxControlPayload`.
inline constexpr std::size_t MaxMintTicketPayload = 512;

/// Payload ceiling for `ExplainAdmission`.
///
/// One subject: a machine id or a 43-character identity key, or nothing. Its own constant for
/// `MaxMintTicketPayload`'s reason, and a small one because this verb's self form is answered to
/// a caller the node REFUSES (see its `OpTable` row), so this ceiling is the only thing bounding
/// what a stranger makes the node read.
inline constexpr std::size_t MaxExplainAdmissionPayload = 512;

/// Payload ceiling for a reply read on a SEALED connection by the side that proved.
///
/// The verbs a joining machine sends are answered from tables -- a worker id, a roster -- so this
/// bounds how much a sealing reader buffers before it can check a tag, rather than any reply a
/// joining machine is owed. A roster for a fleet of hundreds is tens of kilobytes.
inline constexpr std::size_t MaxSealedReplyPayload = 4U * 1024U * 1024U;

/// The largest refusal a client reads: an `Error` reply's code and its sentence.
///
/// Every verb may be refused, so every BOUNDED reply ceiling holds one (`BoundedRepliesHoldARefusal`)
/// -- a ceiling below this would turn a server's explanation into a transport failure. Generous
/// against any sentence this project writes, and nothing a stranger can scale.
inline constexpr std::size_t MaxRefusalReply = 64 * 1024;

/// The largest report a client reads from a gated verb: a node's status, its metrics, the fleet
/// document, a cluster's state, the enrollment list.
///
/// These grow with the fleet, and each is read from a server the caller chose and authenticated to,
/// so the ceiling is generous rather than snug; what it forbids is a five-byte header committing
/// four GiB, which no fleet's report comes near.
inline constexpr std::size_t MaxReportReply = 64U * 1024U * 1024U;

/// The largest MINT-TICKET reply a client reads: one machine ticket, whose decoder refuses more
/// than 1 KiB (`Distributed::MaxMachineTicketBytes`, which this dependency-free header cannot
/// name), or a refusal -- so the refusal ceiling, which holds both.
inline constexpr std::size_t MaxMintTicketReply = MaxRefusalReply;

/// The largest FLEET-SUMMARY reply a client reads.
///
/// The one reply here read from a STRANGER by design -- a seed a DNS SRV record named, or anything
/// answering at a remembered address -- so it is the tightest on the table: the largest summary
/// (`MaxFleetSummaryTextBytes` per text field, asserted beside the codec by
/// `LargestFleetSummaryReply`) plus the key and the signature fits well inside it, and so does a
/// refusal.
inline constexpr std::size_t MaxFleetSummaryReply = MaxRefusalReply;

/// The longest text field a fleet summary carries: an id, an endpoint, a leader.
///
/// A bound rather than whatever a `u32` prefix allows, because the summary is read from strangers
/// -- in a beacon and in a FLEET-SUMMARY reply -- and `MaxFleetSummaryReply` has to hold the
/// largest one. A DNS name is at most 253 bytes and a minted id 32; 1 KiB is `RaftWire`'s id bound
/// and generous against both. `DecodeFleetSummaryFields` refuses a longer field.
inline constexpr std::size_t MaxFleetSummaryTextBytes = 1024;

/// The most member ids a fleet summary carries, in EVERY carrier: a discovery datagram -- a beacon,
/// and the proof that answers its challenge -- and a FLEET-SUMMARY reply.
///
/// **A measured bound, not a derived one.** A proof naming this many ids at `MaxIdBytes`, beside a
/// cluster, leader and speaker id at `MaxIdBytes`, three IPv4 endpoints at their longest spelling
/// and the leader's identity key, is one UDP payload no larger than 1232 bytes: IPv6's 1280-byte minimum link MTU less
/// its 40-byte header and UDP's 8, the smallest budget any link a datagram crosses unfragmented is
/// held to. The figure and its conditions live in `DiscoveryWire_test.cpp`, which measures it with
/// `ProofDatagramSize`; this comment does not restate it. Endpoints are bounded only by
/// `MaxFleetSummaryTextBytes`, so a node announcing longer ones sends a larger datagram: the
/// bound is a measurement under stated conditions, and `AnswerFits` still refuses to amplify.
///
/// A fleet recording more machines says so (`FleetSummary::memberTotal`), so a reader never mistakes
/// a cut list for a whole one.
///
/// **One cap for every carrier since `0xFC` 15**, where a reply carried up to 512. The longer list
/// had one reader, split evidence (C) (`Cluster::WeAskedAndTheyListUs`), and that evidence is told
/// to an operator and moves nothing; the summary lists the ids it needs FIRST
/// (`Cluster::SummaryMembers`), so the cut drops them last, and a beacon already cut there. A list
/// no carrier needs is grammar nobody reads, so it went, and with it the per-carrier parameter
/// every summary reader took.
inline constexpr std::size_t MaxFleetSummaryMembers = 10;

/// The longest an ID may be -- a cluster's or a node's -- wherever one enters: ONE bound for both.
///
/// Tighter than every other text field, because an id is what the fleet's own messages carry
/// most: a cluster id rides every discovery CHALLENGE, which must fit the beacon it answers or is
/// not sent, so an unbounded cluster id was a node that silently challenged nobody
/// (`Cluster::DiscoveryWire` asserts that an honest challenge under this bound always fits); and
/// a node id past what a summary carries was a node every peer silently refused. Ids are minted,
/// 32 hex characters, so this is twice what any node of this build names.
///
/// Refused by name where an id ENTERS: the option rows that take one (`--node-id`, the enrollment
/// verbs' `<id>`, and the member grammar `--cluster-admit` and its siblings share), a fleet summary
/// read from the wire, and a formation record read from disk. A cluster's id is minted, never typed.
inline constexpr std::size_t MaxIdBytes = 64;

static_assert(MaxIdBytes <= MaxFleetSummaryTextBytes, "an id is a fleet summary's text, and bounded tighter");

static_assert(MaxRefusalReply <= MaxSealedReplyPayload && MaxSealedReplyPayload <= MaxReportReply,
              "a refusal fits every reply ceiling, and a sealed reply fits a report's");

/// How long a scheduler's lease lives, and therefore how long a client waits.
///
/// **One number, and it has to be, because the two ends bound the same thing from
/// opposite sides.** Above the lease timeout a client waits on a lease the scheduler
/// has already reclaimed and may have re-granted for the same key; below it, a
/// compile the fleet is still holding capacity for is thrown away. Neither end can
/// pick its own value without describing a fleet the other end is not running
/// ([#249](https://github.com/LASTRADA-Software/fastcached/issues/249)).
///
/// It lives HERE for the reason `MaxAuthPayload` and `MaxControlPayload` do: this
/// header is the only thing both ends can include. `fastcache-cc` does not link
/// `FastCache`, so `LeaseTable`'s own header is not reachable from the launcher --
/// which is what left these as two literals coupled by a doc comment. The comment
/// was right that they could not be `static_assert`ed against each other; it was
/// wrong that nothing could be done, because the VALUE could move to where both
/// sides already look.
///
/// No `static_assert` pairs them, deliberately: both now DERIVE from this, so there
/// is nothing left to disagree. An assertion that two aliases of one constant are
/// equal is the guard that fires only when nothing is wrong.
///
/// Sized for "longer than any single translation unit anybody compiles". The cost of
/// too short is duplicated work; the cost of too long is one key not being
/// distributed while a worker that is up but making no progress goes unnoticed. A
/// dead HOST is not in that trade any more -- the compile leg dials with keepalive
/// armed -- so what is left is the narrow row #245's liveness signal closes. Stated
/// on `DefaultDispatchTotal`.
inline constexpr std::chrono::milliseconds DefaultCompileLeaseTimeout { 600'000 };

/// The largest lease lifetime a cluster may agree on.
///
/// The value above is a DEFAULT since #522: a site whose translation units are long
/// says so by setting `lease-lifetime` in replicated state, and every end then reads
/// the number out of the grant rather than out of a constant. This is how far that
/// number may go, and it exists because two separate things stop being free as it
/// grows.
///
/// **The expiry is still load-bearing for replay, which is the binding reason.**
/// #614's spend-once is what carries replay protection now, and `SpentLeases` prunes
/// with the verifier's own predicate, so retention and acceptance stay one window at
/// any value here -- that part is a memory cost and a self-limiting one, since a set
/// holding `(lifetime + slack) x spend rate` entries shrinks as the translation units
/// get longer, which is the deployment that raises this. But `SpentLeases` states
/// three residuals its spend does NOT close, and this constant bounds two of them: a
/// worker restart empties the set, so a token captured beforehand is spendable once
/// afterwards for whatever is left of its `expiresAt`, and a clock stepping backwards
/// is bounded by "the same window". A worker restart is an ORDINARY fleet event --
/// a rolling upgrade is a fleet of them -- so this number is the post-restart replay
/// window, and raising it raises exactly the thing the expiry was chosen to bound.
///
/// **It is also the endpoint's answer window for a compile**, which is the second
/// reason and was not obvious. `CompileResponder::RequestTimeout` is armed before the
/// payload is read and therefore before any grant can be verified, so the only value
/// available at that moment which cannot cut a legal job short is this ceiling. A
/// served member may hold a compile socket this long while uploading; the per-job
/// bound tightens to the grant as soon as one is authenticated.
///
/// One hour. Six times the default, which covers "our translation units are long" with
/// room -- a site whose SINGLE translation unit exceeds an hour has a build problem no
/// lease lifetime fixes -- and small enough that both costs above stay bounded by
/// something an operator can reason about. A rounder, more generous number would buy
/// nothing and widen both.
inline constexpr std::chrono::milliseconds MaxCompileLeaseLifetime { 3'600'000 };

/// How often a worker writes `Status::Progress` while a dispatched compile runs.
///
/// **One number, for `DefaultCompileLeaseTimeout`'s reason**: the worker's cadence
/// and the client's patience bound the same silence from opposite sides, and neither
/// end can pick its own without describing a fleet the other end is not running. It
/// lives here because this header is the only thing both ends include.
///
/// Five seconds, chosen from what it costs in each direction rather than as a round
/// number. Too fast is five bytes plus a wake-up per interval per in-flight compile,
/// on a machine whose job is to run compilers; too slow is the floor on how quickly a
/// silent worker can be noticed, since detection cannot beat the cadence it measures.
/// Against a compile measured in tens of seconds to minutes, five seconds is
/// negligible on the first count and two orders of magnitude better than the ten
/// minutes it replaces on the second.
inline constexpr std::chrono::milliseconds DefaultProgressInterval { 5'000 };

/// How long a client waits in SILENCE on a dispatched compile before giving up.
///
/// The other half of `DefaultProgressInterval`, and the number #245 exists to make
/// expressible: with a pulse on the wire the client stops measuring *how long this
/// compile has taken* — which must stay minutes, and does
/// (`DefaultCompileLeaseTimeout`) — and starts measuring *how long since the worker
/// last said anything*, which can be seconds.
///
/// Thirty seconds is deliberately several missed pulses rather than one. A single
/// missed pulse is not evidence: the worker is a machine running as many compilers as
/// it has slots, its reactor thread is shared with every other connection, and a
/// scheduling hiccup that delays a five-byte write past one interval is ordinary. Two
/// consecutive ones are not, so tolerating five and refusing the sixth is a bound that
/// costs a legitimate build nothing while still answering in seconds where the flat
/// deadline answered in minutes.
inline constexpr std::chrono::milliseconds DefaultCompileIdleTimeout { 30'000 };

// The relation is what makes either number safe, so it is asserted rather than left
// to whoever next retunes one of them: an idle bound at or below the cadence refuses
// a perfectly healthy worker on the ordinary jitter of its own reactor, which reads
// as a fleet that has stopped working. Stated as a multiple, so tightening the bound
// past the point where it stops tolerating a missed pulse is a build failure.
static_assert(DefaultCompileIdleTimeout >= 3 * DefaultProgressInterval,
              "a client must tolerate at least two missed pulses; below that, jitter reads as a dead worker");
static_assert(DefaultCompileIdleTimeout < DefaultCompileLeaseTimeout,
              "an idle bound at or above the total budget bounds nothing that the total did not already bound");

// **This assertion covers the DEFAULT and nothing else, now that the lifetime is a
// replicated setting.** A cluster may agree on a lifetime at or below the idle bound,
// which puts a healthy worker's own reactor jitter above the total and reads as a fleet
// that has stopped working -- the same failure the assertion above exists to prevent,
// reachable at run time instead of at build time. So the relation is asked again where
// the setting is validated (`Cluster::ParseLeaseLifetime`, reached through the
// `lease-lifetime` row's `refuse` column), on the LEADER, before the append: an
// operator gets an error rather than an entry replicated cluster-wide. The
// value is REFUSED there and never clamped -- a clamp would answer `accepted` for a
// number the cluster did not adopt, which is the ticket's own complaint one layer along.
static_assert(DefaultCompileLeaseTimeout <= MaxCompileLeaseLifetime,
              "the default lease lifetime must itself be a value a cluster is allowed to agree on");

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
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Cache,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::Fetch,
                   .name = "fetch",
                   .fieldCount = 1, // key
                   .legalStatuses =
                       static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Miss) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = SessionCapGoverns,
                   .maxReply = ReplyCarriesArtefact,
                   .family = VerbFamily::Cache,
                   .identity = IdentityRequirement::AddressAdmits },
    // `Miss` is legal and is not a failure: see `Op::CacheDrop`. Bounded rather than left to
    // the session cap, unlike `Fetch`, because the request carries a key and never an
    // artefact -- a verb that cannot grow with a build has no reason to accept 256 MiB.
    OpDescriptor { .code = Op::CacheDrop,
                   .name = "cache-drop",
                   .fieldCount = 1, // key
                   .legalStatuses =
                       static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Miss) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Cache,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::Auth,
                   .name = "auth",
                   .fieldCount = 3, // kind, username, secret
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = OpenBeforeAuth,
                   .maxPayload = BoundedTo(MaxAuthPayload),
                   .maxReply = ReplyBoundedTo(MaxRefusalReply),
                   .family = VerbFamily::Session,
                   .identity = IdentityRequirement::AddressAdmits },

    // The node proof (#1428, #178). `RequiresAuth` on both, which is the one thing about these
    // two rows that will look wrong beside `Op::Enroll` above -- `VerbFamily::NodeProof` carries
    // the argument, and it is not a weaker version of enrollment's.
    OpDescriptor { .code = Op::NodeChallenge,
                   .name = "node-challenge",
                   .fieldCount = 2, // the caller's nonce, its ephemeral key
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxNodeProofPayload),
                   .maxReply = ReplyBoundedTo(MaxSealedReplyPayload),
                   .family = VerbFamily::NodeProof,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::ProveNode,
                   .name = "prove-node",
                   .fieldCount = 3, // nodeId, identity key, signature
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxNodeProofPayload),
                   .maxReply = ReplyBoundedTo(MaxSealedReplyPayload),
                   .family = VerbFamily::NodeProof,
                   .identity = IdentityRequirement::AddressAdmits },

    // Distributed execution. None is `preAuth`: causing a compiler to run on
    // another machine is the last thing an unauthenticated peer should reach.
    OpDescriptor { .code = Op::Register,
                   .name = "register",
                   .fieldCount = 5, // fingerprint, endpoint, u32 slots, accepted codecs, capacity
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::ProvenNodeOnly },
    OpDescriptor { .code = Op::NodeAnnounce,
                   .name = "node-announce",
                   .fieldCount = 4, // endpoint, capacity, load, join memos
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   // Not pre-auth: this column is the daemon's credential gate, which a node
                   // never reaches. On a node no verb waits for a credential -- admission is
                   // `RefusePeer`'s, and this verb requires a proven identity there.
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   // Its `Ok` is EMPTY since the certified roster left it (#178), so a refusal's sentence
                   // is the most it ever answers.
                   .maxReply = ReplyBoundedTo(MaxRefusalReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::ProvenNodeOnly },
    OpDescriptor { .code = Op::Heartbeat,
                   .name = "heartbeat",
                   .fieldCount = 3, // workerId, u32 inFlight, load
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::ProvenNodeOnly },
    OpDescriptor { .code = Op::Lease,
                   .name = "lease",
                   .fieldCount = 5, // fingerprint, key, accepted codecs, exclusions, toolchain label
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::Release,
                   .name = "release",
                   .fieldCount = 2, // leaseToken, key
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::AddressAdmits },
    // `Ok | Error` and no third status, which is what keeps `MinSupportedVersion`
    // where it is. Step-over is a REQUEST-side property -- a request carries an op
    // byte a server can refuse by name -- so an old scheduler meeting this answers
    // `UnknownOpcode` and the worker carries on. `Status::Progress` had to move
    // `MinSupportedVersion` for the mirror reason: a REPLY carries a status byte and
    // no kind, so an old client cannot step over one it does not know. A distinct
    // "withdrew nothing" status here would invert that and cost a version bump,
    // which is why an unknown id answers `Ok` instead.
    OpDescriptor { .code = Op::Withdraw,
                   .name = "withdraw",
                   .fieldCount = 1, // workerId
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::ProvenNodeOnly },
    OpDescriptor { .code = Op::ClusterStatus,
                   .name = "cluster-status",
                   .fieldCount = 0, // nothing to ask with
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::ClusterSet,
                   .name = "cluster-set",
                   .fieldCount = 2, // setting name, value
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::OperatorStanding },
    OpDescriptor { .code = Op::ClusterForget,
                   .name = "cluster-forget",
                   .fieldCount = 1, // member id
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::OperatorStanding },
    OpDescriptor { .code = Op::ClusterAdmit,
                   .name = "cluster-admit",
                   .fieldCount = 3, // member id, consensus endpoint, identity key (#178)
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::OperatorStanding },
    OpDescriptor { .code = Op::ClusterAdmitLearner,
                   .name = "cluster-admit-learner",
                   .fieldCount = 3, // member id, consensus endpoint, identity key -- ClusterAdmit's
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Scheduler,
                   .identity = IdentityRequirement::OperatorStanding },
    // The fleet cache verbs: their twins' shapes -- the same objects -- and their own family,
    // which is where their policy lives. `AddressAdmits` because admission is the fold's answer
    // (a proven key, a ticket, loopback), never a column of this table.
    OpDescriptor { .code = Op::SharedStore,
                   .name = "shared-store",
                   .fieldCount = 5, // key, prefetchGroup, srcRoot, buildTree, value
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = SessionCapGoverns,
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::SharedCache,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::SharedFetch,
                   .name = "shared-fetch",
                   .fieldCount = 1, // key
                   .legalStatuses =
                       static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Miss) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = SessionCapGoverns,
                   .maxReply = ReplyCarriesArtefact,
                   .family = VerbFamily::SharedCache,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::MintTicket,
                   .name = "mint-ticket",
                   .fieldCount = 1, // audience
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxMintTicketPayload),
                   .maxReply = ReplyBoundedTo(MaxMintTicketReply),
                   .family = VerbFamily::Session,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::NodeStatus,
                   .name = "node-status",
                   .fieldCount = 0, // nothing to ask with
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Node,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::ExplainAdmission,
                   .name = "explain-admission",
                   .fieldCount = 1, // the subject: a machine id or key, or empty for the caller itself
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   // Reachable before admission, bounded by its own ceiling, because the SELF form
                   // must reach a caller the node refuses -- or it could never report the refusal.
                   // Not a leak: the self form's subject is the caller's own connection and only
                   // what that connection established (its host, the key it proved, the ticket it
                   // presented), so a stranger learns "refused, by no route", which every gated
                   // verb already tells it. The MACHINE form reads the roster and so describes
                   // third parties; it stays gated by membership, and counted, in the node.
                   .preAuth = OpenBeforeAuth,
                   .maxPayload = BoundedTo(MaxExplainAdmissionPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Node,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::NodeMetrics,
                   .name = "node-metrics",
                   .fieldCount = 0, // nothing to ask with
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Node,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::Compile,
                   .name = "compile",
                   // leaseToken, fingerprint, args, preprocessed, accepted codecs, sourceName,
                   // compileDir, compileDirReplacement, sourceRoot, sourceRootReplacement
                   .fieldCount = 10,
                   // `Progress` is legal HERE and on no other verb, which is the whole
                   // scope of the version-3 change: every other verb on this wire is
                   // answered from a table in microseconds, so a liveness pulse on one
                   // would be a frame nobody could ever observe and a second shape every
                   // reply reader would have to handle for nothing.
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)
                                                              | StatusBit(Status::Progress)),
                   .preAuth = RequiresAuth,
                   .maxPayload = SessionCapGoverns, // carries a preprocessed TU; the operator's cap governs
                   .maxReply = ReplyCarriesArtefact,
                   .family = VerbFamily::Compile,
                   .identity = IdentityRequirement::AddressAdmits },

    // Runtime enrollment, and deliberately NOT inside the distributed-execution block
    // above: that block's section comment states that none of it is `preAuth`, and the
    // first row here is. A row placed there would make that sentence false in the one
    // comment a reviewer reads to understand this table's security posture.
    OpDescriptor { .code = Op::Enroll,
                   .name = "enroll",
                   .fieldCount = 7, // nodeId, nodeEndpoint, role, publicKey, nonce, challenge, signature
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   // The second such row this table has ever held, and the first that is
                   // not AUTH. `PreAuthVerbsAreBounded` is what keeps the ceiling below
                   // from being omitted, and this row is the first that has ever made
                   // that assertion discriminate anything.
                   .preAuth = OpenBeforeAuth,
                   .maxPayload = BoundedTo(MaxEnrollPayload),
                   .maxReply = ReplyBoundedTo(MaxSealedReplyPayload),
                   .family = VerbFamily::Enrollment,
                   .identity = IdentityRequirement::AddressAdmits },
    OpDescriptor { .code = Op::EnrollControl,
                   .name = "enroll-control",
                   .fieldCount = 2, // verb, subject
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Enrollment,
                   .identity = IdentityRequirement::OperatorStanding },

    // Fleet formation, beside enrollment and for its reason: the row is pre-auth, so it stays out
    // of the distributed-execution block whose section comment says none of it is.
    OpDescriptor { .code = Op::FleetSummary,
                   .name = "fleet-summary",
                   .fieldCount = 1, // the asker's nonce
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   // The third pre-auth row. A seed answers machines that are members of nothing yet,
                   // and the answer is public: a signed statement of what a beacon already shouts.
                   .preAuth = OpenBeforeAuth,
                   .maxPayload = BoundedTo(MaxFleetSummaryPayload),
                   .maxReply = ReplyBoundedTo(MaxFleetSummaryReply),
                   .family = VerbFamily::Formation,
                   .identity = IdentityRequirement::AddressAdmits },

    // Live stats (#1399). Not pre-auth: a dashboard reads the fleet, which is exactly what
    // an unauthenticated stranger must not be shown.
    OpDescriptor { .code = Op::Subscribe,
                   .name = "subscribe",
                   .fieldCount = 3, // subject, cadence, dashboard token
                   // `Push` for the stream; `Ok` for a stream the node ends for its own orderly
                   // reason; `Error` for a refusal at subscribe time or a revocation mid-stream.
                   .legalStatuses =
                       static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error) | StatusBit(Status::Push)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Live,
                   .identity = IdentityRequirement::AddressAdmits },

    // The cordon (#1303). Gated on locality by the compile surface rather than on a
    // credential: see `CompileResponder::RefusePeer`.
    OpDescriptor { .code = Op::Cordon,
                   .name = "cordon",
                   .fieldCount = 1, // cordon or lift
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Compile,
                   .identity = IdentityRequirement::AddressAdmits },

    // The fleet document, read once (#1391). Not pre-auth, for `Subscribe`'s reason: it is the
    // fleet map.
    OpDescriptor { .code = Op::FleetText,
                   .name = "fleet-text",
                   .fieldCount = 3, // section key, range key, dashboard token
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = RequiresAuth,
                   .maxPayload = BoundedTo(MaxControlPayload),
                   .maxReply = ReplyBoundedTo(MaxReportReply),
                   .family = VerbFamily::Fleet,
                   .identity = IdentityRequirement::AddressAdmits },
};

/// Whether `Status::Progress` is confined to the one verb that can be slow enough to
/// need it.
///
/// A `static_assert`ed invariant rather than a comment, the same shape as
/// `PreAuthVerbsAreBounded` above and for the same kind of reason: a verb that
/// acquires this status acquires a reply stream, and every client reading that verb
/// then has to loop. That is a decision, not something to inherit by editing a mask.
/// @return True when `Op::Compile` is the only row admitting `Progress`.
[[nodiscard]] constexpr bool ProgressIsCompileOnly() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) {
        return row.code == Op::Compile || (row.legalStatuses & StatusBit(Status::Progress)) == 0;
    });
}

static_assert(ProgressIsCompileOnly(), "a progress pulse turns a reply into a stream; only COMPILE is long enough to");

/// Whether every verb reachable before authentication bounds its replies.
///
/// `PreAuthVerbsAreBounded`'s mirror: those verbs are the ones a client asks of a machine it has
/// no reason to trust yet, so their replies must never be sized by what the other end declares.
/// @return True when no pre-auth row is `ReplyCarriesArtefact`.
[[nodiscard]] constexpr bool PreAuthRepliesAreBounded() noexcept
{
    return std::ranges::all_of(OpTable,
                               [](OpDescriptor const& row) { return !row.preAuth.Allowed() || row.maxReply.IsBounded(); });
}

static_assert(PreAuthRepliesAreBounded(), "a verb a stranger may be asked must bound the reply it reads");

/// Whether every bounded reply ceiling holds a refusal, so a server's explanation is never read as
/// a transport failure.
/// @return True when every bounded ceiling is at least `MaxRefusalReply`.
[[nodiscard]] constexpr bool BoundedRepliesHoldARefusal() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) {
        return !row.maxReply.IsBounded() || row.maxReply.Bytes() >= MaxRefusalReply;
    });
}

static_assert(BoundedRepliesHoldARefusal(), "every verb may be refused, so every reply ceiling holds a refusal");

/// Whether a reply of @p status may answer @p op at all, read off the verb's `legalStatuses`.
///
/// What refuses a `Status::Progress` on a verb that does not pulse, and a `Miss` on one that cannot
/// miss: the column already says which statuses each verb may be answered with, so a reader asks
/// it rather than keeping a list of its own.
/// @param op The verb that was asked.
/// @param status The status the reply carries.
/// @return True when the status is one this verb may be answered with.
[[nodiscard]] constexpr bool StatusLegalFor(OpDescriptor const& op, Status status) noexcept
{
    return (op.legalStatuses & StatusBit(status)) != 0;
}

/// Whether a reply declaring @p declared payload bytes may answer @p op -- asked BEFORE the payload
/// is allocated.
/// @param op The verb that was asked.
/// @param declared The payload length the reply header declares.
/// @return True when the verb's `maxReply` admits it.
[[nodiscard]] constexpr bool ReplyFits(OpDescriptor const& op, std::uint32_t declared) noexcept
{
    return !op.maxReply.IsBounded() || declared <= op.maxReply.Bytes();
}

/// Whether `Status::Push` is confined to the one verb whose answer is a stream.
///
/// `ProgressIsCompileOnly`'s twin, for the second status that turns a reply into a stream: a
/// verb that acquires it acquires a reader that loops and a writer that outlives the request,
/// and that is a decision rather than something to inherit by editing a mask.
/// @return True when `Op::Subscribe` is the only row admitting `Push`.
[[nodiscard]] constexpr bool PushIsSubscribeOnly() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) {
        return row.code == Op::Subscribe || (row.legalStatuses & StatusBit(Status::Push)) == 0;
    });
}

static_assert(PushIsSubscribeOnly(), "a push frame is a stream's; only SUBSCRIBE is answered by one");

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

/// Whether no verb names `VerbFamily::Last`, which is a count rather than a family.
/// @return True when every row names a real family.
[[nodiscard]] constexpr bool NoVerbIsInTheCountFamily() noexcept
{
    return std::ranges::none_of(OpTable, [](OpDescriptor const& row) { return row.family == VerbFamily::Last; });
}

static_assert(NoVerbIsInTheCountFamily(), "VerbFamily::Last is the count, not a family a verb can belong to");

/// Whether no two rows claim one wire byte.
///
/// **The collision this catches merges cleanly.** Verbs are added on parallel branches, each
/// taking the next byte free on ITS base, and two enumerators on different lines of `Op` that
/// were given the same value are no textual conflict at all. `FindOp` then answers the first
/// row for both, so the second verb is served as the first -- a request for one operation
/// performs another, and every in-tree test still agrees with itself, because each spells
/// the enumerator rather than the byte.
/// @return True when every `OpTable` row has a distinct `code`.
[[nodiscard]] constexpr bool EveryOpcodeIsDistinct() noexcept
{
    return std::ranges::all_of(
        OpTable, [](OpDescriptor const& row) { return std::ranges::count(OpTable, row.code, &OpDescriptor::code) == 1; });
}

static_assert(EveryOpcodeIsDistinct(), "two verbs claim one wire byte; the later one would be served as the earlier");

/// Whether exactly the verbs a machine JOINS the fleet with require a proven identity (#178).
///
/// `Register`, `NodeAnnounce`, `Heartbeat` and `Withdraw` decide where the fleet's work is sent and
/// what it knows about each machine, so an address may not stand in for who is sending them. Every
/// other verb is a CLIENT's, an operator's or a peer handshake's, and an address still admits those
/// -- which is owner decision 5 on #178 stated as a table rather than as a list at the gate.
///
/// Both directions, because each is a different mistake: a joining verb left open is an endpoint
/// anybody on an admitted host can register, and any other verb marked here refuses every launcher.
/// @return True when the column names exactly those four.
[[nodiscard]] constexpr bool JoiningVerbsNeedAnIdentity() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) {
        auto const joining = row.code == Op::Register || row.code == Op::NodeAnnounce || row.code == Op::Heartbeat
                             || row.code == Op::Withdraw;
        return joining == (row.identity == IdentityRequirement::ProvenNodeOnly);
    });
}

static_assert(JoiningVerbsNeedAnIdentity(),
              "exactly Register, NodeAnnounce, Heartbeat and Withdraw require a proven identity -- see IdentityRequirement");

/// Whether exactly an operator's CONTROL verbs require an operator's standing.
///
/// The cluster verbs that change the fleet -- admit, admit as a learner, forget, set -- and the
/// enrollment decisions (`EnrollControl`: approve, reject, arm or disarm auto-approval,
/// clear, and the list that names the joiners' keys). Both directions: a control verb left at
/// `AddressAdmits` is one an anonymous caller on a `--fleet-open` node sends, and any other verb
/// marked here refuses every launcher -- and every learner's client -- that asks it. `Cordon` and
/// `MintTicket` are not rows: each already answers this machine alone, a narrower rule its
/// responder enforces. And no verb names `IdentifiedCaller`, which is only `OperatorStanding`'s
/// prerequisite: a verb that named it would let a learner's ticket decide the fleet again.
/// @return True when the column names exactly those five.
[[nodiscard]] constexpr bool ControlVerbsNeedOperatorStanding() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) {
        auto const control = row.code == Op::ClusterSet || row.code == Op::ClusterForget || row.code == Op::ClusterAdmit
                             || row.code == Op::ClusterAdmitLearner || row.code == Op::EnrollControl;
        return control == (row.identity == IdentityRequirement::OperatorStanding)
               && row.identity != IdentityRequirement::IdentifiedCaller;
    });
}

static_assert(ControlVerbsNeedOperatorStanding(),
              "exactly the cluster control verbs and EnrollControl require an operator's standing, and no verb names "
              "IdentifiedCaller -- see IdentityRequirement");

/// Whether asking again, UNCHANGED, may be answered differently -- with nobody acting on the
/// request in between.
///
/// **What a script reading a one-shot verb's exit acts on**: a refusal that may clear by itself
/// is 1, retry, and one that will be given again is 2, do not. The code is a structured fact the
/// PEER states, not words, so its permanence is stated here, once, per code -- where the code's
/// own documentation already argues it -- rather than guessed by each reader.
///
/// **A type rather than a `bool`, for `PreAuth`'s reason**: the default constructor is deleted,
/// so an `ErrorTable` row that does not state its answer does not compile, and a new code cannot
/// inherit a permanence nobody decided. Every row also says WHY (`retryWhy`), and
/// `EveryRetryIsReasoned` refuses an empty one.
///
/// **PRIVATE: in-process only.** Neither end transmits it; each reads it off its own table.
class Retry
{
  public:
    /// Deleted on purpose: a row must state its answer. See the class comment.
    Retry() = delete;

    /// @param mayHelp True when the same request asked again may be answered differently.
    constexpr explicit Retry(bool mayHelp) noexcept:
        _mayHelp { mayHelp }
    {
    }

    /// @return True when the same request asked again may be answered differently.
    [[nodiscard]] constexpr bool MayHelp() const noexcept
    {
        return _mayHelp;
    }

  private:
    bool _mayHelp;
};

/// The refusal may clear by itself: the same request, asked again, may be served.
inline constexpr Retry RetryMayHelp { true };

/// The refusal will be given again until somebody changes something.
inline constexpr Retry RetryWillNotHelp { false };

/// One row of the error table: the code, its stable name, the message sent when the
/// caller has nothing more specific to say, and whether asking again may help.
struct ErrorDescriptor
{
    ErrorCode code;                  ///< The wire byte.
    std::string_view name;           ///< Stable lower-case name, for logs and tests.
    std::string_view defaultMessage; ///< Human-readable text sent when no detail is supplied.
    Retry retry;                     ///< Whether the same request asked again may be answered differently.
    std::string_view retryWhy;       ///< Why, for this code.
};

/// Every error code this build emits. Size deduced, for the reason `OpTable`'s is.
inline constexpr std::array ErrorTable {
    ErrorDescriptor { .code = ErrorCode::UnsupportedVersion,
                      .name = "unsupported-version",
                      .defaultMessage = "unsupported wire version",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the two builds' wire ranges do not meet until one of them is upgraded" },
    ErrorDescriptor { .code = ErrorCode::UnknownOpcode,
                      .name = "unknown-opcode",
                      .defaultMessage = "unknown opcode",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "this peer's build does not know the verb, and will not learn it while asked" },
    ErrorDescriptor { .code = ErrorCode::MalformedFrame,
                      .name = "malformed-frame",
                      .defaultMessage = "malformed frame",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the same bytes are malformed the same way every time" },
    ErrorDescriptor { .code = ErrorCode::PayloadTooLarge,
                      .name = "payload-too-large",
                      .defaultMessage = "payload too large",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the same payload exceeds the same cap every time" },
    ErrorDescriptor { .code = ErrorCode::MalformedValue,
                      .name = "malformed-value",
                      .defaultMessage = "malformed compile-value frame",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the same value fails to decode the same way every time" },
    ErrorDescriptor { .code = ErrorCode::StorageWriteFailed,
                      .name = "storage-write-failed",
                      .defaultMessage = "storage write failed",
                      .retry = RetryMayHelp,
                      .retryWhy = "the peer's own storage failed, an I/O arm at the other end that a retry may get past" },
    ErrorDescriptor { .code = ErrorCode::Unauthenticated,
                      .name = "unauthenticated",
                      .defaultMessage = "authentication required",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the same credential is refused the same way until an operator changes one" },
    ErrorDescriptor { .code = ErrorCode::NoWorker,
                      .name = "no-worker",
                      .defaultMessage = "no worker matches this toolchain",
                      .retry = RetryMayHelp,
                      .retryWhy = "a booting fleet has none yet: a node registers nothing until its survey is real, "
                                  "and a restarted scheduler's registry starts empty" },
    ErrorDescriptor { .code = ErrorCode::NoCapacity,
                      .name = "no-capacity",
                      .defaultMessage = "every matching worker is busy",
                      .retry = RetryMayHelp,
                      .retryWhy = "every matching worker is busy with this fleet's own work, which finishes" },
    ErrorDescriptor { .code = ErrorCode::AlreadyInFlight,
                      .name = "already-in-flight",
                      .defaultMessage = "another client is already compiling this key",
                      .retry = RetryMayHelp,
                      .retryWhy = "another client's compile of this key finishes, and the key is then cached or free" },
    ErrorDescriptor { .code = ErrorCode::DispatchNotPermitted,
                      .name = "dispatch-not-permitted",
                      .defaultMessage = "this endpoint does not serve distributed execution",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "this listener's roles do not change while it runs" },
    ErrorDescriptor { .code = ErrorCode::UnknownLease,
                      .name = "unknown-lease",
                      .defaultMessage = "unknown or expired lease",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "a token this scheduler does not hold stays unknown; a retry needs a new lease" },
    ErrorDescriptor { .code = ErrorCode::FingerprintMismatch,
                      .name = "fingerprint-mismatch",
                      .defaultMessage = "this worker's toolchain is not the one the lease named",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "this worker serves another toolchain, and goes on serving it" },
    ErrorDescriptor { .code = ErrorCode::UnsupportedCodec,
                      .name = "unsupported-codec",
                      .defaultMessage = "no codec in common",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the two ends' codec lists do not meet until one of them changes" },
    ErrorDescriptor { .code = ErrorCode::WorkerScratchUnavailable,
                      .name = "worker-scratch-unavailable",
                      .defaultMessage = "the worker could not prepare a scratch directory",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "a full or unwritable scratch disk waits for an operator" },
    ErrorDescriptor { .code = ErrorCode::WorkerSpawnFailed,
                      .name = "worker-spawn-failed",
                      .defaultMessage = "the worker could not start the compiler",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the compiler the worker names cannot be run until its configuration changes" },
    ErrorDescriptor { .code = ErrorCode::WorkerCompilerUnclassified,
                      .name = "worker-compiler-unclassified",
                      .defaultMessage = "the worker cannot classify its own configured compiler",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "this build does not recognise the worker's compiler, and will not while asked" },
    ErrorDescriptor { .code = ErrorCode::NotLeader,
                      .name = "not-leader",
                      .defaultMessage = "this node does not lead the cluster",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "it names where to ask instead, and the same ask here gets the same name; one naming "
                                  "NOBODY is an election, which the reader of the message decides" },
    ErrorDescriptor { .code = ErrorCode::NotAMember,
                      .name = "not-a-member",
                      .defaultMessage = "not a member of this cluster",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "membership is an operator's decision" },
    ErrorDescriptor { .code = ErrorCode::Withdrawn,
                      .name = "withdrawn",
                      .defaultMessage = "every matching worker has withdrawn its capacity",
                      .retry = RetryMayHelp,
                      .retryWhy = "the matching workers' machines are busy with something else, which ends" },
    ErrorDescriptor { .code = ErrorCode::NoCluster,
                      .name = "no-cluster",
                      .defaultMessage = "this node runs no cluster",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "this node runs no cluster, and the remedy is another address" },
    ErrorDescriptor { .code = ErrorCode::InvalidClusterChange,
                      .name = "invalid-cluster-change",
                      .defaultMessage = "the cluster cannot accept that change",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the cluster could never accept the change as asked" },
    ErrorDescriptor { .code = ErrorCode::ClusterChangeInFlight,
                      .name = "cluster-change-in-flight",
                      .defaultMessage = "another cluster change is still committing; ask again shortly",
                      .retry = RetryMayHelp,
                      .retryWhy = "the change already accepted finishes committing, and this one is then admitted" },
    ErrorDescriptor { .code = ErrorCode::ClusterChangeNotNeeded,
                      .name = "cluster-change-not-needed",
                      .defaultMessage = "the cluster is already in that state",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the cluster is already in that state, and stays in it" },
    ErrorDescriptor { .code = ErrorCode::EndpointBusy,
                      .name = "endpoint-busy",
                      .defaultMessage = "this endpoint is serving all it will serve at once",
                      .retry = RetryMayHelp,
                      .retryWhy = "this endpoint's own bound frees as the requests it holds finish" },
    ErrorDescriptor { .code = ErrorCode::MalformedRegistration,
                      .name = "malformed-registration",
                      .defaultMessage = "a worker must name itself in UTF-8",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the same bytes are not text the same way every time" },
    ErrorDescriptor { .code = ErrorCode::LeaseUnauthorized,
                      .name = "lease-unauthorized",
                      .defaultMessage = "this lease was not issued by this cluster",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "a token no voter this worker trusts issued stays unauthorized" },
    ErrorDescriptor { .code = ErrorCode::LeaseEndpointMismatch,
                      .name = "lease-endpoint-mismatch",
                      .defaultMessage = "this lease was issued for another worker",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the lease names another worker, and always will" },
    ErrorDescriptor { .code = ErrorCode::LeaseExpired,
                      .name = "lease-expired",
                      .defaultMessage = "this lease has expired",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "an expired lease stays expired; a retry needs a new one" },
    ErrorDescriptor { .code = ErrorCode::WorkerToolchainSurveyInFlight,
                      .name = "worker-toolchain-survey-in-flight",
                      .defaultMessage = "this worker is still identifying its toolchains",
                      .retry = RetryMayHelp,
                      .retryWhy = "the survey completes or the node exits: transient by construction" },
    ErrorDescriptor { .code = ErrorCode::RequestDeadlineExceeded,
                      .name = "request-deadline-exceeded",
                      .defaultMessage = "this request outran the window this surface allows",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the same work outruns the same window; the remedy is the timeout" },
    // The default names no generation, because the generations are what an operator
    // acts on and neither of them is known here. Both surfaces send
    // `ForeignGenerationMessage`, which states them; this sentence is what a caller
    // that supplies none falls back to.
    ErrorDescriptor { .code = ErrorCode::ForeignValueGeneration,
                      .name = "foreign-value-generation",
                      .defaultMessage = "stored value names a canonicalization generation this build does not "
                                        "implement",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the two generations do not meet until the rollout finishes" },
    ErrorDescriptor { .code = ErrorCode::EnrollmentFull,
                      .name = "enrollment-full",
                      .defaultMessage = "the enrollment list is full; nothing was recorded",
                      .retry = RetryMayHelp,
                      .retryWhy = "nothing about this machine was decided, and the list drains as an operator decides" },
    ErrorDescriptor { .code = ErrorCode::UnknownFleetSelector,
                      .name = "unknown-fleet-selector",
                      .defaultMessage = "this build serves no fleet section or range by that name",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "this build serves no selector by that name, and will not while asked" },
    ErrorDescriptor { .code = ErrorCode::NodeProofUnchallenged,
                      .name = "node-proof-unchallenged",
                      .defaultMessage = "no challenge is outstanding on this connection; ask for one first",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the exchange was got wrong, and the same exchange is wrong again" },
    ErrorDescriptor { .code = ErrorCode::NodeProofRejected,
                      .name = "node-proof-rejected",
                      .defaultMessage = "the node proof's signature does not verify under the key it presented",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the signature does not verify, and the same signature never will" },
    ErrorDescriptor { .code = ErrorCode::NodeKeyUnknown,
                      .name = "node-key-unknown",
                      .defaultMessage = "this cluster holds no such identity key for that node; it has not been admitted",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "admitting a key is an operator's decision" },
    ErrorDescriptor { .code = ErrorCode::NodeKeyRevoked,
                      .name = "node-key-revoked",
                      .defaultMessage = "this cluster has revoked that identity key; the machine was forgotten",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "a revoked key is never admitted again" },
    ErrorDescriptor { .code = ErrorCode::NodeIdentityRequired,
                      .name = "node-identity-required",
                      .defaultMessage = "only a node that has proved its identity on this connection may send that verb",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the connection must prove an identity first, and the same connection has not" },
    ErrorDescriptor { .code = ErrorCode::EnrollmentHostFull,
                      .name = "enrollment-host-full",
                      .defaultMessage = "this host already has as many enrollment requests waiting as one host may; "
                                        "nothing was recorded",
                      .retry = RetryMayHelp,
                      .retryWhy =
                          "nothing about this machine was decided, and the host's rows drain as an operator decides" },
    ErrorDescriptor {
        .code = ErrorCode::TicketRefused,
        .name = "ticket-refused",
        .defaultMessage = "this node did not accept the machine ticket presented; the reason is named in "
                          "the message",
        .retry = RetryWillNotHelp,
        .retryWhy =
            "the node judged what the ticket names, and judges the same machine the same way until its standing changes" },
    ErrorDescriptor { .code = ErrorCode::IdentifiedCallerRequired,
                      .name = "identified-caller-required",
                      .defaultMessage = "an operator's control verb needs a caller this node can identify -- from this "
                                        "machine, or by a proven node key or a machine ticket; --fleet-open admits "
                                        "nobody to it",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "who the caller is does not change by asking again" },
    ErrorDescriptor { .code = ErrorCode::OperatorStandingRequired,
                      .name = "operator-standing-required",
                      .defaultMessage = "an operator's control verb needs this machine, or a voter's identity; a machine "
                                        "ticket or key of a machine that holds no voter's seat proves a machine, not an "
                                        "operator",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the caller's seat changes only when an operator promotes its machine" },
    ErrorDescriptor {
        .code = ErrorCode::NotSharedCache,
        .name = "not-shared-cache",
        .defaultMessage = "this machine is not the fleet's shared cache; the shared-cache setting names "
                          "another machine, or none",
        .retry = RetryWillNotHelp,
        .retryWhy = "the shared-cache setting is the cluster's, and names another machine until an operator changes it" },
    ErrorDescriptor { .code = ErrorCode::WorkerRejectedArgument,
                      .name = "worker-rejected-argument",
                      .defaultMessage = "this worker will not pass one of the job's arguments to its compiler",
                      .retry = RetryWillNotHelp,
                      .retryWhy = "the worker's allowlist does not change while it is asked; an operator adds the "
                                  "argument with --allow-compile-arg, or the compile runs locally" },
    ErrorDescriptor { .code = ErrorCode::GrantUnverifiable,
                      .name = "grant-unverifiable",
                      .defaultMessage = "this worker can verify no grant right now: its applied state names no voter, or "
                                        "it has not heard from a leader it counts",
                      .retry = RetryMayHelp,
                      .retryWhy = "a statement about the worker, which verifies again once its applied state names a "
                                  "voter and a leader it counts speaks to it" },
    ErrorDescriptor { .code = ErrorCode::RosterNotYetApplied,
                      .name = "roster-not-yet-applied",
                      .defaultMessage = "this node has not applied its cluster's state since it started, so it cannot "
                                        "judge that identity key yet",
                      .retry = RetryMayHelp,
                      .retryWhy = "a statement about the answering node, which judges the key once its consensus has "
                                  "applied the log it recovered -- an election or a leader's first word away" },
};

/// Whether every row says why asking again may or may not help.
/// @return True when no `ErrorTable` row leaves `retryWhy` empty.
[[nodiscard]] consteval bool EveryRetryIsReasoned() noexcept
{
    return std::ranges::none_of(ErrorTable, [](ErrorDescriptor const& row) { return row.retryWhy.empty(); });
}

static_assert(EveryRetryIsReasoned(), "every error code says why asking again may or may not help");

/// The codes a one-shot verb meeting them should retry, written out once more so a row that
/// changes its answer fails the BUILD by name rather than turning a script's retry into a stop:
/// the direction that costs is a transient refusal read as a decision, which gives up on a
/// healthy cluster mid-election or mid-change.
inline constexpr std::array RetriableErrorCodes {
    ErrorCode::StorageWriteFailed, ErrorCode::NoWorker,
    ErrorCode::NoCapacity,         ErrorCode::AlreadyInFlight,
    ErrorCode::Withdrawn,          ErrorCode::ClusterChangeInFlight,
    ErrorCode::EndpointBusy,       ErrorCode::WorkerToolchainSurveyInFlight,
    ErrorCode::EnrollmentFull,     ErrorCode::EnrollmentHostFull,
    ErrorCode::GrantUnverifiable,  ErrorCode::RosterNotYetApplied,
};

/// Whether the table's retriable rows are exactly `RetriableErrorCodes`.
/// @return True when the two agree in both directions.
[[nodiscard]] consteval bool RetriableCodesArePinned() noexcept
{
    return std::ranges::all_of(ErrorTable,
                               [](ErrorDescriptor const& row) {
                                   return row.retry.MayHelp() == std::ranges::contains(RetriableErrorCodes, row.code);
                               })
           && std::ranges::count_if(ErrorTable, [](ErrorDescriptor const& row) { return row.retry.MayHelp(); })
                  == std::ssize(RetriableErrorCodes);
}

static_assert(RetriableCodesArePinned(), "ErrorTable's retriable rows and RetriableErrorCodes must name the same codes");

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
///
/// 0x22 was EnrollmentClosed. Since zero-config formation a joiner's request is always recorded
/// and the approval is the gate; #178 already made the window guard no secret.
///
/// 0x28 was RosterExpired, named for the certified roster's lapse (#178). The roster no longer
/// lapses -- every node's is the state its own consensus applied -- so what the code still meant
/// travels as `GrantUnverifiable`, under a name that says it.
inline constexpr std::array<std::uint8_t, 4> RetiredErrorCodes { 0x06, 0x22, 0x24, 0x28 };

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
              "under its old name (0x06 was canonicalization-failed, see issues #59 and #69; 0x22 was "
              "enrollment-closed; 0x24 was enrollment-already-collected, see #178; 0x28 was roster-expired)");

/// Opcodes that once meant something and must never mean anything again: 0x16 was
/// CLUSTER-ADMIT-CLIENT and 0x17 CLUSTER-FORGET-CLIENT (#1309), retired when a machine
/// became admitted and forgotten by its key rather than by its host; 0x1D was
/// CLUSTER-ADMIT-WORKER, retired with principal mode -- every machine that joins is a learner.
///
/// `RetiredErrorCodes`' reason, one table over: a peer built before the retirement still
/// sends the byte, and a NEW verb assigned to it would be read by that peer's operator as
/// admitting or forgetting a host. A frame naming a retired byte is an unknown opcode --
/// `FindOp` answers nullptr and every surface refuses it `UnknownOpcode`, as it would any
/// byte no row claims.
inline constexpr std::array<std::uint8_t, 3> RetiredOpcodes { 0x16, 0x17, 0x1D };

/// Whether the op table has kept clear of every retired opcode.
///
/// Checks the TABLE rather than the enum, for `NoRetiredErrorCodeIsReused`'s reason: a row
/// is what a reuse must add to be dispatched at all, and nothing can enumerate an enum.
/// @return True when no `OpTable` row claims a retired byte.
[[nodiscard]] consteval bool NoRetiredOpcodeIsReused() noexcept
{
    return std::ranges::none_of(OpTable, [](OpDescriptor const& row) {
        return std::ranges::contains(RetiredOpcodes, static_cast<std::uint8_t>(row.code));
    });
}

static_assert(NoRetiredOpcodeIsReused(),
              "a retired opcode must never be reassigned -- a peer built against an older header still sends it "
              "(0x16 was cluster-admit-client and 0x17 cluster-forget-client, see #1309; 0x1D was "
              "cluster-admit-worker)");

/// What a compile-family verb is told at an endpoint that runs no compile worker.
///
/// **One fact, reached from two endpoints** (#206). The daemon is a cache and never ran
/// a worker; a compile node started with `--slots=0` runs none. Both are asked the same
/// question -- a `--cordon`, a compile somebody sent by hand -- and a client that met two
/// codes for one condition would be sent two remedies, which is what `NoCluster` already
/// avoids for the enrollment family.
///
/// `DispatchNotPermitted`, because these verbs are SERVED ELSEWHERE rather than
/// unimplemented -- by the fleet's workers, or by the node on the daemon's own machine --
/// and *a verb another port answers stays `DispatchNotPermitted`*. `UnimplementedVerb`
/// would tell the operator who sent `--cordon` that this build is too old to know the
/// verb. A launcher does not retry `NotPermitted`, which is right for both.
///
/// A STEM rather than a sentence, because the remedy is each endpoint's own: the daemon
/// sends an operator to the compile node on this machine, a node running no worker to a
/// node that runs one. Each table that answers these verbs asserts its row against this
/// at compile time (`SaysNoCompileWorker`), so neither the code nor the fact can be
/// changed at one endpoint alone.
///
/// **Shared DATA, not a way to answer**, which is why it sits here beside `ErrorTable`
/// rather than in `SurfaceRefusal.hpp`: that header holds exactly the functions a surface
/// refuses THROUGH, and `worker-refusals-counted` derives that set from it. A code and a
/// stem both endpoints read are the kind of fact this header already carries per code.
struct NoCompileWorker
{
    /// What the client acts on.
    static constexpr ErrorCode Code = ErrorCode::DispatchNotPermitted;
    /// How every such refusal's words begin; the endpoint completes it with its remedy.
    static constexpr std::string_view Stem = "this endpoint runs no compile worker";
};

/// Whether a refusal says `NoCompileWorker`'s fact, in its code and in its words.
/// @param code The code the refusal sends.
/// @param message The words it sends.
/// @return True when both are `NoCompileWorker`'s.
[[nodiscard]] constexpr bool SaysNoCompileWorker(ErrorCode code, std::string_view message) noexcept
{
    return code == NoCompileWorker::Code && message.starts_with(NoCompileWorker::Stem);
}

/// Whether a surface's refusal row says `NoCompileWorker`'s fact.
/// @param row The row.
/// @return True when its code and its words are `NoCompileWorker`'s.
[[nodiscard]] constexpr bool SaysNoCompileWorker(RefusedVerb const& row) noexcept
{
    return SaysNoCompileWorker(row.code, row.why);
}

/// Verbs that legitimately carry no fields at all.
///
/// Zero is also what a row that forgot its `fieldCount` would hold, and such a row
/// would then demand an empty payload and silently refuse every well-formed request.
/// So "this verb asks nothing" is stated here rather than left looking like an
/// omission, and `FieldCountsAgree` checks the two in both directions -- which is
/// what makes this a check rather than a second place to be wrong.
inline constexpr std::array FieldlessOps { Op::ClusterStatus, Op::NodeStatus, Op::NodeMetrics };

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

/// Whether the same request, asked again, may be answered differently than by @p code.
/// @param code The refusal.
/// @return The row's answer; false for a code this build does not know, which is a refusal it
///         cannot read and reads as one it will be given again.
[[nodiscard]] constexpr bool IsRetriable(ErrorCode code) noexcept
{
    auto const* const row = Describe(code);
    return row != nullptr && row->retry.MayHelp();
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

/// Which credential an AUTH frame carries: the byte is its first field.
///
/// Explicit values because these bytes are transmitted. A byte naming no row of
/// `KnownAuthKinds` makes the whole payload malformed rather than defaulting to a
/// password, so a peer that sends a kind this build has never heard of is told so
/// instead of having its ticket compared against `--requirepass`.
enum class AuthKind : std::uint8_t
{
    Password = 0x01,      ///< `secret` is a shared secret, checked against the surface's policy.
    MachineTicket = 0x02, ///< `secret` is a machine ticket's bytes; `username` must be empty.
};

/// Every `AuthKind` this build decodes.
inline constexpr std::array KnownAuthKinds { AuthKind::Password, AuthKind::MachineTicket };

/// The three fields of an AUTH request, the kind decoded and the rest as spans into
/// the caller's payload buffer.
struct AuthView
{
    AuthKind kind { AuthKind::Password }; ///< Which credential `secret` is.
    std::span<std::byte const> username;  ///< Username; empty selects the default user.
    std::span<std::byte const> secret;    ///< Shared secret, or a machine ticket's bytes.
};

/// The fields of an AUTH request, for encoding.
///
/// The username may be empty, which asks to be verified against the secret alone
/// — the redis one-argument `AUTH <pass>` / `requirepass` form. Carrying it as an
/// always-present (possibly empty) field rather than a separate one-field opcode
/// keeps the arity fixed, so the frame shape does not depend on which credential
/// style the client happens to use.
///
/// `kind` comes first so a request that names only `username` and `secret` is a
/// password, which every such request meant before the kind was carried.
struct AuthRequest
{
    AuthKind kind { AuthKind::Password }; ///< Which credential `secret` is.
    std::string_view username;            ///< Username, or empty for the default user; empty for a ticket.
    std::string_view secret;              ///< Shared secret, or a machine ticket's bytes.
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

namespace Detail
{
    /// Whether two verbs take the same request and may answer the same statuses.
    ///
    /// What makes a fleet cache verb its private twin's twin: the same objects, so the same
    /// payload and the same replies, and only the POLICY -- the family -- differs.
    /// @param a One verb.
    /// @param b The other.
    /// @return True when both are in `OpTable` and agree on arity, statuses and ceiling.
    [[nodiscard]] constexpr bool SameRequestShape(Op a, Op b) noexcept
    {
        auto const* const left = FindOp(static_cast<std::uint8_t>(a));
        auto const* const right = FindOp(static_cast<std::uint8_t>(b));
        return left != nullptr && right != nullptr && left->fieldCount == right->fieldCount
               && left->legalStatuses == right->legalStatuses && left->maxPayload.Bytes() == right->maxPayload.Bytes();
    }
} // namespace Detail

/// The fetch and store verbs one cache upstream speaks, as data.
///
/// A pair rather than a flag, so the client that speaks to a `fastcached` and the one that speaks to
/// the fleet's shared cache are the same code over a different ROW -- the node picks the row from a
/// table keyed on its upstream kind, and nothing branches on which kind it is.
///
/// **A pair naming a verb of another shape is unrepresentable, not refused.** The constructor is
/// `consteval` and walks `OpTable` through `FindOp`, so a pair whose fetch is not FETCH's shape or
/// whose store is not STORE's -- or a shared verb whose row drifts from its twin's -- fails the
/// BUILD at the pair's declaration. That is what lets `EncodeFetchAs` and `EncodeStoreAs` take a
/// pair and frame it without a check: every `CacheVerbs` that exists at run time was proved at
/// compile time, and a request framed under the wrong verb would be refused as malformed, which a
/// client reads as a dead cache.
struct CacheVerbs
{
    Op fetch; ///< The read verb: FETCH's request and replies.
    Op store; ///< The write verb: STORE's request and replies.

    /// @param fetchVerb A verb of FETCH's shape.
    /// @param storeVerb A verb of STORE's shape.
    consteval CacheVerbs(Op fetchVerb, Op storeVerb):
        fetch { fetchVerb },
        store { storeVerb }
    {
        // Reaching the throw makes the evaluation not a constant expression, which a `consteval`
        // call must be: the diagnostic is a compile error at the offending pair, never a run-time
        // exception.
        if (!Detail::SameRequestShape(fetchVerb, Op::Fetch) || !Detail::SameRequestShape(storeVerb, Op::Store))
            throw std::invalid_argument("a cache verb pair names a fetch of FETCH's shape and a store of STORE's");
    }

    [[nodiscard]] friend constexpr bool operator==(CacheVerbs, CacheVerbs) noexcept = default;
};

/// What a `fastcached` -- and a node's private tier -- answers.
inline constexpr CacheVerbs DaemonCacheVerbs { Op::Fetch, Op::Store };

/// What the fleet's shared cache answers.
inline constexpr CacheVerbs FleetSharedCacheVerbs { Op::SharedFetch, Op::SharedStore };

/// Frame a STORE request under the store verb of @p verbs.
/// @param verbs The pair the upstream speaks; its store verb frames the request.
/// @param request The fields to send.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeStoreAs(CacheVerbs verbs,
                                                          StoreRequest const& request,
                                                          WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version,
                                 verbs.store,
                                 { AsBytes(request.key),
                                   AsBytes(request.prefetchGroup),
                                   AsBytes(request.srcRoot),
                                   AsBytes(request.buildTree),
                                   request.value });
}

/// Frame a FETCH request under the fetch verb of @p verbs.
/// @param verbs The pair the upstream speaks; its fetch verb frames the request.
/// @param key The key to look up.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeFetchAs(CacheVerbs verbs,
                                                          std::string_view key,
                                                          WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, verbs.fetch, { AsBytes(key) });
}

/// Frame a STORE request.
/// @param request The fields to send.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeStore(StoreRequest const& request, WireVersion version = CurrentVersion)
{
    return EncodeStoreAs(DaemonCacheVerbs, request, version);
}

/// Frame a FETCH request.
/// @param key The key to look up.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeFetch(std::string_view key, WireVersion version = CurrentVersion)
{
    return EncodeFetchAs(DaemonCacheVerbs, key, version);
}

/// Frame a CACHE-DROP request.
/// @param key The key to remove from the tier that answers.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeCacheDrop(std::string_view key, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::CacheDrop, { AsBytes(key) });
}

/// Frame an AUTH request.
/// @param request The credential to present.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeAuth(AuthRequest const& request, WireVersion version = CurrentVersion)
{
    auto const kind = std::array { static_cast<std::byte>(request.kind) };
    return Detail::EncodeRequest(
        version, Op::Auth, { std::span<std::byte const> { kind }, AsBytes(request.username), AsBytes(request.secret) });
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

/// Frame a progress pulse: five bytes, and nothing else.
///
/// A named encoder rather than `EncodeReply(Status::Progress, {})` written at the one
/// site that sends it, because the empty payload is a CONTRACT
/// (`Status::Progress` says why) and not an argument a caller chose. With no parameter
/// there is nothing to pass, so the frame cannot acquire a payload by somebody
/// deciding a diagnostic would be useful here.
/// @return The framed pulse.
[[nodiscard]] inline std::vector<std::byte> EncodeProgressReply()
{
    return EncodeReply(Status::Progress, {});
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
    if (!IsKnownStatus(raw))
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

/// Split a CACHE-DROP payload into its single key field.
/// @param payload The bytes following the request header.
/// @return The key, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::span<std::byte const>> DecodeCacheDropPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::CacheDrop));
    if (!fields.has_value())
        return std::nullopt;
    return (*fields)[0];
}

/// Split an AUTH payload into its kind and its two credential fields.
///
/// Malformed unless the kind is exactly one byte naming a row of `KnownAuthKinds`,
/// and unless a machine ticket arrives with an empty username: a ticket names its
/// machine inside its signed bytes, so a username beside it would be a second,
/// unsigned claim about who is asking.
/// @param payload The bytes following the request header.
/// @return The decoded fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<AuthView> DecodeAuthPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Auth));
    if (!fields.has_value() || (*fields)[0].size() != 1)
        return std::nullopt;
    auto const kind = static_cast<AuthKind>((*fields)[0].front());
    if (!std::ranges::contains(KnownAuthKinds, kind))
        return std::nullopt;
    if (kind == AuthKind::MachineTicket && !(*fields)[1].empty())
        return std::nullopt;
    return AuthView { .kind = kind, .username = (*fields)[1], .secret = (*fields)[2] };
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

/// Encode a `u64` as its own length-prefixed field's contents.
/// @param value The value.
/// @return Exactly eight big-endian bytes.
[[nodiscard]] inline std::array<std::byte, sizeof(std::uint64_t)> EncodeU64Field(std::uint64_t value)
{
    return WireFields::ToBigEndian<std::uint64_t>(value);
}

/// Read a `u64` from a field that must hold exactly eight bytes.
///
/// Strict about the width for the same reason `DecodeU32Field` is: a field of
/// another length is a sender speaking a shape this build does not know.
/// @param field The field.
/// @return The value, or nullopt when the field is not exactly eight bytes.
[[nodiscard]] inline std::optional<std::uint64_t> DecodeU64Field(std::span<std::byte const> field)
{
    return WireFields::FromBigEndian<std::uint64_t>(field);
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

/// The longest codec list a REGISTER carries that a scheduler records, in ids -- one byte each.
///
/// KEPT: the worker entry holds it for as long as the worker heartbeats, and reads it against
/// every lease. Not text, so only the length question reaches it, and refused and counted past it
/// (`fastcached_dispatch_worker_registrations_field_too_long_total`) like the strings beside it.
///
/// Sized from what a list IS: the codecs a build can produce, most-preferred first --
/// `Cc::AvailableCodecs()`, at most the compressed codecs it tries plus `Identity`, three today.
/// The `static_assert` holding this at twice that sits in `fastcache-cc/CodecEnvelope.cpp`, where
/// the list is built; a codec added there that would overrun it fails the build rather than every
/// registration.
inline constexpr std::size_t MaxCodecListIds = 16;

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

/// How many tiers a sender of this build puts in a `PerTier` list: `StorageTier`'s count.
///
/// Named here because this header names no `Cache/` type, and a payload budget has to know the
/// worst case it adds up (`MaxNodeAnnounceOtherBytes`). `CompileCacheWire_test` asserts it equals
/// the enum's count, so a third tier fails a build rather than a budget.
inline constexpr std::size_t CarriedCacheTiers = 2;

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
            // Two clauses that are ONE bound: the shorter of the table and what the wire
            // carried. A field this build does not know about is not read, and a field the
            // peer did not send is left at its default.
            for (auto const index: std::views::iota(std::size_t { 0 }, std::min(Members.size(), values->size())))
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

/// The most interface addresses one worker reports; a Windows host with virtual adapters
/// was measured at 26 (`Platform/LocalAddresses.hpp`).
inline constexpr std::size_t MaxInterfaceAddresses = 32;

/// The longest address text one entry may be: an IPv6 literal with a scope suffix fits.
inline constexpr std::size_t MaxInterfaceAddressBytes = 64;

static_assert((MaxInterfaceAddresses * (MaxInterfaceAddressBytes + WireFields::FieldPrefixSize))
                      + WireFields::FieldPrefixSize
                  <= MaxControlPayload / 16,
              "a full address list must leave a heartbeat room for its history and everything else");

/// Whether one interface address may travel: non-empty and at most `MaxInterfaceAddressBytes`.
///
/// ONE predicate for both ends, because they must agree: the encoder skips what fails it and
/// the decoder refuses it. An encoder that sent an entry its own decoder refuses would lose
/// the WHOLE record it rides in -- a REGISTER or HEARTBEAT -- over one odd adapter name, and
/// the worker would vanish from the fleet.
/// @param bytes The entry's length in bytes.
/// @return True when an entry of that length is one the wire carries.
[[nodiscard]] constexpr bool IsCarriedInterfaceAddress(std::size_t bytes) noexcept
{
    return bytes != 0 && bytes <= MaxInterfaceAddressBytes;
}

/// Frame a worker's interface addresses as one nested field list.
/// @param addresses What the worker answers on. An entry `IsCarriedInterfaceAddress` rejects
///                  is skipped, and of the rest at most `MaxInterfaceAddresses` are taken,
///                  the first ones.
/// @return The nested list's bytes; empty for an empty list.
[[nodiscard]] inline std::vector<std::byte> EncodeAddressList(std::span<std::string const> addresses)
{
    auto taken = addresses
                 | std::views::filter([](std::string const& address) { return IsCarriedInterfaceAddress(address.size()); })
                 | std::views::take(MaxInterfaceAddresses);
    std::vector<std::span<std::byte const>> fields;
    fields.reserve(std::min(addresses.size(), MaxInterfaceAddresses));
    for (auto const& address: taken)
        fields.push_back(AsBytes(address));
    return WireFields::Encode(WireFields::FieldList { fields });
}

/// Read a worker's interface addresses back, OWNED: both records carrying them are
/// returned by value, and a view here would dangle the moment the encoding died.
///
/// Strict, as `DecodeExclusions` is: more than the cap, an empty entry or one longer
/// than an address can be is refused rather than truncated, and the walk stops at the
/// cap, so a list of thousands of empty entries costs what a list one too long does.
/// @param field The nested list's bytes; empty reads as an empty list.
/// @return The addresses, or nullopt when the list is malformed.
[[nodiscard]] inline std::optional<std::vector<std::string>> DecodeAddressList(std::span<std::byte const> field)
{
    if (field.empty())
        return std::vector<std::string> {};
    auto const parts = WireFields::SplitAtMost(field, MaxInterfaceAddresses);
    if (!parts.has_value())
        return std::nullopt;
    std::vector<std::string> out;
    out.reserve(parts->size());
    for (auto const part: *parts)
    {
        if (!IsCarriedInterfaceAddress(part.size()))
            return std::nullopt;
        out.emplace_back(AsStringView(part));
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

    /// What a person calls this machine, e.g. `buildnode-3`. Empty means "did not say".
    ///
    /// **A LABEL, and nothing may decide from it**
    /// ([#1024](https://github.com/LASTRADA-Software/fastcached/issues/1024)). A node's
    /// identity is minted into its state directory and is what the cluster admits and
    /// counts votes against; this exists so an operator looking at that opaque id knows
    /// which box to walk to. Nothing keys on it, compares it, routes by it or admits by
    /// it -- and that is the whole reason it is safe to carry a value the hostname
    /// supplies, which is mutable, not unique per node, and the very property the
    /// identity was deliberately not built on. The moment anything decides from it,
    /// every problem #1024 removes comes back.
    ///
    /// Node-wide rather than per entry, like `version` and unlike `toolchainLabel`: a
    /// machine with two toolsets is one machine with one name.
    ///
    /// **Owned, not a view**, for the reason `version` documents at length.
    std::string displayName {};

    /// What this machine answers on right now, loopback excluded, for the scheduler's
    /// check that a dial hint is an address the worker reports as its own.
    ///
    /// Refreshed on every HEARTBEAT through `LoadFields::interfaceAddresses`, because a
    /// VPN address moves while the process runs. Bounded by `MaxInterfaceAddresses` and
    /// `MaxInterfaceAddressBytes`: the encoder skips an entry `IsCarriedInterfaceAddress`
    /// rejects, and a received list past either bound is refused with the record.
    ///
    /// **Owned, not a view**, for `version`'s reason.
    std::vector<std::string> interfaceAddresses {};
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
    auto const addresses = EncodeAddressList(capacity.interfaceAddresses);
    return WireFields::Encode({ std::span<std::byte const> { cores },
                                std::span<std::byte const> { memory },
                                std::span<std::byte const> { nodeClass },
                                reserve,
                                std::span<std::byte const> { cache },
                                AsBytes(capacity.version),
                                std::span<std::byte const> { reservedMemory },
                                AsBytes(capacity.toolchainLabel),
                                AsBytes(capacity.displayName),
                                std::span<std::byte const> { addresses } });
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
    out.displayName = std::string { AsStringView(at(8)) };
    // Absent from a peer older than the field, which `at` answers as empty and the list
    // reads as no addresses; a malformed list is refused with the record.
    auto addresses = DecodeAddressList(at(9));
    if (!addresses.has_value())
        return std::nullopt;
    out.interfaceAddresses = *std::move(addresses);
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

/// The most unreachable workers one LEASE may name.
///
/// A CLIENT's memo, sent so the scheduler does not grant the worker the client just
/// failed to reach. Sixteen: a launcher remembers an unreachable worker for about a
/// minute, and a fleet losing more than sixteen machines to one client inside a minute
/// is a partition that the scheduler's own heartbeat expiry already sees. The
/// launcher's memo capacity is asserted equal to this.
inline constexpr std::size_t MaxLeaseExclusions = 16;

/// The longest endpoint one exclusion may spell: a 253-byte DNS name, two brackets
/// and `:65535`.
inline constexpr std::size_t MaxExcludedEndpointBytes = 261;

/// The whole list fits a LEASE with room to spare beside the fingerprint and key.
static_assert((MaxLeaseExclusions * (MaxExcludedEndpointBytes + WireFields::FieldPrefixSize)) + WireFields::FieldPrefixSize
                  <= MaxControlPayload / 8,
              "a full exclusion list must leave a LEASE room for everything else it carries");

/// The longest toolchain label a scheduler records, in bytes: a LEASE's `toolchainLabel` and a
/// REGISTER's `CapacityFields::toolchainLabel` alike.
///
/// A label is KEPT -- the leader remembers what a client refused `no-worker` called its toolchain,
/// for `unserved-toolchain`, and a worker entry what its compiler is called -- so it is bounded
/// where it enters, apart from the frame's own `MaxControlPayload`: text a peer chose, rendered
/// into a condition, a page and the JSON a script parses. A lease or a registration carrying a
/// longer one is refused and counted (`fastcached_dispatch_leases_field_too_long_total`,
/// `fastcached_dispatch_worker_registrations_field_too_long_total`), never truncated: a scheduler
/// that cut it short would be a second author of what the peer said.
///
/// Sized from what a label IS. `Cc::ToolchainLabel` writes the compiler's file name and one version
/// token -- `cl 19.44.35207`, `aarch64-none-elf-gcc 12.2.0` -- and the longest real names are the
/// target-prefixed cross drivers, a little under forty bytes. The `static_assert` below holds the
/// bound at twice the longest of them or more, so a toolchain a little longer than any listed still
/// fits; a name longer than that is not one a person reads anyway.
inline constexpr std::size_t MaxToolchainLabelBytes = 128;

static_assert(std::ranges::all_of(std::to_array<std::string_view>({ "cl 19.44.35207",
                                                                    "clang-cl 22.1.3",
                                                                    "g++-13 13.3.0",
                                                                    "clang++ 17.0.6",
                                                                    "aarch64-none-elf-gcc 12.2.0",
                                                                    "x86_64-w64-mingw32-g++-posix 13.2.0",
                                                                    "arm-none-linux-gnueabihf-g++ 13.3.1",
                                                                    "powerpc64le-linux-gnu-g++-14 14.2.0",
                                                                    "x86_64-unknown-linux-gnu-clang++ 18.1.8" }),
                                  [](std::string_view label) { return label.size() * 2 <= MaxToolchainLabelBytes; }),
              "MaxToolchainLabelBytes must hold twice the longest real toolchain label");

/// The longest toolchain fingerprint a scheduler records, in bytes: a LEASE's and a REGISTER's.
///
/// KEPT, as the label is: a lease refused `no-worker` is remembered under its fingerprint and named
/// by `unserved-toolchain`, so without its own ceiling that record is sixteen times the frame's
/// 64 KiB of text a peer chose; and a worker entry is filed under the fingerprint it registered.
/// Refused and counted past it, like the label.
///
/// Sized from what a fingerprint IS. `ComputeToolchainFingerprint` -- one translation unit, which
/// the launcher and the worker both compile -- renders a `KeyDigest`: `KeyDigest::HexLength`, 32
/// lowercase hex characters. 64 is twice that, room for a digest twice as wide before this bound
/// has to move. The `static_assert` holding it there sits in `fastcache-cc/Dispatch.cpp`, where the
/// lease is built and both this bound and `KeyDigest` are visible; this header may not include an
/// app's. A worker's pinned `<fingerprint>=<compiler>` is a copy of a client's digest, or it
/// matches no lease at any length -- so a pin longer than this was already a worker nothing picks,
/// and is now one the scheduler says so to.
inline constexpr std::size_t MaxToolchainFingerprintBytes = 64;

/// The longest `LeaseRequest::key` a scheduler records, in bytes.
///
/// KEPT by the lease table for as long as the lease lives, and listed among `/fleet.json`'s
/// outstanding leases. The launcher's key is a `KeyDigest` too (`objkey-v*`, `fastcache-cc/CacheKey.cpp`),
/// so it is sized as the fingerprint is and held there by the same `static_assert`.
inline constexpr std::size_t MaxLeaseKeyBytes = 64;

/// The longest endpoint a REGISTER or a NODE-ANNOUNCE names that a scheduler records, in bytes.
///
/// KEPT: it is what a worker entry and a machine's row are filed under, where a granted lease
/// sends a client, and a column of the fleet page and `/fleet.json`. Refused and counted past it,
/// never truncated -- a shortened endpoint is an address nothing answers on.
///
/// Sized from what an endpoint IS: a host, a colon and a port. The longest host anything can dial
/// is a DNS name, 253 characters as text and 254 with the root's trailing dot (RFC 1035's 255
/// octets on the wire); a bracketed IPv6 literal with a zone is a third of that. So 254 and
/// `:65535`. No producer is visible to a `static_assert`: a node advertises what an operator typed
/// into `--advertise`, or its listen surface's host, and a host longer than this is one no resolver
/// answers for.
inline constexpr std::size_t MaxEndpointBytes = 254 + std::string_view { ":65535" }.size();

/// The longest version a REGISTER or a NODE-ANNOUNCE states that a scheduler records, in bytes.
///
/// KEPT by the worker entry and the machine's row, and rendered as the fleet page's version column
/// -- read most during a rolling upgrade, when a peer this fleet did not build is likeliest to fill
/// it with something surprising.
///
/// Sized from what a version IS: `FastCache::VersionString`, which `cmake/Version.cmake` writes as
/// `X.Y.Z`, as `X.Y.Z-<distance>-g<commit>` between tags and with `-dirty` on an unclean tree --
/// under fifty bytes at any abbreviation git picks for a repository this size -- or a vendor's
/// `-DFASTCACHED_VERSION_STRING`. The `static_assert` holding this at twice the running build's
/// sits in `fastcache-compile-node/WorkerTier.cpp`, where the version is put on the wire; this
/// header may not include the generated `Version.hpp`.
inline constexpr std::size_t MaxNodeVersionBytes = 128;

/// The longest display name a REGISTER carries that a scheduler records, in bytes.
///
/// KEPT by the worker entry and rendered as the fleet page's name column (#1024) -- a label that
/// decides nothing, and text a peer chose all the same.
///
/// Sized from what a display name IS: the host's own name, which `QueryHostFacts` reads into a
/// buffer of `MaxHostNameBytes` and a terminator, so it can be no longer -- and 255 is DNS's own
/// ceiling on a name, above what either platform's call returns for a host. The `static_assert`
/// holding the two together sits in `fastcache-compile-node/WorkerTier.cpp`, where the name is put
/// on the wire.
inline constexpr std::size_t MaxDisplayNameBytes = 255;

/// A client asking the scheduler where to compile.
struct LeaseRequest
{
    std::string_view fingerprint; ///< The toolchain the client is compiling with.
    std::string_view key;         ///< The object key, for duplicate suppression.
    CodecList acceptedCodecs;     ///< What the client can decode.
    /// Workers this client could not reach moments ago, by the endpoint they
    /// ADVERTISE, newest first. They narrow THIS request's pick and nothing else.
    std::span<std::string_view const> excluded {};

    /// What a person calls that toolchain, e.g. `cl 19.44.35207`; empty when the client did not say.
    ///
    /// **Display only, and never an identity** -- `CapacityFields::toolchainLabel`'s rule, from the
    /// client's end: the fingerprint decides every match. It travels so a scheduler with no worker
    /// for the fingerprint can NAME what nobody serves (`unserved-toolchain`); the digest is opaque
    /// by design (#194), so without the client's own words an operator is shown a hash. Defaulted
    /// and LAST, so a caller with nothing to say does not spell it.
    ///
    /// UTF-8 and at most `MaxToolchainLabelBytes`, or the scheduler refuses the WHOLE lease: it is
    /// kept, and what is kept is gated where it enters -- as the key and the fingerprint are, against
    /// `MaxLeaseKeyBytes` and `MaxToolchainFingerprintBytes`. Empty is always acceptable.
    std::string_view toolchainLabel {};
};

/// The same, as views into a received payload.
struct LeaseView
{
    std::span<std::byte const> fingerprint;
    std::span<std::byte const> key;
    CodecList acceptedCodecs;
    std::vector<std::span<std::byte const>> excluded; ///< Borrowed from the payload, newest first.
    std::span<std::byte const> toolchainLabel;        ///< Empty when the client did not say.
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
    /// The translation unit's source PATH, as the CLIENT's own compile would record it.
    ///
    /// A compiler records the name of the file it was handed -- clang-cl and gcc in
    /// the COFF/ELF `.file` symbol, MSVC in its compiland record -- so a worker that
    /// invents a name of its own produces an object that differs from a locally
    /// compiled one in that name and nothing else. Measured on clang-cl: seven bytes,
    /// and byte-identical once the names agree.
    ///
    /// **This said "the BASE NAME only" until #800 made it a path, and the stale
    /// sentence survived the change that falsified it**
    /// ([#907](https://github.com/LASTRADA-Software/fastcached/issues/907)). clang takes
    /// `DW_AT_name` from the INPUT FILE PATH, so a dispatched object recorded the
    /// worker's `<scratch>/job-N/<name>` -- a directory on no machine -- and two
    /// dispatches of ONE translation unit produced byte-differing objects under one key
    /// (#660). Closing that needs the client's own spelling, and that is a path. It
    /// travels already put through the client's own `-fdebug-prefix-map` rules, so what
    /// arrives is the MAPPED spelling when the client maps anything and the raw one when
    /// it does not.
    ///
    /// **So the client's directory IS learned, by design.** The old text said the worker
    /// had "no business learning it", and the half of that which survives is narrower and
    /// still true: no business learning it in order to OPEN it. This is never a path the
    /// worker resolves, and it reaches only the debug record of the object it sends back.
    ///
    /// **Two halves of this one value, sanitized differently, and the split is
    /// load-bearing:**
    ///
    /// - The FILE NAME on disk goes through `SafeSourceName` -- last component only,
    ///   checked stem, forced known extension. That is the untrusted-string-becomes-a-path
    ///   defence, unchanged and still correct.
    /// - The prefix-map RULE's right-hand side is this value VERBATIM, because recording
    ///   the client's spelling is the entire purpose. It reaches a COMMAND LINE, so what
    ///   bounds it is the spellability check and the payload cap, **not** `SafeSourceName`.
    ///
    /// Routing the rule through `SafeSourceName` is the plausible wrong repair, and it
    /// fails silently: the object would record `tu.cpp` instead of the client's path,
    /// with every test that checks the on-disk file name still green. `WorkerSourceNameRule`
    /// says so at the line where it would be made.
    std::string_view sourceName;
    /// The directory the CLIENT's own compile runs in, and what its own
    /// `-fdebug-prefix-map` rules spell that directory as. Both empty when the client
    /// maps nothing; a half-filled pair is malformed and a worker refuses it.
    ///
    /// A compiler with debug info on records the directory it ran in -- DWARF's
    /// `DW_AT_comp_dir` -- and that appears on no command line, so nothing in the cache
    /// key can distinguish two producers by it. #203 closed that for a local compile by
    /// appending mapping rules; a worker was sent none, so a dispatched object recorded
    /// a directory a locally mapped one does not, under the same key
    /// ([#506](https://github.com/LASTRADA-Software/fastcached/issues/506)).
    ///
    /// **WHICH directory a dispatched object records depends on the driver, so both
    /// travel.** Measured on gcc 14.2.0 and clang 20.1.2, one translation unit each way,
    /// reading `DW_AT_comp_dir`:
    ///
    /// | preprocess line | what the worker's object records |
    /// | --- | --- |
    /// | `g++ -E` | the WORKER's directory |
    /// | `g++ -E -g` | the CLIENT's directory |
    /// | `clang++ -E`, `clang++ -E -g` | the WORKER's directory |
    ///
    /// gcc's `-fworking-directory` is implicit under `-g`: it emits a line marker naming
    /// the preprocessing directory, and the worker's compile adopts that as its
    /// compilation directory. clang emits no such marker. So a worker maps BOTH
    /// candidates to the replacement and gets the same answer either way. Mapping only
    /// its own directory fixes clang and leaves gcc recording the client's UNMAPPED
    /// path -- which no object comparison can see and reading `comp_dir` can.
    ///
    /// The client cannot send a finished rule for the worker's half, which is a path it
    /// has never seen. And neither half can ride in `args`: `IsAcceptableJobArgument`
    /// refuses any argument body carrying a path separator, deliberately, and both are
    /// full of them.
    ///
    /// **Empty means the client asked for no mapping, and the worker must then add
    /// none.** A worker that mapped to a token of its own would hand a client that
    /// requested nothing an object recording a directory neither machine has -- the same
    /// asymmetry #506 is about, pointing the other way.
    ///
    /// Neither is a path the worker opens; both reach only the debug records of the
    /// object. They are still peer text that ends up on a command line, so the worker
    /// bounds them and restricts their characters before spelling them. On gcc under
    /// `-g` the client's directory is already inside the preprocessed payload, so this
    /// tells a worker nothing it was not being told anyway.
    std::string_view compileDir;
    /// What the client's own mapping spells `compileDir` as -- `.` for the rules
    /// `cmake/portable/CompileCache.cmake` emits. Empty exactly when `compileDir` is.
    std::string_view compileDirReplacement;

    /// The client's SOURCE root, and what its own `-fdebug-prefix-map` rules spell that
    /// root as. Both empty when the client maps nothing; a half-filled pair is malformed
    /// and a worker refuses it, exactly as the compilation-directory pair above.
    ///
    /// **`sourceName` cannot carry this and that is not an oversight** (#883). #800 makes
    /// it the client's already-MAPPED source spelling, which is ONE value, and a rule needs
    /// TWO operands: what the compiler will emit, and what it should say instead. The
    /// worker still has no business learning that directory in order to OPEN it -- which
    /// is the surviving true half of a clause that also called `sourceName` "a base name,
    /// deliberately" and so contradicted its own next line (#907).
    ///
    /// **It exists because gcc and clang take `DW_AT_name` from different places.**
    /// clang takes it from the input file path, so #800's mapped `sourceName` reaches it.
    /// gcc takes it from the `#line` marker in the preprocessed text, which names the
    /// CLIENT's path and which no rule the worker builds from its own scratch directory
    /// can ever match -- so a dispatched gcc object records the producing checkout's
    /// absolute source path where a local one records the mapped spelling. Measured on
    /// gcc 16.2.1 / clang 22.1.8, `readelf --debug-dump=info`.
    ///
    /// A REPLACEMENT operand, never a path the worker opens -- the rule #660 states for
    /// `sourceName` and the reason this can carry a directory at all.
    std::string_view sourceRoot {};
    std::string_view sourceRootReplacement {};
};

/// The same, as views into a received payload.
struct CompileView
{
    std::span<std::byte const> leaseToken;
    std::span<std::byte const> fingerprint;
    std::span<std::byte const> args;
    std::span<std::byte const> source; ///< Still enveloped; decode with DecodeCodecEnvelope.
    CodecList acceptedCodecs;          ///< What the client can decode.
    /// The client's source PATH -- see `CompileRequest::sourceName` for what it is and
    /// why it is not a base name. `SafeSourceName` it before it names a FILE; the
    /// prefix-map rule takes it RAW, deliberately.
    std::span<std::byte const> sourceName;
    /// The client's own compile directory and what its mapping spells it as; both
    /// empty when the client maps nothing. Bound and restrict them before either
    /// reaches a command line.
    std::span<std::byte const> compileDir;
    std::span<std::byte const> compileDirReplacement;

    /// The client's source root and its mapped spelling. See `CompileRequest` for why
    /// `sourceName` cannot carry this: that one is a base name and is already mapped,
    /// which is one value where a rule needs two (#883).
    std::span<std::byte const> sourceRoot {};
    std::span<std::byte const> sourceRootReplacement {};
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

/// What a scheduler tells a worker in answer to REGISTER.
///
/// This **owns** its strings rather than viewing the decoded buffer, and that is the
/// rule from [`.agent/rules/wire-and-protocol.md`](../../../.agent/rules/wire-and-protocol.md)
/// applied rather than a preference: the worker holds every one of these fields for
/// the rest of its life -- the id goes in each heartbeat, the identity into every
/// lease expectation -- so the record outlives the bytes it was decoded from by a
/// long way. `DecodeRegisterReply(EncodeRegisterReply(x))` is the obvious spelling
/// and would be a use-after-free the moment a member became a view.
struct RegisterReplyFields
{
    /// The id the scheduler assigned; what every later heartbeat names.
    std::string workerId;
    /// Which fleet this scheduler leads. The worker pins this and refuses a lease
    /// token bound to any other, which is the whole of #401: the worker is TOLD,
    /// by the scheduler it was configured to reach, rather than inferring it.
    std::string clusterId;
    /// The scheduler term this registration belongs to.
    std::uint64_t epoch = 0;
};

/// Encode a REGISTER reply's payload.
///
/// A nested record read with the variable-arity split, like the capacity record
/// inside the REGISTER request: a field this build has not heard of is skipped and
/// one it expects but was not sent keeps its default. That is what keeps the NEXT
/// fact addable without a fourth version, which is worth having precisely because
/// this change cost a bump.
/// @param reply The facts to send.
/// @return The payload bytes, to be carried as the reply's body.
[[nodiscard]] inline std::vector<std::byte> EncodeRegisterReply(RegisterReplyFields const& reply)
{
    auto const epoch = EncodeU64Field(reply.epoch);
    return WireFields::Encode({ AsBytes(reply.workerId), AsBytes(reply.clusterId), std::span<std::byte const> { epoch } });
}

/// Read a REGISTER reply's payload back.
/// @param payload The reply body.
/// @return The facts, or nullopt when the record is malformed or the epoch is not
///         exactly eight bytes.
[[nodiscard]] inline std::optional<RegisterReplyFields> DecodeRegisterReply(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitAll(payload);
    if (!fields.has_value() || fields->size() < 3)
        return std::nullopt;
    auto const epoch = DecodeU64Field((*fields)[2]);
    if (!epoch.has_value())
        return std::nullopt;
    return RegisterReplyFields { .workerId = std::string { AsStringView((*fields)[0]) },
                                 .clusterId = std::string { AsStringView((*fields)[1]) },
                                 .epoch = *epoch };
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
inline constexpr std::size_t HistorySlotCount = 10;

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
inline constexpr std::size_t HistoryPayloadShare = MaxControlPayload / 2;

static_assert(MaxHistoryBucketsPerHeartbeat * MaxHistoryBucketBytes <= HistoryPayloadShare,
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

    /// Whether an operator cordoned this worker (#1303): it takes no new compile, and the
    /// scheduler must not pick it.
    ///
    /// **A heartbeat fact, and that is what keeps the cordon unreplicated.** It is the
    /// worker's own process state, restated every beat, so a new leader learns it from the
    /// next one and a restarted worker -- which is not cordoned -- corrects every registry
    /// that believed it was. Absent travels as a zero-length field and reads as serving,
    /// which is what a peer older than this field is.
    bool cordoned { false };

    /// What this machine's conditions are (#1364), or ABSENT from a sender that says nothing
    /// about them.
    ///
    /// **Carried by `NodeAnnounce` and by nothing else.** The history rule applies here for the
    /// same reason: a heartbeat is a WORKER's and a workerless machine sends none, while every
    /// node announces itself -- so the one verb that reaches the leader from every machine is the
    /// one that carries what is wrong with it. A heartbeat leaves this disengaged, and the leader
    /// does not read it there.
    ///
    /// Here rather than as a fourth `NodeAnnounce` field for the reason `history` is: that verb's
    /// top-level arity is exact, and this record is the variable-arity one. Absent travels as a
    /// zero-length field, which is also what every peer older than the field sends, so the leader
    /// renders such a machine ABSENT rather than as one with nothing raised.
    std::optional<std::vector<NodeConditionFields>> conditions {};

    /// What this machine answers on right now, loopback excluded -- the heartbeat copy of
    /// `CapacityFields::interfaceAddresses`, restated every beat because a VPN address
    /// moves while the process runs. Empty from a peer that says nothing about them.
    ///
    /// **Owned, not a view**, for `CapacityFields::version`'s reason: this record is
    /// returned by value too.
    std::vector<std::string> interfaceAddresses {};
};

namespace Detail
{
    /// @param bytes What a field holds.
    /// @return What it costs on the wire, its length prefix included.
    [[nodiscard]] consteval std::size_t FramedField(std::size_t bytes) noexcept
    {
        return WireFields::FieldPrefixSize + bytes;
    }
} // namespace Detail

/// The longest address list `EncodeAddressList` writes, its own prefix excluded.
inline constexpr std::size_t MaxAddressListBytes = MaxInterfaceAddresses * Detail::FramedField(MaxInterfaceAddressBytes);

/// The longest capacity record `EncodeCapacity` writes, field by field in its order: cores, memory,
/// class, reserved cores, the cache record (one field holding the tier list, each tier one u64),
/// version, reserved memory, toolchain label, display name, interface addresses.
inline constexpr std::size_t MaxCapacityRecordBytes =
    Detail::FramedField(sizeof(std::uint32_t)) + Detail::FramedField(sizeof(std::uint64_t)) + Detail::FramedField(1)
    + Detail::FramedField(sizeof(std::uint32_t))
    + Detail::FramedField(
        Detail::FramedField(CarriedCacheTiers * Detail::FramedField(Detail::FramedField(sizeof(std::uint64_t)))))
    + Detail::FramedField(MaxNodeVersionBytes) + Detail::FramedField(sizeof(std::uint64_t))
    + Detail::FramedField(MaxToolchainLabelBytes) + Detail::FramedField(MaxDisplayNameBytes)
    + Detail::FramedField(MaxAddressListBytes);

/// The longest load record `EncodeLoad` writes WITHOUT its two variable passengers, whose own
/// shares cover them: CPU, memory, scratch, the cache record (the tier list, each tier four u64s,
/// then hits and misses), the history field's prefix, the cordon byte, the conditions field's
/// prefix, interface addresses.
inline constexpr std::size_t MaxLoadRecordFixedBytes =
    Detail::FramedField(sizeof(std::uint32_t)) + Detail::FramedField(sizeof(std::uint64_t))
    + Detail::FramedField(sizeof(std::uint64_t))
    + Detail::FramedField(
        Detail::FramedField(CarriedCacheTiers * Detail::FramedField(4 * Detail::FramedField(sizeof(std::uint64_t))))
        + Detail::FramedField(sizeof(std::uint64_t)) + Detail::FramedField(sizeof(std::uint64_t)))
    + Detail::FramedField(0) + Detail::FramedField(1) + Detail::FramedField(0) + Detail::FramedField(MaxAddressListBytes);

/// The most join memos one NODE-ANNOUNCE carries: as many as a formation record keeps
/// (`Cluster::MaxAskedJoins`, asserted equal where both are visible), so a node hands over every one.
inline constexpr std::size_t MaxAnnouncedJoinMemos = 8;

/// The longest join-memo list `EncodeJoinMemos` writes, its own prefix excluded: every memo a nested
/// cluster id at `MaxIdBytes`, which `DecodeJoinMemos` refuses past, and a key.
inline constexpr std::size_t MaxJoinMemoListBytes =
    MaxAnnouncedJoinMemos
    * Detail::FramedField(Detail::FramedField(MaxIdBytes) + Detail::FramedField(IdentityPublicKeyBytes));

/// Everything a NODE-ANNOUNCE carries besides its history batch and its condition list, at its
/// longest: the endpoint, the capacity record, the load record's fixed fields, the join memos.
inline constexpr std::size_t MaxNodeAnnounceOtherBytes =
    Detail::FramedField(MaxEndpointBytes) + Detail::FramedField(MaxCapacityRecordBytes)
    + Detail::FramedField(MaxLoadRecordFixedBytes) + Detail::FramedField(MaxJoinMemoListBytes);

/// ONE budget for the verb, summed, rather than two fractions each checked alone (which said
/// nothing about whether the rest still fit). Every term is a constant, never a figure restated here:
///   history     `HistoryPayloadShare`, which holds a full batch of
///               `MaxHistoryBucketsPerHeartbeat` x `MaxHistoryBucketBytes`;
///   conditions  `ConditionPayloadShare`, which holds `MaxNodeConditionListBytes` and is what the
///               other two leave;
///   the rest    `MaxNodeAnnounceOtherBytes`, every other field at its ceiling.
/// `CompileCacheWire_test` encodes the worst case of all three, asserts its size is exactly what
/// these constants say, and decodes it.
/// The part of `MaxControlPayload` a node's condition list may take: everything the history share
/// and every other field leave, so the verb's budget below holds with no slack.
///
/// DERIVED rather than a fraction, so every row keeps its words -- a remedy is what an operator
/// reads, held to `MaxConditionRemedyBytes` -- and the row count is whatever that leaves room for:
/// `MaxNodeConditions` is asserted to be the most full rows this share holds. A further row needs a
/// smaller history share, fewer other bytes, or shorter ceilings; nothing here can simply grow.
inline constexpr std::size_t ConditionPayloadShare = MaxControlPayload - HistoryPayloadShare - MaxNodeAnnounceOtherBytes;

static_assert(MaxNodeConditionListBytes <= ConditionPayloadShare,
              "a node's conditions must fit the share of the payload they are given");

// The bound is the share's, exactly: `NodeConditionWire.hpp` cannot name this share (it is derived
// from fields this header defines after including it), so the literal there is held to it here.
static_assert(MaxNodeConditions == ConditionPayloadShare / MaxNodeConditionRowBytes(),
              "MaxNodeConditions must be the most full rows ConditionPayloadShare holds");

static_assert(HistoryPayloadShare + ConditionPayloadShare + MaxNodeAnnounceOtherBytes <= MaxControlPayload,
              "a NODE-ANNOUNCE's history, conditions and everything else must fit one control payload together");

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
    auto constexpr Cordoned = std::array { std::byte { 1 } };
    // Absent as zero length; an engaged list is never empty -- see `EncodeNodeConditions`.
    auto const conditions = load.conditions.has_value() ? EncodeNodeConditions(*load.conditions) : std::vector<std::byte> {};
    auto const addresses = EncodeAddressList(load.interfaceAddresses);
    return WireFields::Encode(
        { load.cpuBusyPermille.has_value() ? std::span<std::byte const> { cpu } : std::span<std::byte const> {},
          load.availableMemoryBytes.has_value() ? std::span<std::byte const> { memory } : std::span<std::byte const> {},
          load.freeScratchBytes.has_value() ? std::span<std::byte const> { scratch } : std::span<std::byte const> {},
          std::span<std::byte const> { cache },
          std::span<std::byte const> { history },
          load.cordoned ? std::span<std::byte const> { Cordoned } : std::span<std::byte const> {},
          std::span<std::byte const> { conditions },
          std::span<std::byte const> { addresses } });
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
    // One byte when cordoned; absent from a serving worker and from any peer older than
    // the field. Any other width, or a byte that is not the one the encoder writes, is a
    // shape this build does not know and is refused rather than read as either answer.
    if (auto const cordoned = at(5); !cordoned.empty())
    {
        if (cordoned.size() != 1 || cordoned[0] != std::byte { 1 })
            return std::nullopt;
        out.cordoned = true;
    }
    // Absent from every heartbeat and from any peer older than the field, which `at()` answers
    // as an empty span; a malformed list is refused with the record, as a malformed field is.
    if (!ReadNodeConditions(at(6), out.conditions))
        return std::nullopt;
    // Absent from a peer older than the field and read as no addresses; a malformed list is
    // refused with the record.
    auto addresses = DecodeAddressList(at(7));
    if (!addresses.has_value())
        return std::nullopt;
    out.interfaceAddresses = *std::move(addresses);
    return out;
}

/// What a node says about itself, whatever it runs.
///
/// The ENDPOINT is the machine's identity here, as it is for a worker entry -- it is what an
/// operator means by *a node*, and `NodeReports()` already groups by it. No fingerprint and no
/// slots, for `Op::NodeAnnounce`'s reasons.
/// One fleet a node once asked to admit it, as its NODE-ANNOUNCE hands it to the leader: the
/// evidence a split of the node's fleet is told to an operator on, which the leader must hold whichever
/// machine asked.
///
/// **Owns its fields**: the leader keeps it past the frame that carried it.
struct JoinMemoFields
{
    std::string clusterId;                                      ///< The fleet asked.
    std::array<std::byte, IdentityPublicKeyBytes> provenKey {}; ///< The key that proved it when it was asked.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(JoinMemoFields const&, JoinMemoFields const&) = default;
};

/// Encode join memos as ONE nested field: each memo a nested `[cluster id, key]`.
/// @param memos The memos; at most `MaxAnnouncedJoinMemos`.
/// @return The field.
[[nodiscard]] inline std::vector<std::byte> EncodeJoinMemos(std::span<JoinMemoFields const> memos)
{
    assert(memos.size() <= MaxAnnouncedJoinMemos && "a record keeps no more memos than an announcement carries");
    auto encoded = std::vector<std::vector<std::byte>> {};
    encoded.reserve(memos.size());
    for (auto const& memo: memos)
        encoded.push_back(WireFields::Encode({ AsBytes(memo.clusterId), std::span<std::byte const> { memo.provenKey } }));
    auto views = std::vector<std::span<std::byte const>> { encoded.begin(), encoded.end() };
    return WireFields::Encode(WireFields::FieldList { views });
}

/// Read join memos back.
///
/// Refuses more than `MaxAnnouncedJoinMemos`, a memo that is not exactly two fields, an EMPTY cluster
/// id or one past `MaxIdBytes`, and a key that is not exactly one wide -- a prefix of a key is a
/// different key, and a memo holding one would match nothing or the wrong fleet.
/// @param field The nested field `EncodeJoinMemos` wrote; empty for none.
/// @return The memos, owned, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::vector<JoinMemoFields>> DecodeJoinMemos(std::span<std::byte const> field)
{
    auto const parts = WireFields::Detail::SplitUpTo(field, MaxAnnouncedJoinMemos + 1);
    if (!parts.has_value() || parts->size() > MaxAnnouncedJoinMemos || WireFields::EncodedSize(*parts) != field.size())
        return std::nullopt;
    auto memos = std::vector<JoinMemoFields> {};
    memos.reserve(parts->size());
    for (auto const part: *parts)
    {
        auto const fields = WireFields::SplitExactly(part, 2);
        if (!fields.has_value() || (*fields)[0].empty() || (*fields)[0].size() > MaxIdBytes
            || (*fields)[1].size() != IdentityPublicKeyBytes)
            return std::nullopt;
        auto memo = JoinMemoFields { .clusterId = std::string { AsStringView((*fields)[0]) } };
        std::ranges::copy((*fields)[1], memo.provenKey.begin());
        memos.push_back(std::move(memo));
    }
    return memos;
}

struct NodeAnnounceRequest
{
    /// Where this machine answers, as it would register.
    std::string_view endpoint;

    /// The machine's registration facts: cores, memory, class, cache budget and version.
    ///
    /// The SAME nested record `Register` carries rather than a second spelling of the same
    /// facts. Two records would be two places for *what is this machine* to be wrong, and the
    /// fleet page renders them through one column table either way.
    CapacityFields capacity {};

    /// What it is doing now, and the history buckets it is handing over.
    ///
    /// A machine with no worker still has a CPU, a memory figure and a cache, so this is not
    /// an empty passenger on such a node -- and `load.history` is how the batch travels, which
    /// is the second half of #1440: a node whose history rode the worker heartbeat handed over
    /// nothing at all when it had no worker.
    LoadFields load {};

    /// The fleets this node once asked to admit it (`EncodeJoinMemos`): how the leader holds every
    /// member's evidence that a fleet it sees is this one split, whichever machine did the asking.
    /// A top-level field rather than part of `load`: it is not a reading of the machine.
    std::span<JoinMemoFields const> joinMemos {};
};

/// Frame a NODE-ANNOUNCE request.
/// @param request What the node says about itself.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeAnnounce(NodeAnnounceRequest const& request,
                                                               WireVersion version = CurrentVersion)
{
    auto const capacity = EncodeCapacity(request.capacity);
    auto const load = EncodeLoad(request.load);
    auto const memos = EncodeJoinMemos(request.joinMemos);
    return Detail::EncodeRequest(version,
                                 Op::NodeAnnounce,
                                 { AsBytes(request.endpoint),
                                   std::span<std::byte const> { capacity },
                                   std::span<std::byte const> { load },
                                   std::span<std::byte const> { memos } });
}

/// A node's announcement of itself, as received.
///
/// The endpoint BORROWS -- every consumer reads it inside the handler that decoded it -- while
/// the two nested records are decoded into owned values, exactly as `RegisterView` and
/// `HeartbeatView` do. Handing back raw spans instead would push the nested decoding into each
/// caller, which is where two callers come to disagree about whether an ABSENT record is a
/// malformed one. `DecodeCapacity` answers that question once, and its answer is *no*.
struct NodeAnnounceView
{
    std::span<std::byte const> endpoint;
    CapacityFields capacity {};
    LoadFields load {};
    std::vector<JoinMemoFields> joinMemos; ///< Owned: the leader keeps them past the frame.
};

/// Split a NODE-ANNOUNCE payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<NodeAnnounceView> DecodeNodeAnnouncePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::NodeAnnounce));
    if (!fields.has_value())
        return std::nullopt;
    auto capacity = DecodeCapacity((*fields)[1]);
    if (!capacity.has_value())
        return std::nullopt;
    auto load = DecodeLoad((*fields)[2]);
    if (!load.has_value())
        return std::nullopt;
    auto memos = DecodeJoinMemos((*fields)[3]);
    if (!memos.has_value())
        return std::nullopt;
    return NodeAnnounceView {
        .endpoint = (*fields)[0], .capacity = *std::move(capacity), .load = *std::move(load), .joinMemos = *std::move(memos)
    };
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

/// Frame a WITHDRAW request.
///
/// One field, and the id rather than the entry it names: see `Op::Withdraw` for why
/// that is a security decision and not a shape one.
/// @param workerId The id the scheduler issued at registration.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeWithdraw(std::string_view workerId, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::Withdraw, { AsBytes(workerId) });
}

/// A worker retiring one of its registrations.
struct WithdrawView
{
    std::span<std::byte const> workerId;
};

/// Split a WITHDRAW payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<WithdrawView> DecodeWithdrawPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Withdraw));
    if (!fields.has_value())
        return std::nullopt;
    return WithdrawView { .workerId = (*fields)[0] };
}

/// Frame a client's exclusion list as one nested field list.
/// @param excluded Endpoints, newest first; at most `MaxLeaseExclusions` are taken,
///                 the newest ones.
/// @return The nested list's bytes; empty for an empty list.
[[nodiscard]] inline std::vector<std::byte> EncodeExclusions(std::span<std::string_view const> excluded)
{
    auto const taken = excluded.first(std::min(excluded.size(), MaxLeaseExclusions));
    std::vector<std::span<std::byte const>> fields;
    fields.reserve(taken.size());
    for (auto const endpoint: taken)
        fields.push_back(AsBytes(endpoint));
    return WireFields::Encode(WireFields::FieldList { fields });
}

/// Read an exclusion list back.
///
/// Strict, because every entry decides what a scheduler skips: more than the cap, an
/// empty entry or one longer than an endpoint can be is a peer this build does not
/// understand, refused rather than truncated. The walk stops at the cap, so a list
/// of thousands of empty entries costs what a list one too long does.
/// @param field The nested list's bytes.
/// @return The entries, borrowed from @p field, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::vector<std::span<std::byte const>>> DecodeExclusions(
    std::span<std::byte const> field)
{
    auto parts = WireFields::SplitAtMost(field, MaxLeaseExclusions);
    if (!parts.has_value())
        return std::nullopt;
    if (std::ranges::any_of(*parts, [](auto const& part) { return part.empty() || part.size() > MaxExcludedEndpointBytes; }))
        return std::nullopt;
    return parts;
}

/// Frame a LEASE request.
/// @param request What the client wants to compile and with what.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeLease(LeaseRequest const& request, WireVersion version = CurrentVersion)
{
    auto const codecs = EncodeCodecList(request.acceptedCodecs);
    auto const excluded = EncodeExclusions(request.excluded);
    return Detail::EncodeRequest(version,
                                 Op::Lease,
                                 { AsBytes(request.fingerprint),
                                   AsBytes(request.key),
                                   std::span<std::byte const> { codecs },
                                   std::span<std::byte const> { excluded },
                                   AsBytes(request.toolchainLabel) });
}

/// Split a LEASE payload.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed, the exclusion list included.
[[nodiscard]] inline std::optional<LeaseView> DecodeLeasePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Lease));
    if (!fields.has_value())
        return std::nullopt;
    auto excluded = DecodeExclusions((*fields)[3]);
    if (!excluded.has_value())
        return std::nullopt;
    return LeaseView { .fingerprint = (*fields)[0],
                       .key = (*fields)[1],
                       .acceptedCodecs = DecodeCodecList((*fields)[2]),
                       .excluded = *std::move(excluded),
                       .toolchainLabel = (*fields)[4] };
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

    /// The member's identity key as the operator typed it after `@` (#178), or nothing.
    ///
    /// Its TEXT, the 43-character spelling, rather than its bytes: the request carries what
    /// the operator stated, as it carries the endpoint, and the LEADER reads it through the one
    /// parser whatever client sent it. Absent is disengaged here; the zero-length field is only
    /// how the wire spells it.
    std::optional<std::string_view> publicKey;
};

/// The same, as views into a received payload.
struct ClusterAdmitView
{
    std::span<std::byte const> memberId;
    std::span<std::byte const> raftEndpoint;

    /// The key field, or disengaged when the request stated none (#178). Read out of the
    /// zero-length encoding HERE and nowhere else, so every reader past the decoder sees an
    /// absent key as absent rather than as an empty string.
    std::optional<std::span<std::byte const>> publicKey;
};

/// Whether `op` admits a MEMBER: `ClusterAdmit` (a voter) or `ClusterAdmitLearner` (#1449).
/// @param op A verb.
/// @return True for the two verbs that share `ClusterAdmitRequest` and its receipt.
[[nodiscard]] constexpr bool IsMemberAdmission(Op op) noexcept
{
    return op == Op::ClusterAdmit || op == Op::ClusterAdmitLearner;
}

/// Frame a CLUSTER-ADMIT or CLUSTER-ADMIT-LEARNER request.
///
/// Two fields and not one: an id with no address is a node the cluster counts
/// towards quorum and cannot reach, which is the defect `Cluster::ClusterMember`
/// exists to make unrepresentable. The scheduler endpoint is deliberately absent —
/// a member announces its own, and nothing an operator types about somebody else
/// could supply it.
///
/// One encoder for both verbs, because two copies of a framing differing only by an op
/// code is the repetition the op TABLE exists to remove: the payload is the same, and which
/// set the member is admitted into is the verb's byte. The verb is a template parameter,
/// so naming a third one does not compile.
/// @tparam op `Op::ClusterAdmit` or `Op::ClusterAdmitLearner`, held by the type system.
/// @param request Who to admit, and where it answers.
/// @param version Version to advertise.
/// @return The framed request.
template <Op op>
    requires(IsMemberAdmission(op))
[[nodiscard]] inline std::vector<std::byte> EncodeClusterAdmit(ClusterAdmitRequest const& request,
                                                               WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version,
                                 op,
                                 { AsBytes(request.memberId),
                                   AsBytes(request.raftEndpoint),
                                   AsBytes(request.publicKey.value_or(std::string_view {})) });
}

/// Split a CLUSTER-ADMIT or CLUSTER-ADMIT-LEARNER payload.
/// @tparam op Which of the two verbs the payload belongs to, held the same way.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
template <Op op>
    requires(IsMemberAdmission(op))
[[nodiscard]] inline std::optional<ClusterAdmitView> DecodeClusterAdmitPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(op));
    if (!fields.has_value())
        return std::nullopt;
    auto const key = (*fields)[2];
    return ClusterAdmitView { .memberId = (*fields)[0],
                              .raftEndpoint = (*fields)[1],
                              .publicKey = key.empty() ? std::nullopt : std::optional { key } };
}

/// What a CLUSTER-ADMIT reply carries back: what the leader RECORDED, never what is in
/// force.
///
/// **That distinction is the whole of this record, and a renderer that loses it makes
/// things worse than the silence it replaces.** `SchedulerService::Offer` states why it
/// reports only that a command was appended -- a leader cannot know whether a majority
/// has taken it until one answers -- and this record does not weaken that position. It
/// answers a DIFFERENT question, one the leader can answer with no majority at all:
/// *which bytes did you write into the command*. That answer is known the instant the
/// command is built, and it is the one that catches an address typo.
///
/// So anything rendering this says **recorded** and **as received**, and about what
/// happens next says nothing stronger than **appended and replicating** -- never
/// *admitted*, *added* or *committed*. An echo that reads as *in force* when it is
/// *taken down* is a confident wrong signal, which this codebase rates worse than a
/// vague right one.
///
/// *Will be proposed* is safe for the same reason and is a shade weaker than the
/// truth: by the time this record exists `ProposeToCluster` has returned, so the entry
/// is appended and replicating already. Understating what a leader has done is the
/// direction to err in; overstating it is the whole defect
/// ([#1296](https://github.com/LASTRADA-Software/fastcached/issues/1296)).
///
/// **What it buys, stated narrowly, because the wider claim is false.** The leader
/// records verbatim what this client sent, so holding the echo against the same
/// operator's own keystrokes proves the message round-tripped and nothing else. What it
/// is FOR is the comparison against the OTHER machine, which is where the disagreement
/// lives: the joiner mints its own id into `--cluster-dir` and resolves its own
/// consensus endpoint, and the seed can know neither. Printed here, the pair is held
/// against the joiner's own `NodeStatusFields::nodeId` and the address it was given by
/// `--raft-self`/`--listen-raft` -- two printed strings from two machines, rather than
/// one string and a memory.
///
/// **Owning rather than a `*View`**, against this file's local convention for reply
/// bodies and by the conjunction the rule states: a view needs every consumer to read
/// it in scope AND something to depend on not copying. The second clause is false --
/// this is a member id and a `host:port`, and both ARRIVED inside a request the verb's
/// own `MaxControlPayload` row had already bounded, so neither is large -- and
/// what the field DECIDES is an operator's eyeball comparison, so a borrowed one that
/// outlived its buffer would print a plausible endpoint assembled from freed memory,
/// which is the failure this record exists to remove arriving through the record
/// itself. `CallerContext` is the precedent: first clause satisfied, its measurement
/// killed the second, and the rule selected OWN. The encode side goes on borrowing.
///
/// **Two fields and not three.** `ClusterAdmit` passes an empty scheduler endpoint
/// always -- `AddMember` applies that field WHOLESALE, so a re-admit deliberately
/// clears whatever the member had announced -- which would make a third field here a
/// constant, and a constant on a wire is
/// [#1295](https://github.com/LASTRADA-Software/fastcached/issues/1295)'s
/// `worker = true` under a new name: a value that carries no information and that two
/// builds can still disagree about. The CONSEQUENCE is real and an operator moving a
/// node should be told it, but it follows from the VERB and belongs in what a renderer
/// says, not in a field nobody can vary.
struct ClusterAdmitReceipt
{
    std::string memberId;     ///< The id as received, byte for byte.
    std::string raftEndpoint; ///< The consensus endpoint as parsed, byte for byte.

    /// The identity key the leader RECORDED (#178), in its one text spelling, or disengaged
    /// when the command carries none.
    ///
    /// **A third field, and not the constant the paragraph above refuses**: it varies with
    /// the request, and it is exactly the other half of what an operator compares -- the key
    /// the joiner's own `node` report prints against the one the seed wrote down. Read off
    /// the COMMAND, like the other two, so a leader that dropped the key it was sent answers
    /// *none recorded* rather than echoing the request back. Absent travels as a zero-length
    /// field and comes back disengaged; it is never an empty string standing in for none.
    std::optional<std::string> publicKey;

    [[nodiscard]] friend bool operator==(ClusterAdmitReceipt const&, ClusterAdmitReceipt const&) = default;
};

/// Frame the payload of a successful CLUSTER-ADMIT reply.
///
/// Both fields are UTF-8 by construction on the only path that reaches here:
/// `Cluster::Validate` refuses an `AddMember` whose id or endpoint is not text, and
/// this payload is built only once it has accepted. So the receipt is not a door around
/// the gate that keeps a fleet's own rendering parseable -- worth saying, because a
/// reply is the one direction that gate is not written on.
/// @param receipt What the leader wrote into the command.
/// @return The reply payload (not a whole frame).
[[nodiscard]] inline std::vector<std::byte> EncodeClusterAdmitReceipt(ClusterAdmitReceipt const& receipt)
{
    return WireFields::Encode(
        { AsBytes(receipt.memberId), AsBytes(receipt.raftEndpoint), AsBytes(receipt.publicKey.value_or(std::string {})) });
}

/// Read a CLUSTER-ADMIT reply body.
///
/// Exact arity, like every other reply body here: three fields or nothing. An EMPTY
/// payload is refused rather than read as *this leader recorded nothing*, which is not
/// a state that exists -- a reply carrying no receipt came from a build older than this
/// one, and that is `MinSupportedVersion`'s question rather than a shape to tolerate
/// here. The key's zero-length field comes back DISENGAGED here and nowhere else.
/// @param payload The reply body.
/// @return The receipt, or nullopt when malformed.
[[nodiscard]] inline std::optional<ClusterAdmitReceipt> DecodeClusterAdmitReceipt(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, 3);
    if (!fields.has_value())
        return std::nullopt;
    auto const key = (*fields)[2];
    return ClusterAdmitReceipt { .memberId = std::string { AsStringView((*fields)[0]) },
                                 .raftEndpoint = std::string { AsStringView((*fields)[1]) },
                                 .publicKey =
                                     key.empty() ? std::nullopt : std::optional { std::string { AsStringView(key) } } };
}

/// Frame a MINT-TICKET request.
/// @param audience Who the minted ticket will be presented to.
/// @param version Version to advertise; overridable so tests can offer a version the peer does not
///                support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeMintTicketRequest(std::string_view audience,
                                                                    WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::MintTicket, { AsBytes(audience) });
}

/// Read a MINT-TICKET payload.
///
/// The audience is returned as a STRING rather than a view, for `DecodeExplainAdmissionPayload`'s
/// reason: the payload is a borrowed span, and this field decides who a signed ticket will be
/// accepted by.
///
/// An empty audience is refused rather than passed on: a ticket signed for nobody in particular
/// is not a narrower audience, it is a caller that never named one.
/// @param payload The bytes following the request header.
/// @return The audience, or nullopt when malformed or empty.
[[nodiscard]] inline std::optional<std::string> DecodeMintTicketPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::MintTicket));
    if (!fields.has_value() || (*fields)[0].empty())
        return std::nullopt;
    return std::string { AsStringView((*fields)[0]) };
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
                                   AsBytes(request.sourceName),
                                   AsBytes(request.compileDir),
                                   AsBytes(request.compileDirReplacement),
                                   AsBytes(request.sourceRoot),
                                   AsBytes(request.sourceRootReplacement) });
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
                         .sourceName = (*fields)[5],
                         .compileDir = (*fields)[6],
                         .compileDirReplacement = (*fields)[7],
                         .sourceRoot = (*fields)[8],
                         .sourceRootReplacement = (*fields)[9] };
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

    /// How long this grant lives, so the client can bound its own wait by it.
    ///
    /// **In the CLEAR, beside the token rather than read out of it**, and that is a
    /// deliberate refusal to add an unauthenticated claims reader. The expiry is also
    /// inside the signed claims, where the WORKER reads it after verifying -- but the
    /// client holds no roster and verifies nothing, so giving it a way to read
    /// claims without checking them would put exactly the primitive the verify-first rule
    /// forbids into a header three binaries include. What the client needs is not a
    /// claim about the token; it is its own budget, from the scheduler that is already
    /// telling it which worker to dial.
    ///
    /// Without it the client's total came from a compile-time constant (#522). A fleet
    /// that had agreed on a longer lifetime was then obeyed by the scheduler and the
    /// worker and ignored by the client, which gave up mid-compile on a job everybody
    /// else still considered live -- the same failure in the third of three places, and
    /// the one the operator sees.
    std::chrono::milliseconds lifetime { 0 };

    /// Where the worker was last SEEN, as `host:port`, when that differs from the name it
    /// advertises and the worker reports that address as its own; empty otherwise
    /// (`Distributed::DecideDialHint`). The client dials this FIRST and falls back to
    /// `endpoint`, and the token signs `endpoint` -- never this -- so a stale hint that
    /// lands on another machine is refused `LeaseEndpointMismatch` rather than obeyed.
    std::string_view dialHint {};

    /// The identity public key of the worker this grant names -- the key it proved itself with
    /// when it registered -- `IdentityPublicKeyBytes` of it, or empty when the scheduler holds
    /// none for it.
    ///
    /// **What the client authenticates the COMPILE reply against** (W-4). A hint or a name that
    /// has come to answer on another machine -- a VPN address reassigned inside the heartbeat
    /// window, a long-TTL DNS name -- accepted the job and returned whatever it liked, and the
    /// object went into this machine's cache and through it into the fleet's shared one. The
    /// worker signs its reply under this key (`IdentityKeyPurpose::CompileReply`), and the client
    /// verifies that signature before the object reaches any tier. In the CLEAR beside the token
    /// for `lifetime`'s reason -- the client verifies no claims -- and inside the token's signed
    /// claims too, where the worker it names checks it is its own.
    std::span<std::byte const> workerKey {};
};

/// Frame the payload of a successful LEASE reply.
/// @param grant Where to go and what to present.
/// @return The reply payload (not a whole frame).
[[nodiscard]] inline std::vector<std::byte> EncodeLeaseGrant(LeaseGrant const& grant)
{
    auto const codecs = EncodeCodecList(grant.workerCodecs);
    auto const lifetime = EncodeU32Field(static_cast<std::uint32_t>(grant.lifetime.count()));
    return WireFields::Encode({ AsBytes(grant.endpoint),
                                AsBytes(grant.leaseToken),
                                std::span<std::byte const> { codecs },
                                std::span<std::byte const> { lifetime },
                                AsBytes(grant.dialHint),
                                grant.workerKey });
}

/// The fields of a LEASE grant, as views.
struct LeaseGrantView
{
    std::span<std::byte const> endpoint;
    std::span<std::byte const> leaseToken;
    CodecList workerCodecs;
    std::chrono::milliseconds lifetime { 0 };
    std::span<std::byte const> dialHint; ///< Empty when the scheduler has no hint.
    /// The named worker's identity key: `IdentityPublicKeyBytes`, or empty when none was named.
    std::span<std::byte const> workerKey;
};

/// Split a LEASE reply payload.
///
/// Exactly six fields: a reply is not stepped over the way a request is, so a grant
/// of any other arity is a peer this build cannot read. The worker key is empty or
/// exactly a key's width; any other length is a sender this build cannot read either.
/// @param payload The reply body.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<LeaseGrantView> DecodeLeaseGrant(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, 6);
    if (!fields.has_value())
        return std::nullopt;
    if (!(*fields)[5].empty() && (*fields)[5].size() != IdentityPublicKeyBytes)
        return std::nullopt;
    // Strict about the width, like every other numeric field here: a lifetime of
    // another length is a sender speaking a shape this build does not know, and
    // reading four bytes of it would invent a budget.
    auto const lifetime = DecodeU32Field((*fields)[3]);
    if (!lifetime.has_value())
        return std::nullopt;
    return LeaseGrantView { .endpoint = (*fields)[0],
                            .leaseToken = (*fields)[1],
                            .workerCodecs = DecodeCodecList((*fields)[2]),
                            .lifetime = std::chrono::milliseconds { *lifetime },
                            .dialHint = (*fields)[4],
                            .workerKey = (*fields)[5] };
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

    /// The worker's signature, under its identity key, over `correlation` and the digest of
    /// `object` (`IdentityKeyPurpose::CompileReply`): `NodeSignatureBytes` of it, or empty from
    /// a worker that holds no key.
    ///
    /// What lets the client tell the worker its grant named from whatever answered at that
    /// address (W-4). Over the correlation rather than the request, because the correlation is
    /// already the worker's statement of what it compiled; over the object AS SENT, enveloped,
    /// so the client checks it before expanding a byte.
    std::span<std::byte const> signature {};
};

/// Frame the payload of a COMPILE reply.
/// @param result The outcome.
/// @return The reply payload (not a whole frame).
[[nodiscard]] inline std::vector<std::byte> EncodeCompileResult(CompileResult const& result)
{
    auto const code = EncodeU32Field(result.exitCode);
    return WireFields::Encode({ std::span<std::byte const> { code },
                                result.object,
                                result.stdoutText,
                                result.stderrText,
                                result.correlation,
                                result.signature });
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
    std::vector<std::byte> signature;   ///< @see `CompileResult::signature`; empty or `NodeSignatureBytes`.
};

/// Split a COMPILE reply payload.
/// @param payload The reply body.
/// @return The result, or nullopt when malformed.
[[nodiscard]] inline std::optional<CompileResultFields> DecodeCompileResult(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, 6);
    if (!fields.has_value())
        return std::nullopt;
    auto const code = DecodeU32Field((*fields)[0]);
    if (!code.has_value())
        return std::nullopt;
    if (!(*fields)[5].empty() && (*fields)[5].size() != NodeSignatureBytes)
        return std::nullopt;
    auto const own = [](std::span<std::byte const> f) {
        return std::vector<std::byte> { f.begin(), f.end() };
    };
    return CompileResultFields { .exitCode = *code,
                                 .object = own((*fields)[1]),
                                 .stdoutText = own((*fields)[2]),
                                 .stderrText = own((*fields)[3]),
                                 .correlation = own((*fields)[4]),
                                 .signature = own((*fields)[5]) };
}

// --- the operator verbs' replies ------------------------------------------------------
//
// Encoded in primitive length-prefixed fields, the way `SchedulerProtocol.hpp` encodes a
// history bucket, because this header must stay dependency-free: the launcher does not
// link `FastCache`, so nothing here may include `Distributed/` or the node's own headers.

/// A surface a node may serve, as the WIRE names it.
///
/// **Deliberately its own enum rather than the node's `NodeSurface`.** That one is a
/// private enum a node may reorder freely; transmitting it would silently make its
/// declaration order a wire contract, which is the defect `StorageTier` already carries
/// knowingly and which there is no reason to repeat. The node maps its enumerator onto
/// this one, so a reorder there is a compile error at the mapping rather than a client
/// reading `Raft` where the node meant `Admin`.
///
/// Explicit values because these bytes are transmitted.
enum class WireSurface : std::uint8_t
{
    Admin = 0x01,     ///< `/metrics`, `/healthz`, the fleet dashboard.
    Raft = 0x02,      ///< Consensus traffic between peers.
    Discovery = 0x03, ///< The LAN beacon.
};

/// **The `0xFC` port itself is deliberately not a `WireSurface`.**
///
/// A client learns this reply by dialling that port, so telling it the port it just used
/// carries no information -- and it is not free: the node's identity is built BEFORE the
/// listener (it is an argument to `StartNodeSurfaceOrExplain`), while the port worth
/// reporting is the one actually BOUND, which under socket activation is the unit's
/// choice and not the flag's. Reporting it would mean threading the listener back into
/// the thing constructed ahead of it, to say something the reader already knows.
///
/// Where a THIRD party needs this node's frame address, that is `--advertise`'s job and
/// the fleet report's, both of which already carry it.

/// One surface a node reports serving.
struct SurfaceReport
{
    WireSurface surface { WireSurface::Admin }; ///< Which one.
    std::uint32_t port { 0 };                   ///< The port it is served on.
    /// Whether it is served over TLS. Only meaningful for `Admin`.
    ///
    /// A field rather than an assumption: a client that guessed `http://` against a TLS
    /// admin surface fails in a way that reads as the surface being down.
    bool tls { false };
};

/// How far a node's worker has got in identifying the toolchains it will serve.
///
/// **A tri-state, because the question has three answers and a `bool` reports two of
/// them identically.** A node SERVES while it identifies its toolchains -- the walk
/// moved to the heartbeat thread's first round and has been observed running past
/// 300 s cold -- so *still looking* and *looked, found nothing* are the two states an
/// operator most needs separated, and they are exactly the two that a "does this node
/// run a worker" bit collapses
/// ([#1295](https://github.com/LASTRADA-Software/fastcached/issues/1295)).
///
/// **Three and not four, and the next reader will want to make it four.** The node's
/// own `Node::SurveyOutcome` names `Served`, `NothingToServe`, `NoneCouldBeAsked` and
/// `Cancelled`, and
/// [#1060](https://github.com/LASTRADA-Software/fastcached/issues/1060) paid
/// specifically to separate the middle two -- a machine that is misconfigured and
/// stays so, against one that may be fine a beat later. Mirroring that here is the
/// obvious improvement and it is wrong, for two independent reasons:
///
/// 1. **Only the FIRST survey has an outcome.** The periodic re-survey answers
///    `Node::ToolchainRefresh`, which carries `changed` and `served` and no outcome at
///    all -- so a fourth enumerator could be filled honestly once, at startup, and
///    would be guessed on every beat after it. That is a model more permissive than
///    the thing it stands for, which is the shape this tree has paid for repeatedly.
/// 2. **The misconfigured arm is not observable anyway**, because it exits:
///    `SurveyOutcome::NothingToServe` sets `surveyFoundNothing` and requests a stop, so
///    a node still answering `NodeStatus` in `NothingToServe` is always the recoverable
///    kind. A wire state nothing can reach is a state nobody can act on.
///
/// So the counts below carry the detail and the node's own log carries the cause.
///
/// Explicit values because these bytes are transmitted.
enum class ToolchainState : std::uint8_t
{
    /// No survey has finished yet. `toolchainsServed` is what is ready so far and
    /// `toolchainsDiscovered` is what the cheap half found to walk -- the *n of m* an
    /// operator watches during a cold start.
    Surveying = 0x01,

    /// A survey finished and this node serves `toolchainsServed` toolchains, which is
    /// at least one.
    Serving = 0x02,

    /// A survey finished and this node serves none, so it accepts no compiles until a
    /// later survey finds one. Recoverable by construction -- see the note above.
    NothingToServe = 0x03,
};

/// What a node's scheduler role is, as the WIRE names it.
///
/// **Deliberately its own enum rather than `Distributed::SchedulerRole`**, for the
/// reason `WireSurface` is not `NodeSurface`: that one is an in-process enum whose
/// order is already load-bearing for two `EnumTable`s, and transmitting it would
/// silently make its declaration order a wire contract as well. The node maps its
/// enumerator onto this one, so a reorder there is a compile error at the mapping.
///
/// It carries no `Undecided`-means-absent subtlety: a node running no scheduler reports
/// no role at all (the field is disengaged), while `Undecided` is a real reading -- an
/// election is in progress and this node is participating in it. Those are different
/// facts and a client acts on them differently.
///
/// Explicit values because these bytes are transmitted.
enum class WireSchedulerRole : std::uint8_t
{
    Follower = 0x01,  ///< Not the leader; a known leader is at `leaderEndpoint`.
    Undecided = 0x02, ///< No leader is known -- an election is in progress.
    Leader = 0x03,    ///< This node leads and may hand out capacity.
};

/// How the enrollment window decides a joiner: by a person, or by a deadline a person armed.
///
/// A tri-state where it is reported -- `Manual`, `AutoApprove`, and DISENGAGED on a node
/// that runs no enrollment surface at all -- for `ToolchainState`'s reason: absent is not
/// manual. A worker with no scheduler has no window to report on, and a `Manual` there is a
/// claim about a thing that does not exist. `AutoApprove` is the one state in this record an
/// operator may act on within seconds of reading it, which is why it is reported at all
/// rather than left to the metrics: while it lasts, a machine that asks is admitted under the
/// key it asked with, with nobody comparing that key.
///
/// **TRANSMITTED**: explicit values, append-only, and a retired value is never reused --
/// see `RetiredEnrollmentStates`.
enum class WireEnrollmentState : std::uint8_t
{
    // 0x01 was `Closed` and 0x02 was `Open`: a window an operator opened for a moment. Since
    // zero-config formation every request is recorded, so neither describes anything. RETIRED.

    Manual = 0x03,      ///< Every request waits for a person. The default, and a restart's answer.
    AutoApprove = 0x04, ///< A deadline is armed, and a request made before it is admitted.
};

/// Every enrollment state this build implements, as ONE list, for `KnownEnrollRoles`' reason:
/// both decoders refuse or skip a byte outside it, and a retired byte is never in it.
inline constexpr std::array KnownEnrollmentStates { WireEnrollmentState::Manual, WireEnrollmentState::AutoApprove };

/// Enrollment-state bytes that once meant something and must never mean anything again, for
/// `RetiredErrorCodes`' reason: a peer built before the retirement still names the byte, and
/// would render a new state under it as the old one.
inline constexpr std::array<std::uint8_t, 2> RetiredEnrollmentStates { 0x01, 0x02 };

/// Whether the state list has kept clear of every retired byte.
/// @return True when no known state claims a retired byte.
[[nodiscard]] consteval bool NoRetiredEnrollmentStateIsReused() noexcept
{
    return std::ranges::none_of(KnownEnrollmentStates, [](WireEnrollmentState state) {
        return std::ranges::contains(RetiredEnrollmentStates, static_cast<std::uint8_t>(state));
    });
}

static_assert(NoRetiredEnrollmentStateIsReused(),
              "0x01 was closed and 0x02 was open; a retired enrollment-state byte is never reassigned");

/// Whether @p raw names an enrollment state this build understands.
/// @param raw The state byte, as received.
/// @return True when it names one.
[[nodiscard]] constexpr bool IsKnownEnrollmentState(std::uint8_t raw) noexcept
{
    return std::ranges::any_of(KnownEnrollmentStates,
                               [raw](WireEnrollmentState state) { return static_cast<std::uint8_t>(state) == raw; });
}

/// Whether this node's worker is taking new compiles (#1303).
///
/// A tri-state where it is reported, and DISENGAGED on a node that runs no worker, for
/// `WireEnrollmentState`'s reason: absent is not serving. `Draining` and `Drained` are
/// both cordoned, and the difference is the one an operator waiting to reboot acts on --
/// `Drained` says no compile is running, so stopping this node abandons nothing.
///
/// Explicit values because these bytes are transmitted.
enum class WireCordonState : std::uint8_t
{
    Serving = 0x01,  ///< Not cordoned: admitting compiles. A restart's answer.
    Draining = 0x02, ///< Cordoned, with compiles still running.
    Drained = 0x03,  ///< Cordoned, and nothing is running.
};

/// Where a node sits in its own consensus configuration (#1449).
///
/// Mirrors `Consensus::Standing`, which lives in the library this header may not depend on;
/// the node app holds the two together by a table, as it does `WireMembership`. A tri-state
/// and more where it is reported, and DISENGAGED on a node that runs no consensus, for
/// `WireEnrollmentState`'s reason: a standing is a claim about a configuration, and a node
/// with none to hold has nothing to claim.
///
/// It is what consensus COUNTS the node as right now, which is not always the seat the
/// replicated record names: an operator's admit is recorded first and the configuration
/// follows one change at a time, so the two differ for as long as a change replicates.
///
/// Explicit values because these bytes are transmitted.
enum class WireConsensusStanding : std::uint8_t
{
    NoCluster = 0x01, ///< Holds no configuration: waiting to be admitted.
    Voter = 0x02,     ///< Counted by every quorum; may stand for election.
    Learner = 0x03,   ///< Replicated to and counted by no quorum; never stands.
    Outsider = 0x04,  ///< Its configuration names others and not itself: removed, or never admitted.
};

/// What a node concludes about one host's admission (#1471).
///
/// Mirrors `Distributed::Membership`, which lives in the library this header may not depend on.
/// The two are held together by a table in the node app, not by having the same shape here.
///
/// Explicit values because these bytes are transmitted.
enum class WireMembership : std::uint8_t
{
    Outsider = 0x01,  ///< No route has an opinion: refused, and nothing claims authorship.
    Member = 0x02,    ///< Some route admits it.
    Forgotten = 0x03, ///< A replicated tombstone refuses it, outranking every admission route.
};

/// Which routes concluded a verdict, as a BITMASK (#1471).
///
/// **A set rather than one value, and that is the whole point of the verb.** Admission is a fold
/// over several participants, and an operator who turns `--fleet-open` off and finds a machine
/// still served needs to know its KEY admits it too. A single value could not say *both*, and
/// reporting only the winner would answer the question the operator did not ask.
///
/// Empty is the honest answer for `Outsider`: `DecidedBy` never attributes it, because an oracle
/// answering `Outsider` has no opinion rather than an answer it produced, and naming an author
/// for a silence is the confident wrong signal this verb exists to remove.
///
/// Explicit values because these bytes are transmitted, and powers of two because they combine.
/// A NAMESPACE of constants rather than an `enum class`, following `NodeComponentBit` above --
/// which is the same question on the same wire, and the precedent this originally missed.
///
/// **0x01, 0x02 and 0x04 are RETIRED and RESERVED** (`RetiredWireMembershipRoutes`): the address
/// routes they named -- `--fleet-member`'s list, the committed member set and a client tombstone
/// -- are gone, since a machine is admitted and forgotten by its KEY alone. A deployed client may
/// still render them by their old names, so they are never reused.
///
/// A bitmask is not an enumeration: its values combine, so no variable of the type holds one
/// of them and a `switch` over it means nothing. `performance-enum-size` says the quiet part --
/// an `enum class : std::uint32_t` whose named values reach 0x10 is four bytes carrying one --
/// and the tempting fix, narrowing the base type, is wrong twice: it caps a set the WIRE has
/// room to grow at eight, and it makes the type's width disagree with the field's.
namespace WireMembershipRoute
{
    constexpr std::uint32_t OpenPolicy = 0x08;     ///< `--fleet-open`, which admits every caller.
    constexpr std::uint32_t ProvenIdentity = 0x10; ///< The caller proved an identity key the cluster holds live.
    constexpr std::uint32_t KeyTombstone = 0x20;   ///< The caller proved an identity key the cluster REVOKED.
    constexpr std::uint32_t Loopback = 0x40;       ///< The caller is on this machine.
    constexpr std::uint32_t MachineTicket = 0x80;  ///< The caller presented a ticket signed by a machine the roster admits.
} // namespace WireMembershipRoute

/// The route bits no answer may carry again: 0x01 was `--fleet-member`'s list, 0x02 the committed
/// member set, 0x04 a client tombstone.
inline constexpr std::array<std::uint32_t, 3> RetiredWireMembershipRoutes { 0x01, 0x02, 0x04 };

/// @param bits A route bit, or a set of them.
/// @return Whether @p bits carries any retired bit.
[[nodiscard]] constexpr bool CarriesRetiredRoute(std::uint32_t bits) noexcept
{
    return std::ranges::any_of(RetiredWireMembershipRoutes, [bits](std::uint32_t retired) { return (bits & retired) != 0; });
}

/// Every live route bit this header names, for the reservation check below.
///
/// A list kept by hand, so it guards only the NAMES: what actually travels is
/// `Distributed::MembershipWireRoutes`, which this header cannot see, and that table is asked the
/// same question beside it (`MembershipWire.hpp`) -- a row given a retired bit fails there.
inline constexpr std::array<std::uint32_t, 5> LiveWireMembershipRoutes {
    WireMembershipRoute::OpenPolicy, WireMembershipRoute::ProvenIdentity, WireMembershipRoute::KeyTombstone,
    WireMembershipRoute::Loopback,   WireMembershipRoute::MachineTicket,
};

/// Whether no live route reuses a retired bit.
/// @return True when every live bit is clear of every retired one.
[[nodiscard]] constexpr bool NoLiveRouteIsRetired() noexcept
{
    return std::ranges::none_of(LiveWireMembershipRoutes, CarriesRetiredRoute);
}

static_assert(NoLiveRouteIsRetired(), "a retired route bit is reserved: a deployed client reads it by its old name");

/// Where a MACHINE stands in the roster a node holds, as `explain-admission <machine>` reports it.
///
/// **TRANSMITTED**: explicit values, and a byte this build does not know is refused rather than
/// defaulted. Mirrors `Distributed::MachineStanding`, joined by `MachineStandingWire`.
enum class WireMachineStanding : std::uint8_t
{
    Voter = 0x01,   ///< A member counted by every quorum.
    Learner = 0x02, ///< A member replicated to and counted by no quorum.
    Pending = 0x03, ///< Not recorded; this node's enrollment window holds a request under that id.
    Revoked = 0x04, ///< Its key, or the only key its id was recorded under, is revoked.
    Unknown = 0x05, ///< Nothing this node holds names it.
};

/// One node's answer about a machine, or about the caller's own connection.
struct AdmissionExplanationFields
{
    WireMembership verdict { WireMembership::Outsider }; ///< What the fold concluded.
    std::uint32_t decidedBy { 0 };                       ///< `WireMembershipRoute` values, OR-ed.
    std::optional<WireMachineStanding> standing {};      ///< Engaged for a MACHINE question, absent for "who am I".
    std::string subject {};                              ///< The machine asked about, or the id or host this connection is.

    [[nodiscard]] bool operator==(AdmissionExplanationFields const&) const = default;
};

/// What `--node-status` says about the roster a node verifies lease grants against (#178).
///
/// Carried as its own nested record inside `NodeRuntimeFields`, because it is one fact with
/// three parts and an operator reads it as one: which roster, how many vote, and how many keys
/// are revoked. The principal count and the certification lapse it once carried are gone with
/// principal admission and the certified roster: every node's roster is the state its own
/// consensus applied, which admits nobody by key alone and never lapses.
struct NodeRosterFields
{
    std::uint64_t version { 0 }; ///< `ClusterState::rosterVersion` of the roster held.
    std::uint32_t voters { 0 };  ///< Members that vote.
    std::uint32_t revoked { 0 }; ///< Keys the cluster will never admit again.

    [[nodiscard]] friend bool operator==(NodeRosterFields const&, NodeRosterFields const&) = default;
};

/// Where a node's idea of the fleet's shared cache came from.
///
/// Explicit values from `0x01` because these bytes are transmitted: a zero byte names nothing,
/// and a byte a newer sender uses is SKIPPED -- the record stays absent -- which is the
/// enrollment and cordon fields' rule.
enum class WireSharedCacheSource : std::uint8_t
{
    None = 0x01,        ///< No shared cache: the setting names no machine and nothing overrides it.
    Setting = 0x02,     ///< The replicated `shared-cache` setting names a machine.
    Override = 0x03,    ///< This node's `--upstream` wins over the setting.
    ThisMachine = 0x04, ///< The setting names this node, which serves it and reads it in process.
};

/// How a node's last dealing with the fleet's shared cache went.
///
/// Explicit values from `0x01`, for `WireSharedCacheSource`'s reason.
enum class WireSharedCacheState : std::uint8_t
{
    NotTried = 0x01,    ///< Named, and no operation has needed it yet.
    Proven = 0x02,      ///< The last attempt proved the key the roster holds for that machine.
    Unresolved = 0x03,  ///< The state names no dialable machine with a live key.
    WrongKey = 0x04,    ///< The peer proved another key than the roster's.
    Unreachable = 0x05, ///< The dial or the handshake failed.
    Serving = 0x06,     ///< This node serves the shared tier.
    Unavailable = 0x07, ///< This node is named and cannot serve.
    /// The named machine proved its key and refused THIS node's proof: its roster does not hold
    /// this node's key, or holds it revoked. The right machine answered; it is not a network.
    ProofRefused = 0x08,
};

/// What `--node-status` says about the fleet's shared cache from where this node stands.
///
/// Its own nested record inside `NodeRuntimeFields`, for `NodeRosterFields`' reason: one fact an
/// operator reads as one -- which machine, reached where, and how the last attempt went.
struct SharedCacheStatusFields
{
    WireSharedCacheSource source { WireSharedCacheSource::None };  ///< Where the answer came from.
    std::string machineId;                                         ///< Empty for `None` and `Override`.
    std::string endpoint;                                          ///< What is dialled, or served; empty when unresolved.
    WireSharedCacheState state { WireSharedCacheState::NotTried }; ///< How the last attempt went.
    std::string detail;                                            ///< One sentence; empty when there is nothing to add.

    [[nodiscard]] friend bool operator==(SharedCacheStatusFields const&, SharedCacheStatusFields const&) = default;
};

/// Whether a node is pinned to one fleet (`--fleet-id`), as the first byte of its nested record.
///
/// **Transmitted**: the byte rides `NodeRuntimeFields::fleetPin`, so each value is stated and none is
/// ever renumbered.
enum class WireFleetPinTag : std::uint8_t
{
    Unpinned = 0, ///< Trust on first use: nothing follows.
    Pinned = 1,   ///< Pinned: the pin follows as `--fleet-id` spells it, never empty.
};

/// What `--fleet-id` pins a node to, as `NodeStatus` reports it.
///
/// A record of its own rather than an optional text, because there are THREE answers and an optional
/// has two: pinned, pinned to nothing -- the trust-on-first-use default an operator asks about
/// precisely to find out whether it is still in force -- and a node too old to say, which reads as the
/// whole record absent. The pin travels as TEXT, `<cluster-id>@<key>[,<key>...]`, exactly as
/// `--fleet-id` takes it: this header links nothing that could parse a key, and the reader shows it.
struct NodeFleetPinFields
{
    std::optional<std::string> fleet; ///< The pin as `--fleet-id` spells it, or disengaged on an unpinned node.

    [[nodiscard]] friend bool operator==(NodeFleetPinFields const&, NodeFleetPinFields const&) = default;
};

/// What a node's live components report about themselves, as opposed to what its
/// configuration asked for.
///
/// **Nested inside one `NodeStatus` field rather than spread across several, and that
/// is the extensibility decision rather than a tidiness one.** `SplitFields` is exact
/// about arity by design -- the property that makes a fixed message self-describing --
/// so every fact added at `NodeStatusFields`' top level would move its arity and make
/// two builds of this fleet unable to speak at all. This is the same answer
/// `EncodeCapacity` gives inside REGISTER, for the same reason, and it is why version
/// 7 is intended to be the last `NodeStatus` arity change: a fact this build has not
/// heard of is skipped, and one it expects but was not sent keeps its default, which
/// is *did not say*.
///
/// Every member is therefore an `optional` or has a documented "did not say" value.
/// **Absent is not zero**: a node running no worker must be distinguishable from one
/// whose worker serves nothing, and a `0` renders as a real reading in every format
/// that carries it.
struct NodeRuntimeFields
{
    /// How far the toolchain survey has got, or disengaged on a sender that runs no
    /// worker or is too old to say.
    std::optional<ToolchainState> toolchains {};

    /// How many toolchains this node serves right now.
    ///
    /// Meaningful only beside `toolchains`: on its own a `0` is the collapse the
    /// tri-state exists to end.
    std::uint32_t toolchainsServed { 0 };

    /// How many candidates the cheap half of the survey found to walk.
    ///
    /// The *m* of *n of m*. Never smaller than `toolchainsServed` on a healthy node,
    /// and not asserted to be: the two are sampled together but describe different
    /// phases, and refusing a reading is not this decoder's job.
    std::uint32_t toolchainsDiscovered { 0 };

    /// How many concurrent compiles this node offers, or disengaged on one that runs no
    /// compile accounting.
    ///
    /// **Not accompanied by a "limited by" field, and that is a decision.** The
    /// vocabulary for *which ceiling bound this number* is `Distributed::SlotLimit`, and
    /// it is the SCHEDULER's conclusion about a worker -- derived on the leader from what
    /// the worker reported plus its live load. A worker does not compute it, and having
    /// it do so would be a second spelling of the scheduler's arithmetic that can
    /// disagree with the first. What the node owns is the two numbers below.
    std::optional<std::uint32_t> compileSlots {};

    /// How many of those slots are in use right now.
    ///
    /// Disengaged and zero are different answers: a node with no compile accounting says
    /// nothing, a node with an idle worker says zero.
    std::optional<std::uint32_t> compilesInFlight {};

    /// This node's scheduler role, or disengaged on a node that runs no scheduler.
    ///
    /// The question a `components` mask cannot answer: a leading scheduler and a
    /// following one both report `scheduler`, and a follower's registry is empty and
    /// reads exactly like an idle fleet.
    std::optional<WireSchedulerRole> schedulerRole {};

    /// Where the leader answers, or empty when this node names no leader.
    ///
    /// Empty is a READING only on a node that runs consensus, and what it says depends on
    /// the node: beside role `Leader`, that THIS node is the leader, since a leader names
    /// no other; beside `Undecided`, an election in progress; on a learner, that it has
    /// heard from no leader yet, or that its leader has been silent past the election
    /// timeout (#1641). Which kind of node sent it is said beside it, not here:
    /// the `Consensus` component bit or a scheduler role (the client's
    /// `Cli::ReportsConsensus` asks exactly that), and a learner reports this field with
    /// no role at all, since it runs no scheduler. On a node running no consensus the
    /// field is empty and means nothing. The wire has no absent form for it: the encoder
    /// always writes it and the decoder reads a missing one as empty, so the reading is
    /// told from the meaningless empty by the node's kind, never by the field.
    std::string leaderEndpoint {};

    /// How many of this node's registrations a scheduler has accepted.
    ///
    /// One per toolchain served, so it is read beside `registrarsTotal`: *2 of 3* is a
    /// partially registered node, which is an ordinary state mid-survey and an alarming
    /// one an hour later.
    std::optional<std::uint32_t> registrarsRegistered {};

    /// How many registrations this node is trying to hold.
    std::optional<std::uint32_t> registrarsTotal {};

    /// How long ago a scheduler last accepted a registration from this node.
    ///
    /// **Disengaged means NEVER, and zero means just now.** Collapsing those is the
    /// whole reason this is an optional: a worker that has never reached its scheduler
    /// and one that registered this second are the two states an operator is trying to
    /// tell apart, and a bare `0` reports the healthy one for both.
    ///
    /// A duration rather than an instant, for the reason a heartbeat age is: a raw
    /// `core::platform::SteadyTimePoint` on the wire invites the receiver to difference it against its OWN
    /// clock, which is a different clock.
    std::optional<std::uint64_t> lastRegistrationSecondsAgo {};

    /// How this node's enrollment window decides a joiner, or disengaged on a node that runs
    /// no enrollment surface.
    ///
    /// **`AutoApprove` is the one state in this record in which a stranger is admitted with
    /// nobody comparing its key**, so it is reported here rather than left to the logs: an
    /// operator who armed a deadline and walked away has no other way to find out, and the
    /// repeating warning that exists for the same reason only reaches whoever is reading that
    /// node's log.
    ///
    /// A STATE, and therefore in the snapshot rather than exported as a gauge: the
    /// metrics tally the events -- approvals, rosters served -- because a counter is a
    /// tally and a synthetic gauge built out of one would be a second spelling of this
    /// field that can disagree with it.
    std::optional<WireEnrollmentState> enrollment {};

    /// How many joiners are waiting for a decision.
    ///
    /// Disengaged and zero are different answers, which is the whole reason this is an
    /// optional: a node that runs no enrollment surface says nothing, and one that nobody
    /// has asked yet says zero.
    std::optional<std::uint32_t> enrollmentPending {};

    /// Whether the worker is cordoned, and if so whether it has drained (#1303); disengaged
    /// on a node that runs no worker.
    ///
    /// A STATE in the snapshot for `enrollment`'s reason: the refusals a cordon causes are
    /// counted, and the state is what an operator waiting to reboot polls.
    std::optional<WireCordonState> cordon {};

    /// Where this node's consensus peers DIAL it (#1328), or disengaged on a node that
    /// runs no consensus.
    ///
    /// **Not a surface.** `NodeStatusFields::surfaces` reports ports this node BOUND, and
    /// a consensus bind is routinely the wildcard; this is the `host:port` a peer is told
    /// to dial, which the leader records at admission and `--cluster-admit` echoes (#1296).
    /// The two are compared by an operator bringing a machine in, so they travel apart.
    ///
    /// **Never legally empty when engaged.** Absent travels as a zero-length field, so an
    /// engaged empty string would read back as absent -- a sender with no endpoint to state
    /// leaves this disengaged, and a node that runs consensus always states one (a node
    /// naming itself neither way is refused at startup).
    std::optional<std::string> consensusEndpoint {};

    /// Where this node sits in the configuration its consensus operates under, or
    /// disengaged on a node that runs no consensus (#1449).
    ///
    /// The question a `components` mask and a scheduler role cannot answer between them:
    /// a learner and a voter that follows both report `consensus` and `follower`, and only
    /// one of them will ever stand when the leader goes.
    std::optional<WireConsensusStanding> consensusStanding {};

    /// Every condition this node's table holds and what this process found for each (#1364), or
    /// disengaged from a node too old to carry them.
    ///
    /// **Every row, whatever its state**, rather than only the raised ones: a list of raised rows
    /// cannot say *checked and benign* apart from *not evaluated here*, and an empty one cannot say
    /// *nothing raised* apart from *this build has no conditions* -- which is the absent-is-not-zero
    /// rule one level down. A node whose table has rows always sends them, so an engaged list is
    /// never empty and a zero-length field reads back as ABSENT.
    std::optional<std::vector<NodeConditionFields>> conditions {};

    /// This node's identity key (#178), or disengaged on a node that holds none.
    ///
    /// **Its 32 bytes, never its text**: the text is a presentation, and one client
    /// rendering it through the one encoder is what keeps the spelling an operator compares
    /// the same on every tool. Absent is a node with no state directory to keep a key in,
    /// which is a different answer from a key the reader failed to receive -- a sender that
    /// predates the field sends fewer fields, and both read as absent, which is the most a
    /// reader can honestly say about either.
    std::optional<std::array<std::byte, IdentityPublicKeyBytes>> identityPublicKey {};

    /// The roster this node verifies lease grants against (#178), or disengaged on a node that
    /// holds none -- one no other machine can reach, which checks no grant -- or on one too old
    /// to say.
    std::optional<NodeRosterFields> roster {};

    /// How many whole seconds the armed auto-approve deadline has left, or disengaged when none
    /// is armed -- a `Manual` window, a node that runs no enrollment surface, or a sender too
    /// old to say.
    ///
    /// **Absent is not zero.** Engaged exactly beside `enrollment == AutoApprove`, and a `0`
    /// there is a deadline in its last second, which still admits; a `Manual` window sends
    /// nothing rather than a zero that would read as a deadline that just lapsed.
    std::optional<std::uint64_t> enrollmentAutoApproveSecondsLeft {};

    /// Where this node keeps its identity and consensus state, or disengaged from a sender too
    /// old to say.
    ///
    /// **Beside `stateDirectoryReason`, and never without it**: a node run by hand keeps its
    /// state in the account's own directory and the machine's service in the machine's, so one
    /// machine can hold two identities -- and the path alone does not say which of the two an
    /// operator is looking at. Never legally empty when engaged, like `consensusEndpoint`.
    std::optional<std::string> stateDirectory {};

    /// Why the state directory is that one -- named, handed over by the service manager,
    /// machine-wide because the process is privileged, or per-user because it is not -- in the
    /// words the node chose, so no reader restates the list. Engaged exactly beside
    /// `stateDirectory`.
    std::optional<std::string> stateDirectoryReason {};

    /// What this node reads through to, or serves, as the fleet's shared cache; disengaged
    /// on a sender too old to say. See `SharedCacheStatusFields`.
    std::optional<SharedCacheStatusFields> sharedCache {};

    /// What another machine's `--fleet-id` is to be set to, to pin it to this node's fleet: this
    /// node's cluster and the identity keys of the voters its applied state records,
    /// `<cluster-id>@<key>[,<key>...]`, ready to paste. Disengaged on a node with no cluster or no
    /// roster to name voters from, or a sender too old to say. Never legally empty when engaged.
    std::optional<std::string> fleetId {};

    /// What `--fleet-id` pins this node to, or disengaged from a sender too old to say. Every node
    /// that says anything says this: unpinned is an answer, not an absence.
    std::optional<NodeFleetPinFields> fleetPin {};
};

/// What an operator reads `NodeRuntimeFields::consensusEndpoint` under, as prose: the
/// `--cluster-admit` receipt's line and `--print-surfaces`' `dialled at:` block.
///
/// **One fact, in the one header both applications reach** (#1328). The whole value of printing
/// this address on two machines is that an operator compares two strings under the same name, so
/// a second spelling is a rename away from comparing nothing -- and the node's own header, where
/// this lived first, is one the CLI cannot include.
inline constexpr std::string_view ConsensusEndpointLabel = "consensus endpoint";

/// The same name as a RECORD FIELD: the `node` verb's cell, and the stem of the `cluster-admit`
/// receipt's. Kebab-case like every field of a record, and `ConsensusEndpointLabel` with its spaces
/// hyphenated -- a relation a test holds, not one a reader is trusted to keep.
inline constexpr std::string_view ConsensusEndpointField = "consensus-endpoint";

/// The heading this address is shown under where it is not the only thing on the line:
/// `--print-surfaces`' `dialled at:` block, and the `live-stats` node panel's fact line.
///
/// A second name beside `ConsensusEndpointLabel` rather than that label, because it answers a
/// different question -- *which address is this*, set against the ports a node BINDS -- and
/// because the panel's first-label column is shared by every line of the panel: an 18-cell label
/// there moves every value on nodes that run no consensus at all (#1418). One spelling for both
/// places, so the worksheet and the panel an operator holds side by side say the same words.
inline constexpr std::string_view ConsensusEndpointHeading = "dialled at";

namespace Detail
{
    /// Bytes for an engaged optional integral, or NOTHING for a disengaged one.
    ///
    /// The one place *absent travels as a zero-length field* is spelled, so a record
    /// full of optionals cannot acquire a member that emits a zero value instead. A
    /// zero-length field and a field holding zero are different facts on this wire, and
    /// which one a reader gets decides whether it believes a node said something.
    /// @param value The fact, or nothing.
    /// @return Its big-endian bytes, or an empty vector.
    template <typename T>
    [[nodiscard]] inline std::vector<std::byte> OptionalBigEndian(std::optional<T> const& value)
    {
        if (!value.has_value())
            return {};
        auto const bytes = WireFields::ToBigEndian<T>(*value);
        return std::vector<std::byte> { bytes.begin(), bytes.end() };
    }

    /// Bytes for an engaged optional single-byte enum, or NOTHING for a disengaged one.
    /// @param value The enumerator, or nothing.
    /// @return Its one byte, or an empty vector.
    template <typename E>
    [[nodiscard]] inline std::vector<std::byte> OptionalEnumByte(std::optional<E> const& value)
    {
        if (!value.has_value())
            return {};
        return std::vector<std::byte> { static_cast<std::byte>(static_cast<std::uint8_t>(*value)) };
    }

    /// Read an optional integral field out of a nested record.
    ///
    /// Holds the whole of the record's tolerance rule in one place: an EMPTY field is
    /// *did not say* and leaves @p out alone, and a field of the wrong WIDTH is refused
    /// rather than read -- that is a sender speaking a shape this build does not know,
    /// and reading its first bytes would invent a plausible number. Written once because
    /// the alternative is the same three lines per field, where one relaxed copy is a
    /// silent wrong reading nothing else would catch.
    /// @param field The field's bytes, possibly empty.
    /// @param out Set only when the field carried a value.
    /// @return False when the field was present and malformed.
    template <typename T>
    [[nodiscard]] inline bool ReadOptionalBigEndian(std::span<std::byte const> field, std::optional<T>& out)
    {
        if (field.empty())
            return true;
        auto const value = WireFields::FromBigEndian<T>(field);
        if (!value.has_value())
            return false;
        // Assigned as the OPTIONAL rather than dereferenced into one: the round trip
        // through the value is what `bugprone-optional-value-conversion` objects to, and
        // it is right -- the two spellings differ only in offering a dereference that a
        // later edit could move above the check.
        out = value;
        return true;
    }
} // namespace Detail

/// Fields a `NodeRosterFields` record carries: the version, the voter count and the revoked count,
/// in that order. A reader accepts more, and ignores the surplus.
///
/// **Three since `0xFC` 15, closed up.** The record carried a principal count third and a
/// certification lapse fifth (#178); the principal count's position was kept RESERVED and empty
/// only so a reader of the older grammar would not take it for the revoked count -- a position held
/// to avoid a version step. The version stepped, so a peer that would read the older positions is
/// refused by NUMBER before this record is read, and the reserved position went with it.
inline constexpr std::size_t NodeRosterFieldCount = 3;

/// Encode a roster summary as one nested record.
/// @param roster The summary.
/// @return Its bytes; never empty, so an engaged roster never reads back as absent.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeRoster(NodeRosterFields const& roster)
{
    auto const version = WireFields::ToBigEndian<std::uint64_t>(roster.version);
    auto const voters = WireFields::ToBigEndian<std::uint32_t>(roster.voters);
    auto const revoked = WireFields::ToBigEndian<std::uint32_t>(roster.revoked);
    return WireFields::Encode({ std::span<std::byte const> { version },
                                std::span<std::byte const> { voters },
                                std::span<std::byte const> { revoked } });
}

/// Encode a fleet pin as one nested record: its tag, then the pin's text or nothing.
/// @param pin The pin.
/// @return Its bytes; never empty, so an engaged pin -- pinned to nothing included -- never reads back
///         as absent.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeFleetPin(NodeFleetPinFields const& pin)
{
    auto const tag =
        std::array { static_cast<std::byte>(pin.fleet.has_value() ? WireFleetPinTag::Pinned : WireFleetPinTag::Unpinned) };
    return WireFields::Encode(
        { std::span<std::byte const> { tag }, pin.fleet.has_value() ? AsBytes(*pin.fleet) : std::span<std::byte const> {} });
}

/// Read a fleet pin.
/// @param field The nested record's bytes; empty is ABSENT.
/// @param out Set only when the field carried a pin.
/// @return False when the field was present and not a pin this build reads: an unknown tag, an id
///         beside `Unpinned`, or none beside `Pinned`.
[[nodiscard]] inline bool ReadNodeFleetPin(std::span<std::byte const> field, std::optional<NodeFleetPinFields>& out)
{
    if (field.empty())
        return true;
    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value() || parts->size() < 2 || (*parts)[0].size() != 1)
        return false;
    auto const id = (*parts)[1];
    switch (static_cast<WireFleetPinTag>((*parts)[0][0]))
    {
        case WireFleetPinTag::Unpinned:
            if (!id.empty())
                return false;
            out = NodeFleetPinFields {};
            return true;
        case WireFleetPinTag::Pinned:
            if (id.empty())
                return false;
            out = NodeFleetPinFields { .fleet = std::string { AsStringView(id) } };
            return true;
    }
    return false;
}

/// Read a roster summary.
/// @param field The nested record's bytes; empty is ABSENT.
/// @param out Set only when the field carried a roster.
/// @return False when the field was present and not a roster this build reads.
[[nodiscard]] inline bool ReadNodeRoster(std::span<std::byte const> field, std::optional<NodeRosterFields>& out)
{
    if (field.empty())
        return true;
    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value() || parts->size() < NodeRosterFieldCount)
        return false;
    auto const version = WireFields::FromBigEndian<std::uint64_t>((*parts)[0]);
    auto const voters = WireFields::FromBigEndian<std::uint32_t>((*parts)[1]);
    auto const revoked = WireFields::FromBigEndian<std::uint32_t>((*parts)[2]);
    if (!version.has_value() || !voters.has_value() || !revoked.has_value())
        return false;
    out = NodeRosterFields { .version = *version, .voters = *voters, .revoked = *revoked };
    return true;
}

namespace Detail
{
    /// Whether this build names a shared-cache source byte.
    ///
    /// A total `switch` with no `default:`, so an enumerator added without a case fails the
    /// build under `-Werror=switch` -- the argument `CachePrePayloadPolicy` makes for a wire enum
    /// that carries no `Last` to size a table by.
    /// @param source The byte, cast.
    /// @return True when it is one of the enumerators.
    [[nodiscard]] constexpr bool NamesSharedCacheSource(WireSharedCacheSource source) noexcept
    {
        switch (source)
        {
            case WireSharedCacheSource::None:
            case WireSharedCacheSource::Setting:
            case WireSharedCacheSource::Override:
            case WireSharedCacheSource::ThisMachine:
                return true;
        }
        return false;
    }

    /// Whether this build names a shared-cache state byte. See `NamesSharedCacheSource`.
    /// @param state The byte, cast.
    /// @return True when it is one of the enumerators.
    [[nodiscard]] constexpr bool NamesSharedCacheState(WireSharedCacheState state) noexcept
    {
        switch (state)
        {
            case WireSharedCacheState::NotTried:
            case WireSharedCacheState::Proven:
            case WireSharedCacheState::Unresolved:
            case WireSharedCacheState::WrongKey:
            case WireSharedCacheState::Unreachable:
            case WireSharedCacheState::Serving:
            case WireSharedCacheState::Unavailable:
            case WireSharedCacheState::ProofRefused:
                return true;
        }
        return false;
    }
} // namespace Detail

/// Fields a `SharedCacheStatusFields` record carries: source, machine, endpoint, state, detail.
/// A reader accepts more, and ignores the surplus.
inline constexpr std::size_t SharedCacheStatusFieldCount = 5;

/// Encode a shared-cache status as one nested record.
/// @param status The status.
/// @return Its bytes; never empty, so an engaged record never reads back as absent.
[[nodiscard]] inline std::vector<std::byte> EncodeSharedCacheStatus(SharedCacheStatusFields const& status)
{
    auto const source = std::array { static_cast<std::byte>(status.source) };
    auto const state = std::array { static_cast<std::byte>(status.state) };
    return WireFields::Encode({ std::span<std::byte const> { source },
                                AsBytes(status.machineId),
                                AsBytes(status.endpoint),
                                std::span<std::byte const> { state },
                                AsBytes(status.detail) });
}

/// Read a shared-cache status.
/// @param field The nested record's bytes; empty is ABSENT.
/// @param out Set only when the field carried a record this build can name.
/// @return False when the field was present and not a record this build reads.
[[nodiscard]] inline bool ReadSharedCacheStatus(std::span<std::byte const> field,
                                                std::optional<SharedCacheStatusFields>& out)
{
    if (field.empty())
        return true;
    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value() || parts->size() < SharedCacheStatusFieldCount || (*parts)[0].size() != 1
        || (*parts)[3].size() != 1)
        return false;
    auto const source = static_cast<WireSharedCacheSource>((*parts)[0][0]);
    auto const state = static_cast<WireSharedCacheState>((*parts)[3][0]);
    // A byte this build does not name is SKIPPED -- the record stays absent -- never read as a
    // neighbour: the enrollment field's rule, for the same reason.
    if (!Detail::NamesSharedCacheSource(source) || !Detail::NamesSharedCacheState(state))
        return true;
    out = SharedCacheStatusFields { .source = source,
                                    .machineId = std::string { AsStringView((*parts)[1]) },
                                    .endpoint = std::string { AsStringView((*parts)[2]) },
                                    .state = state,
                                    .detail = std::string { AsStringView((*parts)[4]) } };
    return true;
}

/// Frame a runtime record as one nested field list.
///
/// Absent facts travel as ZERO-LENGTH fields rather than as zero values, exactly as
/// `EncodeCapacity`'s reserve does: they mean different things, and a wire that could
/// not tell them apart would put the whole distinction back on the sender.
/// @param runtime The facts to encode.
/// @return The nested record's bytes, to be carried as a single `NodeStatus` field.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeRuntime(NodeRuntimeFields const& runtime)
{
    // Owned locals, every one of them: the spans handed to `Encode` below view these,
    // and a temporary would be gone before the call. The two helpers exist because this
    // record is mostly optionals and the *absent travels as zero length* dance would
    // otherwise be written out seven times -- seven chances to emit a zero VALUE for a
    // fact nobody stated, which is the one mistake this record is shaped to prevent.
    auto const state = Detail::OptionalEnumByte(runtime.toolchains);
    auto const served = WireFields::ToBigEndian<std::uint32_t>(runtime.toolchainsServed);
    auto const discovered = WireFields::ToBigEndian<std::uint32_t>(runtime.toolchainsDiscovered);
    auto const slots = Detail::OptionalBigEndian(runtime.compileSlots);
    auto const inFlight = Detail::OptionalBigEndian(runtime.compilesInFlight);
    auto const role = Detail::OptionalEnumByte(runtime.schedulerRole);
    auto const registered = Detail::OptionalBigEndian(runtime.registrarsRegistered);
    auto const total = Detail::OptionalBigEndian(runtime.registrarsTotal);
    auto const lastRegistration = Detail::OptionalBigEndian(runtime.lastRegistrationSecondsAgo);
    auto const enrollment = Detail::OptionalEnumByte(runtime.enrollment);
    auto const enrollmentPending = Detail::OptionalBigEndian(runtime.enrollmentPending);
    auto const cordon = Detail::OptionalEnumByte(runtime.cordon);
    // Absent as zero length, the same shape every optional here takes; see the member for
    // why an engaged endpoint is never empty.
    auto const consensusEndpoint =
        runtime.consensusEndpoint.has_value() ? AsBytes(*runtime.consensusEndpoint) : std::span<std::byte const> {};
    auto const consensusStanding = Detail::OptionalEnumByte(runtime.consensusStanding);
    // Absent as zero length, and an engaged list is never empty; see the member.
    auto const conditions =
        runtime.conditions.has_value() ? EncodeNodeConditions(*runtime.conditions) : std::vector<std::byte> {};
    auto const identityPublicKey = runtime.identityPublicKey.has_value()
                                       ? std::span<std::byte const> { *runtime.identityPublicKey }
                                       : std::span<std::byte const> {};
    // Absent as zero length; an engaged roster is never empty -- see `EncodeNodeRoster`.
    auto const roster = runtime.roster.has_value() ? EncodeNodeRoster(*runtime.roster) : std::vector<std::byte> {};
    auto const autoApproveLeft = Detail::OptionalBigEndian(runtime.enrollmentAutoApproveSecondsLeft);
    // Absent as zero length; an engaged directory and reason are never empty -- see the members.
    auto const stateDirectory =
        runtime.stateDirectory.has_value() ? AsBytes(*runtime.stateDirectory) : std::span<std::byte const> {};
    auto const stateDirectoryReason =
        runtime.stateDirectoryReason.has_value() ? AsBytes(*runtime.stateDirectoryReason) : std::span<std::byte const> {};
    // Absent as zero length; an engaged record is never empty -- see `EncodeSharedCacheStatus`.
    auto const sharedCache =
        runtime.sharedCache.has_value() ? EncodeSharedCacheStatus(*runtime.sharedCache) : std::vector<std::byte> {};
    // Absent as zero length; an engaged fleet id is never empty, and an engaged pin never encodes empty.
    auto const fleetId = runtime.fleetId.has_value() ? AsBytes(*runtime.fleetId) : std::span<std::byte const> {};
    auto const fleetPin = runtime.fleetPin.has_value() ? EncodeNodeFleetPin(*runtime.fleetPin) : std::vector<std::byte> {};

    // Positional, so the ORDER here is the wire contract for this record. Append only:
    // an insertion shifts every later field and every peer decodes one fact as the next.
    return WireFields::Encode({ state,
                                std::span<std::byte const> { served },
                                std::span<std::byte const> { discovered },
                                slots,
                                inFlight,
                                role,
                                AsBytes(runtime.leaderEndpoint),
                                registered,
                                total,
                                lastRegistration,
                                enrollment,
                                enrollmentPending,
                                cordon,
                                consensusEndpoint,
                                consensusStanding,
                                conditions,
                                identityPublicKey,
                                roster,
                                autoApproveLeft,
                                stateDirectory,
                                stateDirectoryReason,
                                sharedCache,
                                fleetId,
                                fleetPin });
}

/// Read a runtime record back.
///
/// A record holding fewer fields than this build expects is accepted with the rest
/// left at "did not say", and one holding more is accepted with the surplus ignored.
/// What is NOT tolerated is a field of the wrong width -- that is a sender speaking a
/// shape this build does not know, and reading its first bytes would invent a number.
///
/// A `toolchains` byte this build has no name for is left DISENGAGED rather than
/// refused, which is `DecodeNodeStatus`' rule for an unknown `WireSurface` one level
/// up: an older client meeting a newer node reports what it understands rather than
/// declining to report anything.
/// @param field The nested record's bytes.
/// @return The facts, or nullopt when the record itself is malformed.
[[nodiscard]] inline std::optional<NodeRuntimeFields> DecodeNodeRuntime(std::span<std::byte const> field)
{
    // An absent record is not a malformed one: a peer that predates this field, or one
    // that had nothing to say, is answered rather than refused.
    if (field.empty())
        return NodeRuntimeFields {};

    auto const parts = WireFields::SplitAll(field);
    if (!parts.has_value())
        return std::nullopt;

    NodeRuntimeFields out {};
    auto const at = [&](std::size_t index) {
        return index < parts->size() ? (*parts)[index] : std::span<std::byte const> {};
    };

    if (auto const state = at(0); !state.empty())
    {
        if (state.size() != 1)
            return std::nullopt;
        switch (static_cast<ToolchainState>(state[0]))
        {
            case ToolchainState::Surveying:
            case ToolchainState::Serving:
            case ToolchainState::NothingToServe:
                out.toolchains = static_cast<ToolchainState>(state[0]);
                break;
            default:
                break;
        }
    }
    if (auto const served = at(1); !served.empty())
    {
        auto const value = WireFields::FromBigEndian<std::uint32_t>(served);
        if (!value.has_value())
            return std::nullopt;
        out.toolchainsServed = *value;
    }
    if (auto const discovered = at(2); !discovered.empty())
    {
        auto const value = WireFields::FromBigEndian<std::uint32_t>(discovered);
        if (!value.has_value())
            return std::nullopt;
        out.toolchainsDiscovered = *value;
    }

    // The optional half of the record. `ReadOptionalBigEndian` holds the *empty is did
    // not say, wrong width is refused* rule so these cannot drift apart one field at a
    // time -- a relaxed copy would be a silent wrong number rather than a refusal.
    if (!Detail::ReadOptionalBigEndian(at(3), out.compileSlots)
        || !Detail::ReadOptionalBigEndian(at(4), out.compilesInFlight)
        || !Detail::ReadOptionalBigEndian(at(7), out.registrarsRegistered)
        || !Detail::ReadOptionalBigEndian(at(8), out.registrarsTotal)
        || !Detail::ReadOptionalBigEndian(at(9), out.lastRegistrationSecondsAgo)
        || !Detail::ReadOptionalBigEndian(at(11), out.enrollmentPending)
        // Field 18, appended behind the roster. Empty is ABSENT, and so is a record from a
        // build before it: no deadline armed, which is `Manual`'s reading.
        || !Detail::ReadOptionalBigEndian(at(18), out.enrollmentAutoApproveSecondsLeft))
        return std::nullopt;

    if (auto const role = at(5); !role.empty())
    {
        if (role.size() != 1)
            return std::nullopt;
        // Skipped rather than refused when unnamed, the same rule the toolchain state
        // above and `WireSurface` one level up both hold: an older client meeting a
        // newer node reports what it understands rather than declining the whole reply.
        switch (static_cast<WireSchedulerRole>(role[0]))
        {
            case WireSchedulerRole::Follower:
            case WireSchedulerRole::Undecided:
            case WireSchedulerRole::Leader:
                out.schedulerRole = static_cast<WireSchedulerRole>(role[0]);
                break;
            default:
                break;
        }
    }

    if (auto const enrollment = at(10); !enrollment.empty())
    {
        if (enrollment.size() != 1)
            return std::nullopt;
        // Skipped rather than refused when unnamed, for the reason the role above is:
        // an older client meeting a newer node reports what it understands. Leaving it
        // disengaged reads as *this node said nothing about a window*, which is the
        // honest answer to a byte this build cannot name -- and is safe in the one
        // direction that matters, because the alarming state is `AutoApprove` and a
        // client that cannot see it also cannot report a reassuring `Manual`. A RETIRED
        // byte is one this build cannot name, so it is skipped the same way.
        if (IsKnownEnrollmentState(static_cast<std::uint8_t>(enrollment[0])))
            out.enrollment = static_cast<WireEnrollmentState>(enrollment[0]);
    }

    if (auto const cordon = at(12); !cordon.empty())
    {
        if (cordon.size() != 1)
            return std::nullopt;
        // Skipped rather than refused when unnamed, for the enrollment state's reason.
        switch (static_cast<WireCordonState>(cordon[0]))
        {
            case WireCordonState::Serving:
            case WireCordonState::Draining:
            case WireCordonState::Drained:
                out.cordon = static_cast<WireCordonState>(cordon[0]);
                break;
            default:
                break;
        }
    }

    // No width to check and nothing to refuse: empty IS the reading here, and it means
    // no leader is known. See the member.
    out.leaderEndpoint = std::string { AsStringView(at(6)) };

    // Empty is ABSENT here, unlike the leader above: an engaged endpoint is never empty,
    // and a sender that predates the field sends fewer fields, which `at` answers empty.
    if (auto const consensusEndpoint = at(13); !consensusEndpoint.empty())
        out.consensusEndpoint = std::string { AsStringView(consensusEndpoint) };

    if (auto const standing = at(14); !standing.empty())
    {
        if (standing.size() != 1)
            return std::nullopt;
        // Skipped rather than refused when unnamed, for the enrollment state's reason.
        switch (static_cast<WireConsensusStanding>(standing[0]))
        {
            case WireConsensusStanding::NoCluster:
            case WireConsensusStanding::Voter:
            case WireConsensusStanding::Learner:
            case WireConsensusStanding::Outsider:
                out.consensusStanding = static_cast<WireConsensusStanding>(standing[0]);
                break;
            default:
                break;
        }
    }

    // Empty is ABSENT, and so is a record from a build before #1364, which `at` answers empty. A
    // list this build cannot read refuses the record, for the rule every field above follows: a
    // shape this build does not know is not read as a partial answer. Field 15, after #1449's
    // standing at 14: both were appended to one base, and the integration of the two orders them.
    if (!ReadNodeConditions(at(15), out.conditions))
        return std::nullopt;

    // Exactly the width of a key, or absent. A field of any other width is refused rather
    // than read, for the record's own reason: a prefix of a key is a different key, and
    // printing one would hand an operator a string to compare that no machine holds. Field
    // 16: #178 appended it behind #1449's standing on a base without #1364's conditions, and
    // the integration puts it after them.
    if (auto const key = at(16); !key.empty())
    {
        if (key.size() != IdentityPublicKeyBytes)
            return std::nullopt;
        out.identityPublicKey.emplace();
        std::ranges::copy(key, out.identityPublicKey->begin());
    }

    // Field 17 (#178 PR 5). Empty is ABSENT, and so is a record from a build before it.
    if (!ReadNodeRoster(at(17), out.roster))
        return std::nullopt;

    // Fields 19 and 20, appended behind the auto-approve deadline. Empty is ABSENT, as for the
    // consensus endpoint: an engaged directory is never empty. **Both or neither**: a path
    // without its reason cannot be told from the machine's other identity, and a reason without
    // a path names nothing -- so a record carrying one alone is a shape this build does not know,
    // and is refused for the rule every field above follows rather than read as half an answer.
    auto const directory = at(19);
    auto const reason = at(20);
    if (directory.empty() != reason.empty())
        return std::nullopt;
    if (!directory.empty())
    {
        out.stateDirectory = std::string { AsStringView(directory) };
        out.stateDirectoryReason = std::string { AsStringView(reason) };
    }

    // Field 21, appended behind the state directory. Empty is ABSENT, and so is a record from a
    // build before it; a source or state byte this build does not name leaves it absent rather
    // than refusing the whole reply.
    if (!ReadSharedCacheStatus(at(21), out.sharedCache))
        return std::nullopt;

    // Fields 22 and 23, the fleet pin's: empty is ABSENT, as for the consensus endpoint, and a pin
    // record this build cannot read refuses the record for the rule every field above follows.
    if (auto const fleetId = at(22); !fleetId.empty())
        out.fleetId = std::string { AsStringView(fleetId) };
    if (!ReadNodeFleetPin(at(23), out.fleetPin))
        return std::nullopt;

    return out;
}

/// What a node answers `NodeStatus` with.
struct NodeStatusFields
{
    std::string version;                 ///< The node's compiled-in version string.
    std::string nodeId;                  ///< Its minted identity, or empty when it runs no consensus.
    std::uint64_t uptimeSeconds { 0 };   ///< How long this process has served.
    std::vector<SurfaceReport> surfaces; ///< Every surface it actually opened.
    /// Which components it runs, as a bitmask -- see `NodeComponentBit`.
    std::uint32_t components { 0 };
    /// What those components are DOING, as opposed to which of them started.
    ///
    /// The one extensible field: see `NodeRuntimeFields` for why every later runtime
    /// fact belongs in here rather than beside it.
    NodeRuntimeFields runtime {};
};

/// Bits of `NodeStatusFields::components`.
///
/// A mask rather than four booleans, because the set grows and a client must be able to
/// step over a bit it does not know. A bit this build does not name is IGNORED rather
/// than refused: an older client meeting a newer node should report the components it
/// understands, not decline to report anything.
namespace NodeComponentBit
{
    constexpr std::uint32_t CacheTier = 0b0001; ///< Serves the cache verbs.
    constexpr std::uint32_t Worker = 0b0010;    ///< Runs compiles.
    constexpr std::uint32_t Scheduler = 0b0100; ///< Spends the fleet's capacity.
    constexpr std::uint32_t Consensus = 0b1000; ///< Participates in Raft.
} // namespace NodeComponentBit

/// Frame a NODE-STATUS request.
///
/// No fields at all, which is a real shape rather than a placeholder: there is nothing
/// to ask a node about itself WITH, so a payload that is not empty is a client this
/// build does not understand and the field-count check refuses it on the count alone.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeStatusRequest(WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::NodeStatus, {});
}

/// Frame a NODE-METRICS request.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeMetricsRequest(WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::NodeMetrics, {});
}

/// Frame an EXPLAIN-ADMISSION request (#1471).
/// @param subject A machine id or its identity key's text; EMPTY asks about the caller's own
///        connection.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeExplainAdmissionRequest(std::string_view subject,
                                                                          WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::ExplainAdmission, { AsBytes(subject) });
}

/// Read an EXPLAIN-ADMISSION payload.
///
/// The subject is returned as a STRING rather than a view: the payload is a borrowed span and a
/// caller that held a view of it past the frame would read freed bytes, which on this field is a
/// membership decision made from freed memory rather than a wrong number.
///
/// @param payload The bytes following the request header.
/// @return The subject -- empty for the caller itself -- or nullopt when the fields do not exactly
///         fill the payload.
[[nodiscard]] inline std::optional<std::string> DecodeExplainAdmissionPayload(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, OpFieldCount(Op::ExplainAdmission));
    if (!fields.has_value())
        return std::nullopt;
    return std::string { AsStringView((*fields)[0]) };
}

/// Frame an EXPLAIN-ADMISSION reply: `[verdict u8] [routes u32] [standing u8, or EMPTY] [subject]`.
///
/// The standing is EMPTY rather than a sentinel byte when the question was about the caller: a
/// connection has no standing in the roster, and a byte meaning "none" would be a sixth standing
/// every reader had to know not to print.
/// @param fields What this node concluded.
/// @return The reply body.
[[nodiscard]] inline std::vector<std::byte> EncodeAdmissionExplanation(AdmissionExplanationFields const& fields)
{
    auto const verdict = std::array { static_cast<std::byte>(fields.verdict) };
    auto const standing = std::array { static_cast<std::byte>(fields.standing.value_or(WireMachineStanding::Unknown)) };
    auto const standingField =
        fields.standing.has_value() ? std::span<std::byte const> { standing } : std::span<std::byte const> {};
    return WireFields::Encode({ std::span<std::byte const> { verdict },
                                std::span<std::byte const> { EncodeU32Field(fields.decidedBy) },
                                standingField,
                                AsBytes(fields.subject) });
}

/// Read an EXPLAIN-ADMISSION reply.
///
/// **An unknown verdict byte is REFUSED rather than defaulted.** A reader that fell back to
/// `Outsider` would report a host as refused-by-nobody on a build that had learned a fourth
/// answer, which reads exactly like the healthy case — and this verb exists to remove a
/// confident wrong signal, not to add one.
///
/// **An unknown ROUTE bit is kept**, which is the opposite decision and deliberate: the routes
/// are a SET, a bit this build cannot name still says *something decided*, and dropping it would
/// under-report authorship on a fleet mid-upgrade. The verdict is one value and must be known;
/// the set is evidence and may be partial.
///
/// **An unknown STANDING byte is refused**, for the verdict's reason: it is one value, and a
/// reader that guessed would report a machine's place in the roster wrongly and confidently.
///
/// @param payload The reply body.
/// @return The explanation, or nullopt when it is malformed or names no verdict or standing this
///         build has.
[[nodiscard]] inline std::optional<AdmissionExplanationFields> DecodeAdmissionExplanation(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, 4);
    if (!fields.has_value() || (*fields)[0].size() != 1 || (*fields)[2].size() > 1)
        return std::nullopt;

    auto const decidedBy = DecodeU32Field((*fields)[1]);
    if (!decidedBy.has_value())
        return std::nullopt;

    auto const verdict = static_cast<WireMembership>(std::to_integer<std::uint8_t>((*fields)[0][0]));
    switch (verdict)
    {
        case WireMembership::Outsider:
        case WireMembership::Member:
        case WireMembership::Forgotten:
            break;
        default:
            return std::nullopt;
    }

    auto standing = std::optional<WireMachineStanding> {};
    if (!(*fields)[2].empty())
    {
        auto const tag = static_cast<WireMachineStanding>(std::to_integer<std::uint8_t>((*fields)[2][0]));
        switch (tag)
        {
            case WireMachineStanding::Voter:
            case WireMachineStanding::Learner:
            case WireMachineStanding::Pending:
            case WireMachineStanding::Revoked:
            case WireMachineStanding::Unknown:
                standing = tag;
                break;
            default:
                return std::nullopt;
        }
    }
    return AdmissionExplanationFields { .verdict = verdict,
                                        .decidedBy = *decidedBy,
                                        .standing = standing,
                                        .subject = std::string { AsStringView((*fields)[3]) } };
}

/// What a CORDON asks for: cordon the worker, or lift the cordon.
///
/// Explicit values because these bytes are transmitted.
enum class CordonAction : std::uint8_t
{
    Lift = 0x00,   ///< Take compiles again.
    Cordon = 0x01, ///< Refuse new compiles; let the running ones finish.
};

/// Frame a CORDON request.
/// @param action Cordon, or lift it.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeCordonRequest(CordonAction action, WireVersion version = CurrentVersion)
{
    auto const byte = std::array { static_cast<std::byte>(action) };
    return Detail::EncodeRequest(version, Op::Cordon, { std::span<std::byte const> { byte } });
}

/// Read a CORDON payload.
/// @param payload The bytes following the request header.
/// @return The action, or nullopt for a payload that is not one byte naming one.
[[nodiscard]] inline std::optional<CordonAction> DecodeCordonPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Cordon));
    if (!fields.has_value() || (*fields)[0].size() != 1)
        return std::nullopt;
    switch (static_cast<CordonAction>((*fields)[0][0]))
    {
        case CordonAction::Lift:
        case CordonAction::Cordon:
            return static_cast<CordonAction>((*fields)[0][0]);
    }
    return std::nullopt;
}

/// What a CORDON is answered with: the worker's state now, and what is still running.
struct CordonFields
{
    WireCordonState state { WireCordonState::Serving }; ///< After the request took effect.
    std::uint32_t inFlight { 0 };                       ///< Compiles still running.
};

/// Encode a CORDON reply body.
/// @param fields The state and the running count.
/// @return The body, to be carried by an `Ok` reply.
[[nodiscard]] inline std::vector<std::byte> EncodeCordonFields(CordonFields const& fields)
{
    auto const state = std::array { static_cast<std::byte>(fields.state) };
    auto const inFlight = WireFields::ToBigEndian<std::uint32_t>(fields.inFlight);
    return WireFields::Encode({ std::span<std::byte const> { state }, std::span<std::byte const> { inFlight } });
}

/// Read a CORDON reply body.
/// @param body The `Ok` reply's payload.
/// @return The fields, or nullopt when the body is not a state this build names and a count.
[[nodiscard]] inline std::optional<CordonFields> DecodeCordonFields(std::span<std::byte const> body)
{
    auto const parts = WireFields::SplitExactly(body, 2);
    if (!parts.has_value() || (*parts)[0].size() != 1)
        return std::nullopt;
    auto const inFlight = WireFields::FromBigEndian<std::uint32_t>((*parts)[1]);
    if (!inFlight.has_value())
        return std::nullopt;
    switch (static_cast<WireCordonState>((*parts)[0][0]))
    {
        case WireCordonState::Serving:
        case WireCordonState::Draining:
        case WireCordonState::Drained:
            return CordonFields { .state = static_cast<WireCordonState>((*parts)[0][0]), .inFlight = *inFlight };
    }
    return std::nullopt;
}

/// Encode a `NodeStatus` reply body.
/// @param fields What this node is.
/// @return The payload.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeStatus(NodeStatusFields const& fields)
{
    std::vector<std::vector<std::byte>> surfaceRows;
    surfaceRows.reserve(fields.surfaces.size());
    for (auto const& surface: fields.surfaces)
    {
        auto const tag = static_cast<std::uint32_t>(surface.surface);
        surfaceRows.push_back(WireFields::Encode({ std::span<std::byte const> { EncodeU32Field(tag) },
                                                   std::span<std::byte const> { EncodeU32Field(surface.port) },
                                                   std::span<std::byte const> { EncodeU32Field(surface.tls ? 1U : 0U) } }));
    }
    std::vector<std::span<std::byte const>> surfaceViews;
    surfaceViews.reserve(surfaceRows.size());
    for (auto const& row: surfaceRows)
        surfaceViews.emplace_back(row);
    auto const surfaces = WireFields::Encode(WireFields::FieldList { surfaceViews });
    // Named rather than spelled inline, because it must outlive the span that views it
    // -- the surfaces above are held the same way for the same reason.
    auto const runtime = EncodeNodeRuntime(fields.runtime);

    return WireFields::Encode({ AsBytes(fields.version),
                                AsBytes(fields.nodeId),
                                std::span<std::byte const> { EncodeU64Field(fields.uptimeSeconds) },
                                std::span<std::byte const> { EncodeU32Field(fields.components) },
                                std::span<std::byte const> { surfaces },
                                std::span<std::byte const> { runtime } });
}

/// Decode a `NodeStatus` reply body.
/// @param payload The reply body.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<NodeStatusFields> DecodeNodeStatus(std::span<std::byte const> payload)
{
    // Exact at SIX, and it stays exact: the sixth field is the nested record that makes
    // every later runtime fact free, so this number is not expected to move again. See
    // `NodeRuntimeFields`.
    auto const outer = SplitFields(payload, 6);
    if (!outer.has_value())
        return std::nullopt;
    auto const uptime = DecodeU64Field((*outer)[2]);
    auto const components = DecodeU32Field((*outer)[3]);
    if (!uptime.has_value() || !components.has_value())
        return std::nullopt;

    auto const rows = WireFields::SplitAll((*outer)[4]);
    if (!rows.has_value())
        return std::nullopt;

    auto const runtime = DecodeNodeRuntime((*outer)[5]);
    if (!runtime.has_value())
        return std::nullopt;

    NodeStatusFields fields { .version = std::string { AsStringView((*outer)[0]) },
                              .nodeId = std::string { AsStringView((*outer)[1]) },
                              .uptimeSeconds = *uptime,
                              .surfaces = {},
                              .components = *components,
                              .runtime = *runtime };
    fields.surfaces.reserve(rows->size());
    for (auto const& row: *rows)
    {
        auto const parts = WireFields::SplitExactly(row, 3);
        if (!parts.has_value())
            return std::nullopt;
        auto const tag = DecodeU32Field((*parts)[0]);
        auto const port = DecodeU32Field((*parts)[1]);
        auto const tls = DecodeU32Field((*parts)[2]);
        if (!tag.has_value() || !port.has_value() || !tls.has_value())
            return std::nullopt;
        // A port outside the 16-bit range cannot be one, and this number is
        // peer-controlled: refuse rather than truncate, since a truncated port dials
        // something real and wrong.
        if (*port > 0xFFFFU)
            return std::nullopt;
        // A surface tag this build does not name is SKIPPED, not refused -- an older
        // client meeting a newer node reports the surfaces it understands rather than
        // declining to report any.
        switch (static_cast<WireSurface>(*tag))
        {
            case WireSurface::Admin:
            case WireSurface::Raft:
            case WireSurface::Discovery:
                break;
            default:
                continue;
        }
        fields.surfaces.push_back(
            SurfaceReport { .surface = static_cast<WireSurface>(*tag), .port = *port, .tls = *tls != 0U });
    }
    return fields;
}

/// What a joiner is told about its own request.
///
/// Three answers and no fourth, because the fourth -- *the window is closed*, *the
/// list is full*, *ask another machine* -- are refusals with codes of their own, and a
/// success status carrying a failure enumerator is how a client comes to ignore the
/// status byte.
///
/// Explicit values because these bytes are transmitted.
enum class EnrollOutcome : std::uint8_t
{
    /// Not admitted yet: nobody has decided, or an approval has not reached the leader's own
    /// roster. **Carries no roster**, and a client asserts that rather than assumes it -- the
    /// roster names every member and where it answers, which goes to a machine an operator
    /// approved and to nobody else.
    Pending = 0x01,

    /// The cluster records this joiner, and the payload's second field IS the roster
    /// (`Cluster::EncodeRoster`): every member's id, consensus endpoint, seat and key, and every
    /// revoked key. **No secret, so no spend** (#178): the answer is the same
    /// on every poll, and a joiner whose reply was lost simply asks again.
    ///
    /// Answered only once the LEADER's applied roster records the joiner under the key it
    /// asked with, so a joiner that is told `Approved` can check its own entry in the roster
    /// it was handed -- and a roster that lacks it was not produced by this cluster.
    Approved = 0x02,

    /// An operator refused this id. No roster, and asking again will not change it until
    /// that operator decides otherwise.
    Rejected = 0x03,
};

/// What a joiner asks to be admitted as (#178).
///
/// **TRANSMITTED**: explicit values, append-only, and a retired value is never reused -- see
/// `RetiredEnrollRoles`.
enum class EnrollRole : std::uint8_t
{
    // 0x01 was `Member`: a joiner that stated the endpoint its consensus port answers on and
    // was recorded as a voter. A zero-config joiner is recorded as a LEARNER and states no
    // consensus endpoint, so the byte is RETIRED and never reused.
    //
    // 0x02 was `Worker`: a principal admitted by its key alone, never joining consensus.
    // Principal mode is retired -- every machine that joins is a learner member holding a key
    // -- so the byte is RETIRED and never reused.

    /// A consensus learner: an approval records it as a member in the learner seat, holding the
    /// key it asked with. It states no CONSENSUS endpoint -- a learner dials the leader rather
    /// than being dialled -- and becomes a voter only when an operator promotes it.
    Learner = 0x03,
};

/// Every role this build implements, as ONE list, for `KnownEnrollmentDecisions`' reason: the
/// decoder refuses a byte outside it and the renderer asserts a label for every member.
inline constexpr std::array KnownEnrollRoles { EnrollRole::Learner };

/// Role bytes that once meant something and must never mean anything again, for
/// `RetiredErrorCodes`' reason: a peer built before the retirement still names the byte, and
/// would admit a new role under it as the old one.
inline constexpr std::array<std::uint8_t, 2> RetiredEnrollRoles { 0x01, 0x02 };

/// Whether the role list has kept clear of every retired byte.
/// @return True when no known role claims a retired byte.
[[nodiscard]] consteval bool NoRetiredEnrollRoleIsReused() noexcept
{
    return std::ranges::none_of(KnownEnrollRoles, [](EnrollRole role) {
        return std::ranges::contains(RetiredEnrollRoles, static_cast<std::uint8_t>(role));
    });
}

static_assert(NoRetiredEnrollRoleIsReused(),
              "0x01 was member and 0x02 worker; a retired enrollment-role byte is never reassigned");

/// Whether @p raw names a role this build understands.
/// @param raw The role byte, as received.
/// @return True when it names one.
[[nodiscard]] constexpr bool IsKnownEnrollRole(std::uint8_t raw) noexcept
{
    return std::ranges::any_of(KnownEnrollRoles, [raw](EnrollRole role) { return static_cast<std::uint8_t>(role) == raw; });
}

/// How many bytes a roster fingerprint carries on the wire: one SHA-256 digest.
///
/// Spelled HERE rather than taken from `Core/Sha256.hpp`, for `NodeChallengeBytes`' reason:
/// this header is what the launcher compiles in. `EnrollmentResponder.cpp` pins the two to each
/// other, where both are visible.
inline constexpr std::size_t RosterFingerprintBytes = 32;

/// What an operator is asking of the window.
///
/// **TRANSMITTED**: explicit values, append-only, and a retired value is never reused -- see
/// `RetiredEnrollControlVerbs`.
enum class EnrollControlVerb : std::uint8_t
{
    // 0x01 was `Open` and 0x02 was `Close`: a window an operator opened for a moment. Since
    // zero-config formation every request is recorded, so there is nothing to open. RETIRED.

    List = 0x03, ///< Report the window and everything waiting. Names no subject.

    // 0x04 was `Approve` naming an id ALONE, which admitted whatever key the row under that id
    // held when the approval arrived -- after an expiry, a machine the operator never saw. RETIRED:
    // the approval names the key it means (`Approve`, below).

    Reject = 0x05,         ///< Refuse the named id.
    AutoApprove = 0x06,    ///< Arm, or re-arm from now, a deadline before which a joiner is admitted.
    AutoApproveOff = 0x07, ///< End the armed deadline. Names no subject.
    Clear = 0x08,          ///< Drop every request nobody decided about. Names no subject.
    Approve = 0x09,        ///< Admit the named id under the key named WITH it, and hand it the roster.
};

/// Verb bytes that once meant something and must never mean anything again, for
/// `RetiredErrorCodes`' reason: a peer built before the retirement still sends the byte, and
/// would be answered as though it had asked for the new verb.
inline constexpr std::array<std::uint8_t, 3> RetiredEnrollControlVerbs { 0x01, 0x02, 0x04 };

/// What a verb's second field carries.
///
/// A private enum: never transmitted, never persisted. It is a column of
/// `EnrollControlVerbTable`, and no byte of it reaches a wire.
enum class EnrollSubject : std::uint8_t
{
    None,         ///< An empty field.
    NodeId,       ///< A pending id: non-empty.
    Seconds,      ///< A duration: a non-empty run of ASCII digits, whole seconds.
    NodeIdAndKey, ///< A pending id, then the `IdentityPublicKeyBytes` of the key it is approved under.
};

/// One live enroll-control verb.
struct EnrollControlVerbRow
{
    EnrollControlVerb verb; ///< The wire verb.
    std::string_view name;  ///< Its one spelling in a sentence.
    EnrollSubject subject;  ///< What its second field carries.
};

/// One row per verb this build implements.
///
/// **The table is what the decoder judges a request by**: whether the byte is known, and what
/// its second field must hold, are one row, so the encoder, the decoder and the surface cannot
/// come to disagree about which requests are well formed. A plain array rather than an
/// `EnumTable`, for `KnownEnrollmentDecisions`' reason: a WIRE enum carries no `Last`.
inline constexpr std::array EnrollControlVerbTable {
    EnrollControlVerbRow { .verb = EnrollControlVerb::List, .name = "list", .subject = EnrollSubject::None },
    EnrollControlVerbRow { .verb = EnrollControlVerb::Approve, .name = "approve", .subject = EnrollSubject::NodeIdAndKey },
    EnrollControlVerbRow { .verb = EnrollControlVerb::Reject, .name = "reject", .subject = EnrollSubject::NodeId },
    EnrollControlVerbRow {
        .verb = EnrollControlVerb::AutoApprove, .name = "auto-approve", .subject = EnrollSubject::Seconds },
    EnrollControlVerbRow {
        .verb = EnrollControlVerb::AutoApproveOff, .name = "auto-approve-off", .subject = EnrollSubject::None },
    EnrollControlVerbRow { .verb = EnrollControlVerb::Clear, .name = "clear", .subject = EnrollSubject::None },
};

/// Whether a byte names an `EnrollControlVerb` this build implements.
///
/// A table walk rather than a `<=` range test, for `IsKnownStatus`' reason: a range
/// test silently admits every enumerator added later, including one this build cannot
/// act on, and the surface would then dispatch on a verb it has no arm for.
/// @param raw The verb byte, as received.
/// @return True when it names a verb.
[[nodiscard]] constexpr bool IsKnownEnrollControlVerb(std::uint8_t raw) noexcept
{
    return std::ranges::any_of(EnrollControlVerbTable, [raw](EnrollControlVerbRow const& row) {
        return static_cast<std::uint8_t>(row.verb) == raw;
    });
}

/// What @p verb's second field carries.
/// @param verb A verb the decoder accepted.
/// @return Its row's subject; `None` for a value no row names, which `IsKnownEnrollControlVerb`
///         keeps from reaching here.
[[nodiscard]] constexpr EnrollSubject EnrollControlSubjectOf(EnrollControlVerb verb) noexcept
{
    for (auto const& row: EnrollControlVerbTable)
        if (row.verb == verb)
            return row.subject;
    return EnrollSubject::None;
}

/// Whether the verb table has kept clear of every retired byte.
/// @return True when no row claims a retired byte.
[[nodiscard]] consteval bool NoRetiredEnrollControlVerbIsReused() noexcept
{
    return std::ranges::none_of(EnrollControlVerbTable, [](EnrollControlVerbRow const& row) {
        return std::ranges::contains(RetiredEnrollControlVerbs, static_cast<std::uint8_t>(row.verb));
    });
}

static_assert(NoRetiredEnrollControlVerbIsReused(),
              "0x01 was open, 0x02 close and 0x04 approve-by-id-alone; a retired verb byte is never reassigned");

/// A challenge a leader issues for one pending enrollment row: `NodeChallengeBytes` it drew, which
/// the joiner's NEXT request must sign over for that request to refresh the row.
using EnrollChallenge = std::array<std::byte, NodeChallengeBytes>;

/// The bytes a challenge travels and is signed as: all of them, or EMPTY when there is none -- the
/// one spelling every encoder and every signed claim uses, so the two cannot disagree about absence.
/// @param challenge The challenge, or none.
/// @return A view of it, valid while @p challenge lives.
[[nodiscard]] inline std::span<std::byte const> ChallengeBytes(std::optional<EnrollChallenge> const& challenge)
{
    return challenge.has_value() ? std::span<std::byte const> { *challenge } : std::span<std::byte const> {};
}

/// Deleted: a view of a temporary challenge dangles before it is read.
std::span<std::byte const> ChallengeBytes(std::optional<EnrollChallenge> const&& challenge) = delete;

/// A machine asking to be let in, under the identity it minted.
///
/// **The key is PUBLIC and the request carries nothing secret** (#178): what an operator checks
/// is that the key the leader lists is the one the joiner printed.
///
/// **And the request is SIGNED by that key** (`Cluster::SignEnrollRequest`), over every other field:
/// the id and the key are public -- a beacon carries one, the roster both -- so an unsigned request
/// let any host poll under a joiner's id and key with an endpoint of its own, and the last poll
/// before approval was what the record kept. Signed, only the key's holder can create or refresh a
/// row under it; the leader verifies before it records anything (`EnrollmentResponder`).
struct EnrollRequest
{
    std::string_view nodeId; ///< The identity it minted into its own `--cluster-dir`.

    /// The joiner's CURRENT advertised `0xFC` endpoint -- where the fleet reaches its node
    /// surface -- and never a consensus endpoint, which a learner does not have.
    std::string_view nodeEndpoint;

    EnrollRole role { EnrollRole::Learner }; ///< What it asks to be admitted as.
    std::span<std::byte const> publicKey;    ///< Its identity key: `IdentityPublicKeyBytes` of it.

    /// `NodeChallengeBytes` the joiner drew for THIS request, which an `Approved` answer is signed
    /// over: what makes a recorded admission worthless to anybody who replays it at a later ask.
    std::span<std::byte const> nonce;

    /// The latest challenge the leader issued this joiner (`EnrollReplyView::challenge`), or EMPTY on
    /// a first request. **What makes a refresh FRESH**: the joiner's own nonce gives the JOINER
    /// freshness, and nothing gave the LEADER any, so a verbatim replay of an earlier genuine request
    /// rolled a pending row back to an endpoint the joiner had left. A row refreshes only from a
    /// request signed over the challenge it holds now, which it then replaces.
    std::span<std::byte const> challenge;

    /// `NodeSignatureBytes`: the joiner's signature, under `publicKey`, over every field above
    /// (`Cluster::EnrollRequestMessage`).
    std::span<std::byte const> signature;
};

/// The same, decoded from a received payload. The id and the endpoint are views; the role and
/// the key are values, because the decoder has already checked what they are.
struct EnrollView
{
    std::span<std::byte const> nodeId;                          ///< The id, as sent.
    std::span<std::byte const> nodeEndpoint;                    ///< The `0xFC` endpoint, as sent.
    EnrollRole role { EnrollRole::Learner };                    ///< What it asks to be admitted as.
    std::array<std::byte, IdentityPublicKeyBytes> publicKey {}; ///< Its identity key.
    std::array<std::byte, NodeChallengeBytes> nonce {};         ///< What an admission is signed over.
    std::optional<EnrollChallenge> challenge {};                ///< The leader's it answers; none on a first ask.
    std::array<std::byte, NodeSignatureBytes> signature {};     ///< The joiner's, over every field above.
};

/// Frame an ENROLL request.
///
/// Seven fields. The id, the `0xFC` endpoint the joiner currently advertises -- shown to the
/// operator beside the host the request came from -- the role because a learner and a worker
/// are admitted by different commands, the key because it is what the cluster records
/// instead of handing a secret back, a nonce the answering node signs an admission over, the
/// leader's latest challenge (empty on a first ask), and the joiner's own signature over the six
/// before it, which is what makes the endpoint its own and current word.
/// @param request Who is asking, as what, under which key, and where it will answer.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeEnroll(EnrollRequest const& request, WireVersion version = CurrentVersion)
{
    std::array<std::byte, 1> const role { static_cast<std::byte>(request.role) };
    return Detail::EncodeRequest(version,
                                 Op::Enroll,
                                 { AsBytes(request.nodeId),
                                   AsBytes(request.nodeEndpoint),
                                   std::span<std::byte const> { role },
                                   request.publicKey,
                                   request.nonce,
                                   request.challenge,
                                   request.signature });
}

/// Split an ENROLL payload.
///
/// Refuses a role this build does not implement, a key that is not exactly one key wide -- a
/// prefix of a key is a different key, and an operator would be shown a string no machine holds --
/// a nonce that is not exactly `NodeChallengeBytes`, which both ends would sign and verify over
/// different bytes, a challenge that is neither empty nor exactly that wide, and a signature that is
/// not exactly `NodeSignatureBytes`, which verifies nothing. Whether the endpoint suits the role, and whether the signature
/// VERIFIES, are the RESPONDER's to refuse, each with a sentence of its own.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<EnrollView> DecodeEnrollPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Enroll));
    if (!fields.has_value())
        return std::nullopt;
    auto const role = (*fields)[2];
    auto const key = (*fields)[3];
    auto const nonce = (*fields)[4];
    auto const challenge = (*fields)[5];
    auto const signature = (*fields)[6];
    if (role.size() != 1 || !IsKnownEnrollRole(static_cast<std::uint8_t>(role[0])) || key.size() != IdentityPublicKeyBytes
        || nonce.size() != NodeChallengeBytes || (!challenge.empty() && challenge.size() != NodeChallengeBytes)
        || signature.size() != NodeSignatureBytes)
        return std::nullopt;
    auto view = EnrollView { .nodeId = (*fields)[0],
                             .nodeEndpoint = (*fields)[1],
                             .role = static_cast<EnrollRole>(role[0]),
                             .publicKey = {},
                             .nonce = {},
                             .challenge = std::nullopt,
                             .signature = {} };
    std::ranges::copy(key, view.publicKey.begin());
    std::ranges::copy(nonce, view.nonce.begin());
    if (!challenge.empty())
        std::ranges::copy(challenge, view.challenge.emplace().begin());
    std::ranges::copy(signature, view.signature.begin());
    return view;
}

/// The answering node's signature over its answer -- of any outcome: its identity key, and what that key signed.
///
/// Owned, fixed-width fields, for `FleetSummaryReply`'s reason; `Cluster/EnrollAdmissionSignature.hpp`
/// pins both to the types the signature primitive takes.
struct EnrollReplySignature
{
    std::array<std::byte, IdentityPublicKeyBytes> publicKey {}; ///< The answering node's identity key.
    std::array<std::byte, NodeSignatureBytes> signature {};     ///< Its signature over the admission.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(EnrollReplySignature const&, EnrollReplySignature const&) = default;
};

/// What an ENROLL was answered with, as views into the reply payload -- but for the signature, which
/// is copied out as `DecodeFleetSummaryReply` copies its own.
struct EnrollReplyView
{
    /// What the leader decided, so far.
    ///
    /// Defaulted to `Pending` EXPLICITLY, and not with `{}`: this enum starts at `0x01`,
    /// so a value-initialized member would hold 0, which names no enumerator
    /// (`bugprone-invalid-enum-default-initialization`). `Pending` rather than any other
    /// is the harmless one -- it is the outcome documented to carry no roster, so a view
    /// nobody filled in reads as *not admitted* rather than as an approval.
    EnrollOutcome outcome { EnrollOutcome::Pending };

    std::span<std::byte const> roster {}; ///< The roster, and EMPTY for every outcome but `Approved`.

    /// The challenge this joiner's NEXT request must sign over to refresh its row
    /// (`EnrollRequest::challenge`); DISENGAGED for every outcome but `Pending`, which alone leaves
    /// a row to refresh. Inside what the signature covers, so nobody between the ends can swap it.
    std::optional<EnrollChallenge> challenge {};

    /// The answering node's signature over the answer, and DISENGAGED when the reply carries none.
    /// Every outcome is signed, and a joiner holding a key for the fleet it asked treats an answer
    /// that carries none as no answer at all.
    std::optional<EnrollReplySignature> signature {};
};

/// Frame the payload of an ENROLL reply.
///
/// **The roster travels as a field of its own so that "there is no roster here" is a length of
/// zero rather than a convention.** A client asserts that length, which is the only assertion
/// that can catch a server handing the roster to a machine nobody approved -- an outcome byte is
/// equally correct in both the healthy and the broken build.
///
/// **The signature is two fields, both empty or both exactly one wide**, for the roster's reason: a
/// reply carrying none says so by length, and a joiner treats such an answer as no answer.
///
/// Five fields: the outcome, the roster, the challenge, the key and the signature. The certified
/// roster that once rode third is gone from the grammar (#178) -- an approved machine applies its
/// fleet's state and verifies grants against that -- and the six-field reply that carried it is
/// refused on its count.
/// @param outcome What was decided.
/// @param roster `Cluster::EncodeRoster(Cluster::ProjectRoster(state))`'s bytes for `Approved`, empty otherwise.
/// @param challenge What the joiner's next request must sign over: exactly `NodeChallengeBytes` for
///        `Pending`, empty for every other outcome.
/// @param signature The answering node's signature over this answer, whatever its outcome. **No
///        default**: every outcome is signed, so a call that leaves it out fails to build rather than
///        sending an answer every joiner treats as none; a case that means unsigned says `std::nullopt`.
/// @return The reply payload, to be carried by `EncodeReply(Status::Ok, ...)`.
[[nodiscard]] inline std::vector<std::byte> EncodeEnrollReply(EnrollOutcome outcome,
                                                              std::span<std::byte const> roster,
                                                              std::span<std::byte const> challenge,
                                                              std::optional<EnrollReplySignature> const& signature)
{
    std::array<std::byte, 1> const tag { static_cast<std::byte>(outcome) };
    auto const key =
        signature.has_value() ? std::span<std::byte const> { signature->publicKey } : std::span<std::byte const> {};
    auto const signatureBytes =
        signature.has_value() ? std::span<std::byte const> { signature->signature } : std::span<std::byte const> {};
    return WireFields::Encode({ std::span<std::byte const> { tag }, roster, challenge, key, signatureBytes });
}

/// Read an ENROLL reply payload back.
///
/// An outcome byte this build has no name for is REFUSED rather than skipped, which is
/// the opposite of `DecodeNodeStatus`' rule for an unknown surface tag and is the right
/// answer here: a status report may be partially understood and still useful, while a
/// joiner that cannot tell *approved* from *rejected* has no safe default -- treating
/// an unknown outcome as pending polls forever, and treating it as approved reads a roster
/// out of a field that may hold anything. A signature is both of its fields or neither, each
/// exactly one wide: a prefix of a signature verifies nothing, and a caller must not be handed one
/// to try.
/// @param payload The reply payload.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<EnrollReplyView> DecodeEnrollReply(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, 5);
    if (!fields.has_value())
        return std::nullopt;
    auto const challengeBytes = (*fields)[2];
    if (!challengeBytes.empty() && challengeBytes.size() != NodeChallengeBytes)
        return std::nullopt;
    auto const key = (*fields)[3];
    auto const signatureBytes = (*fields)[4];
    auto const unsignatureBytes = key.empty() && signatureBytes.empty();
    if (!unsignatureBytes && (key.size() != IdentityPublicKeyBytes || signatureBytes.size() != NodeSignatureBytes))
        return std::nullopt;
    auto const tag = (*fields)[0];
    if (tag.size() != 1)
        return std::nullopt;
    switch (static_cast<EnrollOutcome>(tag[0]))
    {
        case EnrollOutcome::Pending:
        case EnrollOutcome::Approved:
        case EnrollOutcome::Rejected:
            break;
        default:
            return std::nullopt;
    }
    auto signature = std::optional<EnrollReplySignature> {};
    if (!unsignatureBytes)
    {
        signature.emplace();
        std::ranges::copy(key, signature->publicKey.begin());
        std::ranges::copy(signatureBytes, signature->signature.begin());
    }
    auto challenge = std::optional<EnrollChallenge> {};
    if (!challengeBytes.empty())
        std::ranges::copy(challengeBytes, challenge.emplace().begin());
    return EnrollReplyView {
        .outcome = static_cast<EnrollOutcome>(tag[0]), .roster = (*fields)[1], .challenge = challenge, .signature = signature
    };
}

/// Frame an ENROLL-CONTROL request.
/// @param verb What to do.
/// @param subject What the verb's `EnrollControlVerbTable` row says its second field carries:
///        the pending id for `Approve`/`Reject`, whole seconds in decimal for `AutoApprove`,
///        empty for the rest.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeEnrollControl(EnrollControlVerb verb,
                                                                std::string_view subject = {},
                                                                WireVersion version = CurrentVersion)
{
    std::array<std::byte, 1> const tag { static_cast<std::byte>(verb) };
    return Detail::EncodeRequest(version, Op::EnrollControl, { std::span<std::byte const> { tag }, AsBytes(subject) });
}

/// Frame an ENROLL-CONTROL request arming an auto-approve deadline @p duration from now.
///
/// The duration travels as whole seconds in decimal, the `Seconds` subject's spelling. Whether
/// it is zero or above the ceiling is not this encoder's to judge, and not the decoder's either:
/// those are the leader's named refusals, so an operator hears which rule rather than *a frame
/// this build cannot read*.
///
/// **A negative duration is a programmer error**, asserted rather than encoded: its decimal
/// spelling carries a sign, which the decoder refuses as malformed, so the frame would be refused
/// as a version mismatch rather than as the caller's mistake it is.
/// @param duration How long the deadline lasts, from when the leader applies it; never negative.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeEnrollAutoApprove(std::chrono::seconds duration,
                                                                    WireVersion version = CurrentVersion)
{
    assert(duration.count() >= 0 && "an auto-approve duration is never negative; the decoder refuses a sign");
    return EncodeEnrollControl(EnrollControlVerb::AutoApprove, std::to_string(duration.count()), version);
}

/// Frame an ENROLL-CONTROL request approving @p nodeId under @p key.
///
/// **The key rides with the id** because the id alone names a ROW, and a row can be replaced:
/// once the machine an operator compared stops asking, its row lapses, and the next machine to
/// ask under that id is recorded under ITS key. An approval naming only the id would admit
/// that one. The leader refuses an approval whose key is not the row's, by name.
///
/// The key's bytes follow the id's in the subject field: a key is exactly
/// `IdentityPublicKeyBytes` long, so the split is unambiguous whatever the id holds.
/// @param nodeId The pending id; non-empty.
/// @param key The key the operator compared.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeEnrollApprove(std::string_view nodeId,
                                                                std::span<std::byte const, IdentityPublicKeyBytes> key,
                                                                WireVersion version = CurrentVersion)
{
    std::array<std::byte, 1> const tag { static_cast<std::byte>(EnrollControlVerb::Approve) };
    auto subject = std::vector<std::byte> {};
    subject.reserve(nodeId.size() + key.size());
    auto const id = AsBytes(nodeId);
    subject.insert(subject.end(), id.begin(), id.end());
    subject.insert(subject.end(), key.begin(), key.end());
    return Detail::EncodeRequest(version, Op::EnrollControl, { std::span<std::byte const> { tag }, subject });
}

/// An operator's request, decoded.
struct EnrollControlView
{
    /// What to do.
    ///
    /// Defaulted to `List` EXPLICITLY, for `EnrollReplyView::outcome`'s reason: this
    /// enum starts at `0x03`, so `{}` would hold a 0 that names no enumerator. `List` is
    /// the choice among the live verbs because it is the only one that CHANGES nothing -- a
    /// view nobody filled in must not read as `AutoApprove`, which is the verb that admits
    /// anyone who asks with nobody comparing a key.
    EnrollControlVerb verb { EnrollControlVerb::List };

    /// The second field as sent, empty for the verbs that name none -- and for a `NodeIdAndKey`
    /// verb the id ALONE, the key split off into `key`.
    std::span<std::byte const> subject {};

    /// The duration a `Seconds` verb carries, read out of `subject`. Present exactly for a
    /// `Seconds` verb, and absent for every other.
    std::optional<std::chrono::seconds> duration {};

    /// The key a `NodeIdAndKey` verb names. Present exactly for such a verb, and absent for every
    /// other.
    std::optional<std::array<std::byte, IdentityPublicKeyBytes>> key {};
};

/// Split an ENROLL-CONTROL payload.
///
/// Refuses a verb this build does not implement -- a retired byte among them -- and refuses a
/// second field that does not match what the verb's `EnrollControlVerbTable` row says it
/// carries: `None` needs an empty field, `NodeId` a non-empty one, `Seconds` a non-empty
/// run of ASCII digits that fits a `std::uint64_t`, and `NodeIdAndKey` a non-empty id followed
/// by exactly a key. An `AutoApproveOff` carrying a duration,
/// or an `Approve` naming nobody, is a client this build does not understand, and answering
/// either by ignoring the mismatch is how an operator comes to believe they approved
/// somebody. **Zero and the ceiling are NOT judged here**: those are the leader's named
/// refusals, so an operator hears which rule rather than *a frame this build cannot read*.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<EnrollControlView> DecodeEnrollControlPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::EnrollControl));
    if (!fields.has_value())
        return std::nullopt;
    auto const tag = (*fields)[0];
    if (tag.size() != 1 || !IsKnownEnrollControlVerb(static_cast<std::uint8_t>(tag[0])))
        return std::nullopt;
    auto const verb = static_cast<EnrollControlVerb>(tag[0]);
    auto const subject = (*fields)[1];
    auto view = EnrollControlView { .verb = verb, .subject = subject, .duration = std::nullopt, .key = std::nullopt };
    switch (EnrollControlSubjectOf(verb))
    {
        case EnrollSubject::None:
            if (!subject.empty())
                return std::nullopt;
            return view;
        case EnrollSubject::NodeId:
            if (subject.empty())
                return std::nullopt;
            return view;
        case EnrollSubject::NodeIdAndKey: {
            // An id of at least one byte, then exactly a key: a field no longer than a key names
            // nobody, which is refused rather than read as a key with an empty id.
            if (subject.size() <= IdentityPublicKeyBytes)
                return std::nullopt;
            auto const split = subject.size() - IdentityPublicKeyBytes;
            auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
            std::ranges::copy(subject.subspan(split), key.begin());
            view.subject = subject.first(split);
            view.key = key;
            return view;
        }
        case EnrollSubject::Seconds: {
            // The integer overload into an UNSIGNED type reads ASCII digits and nothing else --
            // no sign, no whitespace, nothing from the locale -- and the WHOLE field must be
            // consumed: `15m` would otherwise read as fifteen seconds. A count that fits the
            // `uint64_t` but not `seconds::rep` is refused too, or the cast below would read
            // `9223372036854775808` as a NEGATIVE duration.
            auto const text = AsStringView(subject);
            auto seconds = std::uint64_t { 0 };
            auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (error != std::errc {} || end != text.data() + text.size()
                || seconds > static_cast<std::uint64_t>(std::chrono::seconds::max().count()))
                return std::nullopt;
            view.duration = std::chrono::seconds { static_cast<std::chrono::seconds::rep>(seconds) };
            return view;
        }
    }
    return std::nullopt;
}

/// How far one pending request has got.
///
/// Explicit values because these bytes are transmitted.
enum class EnrollmentDecision : std::uint8_t
{
    Pending = 0x01,  ///< Waiting for a person.
    Approved = 0x02, ///< Admitted; every poll from now on is handed the roster.

    // 0x03 was `Collected`, the state a spendable-once key hand-over moved a row into (#178
    // retired it with the secret). RETIRED and never reused: a reader that still names it would
    // show an operator a row that collected something -- see `RetiredEnrollmentDecisions`.

    Rejected = 0x04, ///< Refused by a person.
};

/// Every decision this build implements, as ONE list.
///
/// **Named rather than left as a `switch` in the decoder, because two consumers need
/// it**: the decoder refuses a byte outside this set, and the operator-facing renderer
/// asserts at compile time that it has a label for every member. A second list would
/// catch an enumerator going away and be blind to one ARRIVING -- which on the renderer
/// means a row rendering as whatever a fallback said, in the list somebody reads before
/// admitting a machine to the fleet.
///
/// `EnrollmentDecision` cannot be an `EnumTable`: it is a WIRE enum, so a trailing
/// `Last` would permanently claim a byte, which is the reason `ErrorCode` has none
/// either.
inline constexpr std::array KnownEnrollmentDecisions { EnrollmentDecision::Pending,
                                                       EnrollmentDecision::Approved,
                                                       EnrollmentDecision::Rejected };

/// Decision bytes that once meant something and must never mean anything again, for
/// `RetiredErrorCodes`' reason: a peer built before the retirement still names the byte, and
/// would render a new decision under it as the old one.
inline constexpr std::array<std::uint8_t, 1> RetiredEnrollmentDecisions { 0x03 };

/// Whether the decision list has kept clear of every retired byte.
///
/// Checks `KnownEnrollmentDecisions` rather than the enum, for `NoRetiredErrorCodeIsReused`'s
/// reason: the list is what a decision must join to be decoded at all.
/// @return True when no known decision claims a retired byte.
[[nodiscard]] consteval bool NoRetiredEnrollmentDecisionIsReused() noexcept
{
    return std::ranges::none_of(KnownEnrollmentDecisions, [](EnrollmentDecision decision) {
        return std::ranges::contains(RetiredEnrollmentDecisions, static_cast<std::uint8_t>(decision));
    });
}

static_assert(NoRetiredEnrollmentDecisionIsReused(),
              "a retired enrollment decision must never be reassigned -- a peer built against an older header "
              "still reports it under its old name (0x03 was collected, see #178)");

/// Whether @p raw names a decision this build understands.
/// @param raw The decision byte, as received.
/// @return True when it names one.
[[nodiscard]] constexpr bool IsKnownEnrollmentDecision(std::uint8_t raw) noexcept
{
    return std::ranges::any_of(KnownEnrollmentDecisions,
                               [raw](EnrollmentDecision decision) { return static_cast<std::uint8_t>(decision) == raw; });
}

/// One machine waiting at the door, as the leader sees it.
struct EnrollmentPendingEntry
{
    std::string nodeId;       ///< The identity it claims.
    std::string nodeEndpoint; ///< The `0xFC` endpoint it claims, shown beside `peerId`.

    /// The host the KERNEL says the request came from.
    ///
    /// Reported beside the claimed endpoint and never checked against it. #242 settled
    /// that enforcing agreement refuses the documented setup -- DNS names, a node
    /// dialling itself, NAT, VPN, multi-homing -- and stops only a third host. Here the
    /// gate is a person's eyes, so the two are SHOWN and a disagreement is MARKED; at
    /// forty rows nobody notices an unmarked one.
    std::string peerId;

    /// How long ago this id first asked, in seconds.
    ///
    /// A duration rather than an instant, for the reason a heartbeat age is one: a raw
    /// `core::platform::SteadyTimePoint` invites the receiver to difference it against its own clock, which is
    /// a different clock.
    std::uint64_t firstSeenSecondsAgo { 0 };

    /// How many times it has asked. A joiner polls, so this rises on its own and a row
    /// stuck at 1 is a machine that asked once and went away.
    std::uint32_t attempts { 0 };

    /// How many times this id has polled claiming something other than what was
    /// recorded, AFTER a person decided about it.
    ///
    /// Zero for every ordinary enrolment, including one behind NAT: while a row is
    /// pending its claim simply refreshes, and this counts only what arrives once the
    /// row has stopped tracking the machine. Non-zero is a machine that moved after it
    /// was decided about -- ordinary, and fixed by enrolling it again -- or somebody
    /// else answering to a decided id, which is the same reading a `peerId` that
    /// disagrees with `nodeEndpoint` gets: **shown to a person, never acted on by the
    /// node.** #242 settled that gating on the observed host refuses the documented
    /// setup and stops only a third host.
    std::uint32_t claimsChanged { 0 };

    EnrollmentDecision decision { EnrollmentDecision::Pending }; ///< What has been decided about it.

    EnrollRole role { EnrollRole::Learner }; ///< What it asked to be admitted as.

    /// The identity key it asked with, WHOLE (#178).
    ///
    /// **This is what an operator approves**, and the comparison is the whole strength of an
    /// enrollment now that no secret is handed out: the joiner prints its key, the list shows
    /// the key the leader recorded, and an approval admits exactly the key shown. The first key
    /// an id asks with is the one kept; a later poll under the same id with another key is a
    /// DIFFERENT machine, so it is counted in `claimsChanged` and never recorded.
    std::array<std::byte, IdentityPublicKeyBytes> publicKey {};

    /// The fingerprint of the roster this joiner was last handed, or absent until it was
    /// handed one.
    ///
    /// The joiner prints the fingerprint of the roster it RECEIVED; this is the one the leader
    /// SENT. The two agreeing is what says nothing between them rewrote the roster, and it is
    /// per row rather than the cluster's current one because the roster moves with every
    /// admission -- a batch of approvals would otherwise make every joiner's print disagree with
    /// the list for a reason that is no attack at all.
    std::optional<std::array<std::byte, RosterFingerprintBytes>> rosterFingerprint {};

    /// How long ago the auto-approve deadline that approved this row was armed, in seconds, or
    /// absent on a row a person decided -- or nobody has.
    ///
    /// The audit an auto-approval owes: no person compared this row's key, so the list says so
    /// and says WHICH arming admitted it, which is what an operator reading it later needs to
    /// find out who armed what. A duration rather than an instant, for `firstSeenSecondsAgo`'s
    /// reason.
    std::optional<std::uint64_t> autoApprovedArmedSecondsAgo {};

    /// The host this row was CREATED from, as the kernel reported it, with an IPv4-mapped address
    /// folded to its IPv4 form (`UnmappedHost`). Set once, and never refreshed.
    ///
    /// It is the key of the per-host bound: a leader counts a host's undecided rows by it, so
    /// re-polling rows from a second address cannot move them out of the first address's share.
    /// It travels so the list can explain that bound. `EnrollmentHostFull` names the host in this
    /// spelling, and a row whose `peerId` has since moved to another address is MARKED beside
    /// this one, for #242's reason: shown to a person, never acted on.
    std::string firstPeerId {};
};

/// What an ENROLL-CONTROL was answered with.
struct EnrollmentReport
{
    WireEnrollmentState state { WireEnrollmentState::Manual }; ///< How the window decides a joiner.

    /// How many whole seconds the armed auto-approve deadline has left; zero in `Manual`.
    std::uint64_t autoApproveSecondsLeft { 0 };

    std::vector<EnrollmentPendingEntry> pending; ///< Everything waiting, oldest first.
};

/// Frame the payload of an ENROLL-CONTROL reply.
///
/// The entries ride a nested variable-arity field list, for the reason
/// `NodeRuntimeFields` does: a fact added to one of these rows must not move the
/// reply's own arity, and `SplitFields` is exact about arity by design.
/// @param report What the window holds.
/// @return The reply payload, to be carried by `EncodeReply(Status::Ok, ...)`.
[[nodiscard]] inline std::vector<std::byte> EncodeEnrollmentReport(EnrollmentReport const& report)
{
    std::vector<std::vector<std::byte>> rows;
    rows.reserve(report.pending.size());
    for (auto const& entry: report.pending)
    {
        std::array<std::byte, 1> const decision { static_cast<std::byte>(entry.decision) };
        // Positional, so the ORDER here is this row's contract. Append only, and the
        // decoder reads what it knows and skips the rest -- which is what makes the
        // claim above ("a fact added to one of these rows must not move the reply's own
        // arity") true of the ROW as well as of the reply. It was not, until
        // `claimsChanged` was the first fact added and the row's reader was exact.
        std::array<std::byte, 1> const role { static_cast<std::byte>(entry.role) };
        auto const fingerprint = entry.rosterFingerprint.has_value()
                                     ? std::span<std::byte const> { *entry.rosterFingerprint }
                                     : std::span<std::byte const> {};
        // Absent as zero length, the optional-field convention the runtime record holds.
        auto const armedAgo = Detail::OptionalBigEndian(entry.autoApprovedArmedSecondsAgo);
        rows.push_back(WireFields::Encode({ AsBytes(entry.nodeId),
                                            AsBytes(entry.nodeEndpoint),
                                            AsBytes(entry.peerId),
                                            std::span<std::byte const> { EncodeU64Field(entry.firstSeenSecondsAgo) },
                                            std::span<std::byte const> { EncodeU32Field(entry.attempts) },
                                            std::span<std::byte const> { decision },
                                            std::span<std::byte const> { EncodeU32Field(entry.claimsChanged) },
                                            std::span<std::byte const> { role },
                                            std::span<std::byte const> { entry.publicKey },
                                            fingerprint,
                                            armedAgo,
                                            AsBytes(entry.firstPeerId) }));
    }
    std::vector<std::span<std::byte const>> views;
    views.reserve(rows.size());
    for (auto const& row: rows)
        views.emplace_back(row);

    // Named locals, all three: the spans handed to `Encode` view them, and a temporary
    // would be gone before the call -- the same reason `EncodeNodeStatus` names its own.
    std::array<std::byte, 1> const state { static_cast<std::byte>(report.state) };
    auto const secondsLeft = EncodeU64Field(report.autoApproveSecondsLeft);
    auto const entries = WireFields::Encode(WireFields::FieldList { views });

    return WireFields::Encode({ std::span<std::byte const> { state },
                                std::span<std::byte const> { secondsLeft },
                                std::span<std::byte const> { entries } });
}

/// Read an ENROLL-CONTROL reply payload back.
///
/// A `decision` or `role` byte this build has no name for is refused rather than defaulted:
/// the reader is a person deciding whether to admit somebody to the fleet, and a row whose
/// state renders as `pending` when it is really something else is the one wrong answer this
/// report must not give.
/// @param payload The reply payload.
/// @return The report, or nullopt when malformed.
[[nodiscard]] inline std::optional<EnrollmentReport> DecodeEnrollmentReport(std::span<std::byte const> payload)
{
    auto const outer = WireFields::SplitExactly(payload, 3);
    if (!outer.has_value())
        return std::nullopt;
    auto const state = (*outer)[0];
    // REFUSED rather than skipped, unlike the runtime record's copy of this byte: the reader is a
    // person deciding whom to admit, and a mode guessed is the one wrong answer this report must
    // not give. A retired byte is one this build cannot name.
    if (state.size() != 1 || !IsKnownEnrollmentState(static_cast<std::uint8_t>(state[0])))
        return std::nullopt;
    auto const secondsLeft = DecodeU64Field((*outer)[1]);
    if (!secondsLeft.has_value())
        return std::nullopt;

    auto const rows = WireFields::SplitAll((*outer)[2]);
    if (!rows.has_value())
        return std::nullopt;

    EnrollmentReport report { .state = static_cast<WireEnrollmentState>(state[0]),
                              .autoApproveSecondsLeft = *secondsLeft,
                              .pending = {} };
    report.pending.reserve(rows->size());
    for (auto const& row: *rows)
    {
        // At LEAST the twelve a row carries, and any surplus is skipped: a row from a build that
        // records one more fact than this one knows about is read for what it does know, exactly
        // as `DecodeNodeRuntime` reads its own record. A row SHORTER than twelve is refused,
        // because those positions are not optional -- the key above all, which is the thing the
        // operator is reading this list to compare -- and reading a missing one would invent a
        // value rather than omit a fact. The two optional facts, the fingerprint and the
        // auto-approval, are optional in their VALUE and travel as a zero-length field.
        auto const parts = WireFields::SplitAll(row);
        if (!parts.has_value() || parts->size() < 12)
            return std::nullopt;
        auto const firstSeen = DecodeU64Field((*parts)[3]);
        auto const attempts = DecodeU32Field((*parts)[4]);
        auto const decision = (*parts)[5];
        auto const changed = DecodeU32Field((*parts)[6]);
        auto const role = (*parts)[7];
        auto const key = (*parts)[8];
        auto const fingerprint = (*parts)[9];
        if (!firstSeen.has_value() || !attempts.has_value() || decision.size() != 1 || !changed.has_value()
            || role.size() != 1 || key.size() != IdentityPublicKeyBytes
            || (!fingerprint.empty() && fingerprint.size() != RosterFingerprintBytes))
            return std::nullopt;
        if (!IsKnownEnrollmentDecision(static_cast<std::uint8_t>(decision[0]))
            || !IsKnownEnrollRole(static_cast<std::uint8_t>(role[0])))
            return std::nullopt;
        auto entry = EnrollmentPendingEntry { .nodeId = std::string { AsStringView((*parts)[0]) },
                                              .nodeEndpoint = std::string { AsStringView((*parts)[1]) },
                                              .peerId = std::string { AsStringView((*parts)[2]) },
                                              .firstSeenSecondsAgo = *firstSeen,
                                              .attempts = *attempts,
                                              .claimsChanged = *changed,
                                              .decision = static_cast<EnrollmentDecision>(decision[0]),
                                              .role = static_cast<EnrollRole>(role[0]),
                                              .publicKey = {},
                                              .rosterFingerprint = std::nullopt,
                                              .autoApprovedArmedSecondsAgo = std::nullopt,
                                              .firstPeerId = std::string { AsStringView((*parts)[11]) } };
        if (!Detail::ReadOptionalBigEndian((*parts)[10], entry.autoApprovedArmedSecondsAgo))
            return std::nullopt;
        std::ranges::copy(key, entry.publicKey.begin());
        if (!fingerprint.empty())
        {
            entry.rosterFingerprint.emplace();
            std::ranges::copy(fingerprint, entry.rosterFingerprint->begin());
        }
        report.pending.push_back(std::move(entry));
    }
    return report;
}

// ---- Fleet formation ---------------------------------------------------------------------

/// Whether a fleet has admitted anybody but its founder -- or whether its speaker is on its way to
/// another fleet, which makes the summary a POINTER.
///
/// **TRANSMITTED**: in a node's beacon and in its `FleetSummary` reply, so the values are
/// explicit and the list is append-only. A byte this build has no name for is refused by the
/// decoder rather than read as `Solitary`, because the two meet in a yield decision: a node
/// that read an established fleet as solitary would treat it as a peer that may be asked to
/// yield, or would yield to it for the wrong reason.
enum class FleetState : std::uint8_t
{
    Solitary = 0x01,    ///< Only its founder has ever been admitted.
    Established = 0x02, ///< Somebody besides its founder has been admitted, ever.
    /// Its speaker has asked another fleet to take it, and its cluster ends when that fleet does:
    /// the leader slots name THAT fleet's leader, not one of its own, and are read into
    /// `FleetSummary::pointsAt` (`FleetStateTable`). A node meeting it asks the fleet it names
    /// rather than joining one that is about to be left.
    Pending = 0x03,
};

/// Every fleet state this build implements, as ONE list, for `KnownEnrollRoles`' reason: the
/// decoder refuses a byte outside it.
inline constexpr std::array KnownFleetStates { FleetState::Solitary, FleetState::Established, FleetState::Pending };

/// Whose leader a summary's leader slots -- the leader's id, its `0xFC` endpoint and its key -- name.
///
/// **Private**: never transmitted or persisted; the state byte is what travels.
enum class LeaderSlots : std::uint8_t
{
    OwnLeader,  ///< The leader of the speaker's own fleet: `leaderId`, `leaderNodeEndpoint`, `leaderKey`.
    AskedFleet, ///< The leader of the fleet the speaker asked to join: `FleetSummary::pointsAt`.
};

/// One fleet state and what its leader slots name.
struct FleetStateRow
{
    FleetState state;        ///< The state.
    LeaderSlots leaderSlots; ///< Whose leader its slots name.
};

/// What each state's leader slots name, and so which MEMBERS the codec reads them into.
///
/// **This table is what keeps a pointer from being read as a leader.** A pending node's slots name
/// the fleet it ASKED, and a reader that dialled them as the fleet's own leader would join a
/// cluster about to be left. So the codec never puts them where such a reader looks: under
/// `AskedFleet` they arrive in `pointsAt` and the summary's own leader fields are EMPTY, which every
/// reader of those fields already treats as "no leader to ask". Nothing about a reader has to
/// remember to consult the state.
inline constexpr std::array FleetStateTable {
    FleetStateRow { .state = FleetState::Solitary, .leaderSlots = LeaderSlots::OwnLeader },
    FleetStateRow { .state = FleetState::Established, .leaderSlots = LeaderSlots::OwnLeader },
    FleetStateRow { .state = FleetState::Pending, .leaderSlots = LeaderSlots::AskedFleet },
};

static_assert(FleetStateTable.size() == KnownFleetStates.size()
                  && std::ranges::all_of(KnownFleetStates,
                                         [](FleetState state) {
                                             return std::ranges::count(FleetStateTable, state, &FleetStateRow::state) == 1;
                                         }),
              "every fleet state this build knows needs exactly one FleetStateTable row");

/// Whether @p state's leader slots name the fleet its speaker asked rather than its own.
/// @param state A state `KnownFleetStates` holds.
/// @return True under `LeaderSlots::AskedFleet`.
[[nodiscard]] constexpr bool LeaderSlotsNameAskedFleet(FleetState state) noexcept
{
    return std::ranges::any_of(FleetStateTable, [state](FleetStateRow const& row) {
        return row.state == state && row.leaderSlots == LeaderSlots::AskedFleet;
    });
}

/// The leader of the fleet a pending node asked to join, as the summary it decided on named it.
///
/// **A HINT, never proof.** It is what the pending node says; a node following it proves whoever
/// answers at `leaderNodeEndpoint` itself and decides on THAT node's own proven summary, and a key
/// stated here is one the answer must match, never one taken as proven.
struct JoinPointer
{
    std::string leaderId {};           ///< The asked fleet's leader, or empty when it named none.
    std::string leaderNodeEndpoint {}; ///< Where that leader's `0xFC` port answers, or empty.
    std::optional<std::array<std::byte, IdentityPublicKeyBytes>> leaderKey {}; ///< Its key as stated, or none.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(JoinPointer const&, JoinPointer const&) = default;
};

/// What a node says about the fleet it is in: the body of its beacon, and of its `FleetSummary`
/// reply.
///
/// **Owns every field.** A decoder returns it by value, so a view member would be a
/// use-after-free the moment the payload it was decoded from is dropped -- and this struct is
/// handed from a reply reader to a yield decision that outlives the frame.
struct FleetSummary
{
    std::string clusterId {};                  ///< The fleet's id, as its founder minted it.
    FleetState state { FleetState::Solitary }; ///< Whether anybody but the founder was ever admitted.
    std::uint64_t createdAtUnixSeconds { 0 };  ///< When the founder created the fleet.
    std::string leaderId {};                   ///< Empty when this node knows no leader, and under `Pending`.
    std::string leaderNodeEndpoint {};         ///< host:port of the leader's 0xFC port, where a joiner sends Enroll.
    std::string nodeId {};                     ///< Who is speaking.
    std::string raftEndpoint {};               ///< Where the speaker answers Raft; EMPTY for a learner.

    /// The machines the fleet records, by id: as many as the carrier holds, the ones a reader most
    /// needs first.
    ///
    /// **A claim, never evidence on its own.** The signature proves who SPOKE, not that the fleet
    /// records whom it names -- anybody can mint a fleet listing any id, and ids ride every beacon --
    /// so a reader acts on this list only beside a key it already holds.
    std::vector<std::string> members {};

    /// How many machines the fleet records: `members.size()` when the list is whole, more when a
    /// carrier cut it. Never fewer; the decoder refuses that.
    std::uint64_t memberTotal { 0 };

    /// Where the speaker's OWN `0xFC` port answers, so a peer holding its key can ask it again;
    /// EMPTY states none, as `leaderNodeEndpoint` does while no leader is known.
    std::string nodeEndpoint {};

    /// The identity key of the leader `leaderId` names, as the speaker believes it -- inside what the
    /// speaker signs, so a key proven for the speaker VOUCHES for it: a joiner told to ask that
    /// leader holds its answer to this key rather than to whichever key answers there. Disengaged
    /// when the speaker knows none, and under `Pending`.
    std::optional<std::array<std::byte, IdentityPublicKeyBytes>> leaderKey {};

    /// Under `Pending` alone, what the leader slots name: the fleet the speaker asked
    /// (`FleetStateTable`). Empty in every other state, where the slots are this fleet's own.
    JoinPointer pointsAt {};

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(FleetSummary const&, FleetSummary const&) = default;
};

/// How many length-prefixed fields `EncodeFleetSummaryFields` writes, in `FleetSummary`'s order.
inline constexpr std::size_t FleetSummaryFieldCount = 11;

/// Where the leader's identity key sits: empty, or exactly `IdentityPublicKeyBytes`.
inline constexpr std::size_t FleetSummaryLeaderKeyField = 10;

/// Where the member ids sit among the summary's fields: ONE nested field of ids.
inline constexpr std::size_t FleetSummaryMembersField = 7;

/// Where the member total sits: eight big-endian bytes.
inline constexpr std::size_t FleetSummaryMemberTotalField = 8;

/// The summary's endpoints a peer DIALS -- where the leader's `0xFC` port answers, where the
/// speaker answers Raft, and where its own `0xFC` port does -- as ONE list every rule about a
/// dialled endpoint walks: the decoder holds each to `ParseDialEndpoint`, and
/// `Cluster::AnnouncesOnlyThisMachine` refuses to announce one only the dialler reaches.
///
/// One rule per summary rather than one per field, because a reader dials whichever it needs and
/// cannot be told which a sender checked. EMPTY is legal in each, and states none. A pending node's
/// pointer is dialled too, and `DialledEndpointTexts` walks it beside these.
inline constexpr std::array<std::string FleetSummary::*, 3> FleetSummaryDialledEndpoints {
    &FleetSummary::leaderNodeEndpoint,
    &FleetSummary::raftEndpoint,
    &FleetSummary::nodeEndpoint,
};

/// Every endpoint in @p summary a peer may dial: `FleetSummaryDialledEndpoints`' three, then the
/// endpoint a pending node points at. The ONE list both the decoder's dial rule and
/// `Cluster::EndpointsOnlyThisMachine` walk.
/// @param summary The summary.
/// @return The endpoints, borrowed from @p summary; an empty one states none.
[[nodiscard]] inline std::array<std::string_view, FleetSummaryDialledEndpoints.size() + 1> DialledEndpointTexts(
    FleetSummary const& summary)
{
    auto texts = std::array<std::string_view, FleetSummaryDialledEndpoints.size() + 1> {};
    std::ranges::transform(FleetSummaryDialledEndpoints, texts.begin(), [&summary](std::string FleetSummary::* endpoint) {
        return std::string_view { summary.*endpoint };
    });
    texts.back() = summary.pointsAt.leaderNodeEndpoint;
    return texts;
}

/// @p summary as every carrier carries it: at most `MaxFleetSummaryMembers` ids, the first ones
/// kept in the order the summary lists them, and `memberTotal` untouched, so a reader can tell it
/// was cut.
///
/// The ONE place a list is cut, so the ids a reader most needs are the ones every carrier keeps:
/// whoever builds the summary lists them first.
/// @param summary The summary, its whole list.
/// @return The summary as a carrier holds it.
[[nodiscard]] inline FleetSummary CarriedSummary(FleetSummary summary)
{
    if (summary.members.size() > MaxFleetSummaryMembers)
        summary.members.resize(MaxFleetSummaryMembers);
    return summary;
}

/// Encode a fleet summary as its fields, for a beacon or a reply to carry.
///
/// The state is one byte and the creation time eight big-endian bytes; every other field is text,
/// written as it stands. An empty field is a length of zero rather than an absent one, so the arity
/// never depends on what a node knows.
///
/// **Precondition: the cluster id is not empty.** `DecodeFleetSummaryFields` refuses an empty one
/// as malformed, so encoding it would be sending what every peer drops; a node's cluster id is
/// minted into its formation record, never empty. **And the leader slots are written from the
/// members `FleetStateTable` says they name**: the pointer under `Pending`, this fleet's own
/// leader otherwise -- the other set is empty, or the decoder would hand back a different summary.
/// @param summary What the node says; its `clusterId` is not empty.
/// @return `FleetSummaryFieldCount` fields.
[[nodiscard]] inline std::vector<std::byte> EncodeFleetSummaryFields(FleetSummary const& summary)
{
    assert(!summary.clusterId.empty() && "a fleet summary names its cluster; the decoder refuses an empty id");
    assert(summary.memberTotal >= summary.members.size() && "a cut list names fewer than the total, never more");
    auto const pointer = LeaderSlotsNameAskedFleet(summary.state);
    assert((pointer ? summary.leaderId.empty() && summary.leaderNodeEndpoint.empty() && !summary.leaderKey.has_value()
                    : summary.pointsAt == JoinPointer {})
           && "the leader slots carry one set: the pointer under Pending, this fleet's own leader otherwise");
    auto const& leaderId = pointer ? summary.pointsAt.leaderId : summary.leaderId;
    auto const& leaderNodeEndpoint = pointer ? summary.pointsAt.leaderNodeEndpoint : summary.leaderNodeEndpoint;
    auto const& leaderKey = pointer ? summary.pointsAt.leaderKey : summary.leaderKey;
    auto const key = leaderKey.has_value() ? std::span<std::byte const> { *leaderKey } : std::span<std::byte const> {};
    std::array<std::byte, 1> const state { static_cast<std::byte>(summary.state) };
    auto const created = EncodeU64Field(summary.createdAtUnixSeconds);
    auto ids = std::vector<std::span<std::byte const>> {};
    ids.reserve(summary.members.size());
    for (auto const& id: summary.members)
        ids.push_back(AsBytes(id));
    auto const members = WireFields::Encode(WireFields::FieldList { ids });
    auto const total = EncodeU64Field(summary.memberTotal);
    return WireFields::Encode({ AsBytes(summary.clusterId),
                                std::span<std::byte const> { state },
                                std::span<std::byte const> { created },
                                AsBytes(leaderId),
                                AsBytes(leaderNodeEndpoint),
                                AsBytes(summary.nodeId),
                                AsBytes(summary.raftEndpoint),
                                std::span<std::byte const> { members },
                                std::span<std::byte const> { total },
                                AsBytes(summary.nodeEndpoint),
                                key });
}

/// One text field of `EncodeFleetSummaryFields`' and the most bytes a reader accepts in it.
struct FleetSummaryTextField
{
    std::size_t index;    ///< Its position among the summary's fields.
    std::size_t maxBytes; ///< Its bound: longer is refused.
};

/// Which of `EncodeFleetSummaryFields`' fields are text -- the cluster, the leader, its endpoint,
/// the node, its Raft endpoint and its own `0xFC` endpoint -- each with its bound: the three ids
/// `MaxIdBytes`, the three endpoints `MaxFleetSummaryTextBytes`. The member ids are a nested list
/// and are bounded where it is read (`DecodeFleetSummaryMembers`).
inline constexpr std::array<FleetSummaryTextField, 6> FleetSummaryTextFields { {
    { .index = 0, .maxBytes = MaxIdBytes },
    { .index = 3, .maxBytes = MaxIdBytes },
    { .index = 4, .maxBytes = MaxFleetSummaryTextBytes },
    { .index = 5, .maxBytes = MaxIdBytes },
    { .index = 6, .maxBytes = MaxFleetSummaryTextBytes },
    { .index = 9, .maxBytes = MaxFleetSummaryTextBytes },
} };

/// The largest FLEET-SUMMARY reply payload a peer can send that `DecodeFleetSummaryReply` accepts:
/// the nested summary at its largest -- `MaxFleetSummaryMembers` ids at the id bound -- the key and
/// the signature, each behind its length prefix.
/// @return The size in bytes.
[[nodiscard]] consteval std::size_t LargestFleetSummaryReply() noexcept
{
    auto text = std::size_t { 0 };
    for (auto const& field: FleetSummaryTextFields)
        text += field.maxBytes;
    auto const members = MaxFleetSummaryMembers * (WireFields::FieldPrefixSize + MaxIdBytes);
    auto const summary = (FleetSummaryFieldCount * WireFields::FieldPrefixSize) + text + 1       /* state */
                         + sizeof(std::uint64_t) /* created */ + members + sizeof(std::uint64_t) /* memberTotal */
                         + IdentityPublicKeyBytes /* leader key */;
    return (3 * WireFields::FieldPrefixSize) + summary + IdentityPublicKeyBytes + NodeSignatureBytes;
}

static_assert(LargestFleetSummaryReply() <= MaxFleetSummaryReply,
              "the fleet summary reply ceiling must hold the largest summary a peer can make -- every member a reply "
              "carries at the id bound -- with its key and signature");

/// Read the member ids of a summary back, holding them to `MaxFleetSummaryMembers`.
///
/// Refuses more than `MaxFleetSummaryMembers` ids, an empty id, one past `MaxIdBytes`, one that is not UTF-8
/// -- an id is text a peer sent, and a fleet page renders it -- and an id named twice, which would
/// make a count of the list say more than the fleet does. Walked at most one id past the cap, so a
/// hostile list of empty fields costs the cap and no more.
/// @param field The nested field `EncodeFleetSummaryFields` wrote.
/// @return The ids, owned, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::vector<std::string>> DecodeFleetSummaryMembers(std::span<std::byte const> field)
{
    auto const ids = WireFields::Detail::SplitUpTo(field, MaxFleetSummaryMembers + 1);
    if (!ids.has_value() || ids->size() > MaxFleetSummaryMembers || WireFields::EncodedSize(*ids) != field.size())
        return std::nullopt;
    auto members = std::vector<std::string> {};
    members.reserve(ids->size());
    for (auto const id: *ids)
    {
        auto const text = AsStringView(id);
        if (text.empty() || text.size() > MaxIdBytes || !IsValidUtf8(text) || std::ranges::contains(members, text))
            return std::nullopt;
        members.emplace_back(text);
    }
    return members;
}

/// Read a fleet summary back.
///
/// Refuses a field count other than `FleetSummaryFieldCount`, a text field longer than its bound
/// (`FleetSummaryTextFields`: an id past `MaxIdBytes` included), an EMPTY cluster id, a state
/// byte this build has no name for (see `FleetState`), a creation time or member total that is not
/// exactly eight bytes, a member list `DecodeFleetSummaryMembers` refuses, a total smaller than the
/// list, a leader key that is neither empty nor exactly one key wide, and a dialled endpoint
/// (`DialledEndpointTexts`) that is neither empty nor one `ParseDialEndpoint` reads -- refused here
/// as MALFORMED, so nothing downstream ever dials it. The leader slots are read into the members
/// the state's `FleetStateTable` row names.
///
/// A cluster id is minted and never empty, so an empty one is malformed rather than a value: read
/// as one, it would sort below every real id and win every same-second tie-break a yield decision
/// makes, and two of them would read as the same fleet.
/// @param blob What `EncodeFleetSummaryFields` wrote.
/// @return The summary, owning every field, or nullopt when malformed.
[[nodiscard]] inline std::optional<FleetSummary> DecodeFleetSummaryFields(std::span<std::byte const> blob)
{
    auto const fields = WireFields::SplitExactly(blob, FleetSummaryFieldCount);
    if (!fields.has_value() || (*fields)[0].empty())
        return std::nullopt;
    // Every text field inside its bound, so the largest summary a peer can make a reader hold is a
    // constant and `MaxFleetSummaryReply` can be asserted to hold it -- and a cluster id inside the
    // one a discovery challenge needs.
    for (auto const& field: FleetSummaryTextFields)
        if ((*fields)[field.index].size() > field.maxBytes)
            return std::nullopt;
    auto const state = (*fields)[1];
    if (state.size() != 1 || !std::ranges::contains(KnownFleetStates, static_cast<FleetState>(state[0])))
        return std::nullopt;
    auto const created = DecodeU64Field((*fields)[2]);
    auto const total = DecodeU64Field((*fields)[FleetSummaryMemberTotalField]);
    auto members = DecodeFleetSummaryMembers((*fields)[FleetSummaryMembersField]);
    auto const keyField = (*fields)[FleetSummaryLeaderKeyField];
    if (!created.has_value() || !total.has_value() || !members.has_value() || *total < members->size()
        || (!keyField.empty() && keyField.size() != IdentityPublicKeyBytes))
        return std::nullopt;
    auto leaderKey = std::optional<std::array<std::byte, IdentityPublicKeyBytes>> {};
    if (!keyField.empty())
    {
        leaderKey.emplace();
        std::ranges::copy(keyField, leaderKey->begin());
    }
    auto summary = FleetSummary { .clusterId = std::string { AsStringView((*fields)[0]) },
                                  .state = static_cast<FleetState>(state[0]),
                                  .createdAtUnixSeconds = *created,
                                  .leaderId = std::string { AsStringView((*fields)[3]) },
                                  .leaderNodeEndpoint = std::string { AsStringView((*fields)[4]) },
                                  .nodeId = std::string { AsStringView((*fields)[5]) },
                                  .raftEndpoint = std::string { AsStringView((*fields)[6]) },
                                  .members = *std::move(members),
                                  .memberTotal = *total,
                                  .nodeEndpoint = std::string { AsStringView((*fields)[9]) },
                                  .leaderKey = leaderKey,
                                  .pointsAt = {} };
    // A pointer's slots go where only a reader of the pointer looks.
    if (LeaderSlotsNameAskedFleet(summary.state))
        summary.pointsAt = JoinPointer { .leaderId = std::exchange(summary.leaderId, {}),
                                         .leaderNodeEndpoint = std::exchange(summary.leaderNodeEndpoint, {}),
                                         .leaderKey = std::exchange(summary.leaderKey, std::nullopt) };
    // One dial rule for every endpoint a reader might dial; empty states none.
    auto const dialable = [](std::string_view text) {
        return text.empty() || ParseDialEndpoint(text).has_value();
    };
    if (!std::ranges::all_of(DialledEndpointTexts(summary), dialable))
        return std::nullopt;
    return summary;
}

/// Frame a FLEET-SUMMARY request.
///
/// One field: the asker's nonce, drawn fresh per question, which the answering node signs over so
/// a recorded reply cannot be replayed to a later asker.
/// @param nonce The asker's nonce.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeFleetSummaryRequest(std::span<std::byte const, NodeChallengeBytes> nonce,
                                                                      WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::FleetSummary, { std::span<std::byte const> { nonce } });
}

/// Split a FLEET-SUMMARY payload.
///
/// Refuses a nonce that is not exactly `NodeChallengeBytes` wide: a shorter one is a weaker
/// challenge than the asker thinks it sent, and a longer one is a sender this build does not know.
/// @param payload The bytes following the request header.
/// @return The nonce, copied out, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::array<std::byte, NodeChallengeBytes>> DecodeFleetSummaryRequestPayload(
    std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::FleetSummary));
    if (!fields.has_value() || (*fields)[0].size() != NodeChallengeBytes)
        return std::nullopt;
    auto nonce = std::array<std::byte, NodeChallengeBytes> {};
    std::ranges::copy((*fields)[0], nonce.begin());
    return nonce;
}

/// A FLEET-SUMMARY reply: the summary, the answering node's identity key, and its signature.
///
/// The codec carries the key and the signature and checks only their widths. What the signature
/// covers -- the asker's nonce and the summary -- is decided where it is made and where it is
/// checked, never here, so that a reply this decoder accepts is not mistaken for one that verified.
struct FleetSummaryReply
{
    FleetSummary summary {};                                    ///< What the answering node says.
    std::array<std::byte, IdentityPublicKeyBytes> publicKey {}; ///< The answering node's identity key.
    std::array<std::byte, NodeSignatureBytes> signature {};     ///< Its signature over the nonce and the summary.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(FleetSummaryReply const&, FleetSummaryReply const&) = default;
};

/// Frame the payload of a FLEET-SUMMARY reply.
///
/// Three fields: the summary's own fields as ONE nested field, so the beacon and the reply share
/// `EncodeFleetSummaryFields` byte for byte, then the key and the signature.
/// @param reply The reply.
/// @return The reply payload, to be carried by `EncodeReply(Status::Ok, ...)`.
[[nodiscard]] inline std::vector<std::byte> EncodeFleetSummaryReply(FleetSummaryReply const& reply)
{
    auto const summary = EncodeFleetSummaryFields(reply.summary);
    return WireFields::Encode({ std::span<std::byte const> { summary },
                                std::span<std::byte const> { reply.publicKey },
                                std::span<std::byte const> { reply.signature } });
}

/// Read a FLEET-SUMMARY reply payload back.
///
/// Refuses a key or a signature that is not exactly one wide -- a prefix of a signature verifies
/// nothing, and a caller must not be handed one to try -- and a summary `DecodeFleetSummaryFields`
/// refuses, at the one member cap every carrier holds.
/// @param payload The reply payload.
/// @return The reply, owning every field, or nullopt when malformed.
[[nodiscard]] inline std::optional<FleetSummaryReply> DecodeFleetSummaryReply(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, 3);
    if (!fields.has_value() || (*fields)[1].size() != IdentityPublicKeyBytes || (*fields)[2].size() != NodeSignatureBytes)
        return std::nullopt;
    auto summary = DecodeFleetSummaryFields((*fields)[0]);
    if (!summary.has_value())
        return std::nullopt;
    auto reply = FleetSummaryReply { .summary = *std::move(summary) };
    std::ranges::copy((*fields)[1], reply.publicKey.begin());
    std::ranges::copy((*fields)[2], reply.signature.begin());
    return reply;
}

// ---- The node proof (#1428, #178) --------------------------------------------------------

/// What a caller opens the node handshake with.
///
/// Owned, fixed-width fields: a decoder returning this by value borrows nothing, and the arrays are
/// what the `static_assert`s in `Distributed/NodeProof.hpp` pin to the types that fill them.
struct NodeChallengeRequest
{
    std::array<std::byte, NodeChallengeBytes> nonce {};        ///< The caller's nonce, fresh per connection.
    std::array<std::byte, NodeEphemeralKeyBytes> ephemeral {}; ///< The caller's ephemeral X25519 public key.

    [[nodiscard]] friend bool operator==(NodeChallengeRequest const&, NodeChallengeRequest const&) = default;
};

/// Frame a NODE-CHALLENGE request.
/// @param request The caller's nonce and ephemeral key.
/// @param version Version to advertise; overridable so tests can offer a version the peer
///                does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeChallenge(NodeChallengeRequest const& request,
                                                                WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(
        version,
        Op::NodeChallenge,
        { std::span<std::byte const> { request.nonce }, std::span<std::byte const> { request.ephemeral } });
}

/// Split a NODE-CHALLENGE payload.
///
/// Each width is EXACT, for `NodeChallengeBytes`' reason: a wider field silently truncated would
/// have both ends sign different inputs.
/// @param payload The bytes following the request header.
/// @return The request, or nullopt when malformed.
[[nodiscard]] inline std::optional<NodeChallengeRequest> DecodeNodeChallengePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::NodeChallenge));
    if (!fields.has_value() || (*fields)[0].size() != NodeChallengeBytes || (*fields)[1].size() != NodeEphemeralKeyBytes)
        return std::nullopt;
    auto request = NodeChallengeRequest {};
    std::ranges::copy((*fields)[0], request.nonce.begin());
    std::ranges::copy((*fields)[1], request.ephemeral.begin());
    return request;
}

/// What a server answers a NODE-CHALLENGE with: who it is, and its half of the handshake.
struct NodeChallengeReply
{
    std::string serverId;                                       ///< The node id the server claims.
    std::array<std::byte, IdentityPublicKeyBytes> serverKey {}; ///< Its identity key.
    std::array<std::byte, NodeChallengeBytes> nonce {};         ///< Its nonce: the challenge a proof must answer.
    std::array<std::byte, NodeEphemeralKeyBytes> ephemeral {};  ///< Its ephemeral X25519 public key.
    std::array<std::byte, NodeSignatureBytes> signature {};     ///< Over both halves, under `serverKey`.

    [[nodiscard]] friend bool operator==(NodeChallengeReply const&, NodeChallengeReply const&) = default;
};

/// Frame the payload of a NODE-CHALLENGE reply.
/// @param reply The server's half.
/// @return The reply payload, to be carried by `EncodeReply(Status::Ok, ...)`.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeChallengeReply(NodeChallengeReply const& reply)
{
    return WireFields::Encode({ AsBytes(reply.serverId),
                                std::span<std::byte const> { reply.serverKey },
                                std::span<std::byte const> { reply.nonce },
                                std::span<std::byte const> { reply.ephemeral },
                                std::span<std::byte const> { reply.signature } });
}

/// Read a NODE-CHALLENGE reply payload back.
///
/// Every fixed field's width is exact, for `DecodeNodeChallengePayload`'s reason.
/// @param payload The reply payload.
/// @return The reply, or nullopt when malformed.
[[nodiscard]] inline std::optional<NodeChallengeReply> DecodeNodeChallengeReply(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, 5);
    if (!fields.has_value() || (*fields)[1].size() != IdentityPublicKeyBytes || (*fields)[2].size() != NodeChallengeBytes
        || (*fields)[3].size() != NodeEphemeralKeyBytes || (*fields)[4].size() != NodeSignatureBytes)
        return std::nullopt;
    auto reply = NodeChallengeReply { .serverId = std::string { AsStringView((*fields)[0]) },
                                      .serverKey = {},
                                      .nonce = {},
                                      .ephemeral = {},
                                      .signature = {} };
    std::ranges::copy((*fields)[1], reply.serverKey.begin());
    std::ranges::copy((*fields)[2], reply.nonce.begin());
    std::ranges::copy((*fields)[3], reply.ephemeral.begin());
    std::ranges::copy((*fields)[4], reply.signature.begin());
    return reply;
}

/// A node proving which machine it is.
///
/// Carries the caller's PUBLIC key rather than leaving the server to look one up by id, which is
/// discovery's shape (#178 PR 4): the server verifies the signature FIRST, under the key presented,
/// and consults its roster second -- so *forged*, *unknown key* and *revoked key* are three
/// answers, and the roster is never an oracle for a caller who cannot sign.
struct ProveNodeRequest
{
    std::string nodeId;                                         ///< The id the caller claims.
    std::array<std::byte, IdentityPublicKeyBytes> publicKey {}; ///< The key it signs with.
    std::array<std::byte, NodeSignatureBytes> signature {};     ///< Over the whole handshake.

    [[nodiscard]] friend bool operator==(ProveNodeRequest const&, ProveNodeRequest const&) = default;
};

/// Frame a PROVE-NODE request.
///
/// No challenge is a field: both nonces are the ones stated on this connection, and a caller
/// echoing them back would give a server two copies to disagree about.
/// @param request The caller's id, key and signature.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeProveNode(ProveNodeRequest const& request,
                                                            WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version,
                                 Op::ProveNode,
                                 { AsBytes(request.nodeId),
                                   std::span<std::byte const> { request.publicKey },
                                   std::span<std::byte const> { request.signature } });
}

/// Split a PROVE-NODE payload.
///
/// The widths are checked HERE rather than at the verifier, because a key or a signature of the
/// wrong length is a malformed FRAME and not a failed proof: answering `NodeProofRejected` to it
/// would report a forgery for a client that cannot encode.
/// @param payload The bytes following the request header.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<ProveNodeRequest> DecodeProveNodePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::ProveNode));
    if (!fields.has_value() || (*fields)[1].size() != IdentityPublicKeyBytes || (*fields)[2].size() != NodeSignatureBytes)
        return std::nullopt;
    auto request =
        ProveNodeRequest { .nodeId = std::string { AsStringView((*fields)[0]) }, .publicKey = {}, .signature = {} };
    std::ranges::copy((*fields)[1], request.publicKey.begin());
    std::ranges::copy((*fields)[2], request.signature.begin());
    return request;
}

// ---- Live stats (#1399) ------------------------------------------------------------------

/// What a subscription is about. Explicit values because these bytes are transmitted; no
/// trailing `Last`, for `EnrollmentDecision`'s reason.
enum class LiveSubject : std::uint8_t
{
    /// A cache's figures: every counter, the storage block and its tiers. Served by a node with
    /// a cache tier and by `fastcached`.
    Cache = 0x00,
    /// A compile node's figures: the cache's plus its host, consensus and `NodeStatusFields`.
    Node = 0x01,
    /// The fleet as the leader renders it. Leader-pinned: a follower refuses with `NotLeader`.
    Fleet = 0x02,
};

/// One subject's wire facts.
struct LiveSubjectRow
{
    LiveSubject subject;             ///< The subject this row describes.
    std::string_view key;            ///< Its name in a refusal and on a command line.
    std::chrono::milliseconds floor; ///< The shortest cadence the server grants.
};

/// Every subject this build serves, with the cadence floor the server clamps a request to.
///
/// **The floors are a server's, never a client's**: a watcher asking for less gets the floor
/// and is told so in `PushKind::Subscribed`. Fleet's floor is the highest because its render
/// scales with the machines, but it is no longer paid per watcher -- the leader renders once per
/// tick and every subscriber shares the bytes -- which is why it sits below the 2000 ms a polling
/// dashboard needed.
inline constexpr std::array LiveSubjectTable {
    LiveSubjectRow { .subject = LiveSubject::Cache, .key = "cache", .floor = std::chrono::milliseconds { 500 } },
    LiveSubjectRow { .subject = LiveSubject::Node, .key = "node", .floor = std::chrono::milliseconds { 500 } },
    LiveSubjectRow { .subject = LiveSubject::Fleet, .key = "fleet", .floor = std::chrono::milliseconds { 1000 } },
};

/// The row describing @p raw, or nullptr for a subject this build does not know.
/// @param raw The subject byte, as received.
/// @return The row, or nullptr.
[[nodiscard]] constexpr LiveSubjectRow const* FindLiveSubject(std::uint8_t raw) noexcept
{
    // A loop returning the row's address rather than `find_if`, for `FindOp`'s reason: an
    // iterator over a `std::array` is a raw pointer on two standard libraries and a class on
    // MSVC's, so naming it `auto*` compiles on one platform only.
    for (auto const& row: LiveSubjectTable)
        if (static_cast<std::uint8_t>(row.subject) == raw)
            return &row;
    return nullptr;
}

/// The longest cadence the server grants. A subscriber asking for more gets this.
inline constexpr std::chrono::milliseconds MaxLiveCadence { 60'000 };

/// How many granted cadences a client waits in silence before it calls the stream dead.
///
/// A snapshot is sent every granted cadence whether or not anything changed, which is what
/// makes silence measurable at all -- the pulse/idle pair's argument (#245). Three, so one
/// frame delayed by a loaded host is not a disconnect.
inline constexpr int LiveIdleCadences = 3;

/// The client's bound on silence for a stream granted @p cadence.
/// @param cadence What `PushKind::Subscribed` granted.
/// @return How long a client waits for the next frame.
[[nodiscard]] constexpr std::chrono::milliseconds LiveIdleBound(std::chrono::milliseconds cadence) noexcept
{
    return cadence * LiveIdleCadences;
}

static_assert(std::ranges::all_of(LiveSubjectTable,
                                  [](LiveSubjectRow const& row) {
                                      return row.floor.count() > 0 && row.floor <= MaxLiveCadence;
                                  }),
              "every live subject's floor must be a real cadence the server can grant");

/// Live subscriptions one node serves at once; the next is refused `EndpointBusy`.
inline constexpr std::size_t MaxLiveSubscriptions = 64;

/// The shortest a single push may stay parked in a write before the connection is ended.
///
/// A reader that has stopped reading fills its socket buffer and parks the writer. The node
/// renders on its own clock, so the ticks that subscriber misses meanwhile are told to it as one
/// gap and nothing queues behind it, but the connection and its kernel buffers are held until
/// this bound. The bound applied is the larger of this and
/// `LiveIdleCadences` granted cadences, so a slow cadence is not cut shorter than its own idle
/// bound.
inline constexpr std::chrono::milliseconds MinLiveStreamWriteStall { 10'000 };

/// What a `Status::Push` frame carries. Explicit values because these bytes are transmitted.
enum class PushKind : std::uint8_t
{
    /// The stream's first frame: what was granted. Exactly once, first.
    Subscribed = 0x00,
    /// The subject's figures at one tick.
    Snapshot = 0x01,
    /// A discrete fact changed; the subject's next snapshot follows on the same tick.
    Event = 0x02,
    /// Cadences this subscriber missed while a push to it stayed parked. The next snapshot is the
    /// present: nothing is queued for a slow subscriber, so nothing was dropped in any order.
    Gap = 0x03,
};

/// A discrete change a stream reports as it happens. Explicit values: transmitted.
enum class LiveEventKind : std::uint8_t
{
    MemberJoined = 0x00,     ///< A machine joined the cluster's member set.
    MemberLeft = 0x01,       ///< A machine left the cluster's member set.
    WorkerRegistered = 0x02, ///< A worker registered with the scheduler.
    /// A worker left the scheduler's registry. It expired or it withdrew: an event is a difference
    /// between two captures, which see only that it is gone, so the name says no more than that.
    WorkerLeft = 0x03,
    LeadershipChanged = 0x04, ///< Who leads the cluster changed.
    SurveyChanged = 0x05,     ///< The node's toolchain survey state changed.
    EnrollmentChanged = 0x06, ///< The enrollment window's mode changed: an auto-approve deadline armed or ended.
};

/// Every event kind this build implements, as ONE list; see `KnownEnrollmentDecisions`.
inline constexpr std::array KnownLiveEventKinds {
    LiveEventKind::MemberJoined,      LiveEventKind::MemberLeft,        LiveEventKind::WorkerRegistered,
    LiveEventKind::WorkerLeft,        LiveEventKind::LeadershipChanged, LiveEventKind::SurveyChanged,
    LiveEventKind::EnrollmentChanged,
};

/// A SUBSCRIBE request's fields.
struct SubscribeRequest
{
    LiveSubject subject { LiveSubject::Node }; ///< What to stream.
    std::uint32_t cadenceMillis { 0 };         ///< The cadence asked for; the server clamps it.
    /// The dashboard credential, or empty. Checked for `Fleet` when the node configures one; a
    /// credential is a secret, so the request is the only place it travels.
    std::string dashboardToken {};
};

/// Frame a SUBSCRIBE request.
/// @param request What to subscribe to.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeSubscribeRequest(SubscribeRequest const& request,
                                                                   WireVersion version = CurrentVersion)
{
    auto const subject = std::array { static_cast<std::byte>(request.subject) };
    return Detail::EncodeRequest(version,
                                 Op::Subscribe,
                                 { std::span<std::byte const> { subject },
                                   std::span<std::byte const> { EncodeU32Field(request.cadenceMillis) },
                                   AsBytes(request.dashboardToken) });
}

/// Decode a SUBSCRIBE request's payload.
/// @param payload The request body.
/// @return The request, or nullopt when malformed or naming a subject this build does not know.
[[nodiscard]] inline std::optional<SubscribeRequest> DecodeSubscribeRequest(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, 3);
    if (!fields.has_value() || (*fields)[0].size() != 1)
        return std::nullopt;
    auto const subject = std::to_integer<std::uint8_t>((*fields)[0][0]);
    auto const cadence = DecodeU32Field((*fields)[1]);
    if (FindLiveSubject(subject) == nullptr || !cadence.has_value())
        return std::nullopt;
    return SubscribeRequest { .subject = static_cast<LiveSubject>(subject),
                              .cadenceMillis = *cadence,
                              .dashboardToken = std::string { AsStringView((*fields)[2]) } };
}

/// A FLEET-TEXT request's fields: the words a reader typed, which the leader looks up.
///
/// **Keys rather than enumerators**, for the reason `/fleet.txt` takes them as query words: which
/// sections and ranges exist is the SERVING build's table, so a client naming one it knows and the
/// leader does not is answered by name (`UnknownFleetSelector`) rather than refused as a malformed
/// frame nobody can act on.
struct FleetTextRequest
{
    std::string section {}; ///< A `FleetSectionTable` key, or empty for every section.
    std::string range {};   ///< A `FleetRangeTable` key, or empty for the default day.
    /// The dashboard credential, or empty; a secret, so the request is the only place it travels.
    std::string dashboardToken {};
};

/// Frame a FLEET-TEXT request.
/// @param request What to read.
/// @param version Version to advertise.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeFleetTextRequest(FleetTextRequest const& request,
                                                                   WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(
        version, Op::FleetText, { AsBytes(request.section), AsBytes(request.range), AsBytes(request.dashboardToken) });
}

/// Decode a FLEET-TEXT request's payload.
/// @param payload The request body.
/// @return The request, or nullopt when its fields do not exactly fill it.
[[nodiscard]] inline std::optional<FleetTextRequest> DecodeFleetTextRequest(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, OpFieldCount(Op::FleetText));
    if (!fields.has_value())
        return std::nullopt;
    return FleetTextRequest { .section = std::string { AsStringView((*fields)[0]) },
                              .range = std::string { AsStringView((*fields)[1]) },
                              .dashboardToken = std::string { AsStringView((*fields)[2]) } };
}

/// The cadence a server grants for @p subject when asked for @p asked.
/// @param subject The subject.
/// @param asked The cadence requested, in milliseconds; zero asks for the floor.
/// @return The requested cadence clamped to `[floor, MaxLiveCadence]`.
[[nodiscard]] constexpr std::chrono::milliseconds GrantLiveCadence(LiveSubject subject, std::uint32_t asked) noexcept
{
    auto const* const row = FindLiveSubject(static_cast<std::uint8_t>(subject));
    auto const floor = row != nullptr ? row->floor : MaxLiveCadence;
    return std::clamp(std::chrono::milliseconds { asked }, floor, MaxLiveCadence);
}

/// `PushKind::Subscribed`'s fields.
struct LiveSubscribedFields
{
    LiveSubject subject { LiveSubject::Node }; ///< What is being streamed.
    std::uint32_t grantedCadenceMillis { 0 };  ///< The cadence the server will keep.
    /// The stats layout digest a `Cache` or `Node` snapshot is encoded in; zero for `Fleet`,
    /// whose body is text. A client of another layout refuses the stream here, by name, before a
    /// single snapshot arrives.
    std::uint64_t statsLayout { 0 };
    std::string endpoint {}; ///< The address the server answers on, for the dashboard's source line.
};

/// `PushKind::Snapshot`'s fields.
///
/// The body's grammar is the SUBJECT's and not this header's: `Fleet` is `RenderFleetText`'s
/// document, `Cache` is one `EncodeStatsReading` field, and `Node` is two fields --
/// `EncodeStatsReading` then `EncodeNodeStatus`. This header stays dependency-free, so the stats
/// codec lives in `Metrics/StatsReadingCodec.hpp`.
struct LiveSnapshotView
{
    std::uint64_t tick { 0 };           ///< The server tick this reading belongs to.
    std::span<std::byte const> body {}; ///< The subject's encoding; borrows the frame.
};

/// `PushKind::Event`'s fields.
struct LiveEventFields
{
    LiveEventKind kind { LiveEventKind::MemberJoined }; ///< What changed.
    std::string detail {};                              ///< Who or what, for a person to read.
};

/// `PushKind::Gap`'s fields.
struct LiveGapFields
{
    std::uint64_t dropped { 0 };   ///< How many of this subscriber's cadences passed without a snapshot.
    std::uint64_t firstTick { 0 }; ///< The first tick missed.
    std::uint64_t lastTick { 0 };  ///< The last tick missed.
};

namespace Detail
{
    /// A push payload: the kind byte, then the kind's fields.
    ///
    /// Framed exactly as `EncodeReply` frames a header: the length bounded first, then a vector
    /// of its final size written through a span. Both simpler spellings fail gcc 14's `-O3`
    /// build once inlined into a large enough caller. `reserve` then `push_back` leaves a
    /// reallocation it reads as freeing an offset pointer (`free-nonheap-object`), and an
    /// unbounded `1 + size` may wrap to an empty vector whose `front()` it reports as a null
    /// dereference. Bounded below `MaxFramePayload`, neither is reachable, and an oversized push
    /// is refused here rather than one layer later by `EncodeReply`.
    /// @throws std::length_error When the payload would exceed the u32 frame length.
    [[nodiscard]] inline std::vector<std::byte> EncodePush(PushKind kind, WireFields::FieldList fields)
    {
        auto const body = WireFields::Encode(fields);
        if (body.size() >= MaxFramePayload)
            throw std::length_error("compile-cache push payload exceeds the u32 wire length");

        std::vector<std::byte> payload(1 + body.size());
        std::span<std::byte> const out { payload };
        out[0] = static_cast<std::byte>(kind);
        std::ranges::copy(body, out.subspan(1).begin());
        return payload;
    }
} // namespace Detail

/// Encode a `Subscribed` push's payload.
/// @param fields What was granted.
/// @return The payload.
[[nodiscard]] inline std::vector<std::byte> EncodeLiveSubscribed(LiveSubscribedFields const& fields)
{
    auto const subject = std::array { static_cast<std::byte>(fields.subject) };
    auto const cadence = EncodeU32Field(fields.grantedCadenceMillis);
    auto const layout = EncodeU64Field(fields.statsLayout);
    return Detail::EncodePush(PushKind::Subscribed,
                              WireFields::AsFields({ std::span<std::byte const> { subject },
                                                     std::span<std::byte const> { cadence },
                                                     std::span<std::byte const> { layout },
                                                     AsBytes(fields.endpoint) }));
}

/// Encode a `Snapshot` push's payload.
/// @param tick The server tick.
/// @param body The subject's encoding.
/// @return The payload.
[[nodiscard]] inline std::vector<std::byte> EncodeLiveSnapshot(std::uint64_t tick, std::span<std::byte const> body)
{
    auto const at = EncodeU64Field(tick);
    return Detail::EncodePush(PushKind::Snapshot, WireFields::AsFields({ std::span<std::byte const> { at }, body }));
}

/// Encode an `Event` push's payload.
/// @param fields What changed.
/// @return The payload.
[[nodiscard]] inline std::vector<std::byte> EncodeLiveEvent(LiveEventFields const& fields)
{
    auto const kind = std::array { static_cast<std::byte>(fields.kind) };
    return Detail::EncodePush(PushKind::Event,
                              WireFields::AsFields({ std::span<std::byte const> { kind }, AsBytes(fields.detail) }));
}

/// Encode a `Gap` push's payload.
/// @param fields What was missed.
/// @return The payload.
[[nodiscard]] inline std::vector<std::byte> EncodeLiveGap(LiveGapFields const& fields)
{
    auto const dropped = EncodeU64Field(fields.dropped);
    auto const first = EncodeU64Field(fields.firstTick);
    auto const last = EncodeU64Field(fields.lastTick);
    return Detail::EncodePush(PushKind::Gap,
                              WireFields::AsFields({ std::span<std::byte const> { dropped },
                                                     std::span<std::byte const> { first },
                                                     std::span<std::byte const> { last } }));
}

/// The kind of a push payload, and the bytes after it.
struct PushView
{
    PushKind kind { PushKind::Subscribed }; ///< What the frame carries.
    std::span<std::byte const> fields {};   ///< The kind's fields; borrows the frame.
};

/// Split a push payload into its kind and fields.
/// @param payload A `Status::Push` frame's payload.
/// @return The view, or nullopt when empty or naming a kind this build does not know.
[[nodiscard]] inline std::optional<PushView> DecodePush(std::span<std::byte const> payload)
{
    if (payload.empty() || std::to_integer<std::uint8_t>(payload[0]) > static_cast<std::uint8_t>(PushKind::Gap))
        return std::nullopt;
    return PushView { .kind = static_cast<PushKind>(std::to_integer<std::uint8_t>(payload[0])),
                      .fields = payload.subspan(1) };
}

/// Decode a `Subscribed` push's fields.
/// @param fields `PushView::fields`.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<LiveSubscribedFields> DecodeLiveSubscribed(std::span<std::byte const> fields)
{
    auto const parts = WireFields::SplitExactly(fields, 4);
    if (!parts.has_value() || (*parts)[0].size() != 1)
        return std::nullopt;
    auto const subject = std::to_integer<std::uint8_t>((*parts)[0][0]);
    auto const cadence = DecodeU32Field((*parts)[1]);
    auto const layout = DecodeU64Field((*parts)[2]);
    if (FindLiveSubject(subject) == nullptr || !cadence.has_value() || !layout.has_value())
        return std::nullopt;
    return LiveSubscribedFields { .subject = static_cast<LiveSubject>(subject),
                                  .grantedCadenceMillis = *cadence,
                                  .statsLayout = *layout,
                                  .endpoint = std::string { AsStringView((*parts)[3]) } };
}

/// Decode a `Snapshot` push's fields.
/// @param fields `PushView::fields`.
/// @return A view borrowing @p fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<LiveSnapshotView> DecodeLiveSnapshot(std::span<std::byte const> fields)
{
    auto const parts = WireFields::SplitExactly(fields, 2);
    if (!parts.has_value())
        return std::nullopt;
    auto const tick = DecodeU64Field((*parts)[0]);
    if (!tick.has_value())
        return std::nullopt;
    return LiveSnapshotView { .tick = *tick, .body = (*parts)[1] };
}

/// Decode an `Event` push's fields.
/// @param fields `PushView::fields`.
/// @return The fields, or nullopt when malformed or naming a kind this build does not know.
[[nodiscard]] inline std::optional<LiveEventFields> DecodeLiveEvent(std::span<std::byte const> fields)
{
    auto const parts = WireFields::SplitExactly(fields, 2);
    if (!parts.has_value() || (*parts)[0].size() != 1)
        return std::nullopt;
    auto const raw = std::to_integer<std::uint8_t>((*parts)[0][0]);
    if (!std::ranges::any_of(KnownLiveEventKinds,
                             [raw](LiveEventKind kind) { return static_cast<std::uint8_t>(kind) == raw; }))
        return std::nullopt;
    return LiveEventFields { .kind = static_cast<LiveEventKind>(raw), .detail = std::string { AsStringView((*parts)[1]) } };
}

/// Decode a `Gap` push's fields.
/// @param fields `PushView::fields`.
/// @return The fields, or nullopt when malformed.
[[nodiscard]] inline std::optional<LiveGapFields> DecodeLiveGap(std::span<std::byte const> fields)
{
    auto const parts = WireFields::SplitExactly(fields, 3);
    if (!parts.has_value())
        return std::nullopt;
    auto const dropped = DecodeU64Field((*parts)[0]);
    auto const first = DecodeU64Field((*parts)[1]);
    auto const last = DecodeU64Field((*parts)[2]);
    if (!dropped.has_value() || !first.has_value() || !last.has_value())
        return std::nullopt;
    return LiveGapFields { .dropped = *dropped, .firstTick = *first, .lastTick = *last };
}

} // namespace FastCache::CompileCacheWire
