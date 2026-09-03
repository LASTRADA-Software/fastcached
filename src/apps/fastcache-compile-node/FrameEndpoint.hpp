// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeSurfaces.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Protocol/CompileCacheAuth.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

class NodeIoLoop;

/// A refusal the endpoint decides and the surface owning the verb answers.
///
/// Its own enum rather than more `CompileCacheWire::PrePayloadDecision` enumerators,
/// because that one is a WIRE enum: it describes a frame's own header, both binaries
/// compile it in, and the daemon reaches the same decisions from the same
/// `DecidePrePayload`. These describe **this process** -- the memory it is already
/// holding, and what this connection has proved -- so a peer cannot compute them and
/// there is nothing for the daemon to agree about. Widening a shared header to carry
/// a local decision would be a wire change bought for nothing.
///
/// Every one of these was answered in the accept loop with `Wire::EncodeErrorReply`
/// and counted nothing, which is
/// [#447](https://github.com/LASTRADA-Software/fastcached/issues/447): the merged
/// listener refused correctly on the wire while the only thing an operator can see
/// stayed flat.
///
/// **The endpoint's refusal for a connection it has no room for is deliberately not
/// here.** That one is decided at accept, before a header exists, so it names no verb
/// and no owner can be asked for it; `FrameServer` counts it itself. See
/// `IFrameResponder::EndpointRefusalReply`.
enum class EndpointRefusal : std::uint8_t
{
    /// A slot was free and the memory was not: this request's declared length does
    /// not fit in what the surface is already holding in flight.
    ///
    /// Answered `EndpointBusy`, which it shares on the wire with the endpoint's
    /// own at-capacity refusal and must never share a counter with: this says one
    /// request is too big *right now* and that one says the surface is full. A
    /// client does the same thing about both, an operator does not.
    InFlightBudget,

    /// An `AUTH` payload that would not decode into a credential at all.
    CredentialMalformed,

    /// An `AUTH` payload that decoded and did not verify.
    ///
    /// **The credential-guessing signal**, and it is the one refusal here that is not
    /// merely bookkeeping: a peer presenting a wrong token is exactly what an
    /// operator asking "is my scheduler port being probed" is looking for, and until
    /// #447 it answered `Unauthenticated` on the wire and moved nothing at all.
    CredentialRejected,

    /// The surface was still answering when the verb's own window ran out.
    ///
    /// **The one refusal here that is decided by a clock rather than by a frame**,
    /// and the only one the connection writes on its own initiative rather than in
    /// answer to something the peer just sent. `FrameServer::CloseOverdue` decides
    /// it; `ServeConnection` writes it, from the one place that owns the write side.
    ///
    /// Reachable only for a connection parked INSIDE the responder. A peer swept
    /// while the connection is parked on the socket -- dribbling a header, dribbling
    /// a declared payload -- is ended by the close, because the close is the only
    /// thing that ends a parked read and it is the write side gone. See
    /// `FrameServer::CloseOverdue` for why that asymmetry is the design rather than
    /// a shortfall ([#523](https://github.com/LASTRADA-Software/fastcached/issues/523)).
    ///
    /// **Counted by the ENDPOINT, not by the surface**, which is why every responder
    /// answers it through `Cc::RefuseWithoutCounter`. The deadline is the endpoint's
    /// fact: `FrameAnswerDeadlineSweeps` already tallies the sweep and
    /// `FrameDeadlineRefusalsSent` tallies how many of those peers were told why.
    /// Three per-surface copies of one endpoint fact would be three things to drift.
    AnswerDeadline,

    /// The count. See `Core/EnumTable.hpp`.
    Last,
};

/// One row per `EndpointRefusal`, in enumerator order.
struct EndpointRefusalCode
{
    EndpointRefusal refusal;          ///< Which endpoint decision this describes.
    CompileCacheWire::ErrorCode code; ///< What every surface tells the client about it.
};

/// What the client is told, per refusal.
///
/// **A property of the refusal, not of the surface**, which is why it is one table
/// here rather than an arm in each responder: three surfaces answering the same
/// question separately is three answers that agree today, and a divergence between
/// two of them would be one listener telling a client different things about one
/// decision, which nothing could see. The COUNTER is the half that legitimately
/// differs -- the scheduler moves one for a wrong credential, the cache cannot
/// produce that refusal at all -- so it stays with the surface.
///
/// The direct counterpart of `CompileCacheWire::ErrorCodeFor(PrePayloadDecision)`,
/// which the same call sites ask two lines away.
inline constexpr EnumTable<EndpointRefusal, EndpointRefusalCode> EndpointRefusalCodes { {
    { .refusal = EndpointRefusal::InFlightBudget, .code = CompileCacheWire::ErrorCode::EndpointBusy },
    { .refusal = EndpointRefusal::CredentialMalformed, .code = CompileCacheWire::ErrorCode::MalformedFrame },
    { .refusal = EndpointRefusal::CredentialRejected, .code = CompileCacheWire::ErrorCode::Unauthenticated },
    { .refusal = EndpointRefusal::AnswerDeadline, .code = CompileCacheWire::ErrorCode::RequestDeadlineExceeded },
} };

static_assert(RowsInEnumeratorOrder(EndpointRefusalCodes, &EndpointRefusalCode::refusal),
              "EndpointRefusalCodes must hold one row per EndpointRefusal, in enumerator order");

/// Why no surface counts `EndpointRefusal::AnswerDeadline`, stated once for all three.
///
/// Beside the enumerator rather than in any one responder, exactly as
/// `EndpointRefusalCodes` is: this is a property of the refusal, not of the surface
/// that happens to answer it. Three surfaces stating one fact separately is three
/// sentences that agree today, and the divergence would be invisible.
///
/// The deadline belongs to the ENDPOINT. It decides the sweep and it already carries
/// both halves: `FrameAnswerDeadlineSweeps` for the sweep, and
/// `FrameDeadlineRefusalsSent` for the peer that was told why. A surface counting it
/// again would be a third tally of one event, summed by whoever met two of them first.
inline constexpr std::string_view AnswerDeadlineIsTheEndpointsRationale =
    "the answer deadline is the endpoint's decision and the endpoint counts it, in the sweep row and the "
    "refusal-sent row; a per-surface copy would be a third tally of one event";

/// Whether a refusal row states exactly one of the two claims a refusal can make.
///
/// `Cc::Refuse` says a rise here means something an operator acts on;
/// `Cc::RefuseWithoutCounter` says a rise would mean nothing, and why. A row must
/// assert one of those and not the other: a row asserting NEITHER is what a guard
/// short-circuiting on the absent counter passes vacuously, shipping a new refusal
/// uncounted and unexplained, and a row asserting BOTH is an author who could not
/// choose, answered here rather than at whichever call site read the fields in the
/// luckier order.
///
/// **One predicate for all three surfaces**, beside the enumerator like
/// `EndpointRefusalCodes` and `AnswerDeadlineIsTheEndpointsRationale`, because it is a
/// property of what a refusal row IS rather than of any surface. It was briefly three:
/// `Detail::StatesOneClaim` for the cache, plus the same truth table open-coded twice
/// as `answer.has_value() != !rationale.empty()` -- one rule, three sites, two of them
/// spelled in the inverted form and neither reachable by a grep for the name.
/// @param counted Whether the row carries a counted answer.
/// @param rationale The row's reason for counting nothing; empty when it counts.
/// @return True when exactly one of the two is present.
[[nodiscard]] constexpr bool StatesOneRefusalClaim(bool counted, std::string_view rationale) noexcept
{
    return counted == rationale.empty();
}

/// Answer an endpoint-decided refusal the way its row decided, counted or not.
///
/// The one door both the scheduler and the compile surface reach their
/// `EndpointRefusalReply` through, so the counted and uncounted spellings are chosen
/// from data rather than remembered at two call sites. `CacheResponder` has had this
/// shape since #491 (`Detail::AnswerCacheRefusal`); the other two hand-wrote the same
/// three-line branch, which is the one branch
/// [`metrics-and-observability.md`](../../../.agent/rules/metrics-and-observability.md)
/// singles out -- *"deliberately uncounted must not be spelled like forgot"* -- and so
/// the one that least deserves three independent implementations.
///
/// The code is taken rather than re-derived, because both callers already hold it from
/// `ErrorCodeFor(refusal)` and passing it keeps this function from needing to know
/// which enumeration it is answering about.
/// @param metrics Where a counted refusal is recorded.
/// @param code What the client is told.
/// @param answer The counted row, or nothing where this surface counts none.
/// @param rationale Why nothing is counted; read only when @p answer is absent.
/// @param detail Words for a person, or empty when there are none to add.
/// @return The encoded reply. Never empty.
[[nodiscard]] inline std::vector<std::byte> AnswerEndpointRefusal(IMetricsSink& metrics,
                                                                  CompileCacheWire::ErrorCode code,
                                                                  std::optional<Cc::SurfaceRefusal> const& answer,
                                                                  std::string_view rationale,
                                                                  std::string_view detail)
{
    if (!answer.has_value())
        return Cc::RefuseWithoutCounter({ .code = code, .rationale = rationale }, detail);
    return Cc::Refuse(metrics, *answer, detail);
}

/// The wire code @p refusal is answered with.
/// @param refusal Any enumerator but `Last`.
/// @return The code every surface tells the client.
[[nodiscard]] constexpr CompileCacheWire::ErrorCode ErrorCodeFor(EndpointRefusal refusal) noexcept
{
    return EndpointRefusalCodes[static_cast<std::size_t>(refusal)].code;
}

/// Answers one framed request.
///
/// The seam that lets one accept loop serve every framed surface this node exposes.
/// A node runs a scheduler, a cache tier and a compile worker; each speaks the same
/// `0xFC` framing and differs only in what it answers and how much it will buffer.
/// A second accept loop per surface would be three near-copies of a listener, a
/// shutdown order and a poll timeout -- the copy-paste this codebase treats as a
/// defect rather than a coincidence.
///
/// An interface rather than a `std::function`, deliberately: a responder outlives
/// the endpoint and a closure would keep its whole enclosing scope alive with it.
class IFrameResponder
{
  public:
    virtual ~IFrameResponder() = default;

    IFrameResponder() = default;
    IFrameResponder(IFrameResponder const&) = default;
    IFrameResponder& operator=(IFrameResponder const&) = default;
    IFrameResponder(IFrameResponder&&) = default;
    IFrameResponder& operator=(IFrameResponder&&) = default;

    /// Answer one complete request frame.
    ///
    /// A `Task` because answering may now have to reach the network -- the cache
    /// surface consults an upstream, and that dial suspends rather than blocking
    /// the loop every other connection on this reactor is sharing. A responder
    /// that needs nothing is still free to `co_return` without suspending, which
    /// the scheduler's does; that costs a frame allocation and no round trip.
    ///
    /// @param frame The whole request, header included. A span rather than an
    ///        owning vector because a cache STORE carries an object file and
    ///        copying it here would double the peak footprint on the hot path of
    ///        a parallel build. It must outlive the returned task -- the same
    ///        contract `SendAll` states, and true by construction at the one call
    ///        site, where the backing vector is a local of the calling coroutine.
    /// @param peer The peer's host, for the surfaces whose policy needs one.
    ///        Owned rather than a view: it is short, and every policy-bearing
    ///        responder holds it across a suspension, so a view would make its
    ///        lifetime a rule at each implementation instead of a fact.
    /// @return The encoded reply, or empty to close without answering -- which is
    ///         only ever right when the peer is not speaking this protocol at all.
    [[nodiscard]] virtual Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) = 0;

    /// May this peer send at all, before a byte of its payload is taken?
    ///
    /// **The point is WHEN this is asked, not that it is asked.** Both surfaces
    /// already refused the peers this refuses -- but from inside `Answer`, which runs
    /// after the whole declared payload has been read and charged against the byte
    /// budget. So a caller the surface was always going to refuse could still make
    /// this process allocate for it, which is the cheapest denial there is: the
    /// refusal cost exactly what serving would have
    /// ([#285](https://github.com/LASTRADA-Software/fastcached/issues/285),
    /// [#377](https://github.com/LASTRADA-Software/fastcached/issues/377)).
    ///
    /// **Only decisions that depend on the PEER alone belong here.** That is what
    /// makes the question answerable before the frame exists. A surface's membership
    /// or locality rule qualifies; anything reading the verb, the payload or a
    /// credential does not, and stays in `Answer` where the request is.
    ///
    /// **The responder owns the predicate; the endpoint only asks it earlier.** The
    /// refusal is returned already encoded, so the wording, the wire code and the
    /// counter stay with the surface that decided -- rather than the endpoint growing
    /// its own copy of a rule that would then have two places to drift.
    ///
    /// **The gate inside `Answer` stays, and is still the authority.** This is an
    /// early-out, not a replacement: `Answer` is reachable directly, and a predicate
    /// enforced only at the door is one a later caller can walk around.
    ///
    /// Pure virtual rather than defaulted to "admit everyone", deliberately. A
    /// default would let a surface added later inherit an open door by saying
    /// nothing, which is the shape this codebase records as reopening a hole by
    /// omission. A surface with no peer policy returns `std::nullopt` and says so.
    ///
    /// **Takes the verb as well as the peer, since #290.** Each surface today serves
    /// one verb family, so the surface *is* the policy and every implementation
    /// ignores `opRaw` -- nothing observable changes. It is here because a merged
    /// 0xFC listener cannot express its own acceptance criterion without it: *a cache
    /// FETCH from another machine is refused while a compile from that same peer
    /// succeeds*. Same peer, two verbs, two answers, and a peer-only predicate has
    /// nowhere to put the difference.
    ///
    /// The verb costs nothing to supply. `ServeConnection` decodes the request header
    /// well before it asks this, so the opcode is already in hand at the one call
    /// site -- the earlier note that a peer-only shape is what "lets it be asked
    /// before a frame exists" described an option nobody takes, and it was the
    /// sentence that would have argued against this.
    ///
    /// Raw rather than an `Op`, for the reason `DecidePrePayload` is total over every
    /// byte value: an unknown opcode still has to be refusable, and a policy that
    /// could only be asked about verbs this build knows would admit the ones it does
    /// not.
    ///
    /// @param peer The peer's host, as `Answer` receives it.
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @return The encoded refusal to send back, or nullopt to go on and read the
    ///         payload. A refusal is answered as a **reply and a resynchronization**
    ///         by the caller -- never a close, because the frame declared its length
    ///         and a peer that cannot tell a policy refusal from a dead host retries
    ///         forever.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer,
                                                                           std::uint8_t opRaw) const = 0;

    /// Does this surface require a credential before this verb?
    ///
    /// Asked once per frame rather than cached, because a surface may be
    /// reconfigured and a connection already open must not keep an answer from
    /// before. It is a field read behind a virtual call, not a probe.
    ///
    /// **Takes the verb, since #290.** Every implementation today ignores it, because
    /// each surface serves one verb family -- but a merged 0xFC listener has no
    /// surface-wide answer available. The two production responders answer this
    /// oppositely and both are right: the scheduler requires a credential when one is
    /// configured, and the cache requires none because *a credential readable by every
    /// local build is not a credential*. A merged surface answering `true` refuses
    /// every local `fastcache-cc` FETCH; answering `false` undoes #289. The cache's
    /// reason is a property of its VERBS rather than of the port they arrive on, so it
    /// survives the merge and the answer follows the verb.
    ///
    /// Still not folded into `RefusePeer`, which now also takes the verb: that one
    /// answers before the payload is read and returns an encoded refusal, this one
    /// feeds `DecidePrePayload` alongside the declared length and the connection's
    /// credential state. Two questions at one point in the loop, not one wider one
    /// ([#289](https://github.com/LASTRADA-Software/fastcached/issues/289)).
    ///
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @return True when unauthenticated peers must be refused this verb.
    [[nodiscard]] virtual bool AuthRequired(std::uint8_t opRaw) const noexcept = 0;

    /// Check an `AUTH` payload against this surface's credential.
    ///
    /// The endpoint terminates `AUTH` rather than passing it to `Answer`, because
    /// what the verb changes is **connection state**, and the responder is shared by
    /// every connection on this surface -- exactly as the daemon's handler keeps
    /// `credentialAccepted` in its own loop rather than in the policy.
    ///
    /// @param payload The `AUTH` request payload, already bounded by `MaxAuthPayload`
    ///        through the pre-payload gate.
    /// @return What was established. Only `Accepted` may mark the connection
    ///         authenticated -- `NoPolicy` is answered `Ok` and verifies nothing.
    [[nodiscard]] virtual CredentialOutcome CheckCredential(std::span<std::byte const> payload) const = 0;

    /// Encode -- and count -- a pre-payload refusal `DecidePrePayload` decided.
    ///
    /// The same division of labour as `RefusePeer`: one predicate decides, and the
    /// surface owns the wording, the wire code and the counter, so a refusal cannot
    /// be worded one way here and another way in `Answer`. The endpoint deliberately
    /// does not encode this itself -- it would then own a counter belonging to a
    /// policy it does not implement.
    ///
    /// Pure virtual rather than defaulted for the reason `RefusePeer` is: a surface
    /// that says nothing would silently stop counting its own refusals, and a
    /// security counter reading zero because nobody wired it is indistinguishable
    /// from one reading zero because nothing was refused.
    ///
    /// **Takes the verb, since #290**, for the counter rather than for the wording.
    /// A merged 0xFC listener routes each verb to the surface that owns it, and a
    /// refusal has to be counted against that surface: a cache STORE that overran its
    /// ceiling attributed to the scheduler is a counter that names the wrong
    /// subsystem, which is the one thing an operator uses these to do. The wording
    /// stays verb-blind on purpose -- a peer that failed to authenticate learns
    /// nothing from being told which verb it failed to reach.
    ///
    /// **Takes the wording, since #447**, because the endpoint now reaches this from
    /// the surface-wide frame ceiling as well as from `DecidePrePayload`'s per-verb
    /// one, and that refusal names both numbers: "too large" without the ceiling
    /// tells an operator nothing about the limit they are up against. The two
    /// ceilings are one fact at two heights and share one counter -- an operator does
    /// the same thing about both -- so they are one `PayloadTooLarge` rather than a
    /// second enumerator. Empty where the caller has nothing to add, which is every
    /// pre-payload decision: a peer that failed to authenticate learns nothing from
    /// being told which verb it failed to reach.
    ///
    /// @param decision Any value other than `Serve`.
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @param detail Words for a person, or empty when there are none to add.
    /// @return The encoded refusal to send back. Never empty.
    [[nodiscard]] virtual std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const = 0;

    /// Encode -- and count -- a refusal the ENDPOINT decided about a known verb.
    ///
    /// The companion to `RefusalReply`, and separate from it because the two answer
    /// about different things. `PrePayloadDecision` is a wire enum: it describes the
    /// frame's own header, both binaries compile it in, and the daemon reaches the
    /// same decisions from the same function. These are about **this process** -- the
    /// memory it is already holding, and what this connection has proved -- so they
    /// are decided here and cannot be expressed there.
    ///
    /// What they share is the division of labour, which is the whole point: the
    /// endpoint owns WHEN the question is asked, the surface owns the answer,
    /// including which counter moves. Every one of these used to be encoded in the
    /// accept loop with `Wire::EncodeErrorReply` and counted nothing, so a merged
    /// listener answered them correctly and left the series flat
    /// ([#447](https://github.com/LASTRADA-Software/fastcached/issues/447)).
    ///
    /// **A verb is always known here**, which is what separates these from the
    /// endpoint's one verbless refusal. A connection turned away because the surface
    /// already holds every connection it will is decided before a byte is read, so
    /// there is no owner to route it to and no responder is asked -- `FrameServer`
    /// counts that one itself, against its own row. Splitting them that way is also
    /// what keeps the two `EndpointBusy` refusals apart: `InFlightBudget` says one
    /// request is too big *right now* and `OpenConnections` says the surface is full,
    /// an operator acts on them oppositely, and they share a wire code. Two
    /// categories, so they cannot reach one counter by anybody forgetting.
    ///
    /// @param refusal Which refusal the endpoint decided.
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @param detail Words for a person, or empty when there are none to add.
    /// @return The encoded refusal to send back. Never empty.
    [[nodiscard]] virtual std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                                      std::uint8_t opRaw,
                                                                      std::string_view detail) const = 0;

    /// How long serving @p opRaw may take before the socket is abandoned.
    ///
    /// **Per responder and per VERB, because one number cannot bound both things this
    /// endpoint now carries.** A cache exchange is bounded by a round trip; a
    /// dispatched compile is bounded by how long a COMPILER runs. That is #223,
    /// recorded in `distributed-compilation.md` for the client side -- and it arrived
    /// on the server side the moment a compile could reach a reactor surface (#290).
    ///
    /// The endpoint's own `HeaderTimeout` covers everything before a verb is named,
    /// which is where the slow-loris property lives: a peer that connects and sends
    /// half a header is swept on that short window whatever it might have been about
    /// to ask for. This one is armed only once the header has decoded, so a surface
    /// generous to compiles gives a stranger nothing.
    ///
    /// **A missing hop here is silent in the worst way.** `ServeConnection` is not
    /// parked on the socket while a responder answers, so a sweep closes it without
    /// waking anything: the compile runs to completion, hops home, and its `WriteAll`
    /// fails. Every translation unit worth distributing would be compiled, paid for
    /// and thrown away, while short ones succeeded -- so a smoke test passes.
    ///
    /// Pure virtual for the reason the ceilings below are: a surface that inherits
    /// this answer inherits five seconds, and five seconds is exactly the value that
    /// makes the failure above.
    ///
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @return The window covering the payload read and the answer.
    [[nodiscard]] virtual std::chrono::milliseconds RequestTimeout(std::uint8_t opRaw) const noexcept = 0;

    /// Largest request this surface will buffer.
    ///
    /// Per-responder rather than one constant, and the spread is the point: a
    /// scheduler verb carries a fingerprint and a key, while a cache STORE carries a
    /// whole object file. Sizing both for the larger would hand an unauthenticated
    /// peer a way to make the scheduler allocate megabytes.
    [[nodiscard]] virtual std::size_t MaxRequestBytes() const noexcept = 0;

    /// How many connections this surface will hold open at once.
    ///
    /// This counts CONNECTIONS, not requests, and the distinction became load-bearing
    /// when the endpoint learned to serve a connection until the peer stops (#176).
    /// While each connection was one request, the two were the same number and the
    /// cap read as a request cap; once a connection is long-lived, a slot held for
    /// its lifetime is held across the peer's idle time as well. Naming it for
    /// requests and sizing it for requests -- eight, on the cache surface -- would
    /// have let eight attached peers lock out every other client while barely
    /// sending anything.
    ///
    /// So what it bounds is descriptors and coroutine frames, and it is sized for
    /// those. Memory is NOT what this bounds; `MaxInFlightBytes()` below is, which is
    /// why this can afford to be generous.
    [[nodiscard]] virtual std::size_t MaxOpenConnections() const noexcept = 0;

    /// How many declared payload bytes may be in flight across all connections.
    ///
    /// The connection cap alone does not bound memory -- N connections each
    /// declaring `MaxRequestBytes()` is still N times that. This is what actually
    /// bounds it, and it is checkable at exactly the right moment: the header
    /// declares its length before a single payload byte is read.
    ///
    /// It is therefore also the throttle on concurrent WORK, which is the job the
    /// connection cap above stopped being able to do: a surface serving megabyte
    /// objects limits how many move at once by choosing a budget worth about one of
    /// them, and does not need a second count to say the same thing less precisely.
    /// A refusal here is a reply on a kept connection, so a peer that arrives during
    /// a busy moment is told to come back rather than made to reconnect.
    [[nodiscard]] virtual std::size_t MaxInFlightBytes() const noexcept = 0;

    /// Whether this verb's owner accounts for the request's bytes itself.
    ///
    /// The budget above is charged when the header declares its length and released
    /// when `Answer` returns, which is right for a verb whose answer is a round trip
    /// and wrong for one whose answer is a COMPILE. A compile runs for seconds to
    /// minutes, and `CompileResponder` charges the same frame against
    /// `CompileCapacity` -- the accounting the worker advertises and the dedicated
    /// port already uses. Held in both, one buffer is counted in two pools for the
    /// whole compile, and the endpoint's pool is shared per LISTENER: compiles
    /// filling it refuse a 64 KiB `REGISTER` with `EndpointBusy`, which carries no
    /// redirect and is not followed by the worker half, so a peer expires out of the
    /// registry and every lease answers `NoWorker` -- a fleet outage caused by this
    /// node's own compile load (#448).
    ///
    /// So a verb that answers `true` is charged by the endpoint only while its
    /// payload is READ -- which nothing else can account for, since the owner has no
    /// frame yet -- and by its owner from there on. One buffer, one pool, and the
    /// figure both doors charge is the one the worker advertises.
    ///
    /// **The handoff is tight by construction, and rests on two invariants stated
    /// here because neither is visible at the release site.** `Task` is lazy
    /// (`Async/Task.hpp`: `initial_suspend` is `suspend_always`, with symmetric
    /// transfer at the final one), so `co_await Answer(...)` runs the body on the
    /// awaiting thread with no return to the loop; and the node's framed surfaces
    /// share exactly one reactor thread (`NodeIoLoop`). Between the release and the
    /// owner's charge there is therefore no suspension point and no other thread
    /// serving this pool, so nothing can observe the lowered figure. Were either
    /// invariant broken, `DeclaredRequestFootprint` floors at the payload length, so
    /// the owner always charges at least what was released -- the error would be
    /// bounded by one frame per reactor thread and would err towards over-admission.
    ///
    /// Pure virtual for the reason the ceilings above are: a surface that says
    /// nothing would answer `false` by inheritance and silently double-charge, which
    /// is precisely the defect this column exists to close.
    ///
    /// @param opRaw The third header byte, as received; not necessarily a known verb.
    /// @return True when the owner of @p opRaw charges the request's bytes itself.
    [[nodiscard]] virtual bool HoldsOwnByteBudget(std::uint8_t opRaw) const noexcept = 0;
};

/// Accepts connections and answers framed requests on each until the peer stops.
///
/// Shaped after `WorkerServer` rather than `Server`, and for the reason that governs
/// the whole node: `Connection` is built around a `CacheEngine`, and a scheduler has
/// no cache. Taking an `IListener&` and running its own loop keeps the node clear of
/// the cache stack while still reusing the reactor and the socket abstraction.
///
/// ## Many requests per connection
///
/// A connection is served until the peer stops sending, which is what the daemon
/// serving this same wire has always done (`Protocol/CompileCacheHandler`). This
/// endpoint answered exactly one request and closed, and that was issue #176 rather
/// than a simpler design: a worker dials once per heartbeat ROUND and then registers
/// every toolchain it found over that one connection, so a machine with two
/// toolchains registered one of them, every round, forever -- reported as a transport
/// failure, which named neither the cause nor the toolchain that lost.
///
/// There is still no per-connection state and no handshake; a loop is not a session.
/// What it buys is that a peer with two things to say may say both, which is a
/// property of the wire (a credential is a frame, so presenting one is two frames)
/// rather than an optimisation.
///
/// ## The payload cap is small on purpose
///
/// A scheduler verb carries a fingerprint, an endpoint and a key, none of which is
/// large, so the ceiling is kilobytes rather than the cache's megabytes. A frame over
/// it is refused with a *reply* naming both numbers, not a close.
///
/// It used to carry a second job: membership was checked inside the service, after
/// the frame had been read, so the small cap was what bounded what a stranger could
/// make this endpoint allocate. `IFrameResponder::RefusePeer` closes that directly
/// now (#285, #377) -- a peer the surface will refuse is refused before its payload
/// is read at all -- and the cap is back to being only what it says it is.
class FrameServer
{
  public:
    /// How long a peer may take to NAME a verb before its socket is abandoned.
    ///
    /// This used to be `SO_RCVTIMEO`, applied to every accepted socket by
    /// `BlockingListener::SetTimeouts`. A reactor socket has no such option -- its
    /// reads suspend rather than block -- so without a replacement a client that
    /// connects and sends half a header holds a descriptor and a coroutine frame
    /// until the process dies. A slow-loris on the node's cache port, free.
    ///
    /// Armed once per REQUEST rather than once per connection, so a conversation is
    /// not swept mid-flight. This window covers the idle gap before a request and its
    /// header -- so an attached peer that stops talking is still swept, which is what
    /// keeps the slow-loris property once a connection is long-lived.
    ///
    /// **It stops there, and it used to not.** It was `RequestTimeout` and it covered
    /// the answer as well, which is a five-second ceiling on how long a responder may
    /// take -- correct for a cache round trip and fatal for a compile, which runs for
    /// minutes. `IFrameResponder::RequestTimeout` is what covers the answer now, and
    /// the split is deliberate rather than cosmetic: everything decided BEFORE a verb
    /// is named is decided about a peer that has told this surface nothing, so it must
    /// stay short whatever the surface goes on to serve. Renamed rather than
    /// redocumented, because a name that has quietly stopped describing its own fact is
    /// how #365 happened.
    static constexpr std::chrono::milliseconds HeaderTimeout { 5'000 };

    /// How often the sweeper looks for connections past their deadline.
    ///
    /// ONE sweeper per server rather than one timer per connection, and that is the
    /// difference between a bounded cost and a parked frame per client.
    /// `IReactor::Schedule` cannot be cancelled, so a per-connection timer would
    /// stay on the wheel for the full interval after its connection had already
    /// finished -- the leak `Async/DeadlineTimer` documents at length.
    static constexpr std::chrono::milliseconds SweepInterval { HeaderTimeout / 4 };

    /// How long a REFUSED connection is given to read its refusal and hang up.
    ///
    /// Shorter than `HeaderTimeout` because there is less to wait for: the peer has
    /// its answer already and has only to close, which is a round trip rather than a
    /// request. A refusal takes no connection slot, so nothing counts one -- and a
    /// surface that is refusing is one already at capacity, which is exactly when a
    /// flood must not be able to park a socket per attempt for as long as a real
    /// request gets.
    ///
    /// One sweep interval, because the sweeper's cadence is the floor on how promptly
    /// any deadline can be acted on: asking for less would be a number that reads like
    /// a guarantee and is not one. So a refused socket outlives its answer by at most
    /// two ticks, against the six a served request may take.
    static constexpr std::chrono::milliseconds RefusalTimeout { SweepInterval };

    /// How long a swept-but-not-closed connection is given to come back and explain
    /// itself before it is closed after all.
    ///
    /// A connection swept while parked INSIDE the responder is not closed: closing it
    /// there wakes nothing (`IFrameResponder::RequestTimeout` says why) and only
    /// destroys the write side the refusal has to leave by. It is marked instead, and
    /// the connection writes its own refusal when `Answer` returns.
    ///
    /// **This is what keeps that deferral from being unbounded.** `Answer` is an
    /// interface, so nothing here can promise it returns at all -- a wedged responder
    /// would otherwise hold the descriptor for the life of the process, where the old
    /// unconditional close released it. When this expires the sweeper closes as it
    /// always did, so the fix can only ever cost this much promptness and never the
    /// property it replaced.
    ///
    /// **Derived from the verb's own window, and it shipped once as a flat constant
    /// that was wrong by a factor of 120.** `SweepInterval * 4` resolves to five
    /// seconds; `CompileResponder::RequestTimeout` is `DefaultCompileLeaseTimeout`,
    /// which is six hundred. A translation unit that has just outrun a ten-minute
    /// grant does not return inside five seconds, so the deferral expired, the socket
    /// was closed, and the connection found nothing owed when `Answer` finally came
    /// back -- delivering the explanation reliably on the cache surface, where the
    /// problem is mild, and silently not at all on the compile surface, which is the
    /// one #523 was filed about. The constant's own justification was that its value
    /// is "a property of this mechanism and not of a site's workload", written three
    /// hundred lines below `IFrameResponder::RequestTimeout` arguing that ONE NUMBER
    /// CANNOT BOUND BOTH THINGS this endpoint carries -- and pure virtual so that no
    /// surface can inherit five seconds, because five seconds is exactly the value
    /// that makes this failure.
    ///
    /// So the rule is: **a responder gets at most twice its own budget before the
    /// socket goes.** Ten seconds on the cache, twenty minutes on a compile; both
    /// bounded, both proportional to what the surface itself said its work costs, and
    /// neither able to drift from `RequestTimeout` because it is the same number. No
    /// flag and no new virtual -- the value comes from a question the endpoint
    /// already asks at the point it arms the answer window.
    ///
    /// A pure function over the window rather than arithmetic inlined at the sweep,
    /// so the magnitude can be pinned on both sides without a case that waits out a
    /// wall-clock spread the instrument's own noise is comparable to.
    /// @param window The verb's answer window, as the responder named it.
    /// @return How long past the sweep the descriptor is held open.
    [[nodiscard]] static constexpr std::chrono::milliseconds ExplanationGraceFor(std::chrono::milliseconds window) noexcept
    {
        // Floored at the sweeper's cadence, which is the floor on how promptly any
        // deadline here can be acted on: a grace under one tick would be a number
        // that reads like a guarantee and is not one. The same argument
        // `RefusalTimeout` above makes for the same reason.
        return std::max(window, SweepInterval);
    }

    /// @param io The loop this server accepts and answers on.
    /// @param listener Bound listener; must outlive the run.
    /// @param responder Answers each request; must outlive the run.
    /// @param what Names this surface in log lines, so three endpoints in one
    ///        process are distinguishable when one of them stops accepting.
    /// @param metrics Where this endpoint's OWN refusal is recorded. Exactly one
    ///        refusal here belongs to the endpoint rather than to a surface: a
    ///        connection turned away because every connection slot is taken is
    ///        decided at accept, before a header exists, so it names no verb and
    ///        `MergedResponder` has nothing to route it by. Every other refusal is
    ///        the owning surface's and reaches its counter through `IFrameResponder`.
    /// @param logger Shared logger.
    FrameServer(NodeIoLoop& io,
                IListener& listener,
                IFrameResponder& responder,
                std::string_view what,
                IMetricsSink& metrics,
                ILogger& logger) noexcept;

    FrameServer(FrameServer const&) = delete;
    FrameServer(FrameServer&&) = delete;
    FrameServer& operator=(FrameServer const&) = delete;
    FrameServer& operator=(FrameServer&&) = delete;
    ~FrameServer();

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    ///
    /// Each accepted connection is served by a `DetachedTask` of its own rather
    /// than inline, which is the entire point: the cache surface consults an
    /// upstream from inside its answer, and serving that inline is what made one
    /// slow dial stall every other client of this node.
    [[nodiscard]] Task<void> Run();

    /// Stop accepting, close what is open, and wait for the connections to end.
    ///
    /// Safe from any thread. The closes are POSTED onto the reactor rather than
    /// done here, and that is not caution: on epoll and kqueue `ISocket::Close`
    /// completes a parked awaitable by resuming its coroutine INLINE, so closing
    /// from another thread would run this server's connection tasks there while the
    /// reactor thread is still driving them. IOCP routes cancellation back through
    /// the port and does not, which is exactly what would make it a race that
    /// passes CI on Windows.
    void Shutdown() noexcept;

    /// Report a loop that threw, without throwing. Called by the owning loop's
    /// exception firewall.
    void NoteLoopThrew() noexcept;

    /// @return How many connections are being served right now. For tests.
    [[nodiscard]] std::size_t OpenConnections() const noexcept;

    /// @return How many declared payload bytes are in flight. For tests.
    [[nodiscard]] std::size_t InFlightBytes() const noexcept;

    /// Implementation detail; public so the .cpp's connection tasks can name it.
    struct State;

  private:
    std::shared_ptr<State> _state;
};

/// The node's scheduler port: listener, server and the thread that serves them,
/// owned as one thing.
///
/// A class rather than three locals in `main()`, for the reason `AdminEndpoint`
/// records: the three have a *destruction order* -- the server must close its
/// listener before the thread serving it can be joined -- and holding them separately
/// expresses that as a `Shutdown()` somebody has to remember at every return path.
/// Here it is the destructor. And `main.cpp` in this binary is in no test target, so
/// wiring that lives there has no unit coverage at all.
class FrameEndpoint
{
  public:
    /// Bind the endpoint and start serving it.
    ///
    /// The error is a diagnostic string rather than one of the project's four error
    /// enums, the same deliberate departure `AdminEndpoint::Start` documents: this
    /// fails in two ways belonging to two taxonomies -- a malformed listen spec is a
    /// `ConfigError`, an address that will not bind is a `NetError` -- the caller's
    /// response is identical either way, and what it needs is the text.
    /// **A surface, never an address.** This used to take the listen spec, the host a
    /// bare port falls back to and the surface's name as three loose arguments, and
    /// every caller supplied its own -- so the port map lived here as well as in the
    /// four other places #288 found it, and a new surface could be opened without
    /// appearing on the map an operator is told to build a firewall from.
    ///
    /// Taking the enumerator makes that impossible rather than merely discouraged:
    /// there is no argument to pass a bare string to, so a port cannot be opened
    /// without a row, and a row cannot exist without `RowsInEnumeratorOrder`
    /// accepting it. A guard that fails the build beats one that fails a suite.
    /// @param io The loop this endpoint accepts and answers on. It must be
    ///        `Start()`ed after every endpoint has been created, so the listener is
    ///        bound and adopted before anything begins accepting.
    /// @param surface Which surface to serve. Its row supplies the address, the host
    ///        a bare port takes and the name used in log lines -- including the
    ///        asymmetry between them, which is the anti-leeching rule rather than a
    ///        caller's preference: a scheduler no peer can dial does nothing, while a
    ///        node's private cache reachable from the network is this machine's whole
    ///        build output served to strangers.
    /// @param cfg What the operator asked for; the row resolves the endpoint from it.
    /// @param responder Answers each request; must outlive the endpoint.
    /// @param metrics Where this endpoint's own at-capacity refusal is counted; see
    ///        the `FrameServer` constructor for why exactly one refusal is its own.
    /// @param logger Where to announce the bound address.
    /// @return The running endpoint, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<FrameEndpoint>, std::string> Start(NodeIoLoop& io,
                                                                                          NodeSurface surface,
                                                                                          NodeConfig const& cfg,
                                                                                          IFrameResponder& responder,
                                                                                          IMetricsSink& metrics,
                                                                                          ILogger& logger);

    /// Serve a surface on a descriptor a supervisor already bound and listened.
    ///
    /// The socket-activation counterpart to `Start`, and everything after the
    /// listener is obtained is the same -- which is why they are two factories over
    /// one private constructor rather than one factory with a flag: what differs is
    /// exactly where the listener comes from, and nothing else.
    ///
    /// **No row is resolved and no address is read from `cfg`.** Under activation the
    /// unit owns the address; `--listen-node` holds a value that describes nothing,
    /// which is the whole of the rule that a flag naming a listener this process did
    /// not open cannot answer questions about that listener. The port comes back from
    /// the descriptor itself, and the HOST can only come from `--advertise` -- which
    /// is why activation makes that flag mandatory and refuses at startup without it.
    ///
    /// **The descriptor is handed on, never held.** `PlatformListener::Adopt` takes
    /// ownership including on its own failure paths, so this function neither closes
    /// it on a refusal nor lets anything else do so. A failed adoption is a startup
    /// refusal exactly as a failed bind is: an activated node that cannot serve the
    /// descriptor it was given is in the position that refusal exists for, and
    /// warning past it would leave a worker registering an endpoint nothing answers.
    ///
    /// @param io The loop this endpoint accepts and answers on.
    /// @param surface Which surface to serve. Its row supplies the name used in log
    ///        lines; its address is not consulted, because the unit chose it.
    /// @param descriptor An already-bound, already-listening descriptor. Ownership
    ///        passes to the listener built here, whether or not that succeeds.
    /// @param advertisedHost The host clients are told to dial, from `--advertise`.
    ///        The only honest answer to "what address is this", since the process
    ///        cannot ask the unit and a wildcard bind names nothing dialable.
    /// @param responder Answers each request; must outlive the endpoint.
    /// @param metrics Where this endpoint's own at-capacity refusal is counted; see
    ///        the `FrameServer` constructor for why exactly one refusal is its own.
    /// @param logger Where to announce the adopted address.
    /// @return The running endpoint, or why the descriptor could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<FrameEndpoint>, std::string> StartAdopted(
        NodeIoLoop& io,
        NodeSurface surface,
        int descriptor,
        std::string_view advertisedHost,
        IFrameResponder& responder,
        IMetricsSink& metrics,
        ILogger& logger);

    /// Stop serving. The loop's own thread is joined by `NodeIoLoop`.
    ~FrameEndpoint();

    FrameEndpoint(FrameEndpoint const&) = delete;
    FrameEndpoint& operator=(FrameEndpoint const&) = delete;

    // Neither movable, for the reason `AdminEndpoint` gives: the reactor holds
    // pointers into `_server`, and `_server` a reference to `_listener`.
    FrameEndpoint(FrameEndpoint&&) = delete;
    FrameEndpoint& operator=(FrameEndpoint&&) = delete;

    /// The address this endpoint actually bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

    /// @return How many declared payload bytes are in flight. For tests.
    ///
    /// Forwarded from `FrameServer` rather than exposing the server itself: the one
    /// thing a case needs to read from outside is this figure, and the handoff in
    /// `IFrameResponder::HoldsOwnByteBudget` is an ORDERING that nothing else can
    /// observe (#448).
    [[nodiscard]] std::size_t InFlightBytes() const noexcept;

  private:
    FrameEndpoint(NodeIoLoop& io,
                  std::unique_ptr<IListener> listener,
                  IFrameResponder& responder,
                  std::string_view what,
                  std::string boundEndpoint,
                  IMetricsSink& metrics,
                  ILogger& logger);

    /// `IListener` and not the platform type, deliberately: naming the concrete one
    /// would drag `<windows.h>` into every header that includes this, and nothing
    /// here needs more than Accept, Close and BoundPort -- the last of which is on
    /// the interface now precisely so this could stop being concrete.
    std::unique_ptr<IListener> _listener;
    std::unique_ptr<FrameServer> _server;
    std::string _boundEndpoint;
};

} // namespace FastCache::Node
