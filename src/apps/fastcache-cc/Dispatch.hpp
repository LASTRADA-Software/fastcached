// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"
#include "CodecEnvelope.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

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

/// The result of one dispatch attempt.
struct DispatchResult
{
    DispatchStatus status { DispatchStatus::Unavailable };
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
    /// The translation unit's path, as the build system spelled it. Only its base
    /// name travels -- see `Dispatch`, which takes it -- because that is what a
    /// compiler records in the object and the worker has no use for the rest.
    std::string_view sourceName;
    /// The directory this client's own compile runs in, and what its own
    /// `-fdebug-prefix-map` rules spell that as -- from `MappedCompileDirectory`. Both
    /// empty when the build maps nothing, which is what tells the worker to map nothing
    /// either.
    std::string_view compileDir;
    std::string_view compileDirReplacement;
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
