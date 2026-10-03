// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>
#include <core/Ranges.hpp>

namespace FastCache::Node
{

namespace Detail
{
    /// What the CACHE surface does about one refusal it may be asked to answer.
    ///
    /// **Exactly one of the two is set**, checked per row rather than described: a
    /// counter says a rise here is something an operator acts on, a rationale says a
    /// rise would mean nothing and carries the argument for that. Those are the two
    /// claims `Cc::Refuse` and `Cc::RefuseWithoutCounter` make, and pairing them here
    /// is what keeps the answer beside the arm instead of inside a `switch` where a
    /// missing case would only imply it.
    ///
    /// The wire code is deliberately NOT a column. It is a property of the refusal
    /// rather than of this surface -- `ErrorCodeFor` owns it for both enumerations --
    /// and a row restating it is a second statement of one fact, which is a second
    /// thing to be wrong. The scheduler's table restates it and `static_assert`s the
    /// agreement; not restating it is the same guarantee for no rows.
    struct CacheRefusalPolicy
    {
        /// What rises, or nothing where this surface deliberately counts none.
        std::optional<IMetricsSink::Counter> counter;

        /// Why nothing rises. Empty exactly when `counter` is set.
        ///
        /// Never sent and never read at run time -- see `Cc::UncountedRefusal` for why
        /// it exists at all. Spelled `rationale` and not `why` for the reason stated
        /// there: `CompileCacheWire::RefusedVerb::why` is text a CLIENT is sent.
        std::string_view rationale;
    };

    /// Whether @p policy states one claim rather than none or both.
    /// @param policy The row to check.
    /// @return True when exactly one of the counter and the rationale is present.
    [[nodiscard]] constexpr bool StatesOneClaim(CacheRefusalPolicy const& policy) noexcept
    {
        // Delegated rather than restated. The rule belongs to a refusal ROW and not to
        // this surface, so it lives beside the three spellings it is about, in
        // `Protocol/SurfaceRefusal.hpp` (#640) -- any surface that refuses owes it, and
        // it sat in the node's endpoint header only because `Protocol/` was outside
        // #523's grant. Three surfaces spelling one truth table separately is how it
        // briefly came to exist in two idioms, one of them the double-negated form --
        // and a grep for the name reached only one of the three.
        return Cc::StatesOneRefusalClaim(policy.counter.has_value(), policy.rationale);
    }

    /// Answer a cache-surface refusal the way its row decided.
    ///
    /// One door for both of `CacheResponder`'s arms, so the counted and the uncounted
    /// spellings are chosen from data rather than remembered at each call site.
    /// @param metrics Where a counted refusal is recorded.
    /// @param code What the client is told, from `ErrorCodeFor`.
    /// @param policy This surface's decision about the arm.
    /// @param detail Words for a person, or empty when there are none to add.
    /// @return The encoded reply.
    [[nodiscard]] inline std::vector<std::byte> AnswerCacheRefusal(IMetricsSink& metrics,
                                                                   CompileCacheWire::ErrorCode code,
                                                                   CacheRefusalPolicy const& policy,
                                                                   std::string_view detail)
    {
        if (policy.counter.has_value())
            return Cc::Refuse(metrics, { .code = code, .counter = *policy.counter }, detail);
        return Cc::RefuseWithoutCounter({ .code = code, .rationale = policy.rationale }, detail);
    }

    /// One row of `CacheEndpointRefusals`.
    struct CacheEndpointRefusal
    {
        EndpointRefusal refusal;   ///< Which endpoint decision this describes.
        CacheRefusalPolicy policy; ///< What this surface does about it.
    };

    /// What the CACHE surface does about each endpoint-decided refusal (#491).
    ///
    /// The byte budget is the arm #491 was filed about, and the one this surface
    /// counts. `MergedResponder::MaxInFlightBytes()` folds to the LARGEST owner's
    /// budget, which on any node holding a tier is this cache's, so the endpoint's
    /// in-flight ceiling IS the cache's ceiling and the refusal that fires in practice
    /// is a `STORE`. Answered correctly and counted nowhere, that is #326's scenario
    /// one surface over: the port is hammered and the graph is flat.
    ///
    /// The earlier position -- that the budget is "a transient the peer retries past"
    /// -- is sound for the SCHEDULER, whose ceiling is kilobytes and whose series is
    /// about credentials. It is the opposite of what happens here, and inheriting it
    /// is how a decision about one surface came to describe another.
    inline constexpr EnumTable<EndpointRefusal, CacheEndpointRefusal> CacheEndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          .policy = { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedEndpointBusy, .rationale = {} } },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSessionsRationale } },
        { .refusal = EndpointRefusal::CredentialRejected,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSessionsRationale } },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .policy = { .counter = std::nullopt, .rationale = AnswerDeadlineIsTheEndpointsRationale } },
        { .refusal = EndpointRefusal::NodeProofUnchallenged,
          .policy = { .counter = std::nullopt, .rationale = NodeProofIsTheProversRationale } },
    } };

    // Positional rows alone would not catch an appended enumerator: it leaves a
    // value-initialised row whose policy states NEITHER claim, and a guard that
    // short-circuits on the absent counter passes vacuously while the new refusal
    // ships uncounted -- which is this issue's own defect re-entering through its fix.
    static_assert(RowsInEnumeratorOrder(CacheEndpointRefusals, &CacheEndpointRefusal::refusal),
                  "CacheEndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    // And that each row asserts exactly one thing. A row with neither is the vacuous
    // pass above; a row with both is an author who could not choose, answered here
    // rather than at whichever call site read the fields in the luckier order.
    static_assert(Cc::RowsStateOneRefusalClaim(CacheEndpointRefusals,
                                               [](CacheEndpointRefusal const& row) {
                                                   return Cc::RefusalClaim { .counted = row.policy.counter.has_value(),
                                                                             .rationale = row.policy.rationale };
                                               }),
                  "every cache endpoint refusal must state either a counter or a rationale, and not both");

    /// What the CACHE surface does about each pre-payload decision (#491).
    ///
    /// A total switch rather than an `EnumTable`, which is **not** a local exception:
    /// `CompileResponder::RefusalFor` answers the same question about the same enum
    /// the same way, and its comment is where the reason is written down. In short,
    /// `PrePayloadDecision` is a wire enum both binaries compile in, so giving it a
    /// `Last` to satisfy a node-local table idiom would be a wire change bought for
    /// nothing. `-Werror=switch` is the guard instead -- the same one
    /// `CompileCacheWire::ErrorCodeFor` relies on for this enumeration -- so a fifth
    /// decision fails the build here rather than falling through to a neighbour's
    /// answer.
    ///
    /// @param decision A decision other than `Serve`.
    /// @return What this surface does about it.
    [[nodiscard]] constexpr CacheRefusalPolicy CachePrePayloadPolicy(CompileCacheWire::PrePayloadDecision decision) noexcept
    {
        switch (decision)
        {
            case CompileCacheWire::PrePayloadDecision::PayloadTooLarge:
                // **The arm #491 names.** Twenty-four bytes and no body is the cheapest
                // probe there is, and the surface-wide frame ceiling on a node holding
                // a tier is this cache's -- so an operator alerting on the compile
                // surface's series watches a flat graph while the port is hammered.
                //
                // The position this replaces -- that the framing arms are "already
                // visible as such to the peer" -- answers a question nobody asked:
                // #491's scenario is a client sending oversized declarations on
                // purpose, so the peer IS the attacker and its visibility is not the
                // property anyone needs.
                return { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedPayloadTooLarge, .rationale = {} };
            case CompileCacheWire::PrePayloadDecision::UnknownOpcode:
                return { .counter = std::nullopt,
                         .rationale = "MergedResponder owns a verb only when FamilyOf names its family, and an opcode "
                                      "with no OpTable row is Unset -- so an unknown one is answered UnservedReply at "
                                      "the door and never reaches this surface" };
            case CompileCacheWire::PrePayloadDecision::Unauthenticated:
                return { .counter = std::nullopt, .rationale = NodeChecksNoPasswordRationale };
            case CompileCacheWire::PrePayloadDecision::Serve:
                break;
        }
        // Unreachable by contract, as `ErrorCodeFor`'s own `Serve` arm is: the endpoint
        // asks this only for a decision that refused. Closed rather than left to fall
        // off the end, and closed UNCOUNTED, because inventing an event for a request
        // that was served is the one wrong answer available here.
        return { .counter = std::nullopt, .rationale = "Serve is not a refusal and the endpoint never asks about it" };
    }

    // The same guard the table above gets, over the switch -- and it is NOT redundant
    // with `-Werror=switch`, which catches a MISSING arm and not an EMPTY one. An arm
    // returning `{ nullopt, {} }` -- a rationale dropped in an edit, or a new decision
    // written in a hurry -- reaches `RefuseWithoutCounter` with nothing to say: a
    // refusal answered correctly, counted nowhere and asserting NOTHING, which is the
    // state this whole change exists to remove. It is also the one state no scan can
    // see, because `worker-refusals-counted` tallies `RefuseUntriaged` and an empty
    // rationale joins no backlog and is reported by nobody.
    //
    // Spelled over a list because `PrePayloadDecision` has no `Last` to iterate. The
    // list cannot go stale unnoticed: a fifth decision fails `-Werror=switch` first,
    // which puts the author in this function with the list on screen.
    static_assert(std::ranges::all_of(std::array { CompileCacheWire::PrePayloadDecision::Serve,
                                                   CompileCacheWire::PrePayloadDecision::UnknownOpcode,
                                                   CompileCacheWire::PrePayloadDecision::PayloadTooLarge,
                                                   CompileCacheWire::PrePayloadDecision::Unauthenticated },
                                      [](CompileCacheWire::PrePayloadDecision decision) {
                                          return StatesOneClaim(CachePrePayloadPolicy(decision));
                                      }),
                  "every cache pre-payload decision must state either a counter or a rationale, and not both");

    /// What the SCHEDULER surface answers each endpoint-decided refusal with.
    ///
    /// `Cc::SurfaceRefusal` rows and `Cc::Refuse`, not an `Increment` beside an
    /// `EncodeErrorReply`: the row IS the refusal, so there is no argument to pass a
    /// bare code to and the counter cannot be left out. That is the property #447
    /// reinstates elsewhere in this change, and a file holding two new security
    /// counters is the last place to ship the other spelling.
    ///
    /// A `std::optional` per row rather than a switch with a silent arm, because every
    /// refusal here deliberately counts NOTHING, each for the reason its row states: the
    /// byte budget says this surface is momentarily full, which the peer retries past;
    /// the credential is the session component's; the answer deadline is the endpoint's
    /// own decision; a proof is the prover's. `nullopt` says so where a missing `case`
    /// would only imply it. `EnumTable` takes its extent from `EndpointRefusal::Last`, so
    /// a new enumerator leaves an empty row here rather than silently borrowing a
    /// neighbour's.
    struct SchedulerEndpointRefusal
    {
        EndpointRefusal refusal;                  ///< Which endpoint decision this describes.
        std::optional<Cc::SurfaceRefusal> answer; ///< The row, or nothing where this surface counts none.

        /// Why nothing is counted, for a row whose `answer` is `nullopt`.
        ///
        /// **On the ROW, because the reasons differ.** It used to be one literal at
        /// the call site, correct while exactly one row was uncounted -- and the
        /// moment a second appeared, that sentence would have explained the byte
        /// budget to somebody reading about a deadline. `UncountedRefusal::rationale`
        /// is a forcing function precisely so an author states the reason for THIS
        /// refusal, and a shared literal is how it stops forcing anything.
        std::string_view rationale;
    };

    inline constexpr EnumTable<EndpointRefusal, SchedulerEndpointRefusal> SchedulerEndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          .answer = std::nullopt,
          .rationale = "the byte budget says this surface is momentarily full, which the peer sees and retries; summed "
                       "into a security series it is what makes that series unreadable" },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSessionsRationale },
        { .refusal = EndpointRefusal::CredentialRejected,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSessionsRationale },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .answer = std::nullopt,
          .rationale = AnswerDeadlineIsTheEndpointsRationale },
        { .refusal = EndpointRefusal::NodeProofUnchallenged,
          .answer = std::nullopt,
          .rationale = NodeProofIsTheProversRationale },
    } };

    // Every uncounted row says why, and no counted row carries a reason it does not
    // need. The same "states one claim" guard the cache table has, and for the same
    // reason: a row asserting neither is the vacuous pass, and one asserting both is
    // an author who could not choose, answered here rather than at whichever call
    // site read the fields in the luckier order.
    static_assert(Cc::RowsStateOneRefusalClaim(SchedulerEndpointRefusals,
                                               [](SchedulerEndpointRefusal const& row) {
                                                   return Cc::RefusalClaim { .counted = row.answer.has_value(),
                                                                             .rationale = row.rationale };
                                               }),
                  "every scheduler endpoint refusal must state either a counted answer or a rationale, not both");

    // Positional rows alone would not have caught this: appending an enumerator leaves
    // a value-initialised row here, whose `answer` is `nullopt` -- so a guard that
    // short-circuits on the absent case passes VACUOUSLY and the new refusal ships
    // uncounted, which is this ticket's own defect re-entering through its fix. The row
    // carries its enumerator so the position is checked whatever the answer is.
    static_assert(RowsInEnumeratorOrder(SchedulerEndpointRefusals, &SchedulerEndpointRefusal::refusal),
                  "SchedulerEndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    // A row states the code a second time, and a second statement of one fact is a
    // second thing to be wrong. Checked rather than trusted, against the one place
    // that property lives.
    static_assert(std::ranges::all_of(SchedulerEndpointRefusals,
                                      [](SchedulerEndpointRefusal const& row) {
                                          return !row.answer.has_value() || row.answer->code == ErrorCodeFor(row.refusal);
                                      }),
                  "a scheduler refusal row must answer the code `ErrorCodeFor` names for its refusal");
} // namespace Detail

/// Serves the fleet's scheduling verbs.
///
/// The peer's host reaches the membership oracle here and nowhere else, which is
/// what keeps `FrameServer` free of any policy: it hands over an identity and does
/// not know what anybody does with it.

class SchedulerResponder final: public IFrameResponder
{
  public:
    /// @param protocol Answers each request; must outlive this.
    /// @param membership Decides who may spend the fleet's capacity; must outlive this.
    /// @param metrics Where an endpoint refusal would be counted; must outlive this.
    SchedulerResponder(Distributed::SchedulerProtocol& protocol,
                       Distributed::IMembershipOracle const& membership,
                       IMetricsSink& metrics) noexcept:
        _protocol { protocol },
        _membership { membership },
        _metrics { metrics }
    {
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Never suspends. The scheduler answers from its own tables -- a decision
    /// layer kept pure so every capacity and expiry rule is a `core::platform::ManualClock` unit
    /// test -- so this is a one-line adapter, and a responder that costs a frame
    /// allocation and no round trip is a legitimate thing to be.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override
    {
        co_return _protocol.Answer(frame, Context(std::move(peer)));
    }

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// Membership, which is decided from what the CONNECTION is -- its address and what it has
    /// proved -- so the transport can ask it before it reads a payload, and a stranger cannot
    /// make the scheduler allocate for a frame that was never going to be served (#285).
    ///
    /// It said *from the peer's host alone* until #1428, and that clause was the whole of why
    /// a worker on a VPN could not be admitted. Since #178 it also refuses a verb a joining
    /// machine sends -- the opcode is what says which -- on a connection that proved no live
    /// identity, which is why the opcode is now read here rather than ignored.
    ///
    /// Delegated rather than reimplemented: `SchedulerService::RefuseUnlessMember` is
    /// the same function `Gate()` calls, so the early check and the authoritative one
    /// cannot disagree, and the `NotAMember` counter is incremented inside it exactly
    /// once per refused request.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override
    {
        return _protocol.RefusePeer(Context(peer), opRaw);
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`: AUTH is the Session family's; this surface is never routed one.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return NotTheSessionSurface();
    }

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// Uncounted: a size or opcode refusal says the peer is confused, and an
    /// unauthenticated one cannot happen on a node that checks no password.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t /*opRaw*/,
                                                      std::string_view detail) const override
    {
        // The caller's wording, which is empty for every decision but the frame
        // ceiling -- and that one names both numbers, because "too large" without the
        // limit tells an operator nothing about a kilobyte cap.
        return Cc::RefuseWithoutCounter({ .code = CompileCacheWire::ErrorCodeFor(decision),
                                          .rationale = decision == CompileCacheWire::PrePayloadDecision::Unauthenticated
                                                           ? NodeChecksNoPasswordRationale
                                                           : "a size or opcode refusal says the peer is confused about "
                                                             "the framing, not about the fleet" },
                                        detail);
    }

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// Every arm is uncounted, and each says why in its own row of
    /// `Detail::SchedulerEndpointRefusals` -- the rationale travels with the row rather
    /// than sitting here, because a sentence written once at the call site explained the
    /// budget correctly and would have explained a deadline wrongly.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t /*opRaw*/,
                                                              std::string_view detail) const override
    {
        auto const& row = Detail::SchedulerEndpointRefusals[static_cast<std::size_t>(refusal)];
        return AnswerEndpointRefusal(_metrics, ErrorCodeFor(refusal), row.answer, row.rationale, detail);
    }

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// A round trip. `SchedulerProtocol::Answer` never suspends -- it answers from its
    /// own tables -- so what this covers is a peer sending a payload it already has in
    /// hand, and the endpoint's own header window is the right size for that.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// Kilobytes, not megabytes.
    ///
    /// Membership is checked *inside* the service — after the frame is read — so an
    /// unauthenticated peer can make this endpoint buffer whatever it declares. A
    /// scheduler verb carries a fingerprint, an endpoint and a key, so this is the
    /// honest ceiling; sizing it like the cache's would hand a stranger a way to
    /// make the scheduler allocate megabytes.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return 64ULL * 1024ULL;
    }

    /// Hundreds: a fleet's worth of workers, each attached for a heartbeat round.
    ///
    /// A worker dials once per round and then speaks for every toolchain it found, so
    /// a connection here is held for as long as that conversation takes rather than
    /// for one verb. 256 of them is a large fleet, and at 64 KiB a request the memory
    /// behind them is bounded below by the byte budget anyway.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 256;
    }

    /// 16 MiB: the connection cap times the request cap, so the byte budget never
    /// refuses anything the connection cap would have allowed.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return 256ULL * 64ULL * 1024ULL;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No: a scheduler verb is answered from memory in microseconds, so the
    /// endpoint's reservation is released almost as soon as it is taken and there is
    /// nothing a second accounting would add.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// None. A scheduler verb is answered from memory in microseconds, so a client
    /// cannot realistically vanish inside one -- and the write that would discover it
    /// is the very next statement anyway. Watching would buy a park and a wake per
    /// request to learn something the write already reports.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// None, and for the mirror of the reason above: a scheduler verb is answered from
    /// memory in microseconds, so there is no interval in which a client could wonder
    /// whether this node is still there. A pulse would be a frame that cannot arrive
    /// before the reply it precedes, on a verb whose client is not looping for one.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Not a stream**: every scheduler verb is one decision and one reply; the fleet a watcher follows is
    /// the live-stats component's subscription, which reads this scheduler rather than being it.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None.** An identity is not this surface's question, and a proof could not widen this
    /// gate even if one were verified here: a cache tier serves THIS MACHINE, which is a property
    /// of the verb rather than of a member list (#287). See `RefusePeer` above.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    /// Who is asking, as both the gate and the early refusal need it.
    ///
    /// One place the peer becomes a `CallerContext`, so the classification behind an
    /// early refusal is by construction the classification the verb would have got.
    ///
    /// By value and moved, which is the shape #395 was filed about. It was never a
    /// use-after-free on either production path -- the old view pointed into the
    /// coroutine frame's own `std::string` here, and into `FrameEndpoint`'s
    /// connection-lifetime peer at the refusal site, both of which outlived the
    /// context. It was a hazard the TYPE invited and the next author would have
    /// taken, which is what that ticket is about and why nothing shipped broken.
    ///
    /// `CallerContext::peerId` owns now, so there is no parameter for the returned
    /// context to point into, and a caller already holding a `std::string` hands it
    /// over rather than having it copied.
    ///
    /// **The identity is folded through `Distributed::CallerContextOf`, since #1428 and #178.**
    /// It has to happen HERE rather than at the two call sites, because this is the one place the
    /// peer becomes a `CallerContext` -- and a fold at one of them would make the door and the
    /// authoritative gate answer differently about one connection, which is a caller admitted and
    /// then refused a line later. The host is moved into the context only after the fold has read
    /// it, inside that function, where the order is a statement sequence rather than an argument
    /// list's.
    ///
    /// @param peer The caller, whose host is taken over by the returned context.
    [[nodiscard]] Distributed::CallerContext Context(PeerIdentity peer) const
    {
        return Distributed::CallerContextOf(_membership, std::move(peer));
    }

    Distributed::SchedulerProtocol& _protocol;
    Distributed::IMembershipOracle const& _membership;
    IMetricsSink& _metrics;
};

/// Serves this node's own cache tier to clients on this machine, and to nobody else.
///
/// **This machine only, always** (#287). Not "this machine and the members an
/// operator listed", which is what it was until the locality rule landed, and not
/// "whatever the bind lets through".
///
/// The bind is not the policy and never was. A tier reachable only over loopback is
/// closed by accident rather than by decision, and the accident evaporates the moment
/// somebody widens `--listen-node` -- and, since #290, the moment they stop being
/// separately bindable at all: the cache and scheduler verbs now share one listener,
/// which binds the wildcard on any node that schedules. Making locality a property of
/// the **verb** is what survived that merge; "it is only bound to loopback" did not,
/// and this is the layer that now carries the whole rule rather than half of it.
///
/// Membership is *not* consulted here, and its absence is the fix. A member is a
/// machine that may spend this node's CPU and be leased its slots; it is not a
/// machine that may read this node's whole build output,
/// which is what a cache tier is. Those were one list answering two questions, and
/// the second answer was wrong: a peer could `FETCH` every object this machine had
/// ever compiled. The compile surface still asks membership, because "may you run a
/// job here" is the question membership is actually about -- and since #290's second
/// half it asks it on this same listener, which is precisely why the two answers have
/// to follow the verb rather than the port.
///
/// Deliberately *stricter* than `fastcached`'s own cache, which serves non-members on
/// purpose. That one is shared infrastructure somebody operates; this is a
/// developer's private tier, and the two are different things that happen to speak
/// one protocol.
class CacheResponder final: public IFrameResponder
{
  public:
    /// @param proxy Answers each request; must outlive this.
    /// @param locality Decides whether a caller is on this machine; must outlive this.
    /// @param metrics Where a refusal is counted.
    CacheResponder(CacheProxy& proxy, ILocalityOracle const& locality, IMetricsSink& metrics) noexcept:
        _proxy { proxy },
        _locality { locality },
        _metrics { metrics }
    {
    }

    /// @copydoc IFrameResponder::Answer
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override
    {
        // Refused as a *reply*, never by closing: a client that cannot tell a policy
        // refusal from a dead host retries forever and reports a flaky network, which
        // is the failure the declared frame length exists to make avoidable.
        //
        // Answered before any suspension, deliberately: a caller that is not on this
        // machine must not be able to make this node dial its upstream.
        //
        // `NotAMember` rather than a code of its own, and that is a decision about
        // the client rather than about the wording. `fastcache-cc` reads a FETCH
        // outcome as "is this daemon worth a second command", steps over the refusal
        // and compiles; a new code would be an unknown one to every launcher already
        // deployed, and an unknown refusal is the one shape that has cost this tree a
        // permanent 0% hit rate before. The sentence carries what changed.
        // The verb, read back out of the frame this call was handed. `Answer` is
        // reachable directly -- that is why this gate exists here as well as at the
        // door -- so it cannot take the endpoint's word for the opcode. A frame too
        // short to carry a header cannot name a verb, and `0xFF` is unassigned, so it
        // asks about a verb no policy admits rather than about a verb it guessed.
        auto const decodedHeader = CompileCacheWire::DecodeRequestHeader(frame);
        auto const opRaw = decodedHeader.has_value() ? decodedHeader->opRaw : std::uint8_t { 0xFF };
        if (auto refusal = RefusePeer(peer, opRaw); refusal.has_value())
            co_return *std::move(refusal);
        co_return co_await _proxy.Answer(frame);
    }

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// Locality, which is decided from the peer's host alone -- so the transport asks
    /// it before it reads a payload, and a caller this tier will not serve cannot
    /// charge the surface's byte budget on the way to being refused (#377).
    ///
    /// **This is the one implementation of the rule**, and `Answer` above calls it
    /// rather than repeating it, so the early refusal and the authoritative one are
    /// the same code and the counter moves exactly once per refused request whichever
    /// path reached it.
    ///
    /// **A proven identity widens nothing here, and that is the fix rather than an
    /// oversight** (#287, #1428, #178). This tier serves THIS MACHINE, always: locality is a
    /// property of the verb, and a machine the fleet admitted is still not this one. The proof
    /// establishes membership, and membership is the list this surface deliberately does not
    /// consult -- a member may spend this node's CPU, and it may not read every object
    /// this machine has ever compiled. So the identity's `proven` is not read, and a proven peer
    /// is refused exactly as it is today.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t /*opRaw*/) const override
    {
        if (_locality.IsThisMachine(peer.host))
            return std::nullopt;
        return Cc::Refuse(_metrics,
                          { .code = CompileCacheWire::ErrorCode::NotAMember,
                            .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal },
                          "this node serves its cache to its own machine only");
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`: AUTH is the Session family's; this surface is never routed one.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return NotTheSessionSurface();
    }

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// **The frame ceiling is counted here and nowhere else** (#491). Which arm each
    /// decision takes, and the argument for it, is
    /// `Detail::CachePrePayloadPolicy` -- one place, beside the other arm's table,
    /// rather than a `switch` in this method where "no counter" and "no case" would
    /// read the same.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t /*opRaw*/,
                                                      std::string_view detail) const override
    {
        return Detail::AnswerCacheRefusal(
            _metrics, CompileCacheWire::ErrorCodeFor(decision), Detail::CachePrePayloadPolicy(decision), detail);
    }

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// **The byte budget is counted here** (#491), because it is the one a node with a
    /// cache tier actually reaches: the endpoint's in-flight ceiling folds to the
    /// largest owner's, which is this cache's, so a `STORE` is what runs into it.
    /// `Detail::CacheEndpointRefusals` carries that and the two credential arms, which
    /// `MergedResponder` routes to the session component and this surface therefore
    /// never sees.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t /*opRaw*/,
                                                              std::string_view detail) const override
    {
        return Detail::AnswerCacheRefusal(_metrics,
                                          ErrorCodeFor(refusal),
                                          Detail::CacheEndpointRefusals[static_cast<std::size_t>(refusal)].policy,
                                          detail);
    }

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// A round trip, and it is one even though answering may dial an upstream: that
    /// dial has a ceiling of its own well inside this, and a cache exchange that has
    /// not finished in five seconds has already lost to compiling locally -- which is
    /// what the launcher does the moment this surface stops being worth a second
    /// command.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// Megabytes, because a STORE carries a whole object file.
    ///
    /// Matches the daemon's own default value ceiling: a node that refused what the
    /// shared cache would accept would silently stop caching this machine's largest
    /// translation units, which are exactly the ones worth caching.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return 256ULL * 1024ULL * 1024ULL;
    }

    /// Hundreds, and NOT eight, which is what it was while a connection was a request.
    ///
    /// Eight was the right number of object files to have in flight at once and the
    /// wrong number of connections to allow: a wide build has a launcher per job, and
    /// once a connection outlives its request (#176), sizing this for the expensive
    /// thing would have made the ninth `fastcache-cc` on a `-j16` build wait for a
    /// slot rather than for a byte budget. Worse on an open port -- eight attached
    /// peers sending almost nothing would have closed the surface to everyone else.
    ///
    /// What bounds the expensive thing is `MaxInFlightBytes()` below, which is worth
    /// about one object file on purpose and refuses with a reply rather than a close.
    /// So this bounds descriptors, and is sized for descriptors.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 256;
    }

    /// 256 MiB across all of them, which is deliberately ONE request's worth.
    ///
    /// So the common case -- a handful of ordinary objects of a few megabytes -- runs
    /// fully in parallel, while a single 256 MiB monster cannot be joined at all.
    /// That keeps this endpoint's peak footprint where the serialized loop used to
    /// hold it, without reintroducing the serialization -- and since the connection
    /// cap above stopped being a request cap, this is the only thing that does.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return 256ULL * 1024ULL * 1024ULL;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No, and this is the surface the endpoint's budget was SIZED for: a cache
    /// answer is a lookup, or at worst one upstream round trip with a multi-second
    /// ceiling. It holds no accounting of its own, so releasing here would leave the
    /// object file it is buffering counted by nothing at all.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// None, and the reason is the same one that sizes this surface's window: a cache
    /// answer is a lookup or one bounded upstream round trip, so the interval in which
    /// a client could disappear unnoticed is the interval before the next statement.
    ///
    /// It also keeps the connection-lifetime concession off this surface entirely. A
    /// watch that does not fire ends the connection, and this is the surface where a
    /// peer genuinely does stay attached across requests (#176) -- so answering
    /// anything else here would trade a long-lived cache connection for a disconnect
    /// notice nobody needs.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// None. A cache answer is a lookup or one bounded upstream round trip, and its
    /// client bounds the exchange by a round trip to match -- so there is no silence
    /// here long enough to be worth reporting on, and `Status::Progress` is not even
    /// legal on these verbs (`CompileCacheWire::OpTable`, which asserts it).
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Not a stream**: a cache exchange is one round trip, and live stats for this machine are the live-stats
    /// component's subscription, not a tier's.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None, and that is the whole reason the proof verbs are a family of their own.** A proof
    /// is judged against the roster, which a scheduler does not own; a scheduler verifying one
    /// would also make a pure worker -- an ordinary deployment -- unprovable.
    ///
    /// What this surface does with a proof is READ it: `RefusePeer` folds it into the membership
    /// answer, which is where a proven key holder is admitted.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    CacheProxy& _proxy;
    ILocalityOracle const& _locality;
    IMetricsSink& _metrics;
};

/// Serves several verb families on one `0xFC` listener.
///
/// The node used to open a listener per family, and the listener was then the policy:
/// a frame that arrived on the cache port was a cache frame, so "which component
/// answers", "who is admitted", "is a credential required" and "which counter does a
/// refusal move" were all answered by the port it came in on. One port cannot answer
/// any of those, so each becomes a lookup: `CompileCacheWire::FamilyOf` names the
/// family, and this picks the component that owns it
/// ([#290](https://github.com/LASTRADA-Software/fastcached/issues/290)).
///
/// **It is a router and nothing else.** Every policy stays in the component it
/// belonged to before the merge -- the cache's locality rule, the scheduler's
/// membership gate and credential -- and this adds none of its own. That is what
/// keeps the merge a topology change: the answers do not move, only the question of
/// which component is asked.
///
/// ## A family with no component
///
/// Legitimate and common: a node with no cache tier still serves the scheduler and its
/// own compiles. Those verbs are refused
/// `UnimplementedVerb`, which is the honest code -- this endpoint really does not
/// implement them -- and it is the one refusal `Cc::CacheProtocol` steps over rather
/// than treating as fatal.
///
/// Note the asymmetry with `CacheProxy`'s own refusals, which stay
/// `DispatchNotPermitted`: those name a verb served on ANOTHER port of the same node,
/// and telling a client "unknown opcode" there says this daemon is too old when it is
/// in fact merely configured differently. Here there is no other port, so the two
/// facts coincide.
/// What a node has to route to, one field per verb family.
///
/// **A record rather than four pointer parameters**, and the fourth is what forced it:
/// four adjacent `IFrameResponder*` arguments are four things a call site can silently
/// transpose, and the compiler cannot tell them apart. `clang-tidy` says so out loud
/// (`bugprone-easily-swappable-parameters`), and it is right -- a transposed pair here
/// routes every cache verb to the scheduler, which answers *served nowhere* for traffic
/// the node is holding a tier for.
///
/// Designated initialisers at the call sites mean the NAME travels with the pointer, so
/// a fifth component is a field somebody adds rather than an argument nobody can tell
/// from its neighbours. Same reasoning as `NodeComponents`, one layer over.
struct SurfaceComponents
{
    /// Answers the cache verbs, or nullptr when this node holds no tier.
    IFrameResponder* cache { nullptr };

    /// Answers the scheduler verbs, or nullptr when this node does not schedule.
    IFrameResponder* scheduler { nullptr };

    /// Answers the compile verbs, or nullptr when this node runs no worker.
    IFrameResponder* compile { nullptr };

    /// Answers the operator verbs -- what this node is, and its counters.
    ///
    /// Unlike the other three this is **never null on a built node**: the others name
    /// components a node may not run, and these verbs must not depend on which of them
    /// it runs.
    IFrameResponder* node { nullptr };

    /// Answers the enrollment verbs, or nullptr when this node runs no consensus.
    ///
    /// Null is the ORDINARY state and the important one: a node with no cluster has
    /// nothing to enrol anybody into, so the whole family is then refused at the door
    /// with the same sentence every unserved family gets. A window that could never
    /// admit anybody should not be openable, which is why this is a component a node
    /// may not run rather than a surface that is always present and always refuses.
    IFrameResponder* enrollment { nullptr };

    /// Answers `Subscribe`: live stats as a stream rather than a poll (#1399).
    ///
    /// Like `node`, **never null on a built node**, and for its reason: watching a node must not
    /// depend on which components it runs. What a node without a scheduler cannot serve is the
    /// FLEET subject, and that is the component's refusal to make per subject, not a missing
    /// family.
    IFrameResponder* live { nullptr };

    /// Answers `FleetText`: the fleet document, read once (#1391).
    ///
    /// Like `live`, **never null on a built node**: a node without a scheduler has no fleet, and
    /// saying so -- and where the fleet is served instead -- is this component's answer to give,
    /// not a family missing at the door.
    IFrameResponder* fleet { nullptr };

    /// Verifies which machine a caller is, or nullptr when this node runs no consensus (#178).
    ///
    /// Like `enrollment`, null is an ORDINARY state: a proof is judged against the cluster's
    /// applied roster, which only a consensus member holds, so every other node refuses the family
    /// at the door -- `NoCluster` rather than `UnimplementedVerb`, because a caller told the latter
    /// goes off to upgrade a node that is already current.
    IFrameResponder* nodeProof { nullptr };

    /// Answers `FleetSummary`: which fleet this node is in, signed over the asker's nonce.
    ///
    /// Like `node`, **never null on a built node**: a joiner probing a seed must be answered
    /// whatever the seed runs. A node with no fleet to offer says so in the component's own
    /// refusal (`NoCluster`), not as a family missing at the door.
    IFrameResponder* formation { nullptr };

    /// Answers the `Session` family: `AUTH`, and what a verified ticket establishes.
    ///
    /// Like `node`, **never null on a built node**: the node checks no password, and which
    /// machine a ticket speaks for is a question every node answers, whatever it runs.
    IFrameResponder* session { nullptr };

    /// Answers the fleet's shared cache: `SharedFetch` and `SharedStore`.
    ///
    /// Like `node`, **never null on a built node**: a node the `shared-cache` setting does not
    /// name answers an admitted caller `NotSharedCache`, so the family is never unrouted and a node
    /// named at runtime needs no component it did not already have. Null only in a test that
    /// routes nothing there.
    IFrameResponder* sharedCache { nullptr };
};

/// Whether a verb family's owner is there on a built node. In-process only, never
/// transmitted or persisted, so its order is free.
enum class FamilyPresence : std::uint8_t
{
    /// Not a classification. **The one explicit value in this private enum, and it is
    /// load-bearing:** a row that does not state the column is value-initialised to zero,
    /// so zero must mean unclassified for `EveryFamilyIsClassified` to refuse it rather
    /// than let it take the first real answer. No other enumerator carries a value.
    Unstated = 0,
    NoOwner,              ///< No component answers the family on any node.
    WhenItsComponentRuns, ///< Null on a node that does not run the family's component.
    /// Never null on a built node: the family does not depend on WHICH components a node
    /// runs. These owners coexist on every port, so their connection allowances ADD.
    OnEveryBuiltNode,
};

/// Whether the merged listener reads a family owner's session ceilings. In-process only,
/// never transmitted or persisted, so its order is free.
enum class SessionCeilings : std::uint8_t
{
    Unstated = 0, ///< Not a classification. Explicitly zero for `FamilyPresence::Unstated`'s reason.
    Folded,       ///< Its three ceilings take part in `MergedResponder`'s fold.
    NotRead,      ///< The surface never consults them; the row says why.
};

/// One verb family's route on the merged `0xFC` listener.
struct FamilyRoute
{
    CompileCacheWire::VerbFamily family;         ///< The family this row describes.
    IFrameResponder* SurfaceComponents::* owner; ///< The member that answers it, or null for none.
    FamilyPresence presence;                     ///< Whether that owner exists on a built node.
    SessionCeilings ceilings;                    ///< Whether the surface reads its ceilings.
    std::string_view component;                  ///< The owner's member name, for a refusal to say; empty for none.
};

/// **The merged listener's one routing table** (#206): which component answers each verb
/// family, whether that component is on every built node, and whether its ceilings are the
/// surface's. `MergedResponder` routes every frame through it, `AnswersAnyFamily` decides
/// the bind from it, and the ceiling fold and the connection sum iterate its columns --
/// so no one of them can keep a list of members that the next family is missing from.
inline constexpr EnumTable<CompileCacheWire::VerbFamily, FamilyRoute> FamilyRoutes { {
    { .family = CompileCacheWire::VerbFamily::Unset,
      .owner = nullptr,
      .presence = FamilyPresence::NoOwner,
      .ceilings = SessionCeilings::NotRead,
      .component = "" },
    // `Session` is its own component's, on every built node: the node checks no password, and
    // what an `AUTH` can still establish -- which machine a ticket speaks for -- must not
    // depend on which components a node runs. Its ceilings are NOT read: `AUTH` is bounded
    // by its own `OpTable` row, and every other owner on the port has larger ones.
    { .family = CompileCacheWire::VerbFamily::Session,
      .owner = &SurfaceComponents::session,
      .presence = FamilyPresence::OnEveryBuiltNode,
      .ceilings = SessionCeilings::NotRead,
      .component = "session" },
    { .family = CompileCacheWire::VerbFamily::Cache,
      .owner = &SurfaceComponents::cache,
      .presence = FamilyPresence::WhenItsComponentRuns,
      .ceilings = SessionCeilings::Folded,
      .component = "cache" },
    { .family = CompileCacheWire::VerbFamily::Scheduler,
      .owner = &SurfaceComponents::scheduler,
      .presence = FamilyPresence::WhenItsComponentRuns,
      .ceilings = SessionCeilings::Folded,
      .component = "scheduler" },
    { .family = CompileCacheWire::VerbFamily::Compile,
      .owner = &SurfaceComponents::compile,
      .presence = FamilyPresence::WhenItsComponentRuns,
      .ceilings = SessionCeilings::Folded,
      .component = "compile" },
    // *What do you serve?* must work on whatever a node runs, so this owner is never null
    // on a built node the way the component families' legitimately are.
    { .family = CompileCacheWire::VerbFamily::Node,
      .owner = &SurfaceComponents::node,
      .presence = FamilyPresence::OnEveryBuiltNode,
      .ceilings = SessionCeilings::Folded,
      .component = "node" },
    // Legitimately null, and on most deployments it is: a node that runs no consensus has no
    // cluster to let anybody into, so the family is refused at the door with the sentence
    // every unserved family gets. Its ceilings are NOT read, and `EnrollmentResponder` says
    // why at each of them: it exists only beside a scheduler, whose ceilings are larger, and
    // folding it in would make a later raise of its number widen every surface on the port.
    { .family = CompileCacheWire::VerbFamily::Enrollment,
      .owner = &SurfaceComponents::enrollment,
      .presence = FamilyPresence::WhenItsComponentRuns,
      .ceilings = SessionCeilings::NotRead,
      .component = "enrollment" },
    // For `Node`'s reason: a watcher asks what a node is doing whatever it runs.
    { .family = CompileCacheWire::VerbFamily::Live,
      .owner = &SurfaceComponents::live,
      .presence = FamilyPresence::OnEveryBuiltNode,
      .ceilings = SessionCeilings::Folded,
      .component = "live" },
    // For `Live`'s reason: a reader pointed at a worker is told where the fleet is served
    // rather than that nothing here speaks the verb.
    { .family = CompileCacheWire::VerbFamily::Fleet,
      .owner = &SurfaceComponents::fleet,
      .presence = FamilyPresence::OnEveryBuiltNode,
      .ceilings = SessionCeilings::Folded,
      .component = "fleet" },
    // Legitimately null: a node running no consensus holds no roster to verify a proof
    // against. Its ceilings are NOT read, for `Enrollment`'s reason -- the fold is a MAXIMUM
    // over connections and a SUM over every-node owners, so folding a small number in would
    // change nothing today and would put a later raise of it in front of every surface on the
    // port. `Node`, `Live` and `Fleet` are every-node owners, so the fold is never empty.
    { .family = CompileCacheWire::VerbFamily::NodeProof,
      .owner = &SurfaceComponents::nodeProof,
      .presence = FamilyPresence::WhenItsComponentRuns,
      .ceilings = SessionCeilings::NotRead,
      .component = "nodeProof" },
    // For `Node`'s reason: a joiner asks a seed which fleet it is in whatever the seed runs, and a
    // seed with none to offer says so itself. Its ceilings are NOT read, for `Enrollment`'s
    // reason: the verb carries one nonce under its own `OpTable` ceiling, and a ceiling folded in
    // would widen nothing today and every surface on the port after a later raise. Its connection
    // allowance ADDS to the other every-node owners', since they coexist on every port.
    { .family = CompileCacheWire::VerbFamily::Formation,
      .owner = &SurfaceComponents::formation,
      .presence = FamilyPresence::OnEveryBuiltNode,
      .ceilings = SessionCeilings::NotRead,
      .component = "formation" },
    // The fleet's shared cache, on every built node: a node the setting does not name refuses
    // an admitted caller `NotSharedCache` itself rather than leaving the family unrouted. Its
    // ceilings ARE read, because any node may be named at runtime and must then take a STORE of
    // an object's size -- and being an every-node owner, its connection allowance ADDS.
    { .family = CompileCacheWire::VerbFamily::SharedCache,
      .owner = &SurfaceComponents::sharedCache,
      .presence = FamilyPresence::OnEveryBuiltNode,
      .ceilings = SessionCeilings::Folded,
      .component = "sharedCache" },
} };

static_assert(RowsInEnumeratorOrder(FamilyRoutes, &FamilyRoute::family),
              "one FamilyRoutes row per VerbFamily, in enumerator order");

/// Whether every family states both columns, and states them consistently.
///
/// A new `VerbFamily` gets a row or `RowsInEnumeratorOrder` fails; this is what makes that
/// row SAY whether its owner is on every node and whether its ceilings are read, rather
/// than inherit whichever answer a zero spells. An owner that is null exactly when the row
/// says there is none, and no two every-node rows naming one member, because the connection
/// sum would count that owner twice.
/// @return True when the table is classified.
[[nodiscard]] consteval bool EveryFamilyIsClassified() noexcept
{
    auto const stated = std::ranges::all_of(FamilyRoutes, [](FamilyRoute const& row) {
        return row.presence != FamilyPresence::Unstated && row.ceilings != SessionCeilings::Unstated
               && (row.owner == nullptr) == (row.presence == FamilyPresence::NoOwner);
    });
    auto const distinctEveryNodeOwners = std::ranges::all_of(FamilyRoutes, [](FamilyRoute const& row) {
        return row.presence != FamilyPresence::OnEveryBuiltNode
               || std::ranges::count_if(FamilyRoutes, [&row](FamilyRoute const& other) {
                      return other.presence == FamilyPresence::OnEveryBuiltNode && other.owner == row.owner;
                  }) == 1;
    });
    return stated && distinctEveryNodeOwners;
}

static_assert(EveryFamilyIsClassified(),
              "every FamilyRoutes row states its presence and its ceilings, and every-node owners are distinct");

static_assert(std::ranges::all_of(FamilyRoutes,
                                  [](FamilyRoute const& row) { return row.component.empty() == (row.owner == nullptr); }),
              "a family with an owner names that owner's member, and one with none names nothing");

/// The first family every built node answers whose owner @p components leaves null, or null.
///
/// **What makes "never null on a built node" true rather than stated** (`SurfaceComponents`): a
/// node that forgot to route one of these families answers it `UnimplementedVerb`, which a client
/// reads as *this node is too old*, and nothing else would notice. Asked of the table's
/// `OnEveryBuiltNode` column, so the next such family is checked without an edit here.
/// @param components What the listener would route to.
/// @return The route whose owner is missing, or nullptr when every one is present.
[[nodiscard]] constexpr FamilyRoute const* MissingEveryNodeOwner(SurfaceComponents const& components) noexcept
{
    for (auto const& row: FamilyRoutes)
        if (row.presence == FamilyPresence::OnEveryBuiltNode && components.*row.owner == nullptr)
            return &row;
    return nullptr;
}

/// The component of @p components that answers @p family, or nullptr when this node serves
/// it nowhere. `FamilyRoutes` is the table; this reads it.
/// @param components What the listener routes to.
/// @param family The family a verb belongs to.
/// @return The owner, or nullptr.
[[nodiscard]] constexpr IFrameResponder* FamilyOwner(SurfaceComponents const& components,
                                                     CompileCacheWire::VerbFamily family) noexcept
{
    auto const index = static_cast<std::size_t>(family);
    if (index >= FamilyRoutes.size())
        return nullptr;
    auto const& row = FamilyRoutes[index];
    return row.owner == nullptr ? nullptr : components.*row.owner;
}

/// Whether @p components give the node's `0xFC` listener anything to answer.
///
/// **Every family counts, the operator verbs and live stats included** (#206). A node
/// running consensus and nothing else -- no worker, no cache tier, no scheduler -- is a
/// legitimate member, and `--node-status` and a live subscription are how it is watched
/// without an admin surface; a predicate reading only the cache, scheduler and compile
/// components bound no port for it while `--print-surfaces` named one. The operator
/// families are never null on a built node, so on every configuration the startup table
/// accepts this answers yes and the ROW decides, which is what `--print-surfaces` reads.
///
/// Asked of the VERBS through `FamilyRoutes` rather than of a list of members: a list is
/// exact about the families it names and silent about the next one to arrive.
/// @param components What the listener would route to.
/// @return True when some verb has a component to answer it.
[[nodiscard]] constexpr bool AnswersAnyFamily(SurfaceComponents const& components) noexcept
{
    return std::ranges::any_of(CompileCacheWire::OpTable,
                               [&components](auto const& row) { return FamilyOwner(components, row.family) != nullptr; });
}

class MergedResponder final: public IFrameResponder
{
  public:
    /// @param components What to route to; every non-null member must outlive this.
    explicit MergedResponder(SurfaceComponents const& components) noexcept:
        _components { components }
    {
    }

    /// The component that owns @p opRaw, or nullptr when this node serves it nowhere.
    ///
    /// `FamilyRoutes` is the table; this reads the verb's family off the header byte.
    ///
    /// `Compile` is a row there like any other, and that was the second half of #290 --
    /// but the routing was never the work. It is served by `CompileResponder`, which
    /// hops onto an executor before it compiles and back onto the reactor before it
    /// answers: a compile blocks for seconds, and neither running it on this reactor
    /// (#213) nor returning its reply off that reactor is visible at any call site.
    /// The worker's dedicated port is gone (#290 stage 3), so this is not an additional
    /// door onto the worker -- it is the only one, and the same policy and slot
    /// accounting reach it here.
    ///
    /// @param opRaw The third header byte, as received.
    /// @return The owner, or nullptr.
    [[nodiscard]] IFrameResponder* OwnerOf(std::uint8_t opRaw) const noexcept
    {
        return FamilyOwner(_components, CompileCacheWire::FamilyOf(opRaw));
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Reachable directly as well as through the endpoint, so it decodes the header
    /// itself rather than taking anybody's word for the verb -- the same reason
    /// `CacheResponder::Answer` re-asks its own gate.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override
    {
        auto const header = CompileCacheWire::DecodeRequestHeader(frame);
        if (!header.has_value())
            // Empty is CLOSE, and it is the right answer to exactly this: a frame whose
            // header will not decode is not this protocol, which is the one condition
            // `CacheProxy::Answer` also closes on. Every other refusal is a reply.
            co_return std::vector<std::byte> {};

        auto* const owner = OwnerOf(header->opRaw);
        if (owner == nullptr)
            co_return UnservedReply(header->opRaw);
        co_return co_await owner->Answer(frame, std::move(peer));
    }

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// A verb nobody serves is refused here, at the door, before a payload is read --
    /// which is what keeps an unserved verb from costing this surface a buffer.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override
    {
        auto const* const owner = OwnerOf(opRaw);
        if (owner == nullptr)
            return UnservedReply(opRaw);
        return owner->RefusePeer(peer, opRaw);
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// The session component's, because `AUTH` is a `Session` verb. It is on every built node;
    /// a composition that names none has nothing to establish, and answers so.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> payload) const override
    {
        if (_components.session == nullptr)
            return NotTheSessionSurface();
        return _components.session->CheckCredential(payload);
    }

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// Routed for the COUNTER. The wording is the same either way, but a cache STORE
    /// that overran its ceiling counted against the scheduler names the wrong
    /// subsystem, and naming the subsystem is what these counters are read for.
    /// **The unowned arm answers `UnservedReply()`, not the decision's own code**, and
    /// it is reachable: the endpoint weighs its surface-wide frame ceiling BEFORE it
    /// asks `RefusePeer`, so a 24-byte header naming a verb nothing here serves and
    /// declaring a gigabyte arrives at this arm. What that peer needs told is that the
    /// verb is served nowhere on this node -- "too large" would send them to shrink a
    /// frame that was never going to be answered -- and it is the same sentence every
    /// other route to an unowned verb gives.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override
    {
        auto const* const owner = OwnerOf(opRaw);
        if (owner == nullptr)
            return UnservedReply(opRaw);
        return owner->RefusalReply(decision, opRaw, detail);
    }

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// Routed for the counter, exactly as `RefusalReply` is and for the same reason:
    /// the wording is the same either way, and a cache STORE that overran the byte
    /// budget counted against the scheduler names the wrong subsystem.
    ///
    /// **The verb is NOT always verified here.** On a plain connection the endpoint
    /// decodes the header and routes an unowned verb to `UnservedReply` before any
    /// budget is asked. A SEALED frame is refused over budget from a header whose tag
    /// was never read (`SealFault::OverBudget`), so its verb is a byte nobody vouched
    /// for, and it may name anything.
    ///
    /// **So an unowned verb refused over budget is answered by the owner whose
    /// budget it is**, and counted on that owner's busy row. The refusal is certain and
    /// the verb is not: the budget that ran out is the one `MaxInFlightBytes` folded to,
    /// which is `BudgetOwner()`'s, and the busy answer is true whatever the frame
    /// would have asked. Answered `UnservedReply` instead, it moved no counter -- an
    /// injector declaring a large length on an unowned verb would have turned a counted
    /// broken seal into a refusal nothing records. The row that moves was still chosen
    /// by an unverified byte (owned verb: its owner's; unowned: the budget's), which is
    /// observability rather than safety: the connection ends either way.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override
    {
        auto const* owner = OwnerOf(opRaw);
        if (owner == nullptr && refusal == EndpointRefusal::InFlightBudget)
            owner = BudgetOwner();
        if (owner == nullptr)
            return UnservedReply(opRaw);
        return owner->EndpointRefusalReply(refusal, opRaw, detail);
    }

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// **Routed to the owner, and deliberately NOT the largest.** The three ceilings
    /// below fold with `Largest` because #284 made the payload cap a property of the
    /// verb, so a generous session cap cannot make a scheduler verb generous. There is
    /// no such column for time: a surface-wide maximum would hand every cache and
    /// scheduler verb the compile window, which is the slow-loris property given away
    /// to buy nothing.
    ///
    /// A verb nobody owns takes the endpoint's own header window. It is unreachable --
    /// `RefusePeer` has already refused it -- and the short answer is the safe one for
    /// a question asked about a peer this surface will not serve.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t opRaw) const noexcept override
    {
        auto const* const owner = OwnerOf(opRaw);
        return owner == nullptr ? FrameServer::HeaderTimeout : owner->RequestTimeout(opRaw);
    }

    /// @copydoc IFrameResponder::MaxRequestBytes
    ///
    /// The largest of the owners', which is safe only because #284 made the ceiling a
    /// property of the VERB: this is the session cap, and the session cap governs
    /// exactly the three payload-bearing verbs. Every scheduler verb declares
    /// `BoundedTo(MaxControlPayload)` in the wire table and stays bounded in kilobytes
    /// on a surface whose session cap is the cache's 256 MiB. Without that column this
    /// number could not exist, which is why #284 was a blocker rather than a nicety.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return Largest(&IFrameResponder::MaxRequestBytes);
    }

    /// @copydoc IFrameResponder::MaxOpenConnections
    ///
    /// The largest, not the sum and not the smallest. This one surface now carries both
    /// populations -- every local `fastcache-cc` and every peer in the fleet -- and the
    /// smaller ceiling would close the port to one population because the other exists.
    ///
    /// **Except the operator families, whose allowances ADD** (#206). They are on every
    /// node and they coexist: an operator's `--node-status` or `fleet` read must still
    /// connect while every live subscription is held. On a node running only consensus
    /// their largest alone was the live cap, so the port refused new connections exactly
    /// when the subscriptions were full and the live responder's own counted refusal never
    /// fired. The fleet's shared cache is an every-node owner too, sized for one kept session
    /// per fleet node, so the sum can now pass a component owner's ceiling -- and whichever
    /// is larger is the listener's.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        std::size_t coexisting = 0;
        for (auto const& row: FamilyRoutes)
            if (auto const* const owner = row.owner == nullptr ? nullptr : _components.*row.owner;
                row.presence == FamilyPresence::OnEveryBuiltNode && owner != nullptr)
                coexisting += owner->MaxOpenConnections();
        return std::max(Largest(&IFrameResponder::MaxOpenConnections), coexisting);
    }

    /// @copydoc IFrameResponder::MaxInFlightBytes
    ///
    /// The largest, and in practice the cache's: a scheduler verb's payload is
    /// kilobytes, so the budget that matters is the one sized for object files.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return Largest(&IFrameResponder::MaxInFlightBytes);
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// **Routed to the owner, and emphatically NOT folded**, which is the same split
    /// `RequestTimeout` makes: the three ceilings fold with `Largest` because they
    /// are properties of the surface, and this is a property of the VERB. `Largest`
    /// has no meaning here, and either fold would be a defect -- an `any_of` would
    /// release the endpoint's reservation for cache and scheduler verbs the moment a
    /// worker is configured, leaving their buffers counted by nothing; an `all_of`
    /// would double-charge every compile the moment a cache tier is, which is #448
    /// with an extra condition on it.
    ///
    /// A verb nobody owns answers `false` and keeps the endpoint's accounting. It is
    /// unreachable -- `RefusePeer` has already refused it -- and `false` is the
    /// answer that accounts for a buffer rather than the one that assumes somebody
    /// else will.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t opRaw) const noexcept override
    {
        auto const* const owner = OwnerOf(opRaw);
        return owner != nullptr && owner->HoldsOwnByteBudget(opRaw);
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// **Routed to the owner**, for the reason the line above is: this is a property
    /// of the VERB, not of the merged listener, and the counter a watch raises belongs
    /// to the surface whose work was abandoned. Folding would be worse than useless
    /// here -- there is no counter to fold two answers into.
    ///
    /// A verb nobody owns is not watched. It is unreachable, `RefusePeer` having
    /// already refused it, and not-watching is the answer that changes nothing.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t opRaw) const noexcept override
    {
        auto const* const owner = OwnerOf(opRaw);
        return owner != nullptr ? owner->PeerWatchCounter(opRaw) : std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// **Routed to the owner**, for the reason every line above it is: whether a verb is
    /// slow enough to owe its client a liveness signal is a property of the VERB and of
    /// the surface that does its work, never of the listener they happen to share.
    ///
    /// A verb nobody owns is not pulsed. It is unreachable, `RefusePeer` having already
    /// refused it, and not-pulsing is the answer that changes nothing.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t opRaw) const noexcept override
    {
        auto const* const owner = OwnerOf(opRaw);
        return owner != nullptr ? owner->ProgressInterval(opRaw) : std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Routed to the owner**, for the reason every line above it is: whether a verb is a
    /// subscription is a property of the VERB and of the surface that serves it. A verb nobody owns
    /// is not a stream; it is unreachable, `RefusePeer` having already refused it.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t opRaw) noexcept override
    {
        auto* const owner = OwnerOf(opRaw);
        return owner != nullptr ? owner->StreamFor(opRaw) : nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// Routed by FAMILY rather than by verb, because the seam is not per verb -- and that is
    /// what makes the endpoint's handling of the two proof verbs agree with the door by
    /// CONSTRUCTION rather than by two computations kept in step: `RefusePeer` above refuses the
    /// family when `FamilyOwner` is null, and this answers null under exactly the same
    /// condition, from the same `FamilyRoutes` row.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        auto* const owner = FamilyOwner(_components, CompileCacheWire::VerbFamily::NodeProof);
        return owner != nullptr ? owner->NodeProver() : nullptr;
    }

  private:
    /// A verb family this node may run no component for, refused with its own code rather
    /// than `UnimplementedVerb`. See `UnservedReply` for why each row exists.
    struct UnservedFamily
    {
        CompileCacheWire::VerbFamily family; ///< Whose verbs this row answers.
        Cc::UncountedRefusal refusal;        ///< What the client is told, and why nothing rises.
        std::string_view detail;             ///< Words for the operator who sent it.
    };

    /// Every family with an answer of its own; any other unserved family is unimplemented.
    static constexpr auto UnservedFamilies = std::to_array<UnservedFamily>({
        { .family = CompileCacheWire::VerbFamily::Enrollment,
          .refusal = { .code = CompileCacheWire::ErrorCode::NoCluster,
                       .rationale = "what a node without consensus answers every enrolment attempt aimed at it, which "
                                    "is an ordinary misdirection rather than an event; counted, it would bury the scan "
                                    "it would be read for, exactly as the unserved-family answer below would" },
          .detail = "this node runs no consensus, so it belongs to no cluster and there is nothing here to join; ask a "
                    "node that runs consensus -- --node-status names the components a node serves" },
        { .family = CompileCacheWire::VerbFamily::Compile,
          .refusal = { .code = CompileCacheWire::NoCompileWorker::Code,
                       .rationale = "what a node running no worker answers a compile or a cordon aimed at it: nothing "
                                    "leases this node, so each one is a person or a script at the wrong machine, "
                                    "which is a misdirection rather than an event a series should count" },
          .detail = "this endpoint runs no compile worker: it was started with --slots=0, so nothing here compiles "
                    "and there is nothing to cordon; ask a node that runs one -- --node-status names the components "
                    "a node serves" },
        { .family = CompileCacheWire::VerbFamily::NodeProof,
          .refusal = { .code = CompileCacheWire::ErrorCode::NoCluster,
                       .rationale = "what a node running no consensus answers every proof aimed at it: it is no "
                                    "scheduler, so a proof here is a node registering at the wrong machine; counted, "
                                    "one misdirected node would dominate the series that says whether an identity is "
                                    "WRONG somewhere" },
          .detail = "this node runs no consensus, so it holds no roster to prove an identity against and is no "
                    "scheduler of any fleet; admission at this endpoint is decided by your address -- --node-status "
                    "names the components a node serves" },
    });

    // The compile row says `CompileCacheWire::NoCompileWorker`'s fact, which the daemon answers too, so
    // neither endpoint can reword it or move its code without the other (#206).
    static_assert(CompileCacheWire::SaysNoCompileWorker(
        core::findOrNull(UnservedFamilies, CompileCacheWire::VerbFamily::Compile, &UnservedFamily::family)->refusal.code,
        core::findOrNull(UnservedFamilies, CompileCacheWire::VerbFamily::Compile, &UnservedFamily::family)->detail));

    /// What a verb this node serves nowhere is answered with.
    ///
    /// A sentence rather than a bare code, because the operator action differs from
    /// every other refusal here: nothing is misconfigured on the CLIENT's side, and the
    /// node has to be told to hold a tier or to schedule before this verb exists.
    ///
    /// **Deliberately not counted, and that is the hard half of #447 rather than an
    /// omission.** Every other refusal here is an event; this one is an ANSWER that
    /// ordinary, healthy traffic produces continuously. A node runs its components
    /// independently, so a worker with no scheduler refuses every `AUTH` a
    /// `FASTCACHE_TOKEN` launcher sends -- which is per exchange, for a whole build --
    /// and a node with no cache tier refuses every local `FETCH` the same way. Counted,
    /// the series would be dominated by a normal build and a port scan would be
    /// invisible inside it, which is the failure this ticket is about arrived at from
    /// the opposite direction: a signal nothing can be read out of is no better than a
    /// counter that never moves.
    ///
    /// Splitting it into "the ordinary absences" and "the rest" is a real counter and
    /// out of this ticket's scope; #447's own residue is recorded rather than guessed
    /// at. What DID change is that all four routes here -- `Answer`, `RefusePeer`,
    /// `RefusalReply` and `EndpointRefusalReply` -- give one sentence: a peer asking
    /// for a verb served nowhere is told that, rather than being told its frame was
    /// too large and sent to shrink one that was never going to be answered. One arm
    /// is excepted, and `EndpointRefusalReply` says why: an in-flight refusal of a
    /// SEALED frame, whose verb is an unverified byte, is answered busy by the
    /// budget's owner and counted there.
    /// **It takes the VERB, because one unserved family must not answer
    /// `UnimplementedVerb`.** The enrollment family is refused `NoCluster` instead, and
    /// the reason is the rule that *unimplemented is not served elsewhere*: a node
    /// without consensus implements these verbs perfectly well and has no cluster to
    /// let anybody into, so `UnimplementedVerb` -- which the client reads as
    /// `UnknownOpcode` -- told a joiner *the seed is running a build older than this
    /// one*. A joiner pointed at a node that runs no consensus is the commonest operator
    /// mistake, and it produced a confident wrong diagnosis that sends somebody to upgrade
    /// a node that is already current.
    ///
    /// `CompileCacheHandler`'s `RefusedVerbs` table already drew exactly this
    /// distinction for the daemon, with the argument written out beside it -- *"a joiner
    /// told `NoCluster` knows the question does not apply here and goes looking for a
    /// node that runs consensus"* -- and this surface did not carry it across. Same
    /// code, so the two endpoints cannot send a joiner two different remedies for one
    /// condition.
    ///
    /// Taking the verb rather than being duplicated at the four call sites is the point:
    /// `Answer`, `RefusePeer`, `RefusalReply` and `EndpointRefusalReply` all reach an
    /// unowned verb, and a per-family answer chosen at each of them is four places to
    /// forget it. Every refusal here stays UNCOUNTED for the reason above -- a node that
    /// runs no consensus answers this for every enrolment attempt anybody ever points at it.
    ///
    /// **The compile family is the second row, since #206 gave a node the means to run no
    /// worker** (`--slots=0`). Its verbs are not unimplemented there either: `--cordon`
    /// aimed at such a node would otherwise read *this node is too old to know the verb*,
    /// which sends an operator to upgrade a machine that is current. It takes
    /// `DispatchNotPermitted` through `CompileCacheWire::NoCompileWorker`, the one definition the daemon's
    /// cordon and compile rows reach too -- so the two endpoints send one code for one
    /// condition, as the enrollment row does with `NoCluster`.
    /// @param opRaw The third header byte, as received.
    /// @return The encoded refusal.
    [[nodiscard]] static std::vector<std::byte> UnservedReply(std::uint8_t opRaw)
    {
        auto const family = CompileCacheWire::FamilyOf(opRaw);
        if (auto const* const row = core::findOrNull(UnservedFamilies, family, &UnservedFamily::family))
            return Cc::RefuseWithoutCounter(row->refusal, row->detail);

        return Cc::RefuseWithoutCounter({ .code = CompileCacheWire::UnimplementedVerb,
                                          .rationale =
                                              "an ANSWER healthy traffic produces continuously, not an event: a node runs "
                                              "its components independently, so one it does not hold refuses every "
                                              "exchange of a whole build and a port scan would be invisible inside it" },
                                        "this node serves no component for that verb family");
    }

    /// The largest value reported by the owners this folds, for one ceiling.
    ///
    /// One helper rather than three near-identical folds: the three ceilings differ
    /// only in which member function they read, and copy-pasted branches that differ
    /// by a name are what this codebase treats as a defect.
    ///
    /// **It folds every owner whose `FamilyRoutes` row says `SessionCeilings::Folded`**, and
    /// each `NotRead` row says why it is left out -- the table is the list, so none is kept
    /// here. A
    /// per-component connection or byte ceiling has nowhere to live on one listener, one
    /// accept queue and one byte budget (#1338), so a small number here is never a
    /// narrowing, and a larger one widens every surface on the port.
    ///
    /// **The `OnEveryBuiltNode` owners are what keep this from answering 0.** With every
    /// folded owner absent it did, and 0 means different things per ceiling -- no ceiling
    /// at all for `MaxOpenConnections` and `MaxInFlightBytes`, every payload refused for
    /// `MaxRequestBytes`, stated at those three declarations in `FrameEndpoint.hpp`. That was
    /// unreachable while `main.cpp` set `.compile` unconditionally, and #206 reached it: a
    /// `--slots=0` node running only consensus bound this port and closed every connection
    /// it accepted, `--node-status` included. The operator owners are sized as the SMALL ones,
    /// so on any node running a cache, a scheduler or a worker they change no number. **The
    /// fleet's shared cache is the exception**: it is an every-node owner sized for an object,
    /// because any node may be named at runtime, so it moves the request and in-flight ceilings
    /// of a node running neither a cache tier nor a worker -- a scheduler-only one from 64 KiB,
    /// a consensus-only one from the operator owners' -- to 256 MiB, and adds its 128
    /// connections to every node's sum. A sealed frame is charged to that budget before it is
    /// held (`SealedFrameCharge`), so the larger ceiling is not a larger unaccounted buffer.
    /// @param ceiling Which ceiling to read.
    /// @return The largest of the folded owners present; never 0 on a built node.
    [[nodiscard]] std::size_t Largest(std::size_t (IFrameResponder::*ceiling)() const noexcept) const noexcept
    {
        std::size_t out = 0;
        for (auto const& row: FamilyRoutes)
            if (auto const* const owner = row.owner == nullptr ? nullptr : _components.*row.owner;
                row.ceilings == SessionCeilings::Folded && owner != nullptr)
                out = std::max(out, (owner->*ceiling)());
        return out;
    }

    /// The owner whose in-flight budget the endpoint enforces: the folded owner present with the
    /// largest `MaxInFlightBytes`, the first in `FamilyRoutes` order on a tie.
    ///
    /// The same walk as `Largest`, answering WHO rather than how much, because a refusal of that
    /// budget for a verb nobody owns still has to move somebody's row (`EndpointRefusalReply`).
    /// @return That owner; null only when no folded owner is present, which no built node is.
    [[nodiscard]] IFrameResponder const* BudgetOwner() const noexcept
    {
        IFrameResponder const* out = nullptr;
        for (auto const& row: FamilyRoutes)
            if (auto const* const owner = row.owner == nullptr ? nullptr : _components.*row.owner;
                row.ceilings == SessionCeilings::Folded && owner != nullptr
                && (out == nullptr || owner->MaxInFlightBytes() > out->MaxInFlightBytes()))
                out = owner;
        return out;
    }

    SurfaceComponents _components;
};

} // namespace FastCache::Node
