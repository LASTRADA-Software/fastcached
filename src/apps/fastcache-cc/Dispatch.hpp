// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"
#include "CodecEnvelope.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Runs one framed request/reply against an endpoint named at runtime.
///
/// The library's `ISocket` is one *connected* peer, which was enough
/// while there was only ever one address to reach. Distribution talks to two —
/// the scheduler, and then whichever worker the scheduler names — so reaching an
/// endpoint becomes the thing that has to be injected. Tests answer with scripted
/// bytes per endpoint and never open a socket.
///
/// **The seam is the whole exchange rather than the dial, and that is what makes a
/// deadline expressible.** A dialer can only hand back a socket, so the only
/// ceiling it can arm is `SO_RCVTIMEO`, which bounds a single `recv`; a worker
/// dribbling one byte before each expiry holds the build forever without ever
/// exceeding it. Bounding the *exchange* is `DeadlineTimer`'s job and needs the
/// reactor the exchange runs on, which is `ReactorExchange` — so the caller asks
/// for an exchange and hands over the budget it must finish inside.
class IEndpointExchange
{
  public:
    IEndpointExchange() = default;
    virtual ~IEndpointExchange() = default;
    IEndpointExchange(IEndpointExchange const&) = delete;
    IEndpointExchange& operator=(IEndpointExchange const&) = delete;
    IEndpointExchange(IEndpointExchange&&) = delete;
    IEndpointExchange& operator=(IEndpointExchange&&) = delete;

    /// Send `frame` to `hostPort` and read its reply.
    /// @param hostPort The endpoint, e.g. "10.0.0.7:6676".
    /// @param frame A complete framed request.
    /// @param credential Presented with the request; default-constructed sends none.
    /// @param budget The deadlines this exchange must finish inside.
    /// @return The outcome. `Transport` covers every way it did not complete — an
    ///         endpoint that could not be reached, a peer that broke mid-reply, and
    ///         a budget that ran out — because the caller answers all three by
    ///         compiling locally.
    [[nodiscard]] virtual CacheOutcome Exchange(std::string_view hostPort,
                                                std::vector<std::byte> frame,
                                                Credential const& credential,
                                                ExchangeBudget budget) = 0;
};

/// How long a remote compile may take, once the connection is open.
///
/// Ten minutes, and NOT the cache's ten seconds. A cache exchange is answered from
/// memory, so ten seconds is generous there and being impatient is what makes a
/// miss cheap. A compile exchange is bounded by how long a compiler runs, and the
/// worker writes nothing until it has finished — so the client sits in one read for
/// the whole compile. Sharing the cache's number meant every translation unit
/// taking more than ten seconds was abandoned mid-compile and rebuilt locally,
/// which is precisely the set of translation units distribution exists for: the
/// work was done twice, the object crossed the network to nobody, and the fleet's
/// counters went up (#223).
///
/// **Ten minutes because that is `LeaseTable::DefaultLeaseTimeout`, not because it
/// is a comfortable round number.** A client waiting past its lease is waiting on
/// one the scheduler has already reclaimed and may have re-granted for the same
/// key, so nothing above that value can be honoured and everything below it throws
/// away a compile the fleet is still holding capacity for.
///
/// So it is DERIVED, not written beside it (#249). This comment used to say the two
/// could not be `static_assert`ed together because the launcher does not link the
/// library the scheduler lives in — true of `LeaseTable`'s header, and not true of
/// the value, which now lives in `CompileCacheWire.hpp` where both ends already
/// look. Moving either no longer means remembering to move both.
///
/// Deliberately generous rather than tight, because a *flat* deadline has to cover
/// the slowest translation unit anybody legitimately compiles: real ones here run
/// well past a minute, and a ceiling sized for the average reintroduces exactly the
/// defect above, further out.
///
/// **The cost was stated rather than absorbed, and most of it has since been paid.**
/// A flat ceiling cannot tell a slow compile from a dead peer, so this number used to
/// be how long a genuinely dead worker went unnoticed as well — sixty times slower
/// than the ten seconds a dispatch used to get, which on a parallel build is a
/// handful of stalled slots. That half is keepalive's now: the compile leg dials with
/// it armed (`DispatchBudgetsFor`), so a machine powered off, unplugged, suspended or
/// cut off mid-compile is noticed in ~16 s on Linux and macOS and ~30 s on Windows,
/// with this total untouched.
///
/// What is left is the narrow case keepalive cannot reach: a host whose kernel
/// answers the probes while the worker process makes no progress toward a reply.
/// Nothing below the protocol can see that, so separating it from a slow compile
/// needs the worker to say it is still there — an idle bound of seconds against a
/// total that stays long. That is a wire change tracked as #245.
///
/// It is `FASTCACHE_DISPATCH_TIMEOUT_MS` at run time, and `fastcache-cc` is one
/// process per translation unit, so the variable IS the runtime knob: the next
/// compile reads it. Nothing has to be reloaded, and nothing has to be restarted.
constexpr std::chrono::milliseconds DefaultDispatchTotal = CompileCacheWire::DefaultCompileLeaseTimeout;

/// How long a dispatched compile may go SILENT before the client gives up on it.
///
/// The other half of `DefaultDispatchTotal`, and what finally splits the two questions
/// a flat deadline could not answer at once
/// ([#245](https://github.com/LASTRADA-Software/fastcached/issues/245)): the total goes
/// on being sized for the slowest legitimate translation unit, while *how fast is a
/// worker that has stopped making progress noticed* drops from ten minutes to thirty
/// seconds.
///
/// DERIVED, for the reason `DefaultDispatchTotal` is: the worker's pulse cadence and
/// this bound describe one silence from opposite sides, and only the shared wire header
/// can hold a value both binaries agree on. `CompileCacheWire` also carries the
/// `static_assert` that keeps this comfortably above the cadence, so retuning either
/// one into a fleet that refuses healthy workers is a build failure rather than a
/// support ticket.
///
/// It is `FASTCACHE_DISPATCH_IDLE_MS` at run time, and zero turns it off -- which
/// restores exactly the pre-#245 behaviour, one flat deadline, for an operator who
/// would rather have that than a fleet whose workers they do not trust to pulse.
constexpr std::chrono::milliseconds DefaultDispatchIdle = CompileCacheWire::DefaultCompileIdleTimeout;

/// The deadlines a dispatch's exchanges run under.
///
/// Two, because a dispatch is two shapes of conversation and one number cannot
/// serve both — the defect above, stated as a type so a caller cannot pass the
/// cache's budget where the compile's belongs.
struct DispatchBudgets
{
    /// The scheduler's LEASE and RELEASE: short request/reply, answered from the
    /// scheduler's own tables. The launcher's ordinary exchange budget.
    ExchangeBudget control {};

    /// The worker's COMPILE: as long as a compiler runs, and the ONE exchange that
    /// probes for a dead peer.
    ///
    /// The two go together and neither is meaningful alone. A deadline this long is
    /// what makes a vanished worker expensive -- minutes of a held build slot -- and
    /// keepalive is what answers that in seconds without shortening the deadline back
    /// into #223. The control exchanges above leave it off: a lease or a release that
    /// stalls is already bounded by a round trip.
    ExchangeBudget compile { .total = DefaultDispatchTotal, .idle = DefaultDispatchIdle, .keepAlive = KeepAlive::Yes };

    /// Ceiling on the object a worker may declare its reply expands to.
    ///
    /// A byte budget beside the two time budgets, because it bounds the same thing
    /// they do — what one exchange may cost this process — and a peer's declared
    /// decompressed length is the one figure in a reply that decides an allocation
    /// before any of it is validated.
    std::size_t maxDecompressedBytes { DefaultMaxDecompressedBytes };

    /// Field-by-field equality, for the reason `ExchangeBudget`'s own says.
    [[nodiscard]] friend constexpr bool operator==(DispatchBudgets const&, DispatchBudgets const&) = default;
};

/// The launcher's three timeout knobs, named.
///
/// A struct rather than three `std::chrono::milliseconds` parameters, for the reason
/// `ExchangeBudget` gives about two of them: same type, adjacent, and a reader at the
/// call site cannot tell a transposition from the intended order. Three is worse than
/// two, and the only PRODUCTION call site is in `main.cpp`, which is in **no test
/// target** -- so a swap of `controlTotal` and `compileTotal` there would compile, hand
/// the compile leg the cache's ten seconds, restore #223 in full, and nothing in this
/// tree could observe it. Designated initializers put the names back at the call site.
struct DispatchBudgetKnobs
{
    /// Ceiling on opening either connection, name resolution included.
    std::chrono::milliseconds connect { ExchangeBudget {}.connect };

    /// Ceiling on a LEASE or RELEASE round trip.
    std::chrono::milliseconds controlTotal { ExchangeBudget {}.total };

    /// Ceiling on the whole COMPILE exchange.
    std::chrono::milliseconds compileTotal { DefaultDispatchTotal };

    /// Ceiling on SILENCE during the COMPILE exchange.
    ///
    /// A fourth knob rather than a derivation of `compileTotal`, because it answers a
    /// different question and a ratio between them would be a number nobody could
    /// defend: the total is sized by the slowest translation unit in the build, and
    /// the idle bound by how long a worker's reactor may plausibly be late with a
    /// five-byte write. Those two move for entirely unrelated reasons.
    std::chrono::milliseconds compileIdle { DefaultDispatchIdle };
};

/// The budgets a dispatch runs under, built from the launcher's three knobs.
///
/// **One producer, because the compile leg differs from the control leg in TWO
/// fields and not one.** It was derived in `main.cpp` by copying the control budget
/// and replacing its total, on the stated reasoning that only the total differs —
/// which was true when it was written and stopped being true when the compile leg
/// gained `keepAlive` (#247). The copy silently overwrote `KeepAlive::Yes` with the
/// control leg's `No`, so no shipped launcher has ever armed keepalive on the one
/// exchange it exists for, and a worker whose host vanished still cost the full
/// `FASTCACHE_DISPATCH_TIMEOUT_MS`. Nothing could see it: the default member
/// initializer above was the only statement of the intent, `main.cpp` is in no test
/// target, and the docs described the unarmed behaviour, so code and prose agreed
/// by accident.
///
/// So the derivation lives here, where a test reads it, and it *names* each field on
/// which the two legs differ instead of inheriting the rest by copy. A fourth field
/// that must differ is a line in this function -- and, because naming them leaves the
/// intent written twice, the `static_assert` below is what stops the two copies from
/// drifting the way #247's did.
/// @param knobs The launcher's three timeouts, named.
/// @return Both budgets.
[[nodiscard]] constexpr DispatchBudgets DispatchBudgetsFor(DispatchBudgetKnobs const& knobs) noexcept
{
    return DispatchBudgets {
        .control = ExchangeBudget { .connect = knobs.connect, .total = knobs.controlTotal, .keepAlive = KeepAlive::No },
        .compile = ExchangeBudget { .connect = knobs.connect,
                                    .total = knobs.compileTotal,
                                    .idle = knobs.compileIdle,
                                    .keepAlive = KeepAlive::Yes },
    };
}

/// The compile leg, bounded by the grant the scheduler actually issued.
///
/// **The client is the third reader of the fleet's lease lifetime, and it was the one
/// left behind.** The scheduler reclaims at the granted bound and the worker stops
/// serving at it; a client still counting down a compile-time constant gives up
/// mid-compile on a job both other machines consider live, which is #522's premise in
/// the one place an operator watches. So a dispatched compile's total comes from the
/// grant in hand, never from this process's configuration.
///
/// That is why `FASTCACHE_DISPATCH_TIMEOUT_MS` no longer decides a dispatched compile:
/// **a client-side knob overriding the fleet's agreed bound is a worker-side override
/// wearing a client's clothes**, and the ticket forbids one of those exactly one
/// machine along. It stays as the fallback below and as the bound on the paths that
/// hold no grant.
///
/// Only `total` is replaced, and naming it is the point rather than an accident: #247
/// was a copy of a DIFFERENT leg's budget that silently carried the wrong `keepAlive`,
/// and the lesson written above is to name what differs. Here the leg is the same leg
/// -- same connect, same idle, same keepalive -- and one field of it is now known
/// better by the fleet than by this process.
///
/// @param compile The compile leg as configured.
/// @param granted What the scheduler said this lease lives for; zero means it named
///        none, which no version-6 peer does -- so the configured value stands rather
///        than a zero budget failing the compile instantly.
/// @return The budget this compile actually runs under.
[[nodiscard]] constexpr ExchangeBudget UnderGrantedLease(ExchangeBudget compile, std::chrono::milliseconds granted) noexcept
{
    if (granted > std::chrono::milliseconds::zero())
        compile.total = granted;
    return compile;
}

/// The compile leg's intent is now written twice -- here, which production uses, and
/// `DispatchBudgets`'s own default member initializers, which every test that takes
/// the default argument uses. Two independent statements of one rule is what #247 was,
/// so they are held against each other rather than left to agree by inspection: a
/// field added to the type's initializers and forgotten here fails the build instead
/// of shipping a launcher that dials with it unset.
static_assert(DispatchBudgetsFor(DispatchBudgetKnobs {}) == DispatchBudgets {},
              "DispatchBudgetsFor must reproduce DispatchBudgets' own defaults, field for field");

/// How a dispatch attempt ended.
///
/// There is deliberately no "failed" outcome. Every way this can go wrong ends
/// with the caller compiling locally, because the client is holding the source and
/// has a working fallback — distribution must be incapable of breaking a build.
enum class DispatchStatus : std::uint8_t
{
    /// A worker ran the compiler. `exitCode` says what it thought of the code, and
    /// may be non-zero: that is a *successful dispatch* of a failing compile.
    Compiled,
    /// The scheduler declined, or this command line was not dispatchable. Ordinary,
    /// and the reason is in `detail`.
    Declined,
    /// The scheduler or the worker could not be reached, or the exchange broke.
    Unavailable,
    /// The worker answered about a compile other than the one it was asked for
    /// ([#280](https://github.com/LASTRADA-Software/fastcached/issues/280)).
    ///
    /// **Its own status rather than an `Unavailable`, because it is the only one
    /// here that is not ordinary.** Every other way a dispatch ends is a fleet
    /// declining to help — a scheduler with nobody free, a worker that went away —
    /// and an operator seeing those has nothing to fix. This one says a machine
    /// returned an object for work nobody asked it to do, which is a defect
    /// somewhere in the fleet and the one outcome worth waking somebody for. Folded
    /// into `Unavailable` it would be tallied beside "the worker was down" and read
    /// as a network blip.
    ///
    /// The build still succeeds: like every other non-answer the client compiles
    /// locally, because it is holding the source. What it must never do is *use*
    /// the object, which would be a wrong object under a correct key — the failure
    /// this whole mechanism exists to make impossible.
    Mismatched,
    /// The enumerator count, so a table over this enum takes its extent from the
    /// enum itself rather than from a literal. See `Core/EnumTable.hpp`: a length
    /// anchored on an enumerator by name is a guard that fires only when nothing is
    /// wrong. Never a status a `DispatchResult` carries.
    Last,
};

/// Why a dispatch was declined, at the grain an operator ACTS on.
///
/// `DispatchStatus::Declined` is one state and was one sentence -- "the fleet
/// declined this compile" -- for every way a fleet can say no. A wall of those tells
/// nobody whether to add a machine, wait, or fix a credential, and those are not
/// adjacent remedies
/// ([#618](https://github.com/LASTRADA-Software/fastcached/issues/618)).
///
/// **The rows are operator ACTIONS, not wire codes.** Several codes share a row
/// where the answer is the same, and one code can mean the same thing from either
/// end -- `FingerprintMismatch` from a scheduler is "nothing serves your compiler"
/// and from a worker is "the lease named a toolchain I do not have", which an
/// operator fixes in one place. What must never share a row is what this
/// repository's own metrics rules already refuse to sum: `NoWorker`, `NoCapacity`
/// and `Withdrawn` are a misconfigured fleet, a fleet that is too small, and a fleet
/// that is unavailable.
///
/// It is a SECOND field beside `DispatchStatus`, never a widening of it. The four
/// states #427 established -- dispatched, declined, unreachable, and a crossed reply
/// -- are what a reader buckets by; this says which kind of *declined*, and asking
/// it of any other status is meaningless rather than wrong.
enum class DeclineCause : std::uint8_t
{
    /// Nothing in the fleet serves this compiler.
    ///
    /// A machine is missing, or a fingerprint has drifted -- a toolchain upgrade on
    /// the clients and not the workers looks exactly like this.
    NoToolchain,
    /// Every matching machine is full of this fleet's own work: the fleet is too
    /// small. Never summed with the two rows either side of it.
    NoCapacity,
    /// Matching machines have slots free on paper and are not offering them --
    /// somebody is using them, or a scratch disk is full. The fleet is big enough
    /// and unavailable, which is a different purchase from being too small.
    Withdrawn,
    /// Another client already holds the lease for this key.
    ///
    /// **Not a fault, and its own row for exactly that reason.** This is duplicate
    /// suppression working: sixty clients missing one key after a header change is
    /// the ordinary shape of a shared cache, and fifty-nine of them compiling
    /// locally is the design. Tallied beside "no machine serves your compiler" it
    /// reads as a fleet in trouble.
    AlreadyBuilding,
    /// A credential, membership or lease refusal: this client was not allowed to ask.
    ///
    /// Fixed where the client is configured, or on the scheduler's member list --
    /// never by adding machines, which is what every other row above points at.
    NotPermitted,
    /// One machine declined the job it was handed.
    ///
    /// The fleet found a worker and the worker said no: a lease it would not honour,
    /// a toolchain it no longer has, a scratch root it cannot write. One machine to
    /// go and look at, rather than a fleet-shaped problem.
    WorkerRefused,
    /// The fleet could not name a leader to ask.
    ///
    /// Its own row rather than a share of `NotPermitted`, because it is transient by
    /// nature -- an election in progress -- and permanent only when a fleet is
    /// misconfigured. A rate separates those two; a bucket shared with a credential
    /// refusal separates nothing.
    NoLeader,
    /// This launcher and the fleet disagree about the wire.
    ///
    /// A staggered upgrade produces this and stops producing it when the upgrade
    /// finishes, so a rise that persists names a machine that never came back.
    ProtocolMismatch,
    /// A refusal this build has no row for.
    ///
    /// Reachable only from a peer NEWER than this launcher, because
    /// `DeclineCausesAreTotal` requires a row for every code this build's own wire
    /// header knows. It is not a default arm for codes somebody forgot: it is the
    /// honest answer to a sentence this build cannot read, and the same answer
    /// `ParseDispatchOutcome` gives a log column from a later build.
    Unrecognised,
    /// The enumerator count, so a table over this enum takes its extent from the
    /// enum. Never a cause a `DispatchResult` carries.
    Last,
};

/// One wire refusal, and the operator action it belongs to.
struct DeclineCauseRow
{
    CompileCacheWire::ErrorCode code; ///< What the peer answered.
    DeclineCause cause;               ///< What an operator would do about it.
};

/// Every refusal this launcher can meet, classified.
///
/// A scan rather than an `EnumTable`, because `ErrorCode`'s values are neither
/// contiguous nor in declaration order -- `NoCluster = 0x15` is declared after
/// `InvalidClusterChange = 0x16` -- so an extent taken from the enum would index
/// the wrong rows. `CompileCacheWire::FindOp` and `Wire::Describe` scan their own
/// tables for the same reason, so this is the tree's existing idiom.
///
/// The completeness guard below is what makes this a classification rather than a
/// list somebody remembers to extend.
inline constexpr std::array DeclineCauseTable {
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::UnsupportedVersion, .cause = DeclineCause::ProtocolMismatch },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::UnknownOpcode, .cause = DeclineCause::ProtocolMismatch },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::MalformedFrame, .cause = DeclineCause::ProtocolMismatch },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::PayloadTooLarge, .cause = DeclineCause::ProtocolMismatch },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::MalformedValue, .cause = DeclineCause::ProtocolMismatch },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::StorageWriteFailed, .cause = DeclineCause::WorkerRefused },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::Unauthenticated, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::NoWorker, .cause = DeclineCause::NoToolchain },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::NoCapacity, .cause = DeclineCause::NoCapacity },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::AlreadyInFlight, .cause = DeclineCause::AlreadyBuilding },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::DispatchNotPermitted, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::UnknownLease, .cause = DeclineCause::NotPermitted },
    // From either end this is "the fleet does not have your compiler": a scheduler
    // saying no worker carries the fingerprint, or a worker saying the lease named a
    // toolchain it does not serve. One thing to fix, so one row.
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::FingerprintMismatch, .cause = DeclineCause::NoToolchain },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::UnsupportedCodec, .cause = DeclineCause::ProtocolMismatch },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::WorkerScratchUnavailable, .cause = DeclineCause::Withdrawn },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::WorkerSpawnFailed, .cause = DeclineCause::WorkerRefused },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::NotLeader, .cause = DeclineCause::NoLeader },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::NotAMember, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::Withdrawn, .cause = DeclineCause::Withdrawn },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::NoCluster, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::InvalidClusterChange, .cause = DeclineCause::NotPermitted },
    // Cluster administration, which no compile reaches: a launcher can only ever
    // meet these by talking to something that is not the surface it thinks it is,
    // and that is a protocol disagreement rather than a fleet condition. Rows so
    // that neither arrives as `Unrecognised`, which reads exactly like a code this
    // build has never heard of.
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::ClusterChangeInFlight, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::ClusterChangeNotNeeded, .cause = DeclineCause::NotPermitted },
    // Slots were free and MEMORY was not, which the worker's own rule calls a
    // momentary fullness the peer retries past -- the same answer as a withdrawn
    // slot, and not a machine to go and inspect.
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::EndpointBusy, .cause = DeclineCause::Withdrawn },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::MalformedRegistration, .cause = DeclineCause::ProtocolMismatch },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::LeaseUnauthorized, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::LeaseEndpointMismatch, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::LeaseExpired, .cause = DeclineCause::NotPermitted },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::WorkerToolchainSurveyInFlight, .cause = DeclineCause::Withdrawn },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::RequestDeadlineExceeded, .cause = DeclineCause::Withdrawn },
    DeclineCauseRow { .code = CompileCacheWire::ErrorCode::ForeignValueGeneration, .cause = DeclineCause::ProtocolMismatch },
};

/// Whether every refusal this build's wire header knows carries a classification.
///
/// Derived from `CompileCacheWire::ErrorTable` rather than restated, so a code added
/// there cannot silently arrive as `Unrecognised` -- which would read exactly like a
/// peer from the future while being this build forgetting a row. That is the
/// silence-reads-as-coverage failure one level up, and an opt-in list is how it
/// happens.
/// @return True when every wire error code has exactly one row here.
[[nodiscard]] consteval bool DeclineCausesAreTotal() noexcept
{
    for (auto const& known: CompileCacheWire::ErrorTable)
    {
        std::size_t rows = 0;
        for (auto const& row: DeclineCauseTable)
            if (row.code == known.code)
                ++rows;
        if (rows != 1)
            return false;
    }
    return true;
}

static_assert(DeclineCausesAreTotal(),
              "every CompileCacheWire::ErrorCode needs exactly one DeclineCauseTable row, or a refusal this "
              "build knows would be reported as one it has never heard of");

/// What an operator would do about the refusal a peer answered with.
/// @param code What the peer answered.
/// @return The operator action, or `Unrecognised` for a code this build has no row
///         for -- which `DeclineCausesAreTotal` limits to a NEWER peer.
[[nodiscard]] constexpr DeclineCause DeclineCauseFor(CompileCacheWire::ErrorCode code) noexcept
{
    for (auto const& row: DeclineCauseTable)
        if (row.code == code)
            return row.cause;
    return DeclineCause::Unrecognised;
}

/// The result of one dispatch attempt.
struct DispatchResult
{
    DispatchStatus status { DispatchStatus::Unavailable };
    /// Which kind of decline, when `status` is `Declined`. Meaningless otherwise.
    ///
    /// The seed is `Unrecognised` because it is the row that claims least, and it is
    /// never observed on a real decline: `DeclinedBy` is the only way to build one
    /// and it always classifies. There is deliberately no "not classified" row --
    /// nothing could reach it, and a refusal nothing reaches is a series reading
    /// zero because the event is impossible rather than because it did not happen,
    /// which is why `EpochMismatch` was retired rather than left standing.
    DeclineCause decline { DeclineCause::Unrecognised };
    int exitCode { 0 };            ///< The remote compiler's exit code (Compiled only).
    std::vector<std::byte> object; ///< The compiled object, already decoded.
    std::string stdoutText;        ///< The remote compiler's stdout.
    std::string stderrText;        ///< The remote compiler's stderr.
    std::string detail;            ///< Why it was declined or unavailable; empty on success.
    std::string workerEndpoint;    ///< Which worker ran it, for diagnostics.

    /// @return True when a worker actually ran the compiler.
    [[nodiscard]] bool Ran() const noexcept
    {
        return status == DispatchStatus::Compiled;
    }
};

/// Everything one dispatch needs.
struct DispatchRequest
{
    std::string_view schedulerEndpoint; ///< Where to ask for a worker.
    std::string_view fingerprint;       ///< This client's toolchain identity.
    std::string_view objectKey;         ///< The cache key, for duplicate suppression.
    std::span<std::string const> args;  ///< Already filtered by `RemoteCompileArgs`.
    std::string_view preprocessed;      ///< The translation unit, preprocessed.
    /// The translation unit's path, as the build system spelled it, and it travels
    /// WHOLE (#660).
    ///
    /// A compiler with debug info on records the name of the file it was handed, and
    /// clang takes that from the input path rather than from the `#line` marker -- so
    /// truncating this to a base name left a dispatched object recording the worker's
    /// per-job scratch directory, which differs between two dispatches of one
    /// translation unit under one cache key. The worker maps its scratch path back to
    /// this spelling; it never opens it and never joins it to a path.
    std::string_view sourceName;
    /// The directory this client's own compile runs in, and what its own
    /// `-fdebug-prefix-map` rules spell that as -- from `MappedCompileDirectory`. Both
    /// empty when the build maps nothing, which is what tells the worker to map nothing
    /// either.
    std::string_view compileDir;
    std::string_view compileDirReplacement;

    /// The source path as the build system spelled it, RAW, and what this client's own
    /// `-fdebug-prefix-map` rules make of it -- which is `sourceName` above.
    ///
    /// **Two operands, because `sourceName` is one** (#883). gcc takes `DW_AT_name`
    /// from the `#line` marker in the preprocessed text, which names the CLIENT's path,
    /// and no rule the worker builds from its own scratch directory can match that. So
    /// the worker needs to be told what the compiler will emit AND what it should say
    /// instead; `sourceName` is already the mapped half and cannot also be the raw one.
    ///
    /// Empty when nothing maps the source, which is the ordinary case -- a relative
    /// source argument matches no rule. Both empty or both set: a half-filled pair is
    /// malformed and the worker refuses it, exactly as the compilation-directory pair.
    std::string_view sourceRoot;
    std::string_view sourceRootReplacement;
};

/// Ask the scheduler for a worker and have it compile this translation unit.
///
/// Three exchanges, each a request/reply on a fresh connection: a `Lease` to the
/// scheduler, a `Compile` to whichever worker it named, and a `Release` back to the
/// scheduler saying the job is over. Two of the three are short and one is as long
/// as a compile, which is why `DispatchBudgets` carries two deadlines rather than
/// one. The client never waits in a queue — a scheduler with nothing free refuses
/// immediately, and the caller compiles locally. That is not a fallback bolted on
/// afterwards; it is why the scheduler is allowed to refuse at all.
///
/// **The release is not optional and not the caller's to remember.** A lease
/// suppresses every other client's attempt at the same key, so one that is never
/// resolved pins that key for the scheduler's whole lease timeout — ten minutes by
/// default (#212). It therefore happens here, on every path out of the compile,
/// rather than being handed back for the caller to do: expiry exists for a client
/// that *died*, not for one that forgot. It costs a second connection to the
/// scheduler per dispatched translation unit; the one the grant arrived on cannot
/// carry it, because that port sweeps a connection idle for five seconds and a
/// compile is longer than that.
///
/// The residual, deliberately: the lease is resolved when the compile ends, and the
/// caller stores the object afterwards, so for the length of that store the key is
/// neither in flight nor in the cache and a client arriving inside that window can
/// be granted a lease for work already done. It costs one duplicate compile of one
/// object, and the alternative — holding the lease across work this function does
/// not control, on paths where the caller may legitimately never store at all — is
/// how a lease comes to be resolved by expiry again.
///
/// **The object comes back to the client, and the client stores it.** A worker is
/// never given cache credentials. Today a `STORE` is trusted because whoever stores
/// compiled the thing themselves, so the worst they can do is poison their own key
/// space with something they would have gotten anyway. If workers stored, one rogue
/// worker would poison keys every other machine fetches. Routing the result back
/// through the client keeps that trust model exactly as it is, and needs no new
/// authorization anywhere.
///
/// @param exchange How to reach the scheduler and the worker.
/// @param request The job.
/// @param budgets The deadlines each leg runs under.
/// @param credential Presented to both peers; default-constructed sends none.
/// @param acceptedCodecs What this client can decode, most-preferred first.
/// @return What happened. Never throws; every failure is a status.
[[nodiscard]] DispatchResult Dispatch(IEndpointExchange& exchange,
                                      DispatchRequest const& request,
                                      DispatchBudgets const& budgets = {},
                                      Credential const& credential = {},
                                      CompileCacheWire::CodecList const& acceptedCodecs = {});

/// Split a COMPILE request's argument field back into arguments.
///
/// The inverse of what `Dispatch` encodes, and the worker's half of it. One
/// length-prefixed field per argument rather than a joined string, because an
/// argument may contain a space and a receiver splitting on whitespace would turn
/// `-DMSG=hello world` into two flags.
///
/// A truncated field yields **nothing**, never a prefix: a partial argument list is
/// a different compile from the one that was authorized, and running it would
/// produce an object nobody asked for.
/// @param field The encoded argument field.
/// @return The arguments, or an empty list when the field is malformed.
[[nodiscard]] std::vector<std::string> DecodeArgs(std::span<std::byte const> field);

// The exchange that talks over real TCP connections is `MakeTcpExchange()` in
// `ReactorExchange.hpp`, deliberately not here. It needs a reactor, and this header
// is compiled into the compile NODE as well -- which wants `DecodeArgs` and the
// request types and has no use for a client's dialling machinery. Declaring the
// factory beside the reactor keeps that link honest instead of costing the node a
// translation unit it never calls.

} // namespace FastCache::Cc
