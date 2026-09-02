// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Auth/AuthPolicy.hpp>
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
#include <string_view>
#include <vector>

#include <WorkerProtocol.hpp>

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
        return policy.counter.has_value() == policy.rationale.empty();
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
        EndpointRefusal refusal;     ///< Which endpoint decision this describes.
        CacheRefusalPolicy policy;   ///< What this surface does about it.
    };

    /// Why neither credential arm counts, stated once for the two rows that share it.
    ///
    /// The two enumerators are separate because a client is told different things
    /// about them; the ARGUMENT is one argument about one fact -- which family `AUTH`
    /// belongs to -- so it is one sentence. Written out twice it is two literals that
    /// can drift on any edit with nothing to catch it, which is the "second statement
    /// of one fact" the row type's own documentation is shaped to avoid.
    inline constexpr std::string_view CredentialIsTheSchedulersRationale =
        "AUTH is the Session family, which MergedResponder routes to the scheduler; no credential outcome is ever "
        "decided against this surface";

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
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
        { .refusal = EndpointRefusal::CredentialRejected,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
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
    static_assert(std::ranges::all_of(CacheEndpointRefusals,
                                      [](CacheEndpointRefusal const& row) { return StatesOneClaim(row.policy); }),
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
                return { .counter = std::nullopt,
                         .rationale = "AuthRequired() is false here by decision (#287, #290), and DecidePrePayload "
                                      "yields this only for a surface that requires a credential" };
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
    /// A `std::optional` per row rather than a switch with a silent arm, because one
    /// refusal here deliberately counts NOTHING: the byte budget says this surface is
    /// momentarily full, which the peer retries past, and summed into a credential
    /// series it would make that series unreadable. `nullopt` says so where a missing
    /// `case` would only imply it. `EnumTable` takes its extent from
    /// `EndpointRefusal::Last`, so a fourth enumerator leaves an empty row here rather
    /// than silently borrowing a neighbour's.
    struct SchedulerEndpointRefusal
    {
        EndpointRefusal refusal;                  ///< Which endpoint decision this describes.
        std::optional<Cc::SurfaceRefusal> answer; ///< The row, or nothing where this surface counts none.
    };

    inline constexpr EnumTable<EndpointRefusal, SchedulerEndpointRefusal> SchedulerEndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget, .answer = std::nullopt },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .answer = Cc::SurfaceRefusal { .code = CompileCacheWire::ErrorCode::MalformedFrame,
                                         .counter = IMetricsSink::Counter::SchedulerCredentialsMalformed } },
        { .refusal = EndpointRefusal::CredentialRejected,
          .answer = Cc::SurfaceRefusal { .code = CompileCacheWire::ErrorCode::Unauthenticated,
                                         .counter = IMetricsSink::Counter::SchedulerCredentialsRejected } },
    } };

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
    /// @param policy The credential this surface requires, or nullptr for none.
    ///        Shared rather than referenced because "there is no credential" has to
    ///        be representable, and a null reference is not
    ///        ([#289](https://github.com/LASTRADA-Software/fastcached/issues/289)).
    /// @param metrics Where a refused credential is counted; must outlive this.
    SchedulerResponder(Distributed::SchedulerProtocol& protocol,
                       Distributed::IMembershipOracle const& membership,
                       IMetricsSink& metrics,
                       std::shared_ptr<AuthPolicy const> policy = nullptr) noexcept:
        _protocol { protocol },
        _membership { membership },
        _metrics { metrics },
        _policy { std::move(policy) }
    {
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Never suspends. The scheduler answers from its own tables -- a decision
    /// layer kept pure so every capacity and expiry rule is a `ManualClock` unit
    /// test -- so this is a one-line adapter, and a responder that costs a frame
    /// allocation and no round trip is a legitimate thing to be.
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override
    {
        co_return _protocol.Answer(frame, Context(peer));
    }

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// Membership, which is decided from the peer's host alone -- so the transport
    /// can ask it before it reads a payload, and a stranger cannot make the scheduler
    /// allocate for a frame that was never going to be served (#285).
    ///
    /// Delegated rather than reimplemented: `SchedulerService::RefuseUnlessMember` is
    /// the same function `Gate()` calls, so the early check and the authoritative one
    /// cannot disagree, and the `NotAMember` counter is incremented inside it exactly
    /// once per refused request.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer,
                                                                   std::uint8_t /*opRaw*/) const override
    {
        return _protocol.RefusePeer(Context(peer));
    }

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// **Membership is not a credential.** It answers "is this host one an operator
    /// listed", which a host that is not on the list can satisfy by being on the
    /// network the list was written for -- so it is an anti-leeching rule, and #289
    /// is what makes the scheduler verbs safe on a port that faces one.
    ///
    /// The verb is ignored: this listener serves the scheduler verbs and nothing else,
    /// so the surface-wide answer IS the per-verb one. It stops being so the moment
    /// the surfaces merge (#290), which is why the question now carries the verb.
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _policy != nullptr && _policy->Enabled();
    }

    /// @copydoc IFrameResponder::CheckCredential
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(_policy.get(), payload);
    }

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// The credential refusal is the one this surface counts. It is deliberately the
    /// only counted arm: a size or opcode refusal says the peer is confused, an
    /// unauthenticated one says somebody is reaching for verbs they hold no secret
    /// for, and an operator watching for the second does not want the first summed
    /// into it.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t /*opRaw*/,
                                                      std::string_view detail) const override
    {
        // The caller's wording, which is empty for every decision but the frame
        // ceiling -- and that one names both numbers, because "too large" without the
        // limit tells an operator nothing about a kilobyte cap. Nothing is added here:
        // what a message could say is which verb the caller failed to reach, and an
        // unauthenticated peer learns nothing from being told that.
        if (decision == CompileCacheWire::PrePayloadDecision::Unauthenticated)
            return Cc::Refuse(_metrics,
                              { .code = CompileCacheWire::ErrorCodeFor(decision),
                                .counter = IMetricsSink::Counter::SchedulerRequestsRefusedUnauthenticated },
                              detail);
        return Cc::RefuseWithoutCounter({ .code = CompileCacheWire::ErrorCodeFor(decision),
                                          .rationale =
                                              "a size or opcode refusal says the peer is confused; an operator watching "
                                              "for somebody reaching after verbs they hold no secret for does not want "
                                              "it summed into that series" },
                                        detail);
    }

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// The credential refusals are this surface's, because the credential is: `AUTH`
    /// routes here on the merged listener, so a peer presenting a wrong token is
    /// refused by this component and has to be counted against it.
    ///
    /// **They are two counters and not one**, and neither is
    /// `SchedulerRequestsRefusedUnauthenticated` above. That one counts a peer that
    /// never authenticated -- a member with no token file, which is a configuration
    /// mistake; these count a peer that tried and failed, which is a rotated key or a
    /// probe. A client is told `unauthenticated` for all three and an operator does
    /// three different things, which is the whole argument for a row being the
    /// refusal rather than the code.
    ///
    /// The byte budget is not counted, for the reason the size and opcode arms of
    /// `RefusalReply` are not: it says this surface is momentarily full, which the
    /// peer sees and retries, and summing it into a security series is what makes
    /// that series unreadable. `Detail::SchedulerEndpointRefusals` is where that is
    /// said, and where the two that ARE counted carry their rows.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t /*opRaw*/,
                                                              std::string_view detail) const override
    {
        auto const& row = Detail::SchedulerEndpointRefusals[static_cast<std::size_t>(refusal)].answer;
        if (!row.has_value())
            return Cc::RefuseWithoutCounter({ .code = ErrorCodeFor(refusal),
                                              .rationale =
                                                  "the byte budget says this surface is momentarily full, which the peer "
                                                  "sees and retries; summed into a security series it is what makes that "
                                                  "series unreadable" },
                                            detail);
        return Cc::Refuse(_metrics, *row, detail);
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

  private:
    /// Who is asking, as both the gate and the early refusal need it.
    ///
    /// One place the peer becomes a `CallerContext`, so the classification behind an
    /// early refusal is by construction the classification the verb would have got.
    ///
    /// @param peer The caller's peer host. BORROWED: `CallerContext::peerId` is a
    ///             `std::string_view`, so this must outlive the returned context --
    ///             which is why the parameter is a view over the caller's storage
    ///             rather than a `std::string` this function owns. Taking it by value
    ///             compiles, moves, and returns a view into a parameter destroyed at
    ///             the closing brace.
    [[nodiscard]] Distributed::CallerContext Context(std::string_view peer) const
    {
        return Distributed::CallerContext { .membership = _membership.Classify(peer), .peerId = peer };
    }

    Distributed::SchedulerProtocol& _protocol;
    Distributed::IMembershipOracle const& _membership;
    IMetricsSink& _metrics;
    std::shared_ptr<AuthPolicy const> _policy;
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
/// Membership is *not* consulted here, and its absence is the fix. A
/// `--fleet-member` names a machine that may spend this node's CPU and be leased its
/// slots; it does not name a machine that may read this node's whole build output,
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
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override
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
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer,
                                                                   std::uint8_t /*opRaw*/) const override
    {
        if (_locality.IsThisMachine(peer))
            return std::nullopt;
        return Cc::Refuse(_metrics,
                          { .code = CompileCacheWire::ErrorCode::NotAMember,
                            .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal },
                          "this node serves its cache to its own machine only");
    }

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// **No**, and stated rather than inherited. This surface is already closed to
    /// everyone but this machine (#287), so the peer gate above refuses every caller
    /// a credential could -- and a credential here would have to be readable by every
    /// local build, which makes it one an attacker who is already local can read too.
    ///
    /// The interface is pure virtual precisely so this answer has to be written down:
    /// a default would let a surface added later inherit an open door by saying
    /// nothing, which is the shape this codebase records as reopening a hole by
    /// omission.
    ///
    /// The verb is ignored for the same reason the scheduler's is -- this listener
    /// carries one verb family. Note where the reason above actually attaches, though:
    /// "a credential every local build can read is not a credential" is a fact about
    /// the CACHE VERBS, not about the port they arrive on, so it survives a merge onto
    /// a shared listener and the answer there has to follow the verb rather than the
    /// surface (#290).
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// There is no policy, so this answers `NoPolicy` for every payload: `Ok`, and
    /// nothing verified. That is what keeps a token-configured launcher working
    /// against a node whose cache requires none -- the same reason the daemon answers
    /// AUTH `Ok` when auth is off.
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(nullptr, payload);
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
    /// `MergedResponder` routes to the scheduler and this surface therefore never
    /// sees.
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
class MergedResponder final: public IFrameResponder
{
  public:
    /// @param cache Answers the cache verbs, or nullptr when this node holds no tier.
    /// @param scheduler Answers the scheduler verbs, or nullptr when this node does
    ///        not schedule. Also owns the credential, so `AUTH` goes here.
    /// @param compile Answers the compile verbs, or nullptr when this node runs no
    ///        worker. All three must outlive this.
    MergedResponder(IFrameResponder* cache, IFrameResponder* scheduler, IFrameResponder* compile) noexcept:
        _cache { cache },
        _scheduler { scheduler },
        _compile { compile }
    {
    }

    /// The component that owns @p opRaw, or nullptr when this node serves it nowhere.
    ///
    /// `Session` follows the scheduler, because the credential is the scheduler's: the
    /// cache requires none, so an `AUTH` routed to it would be answered "no policy" and
    /// a peer holding the scheduler's secret could never present it.
    ///
    /// `Compile` is a row here like any other, and that was the second half of #290 --
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
        switch (CompileCacheWire::FamilyOf(opRaw))
        {
            case CompileCacheWire::VerbFamily::Cache:
                return _cache;
            case CompileCacheWire::VerbFamily::Session:
            case CompileCacheWire::VerbFamily::Scheduler:
                return _scheduler;
            case CompileCacheWire::VerbFamily::Compile:
                return _compile;
            case CompileCacheWire::VerbFamily::Unset:
                return nullptr;
        }
        return nullptr;
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Reachable directly as well as through the endpoint, so it decodes the header
    /// itself rather than taking anybody's word for the verb -- the same reason
    /// `CacheResponder::Answer` re-asks its own gate.
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override
    {
        auto const header = CompileCacheWire::DecodeRequestHeader(frame);
        if (!header.has_value())
            // Empty is CLOSE, and it is the right answer to exactly this: a frame whose
            // header will not decode is not this protocol, which is the one condition
            // `CacheProxy::Answer` also closes on. Every other refusal is a reply.
            co_return std::vector<std::byte> {};

        auto* const owner = OwnerOf(header->opRaw);
        if (owner == nullptr)
            co_return UnservedReply();
        co_return co_await owner->Answer(frame, std::move(peer));
    }

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// A verb nobody serves is refused here, at the door, before a payload is read --
    /// which is what keeps an unserved verb from costing this surface a buffer.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer, std::uint8_t opRaw) const override
    {
        auto const* const owner = OwnerOf(opRaw);
        if (owner == nullptr)
            return UnservedReply();
        return owner->RefusePeer(peer, opRaw);
    }

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// The whole reason this question had to take the verb. The two owners answer it
    /// oppositely and both are right: the scheduler requires a credential when one is
    /// configured, the cache requires none because a credential every local build can
    /// read is not a credential. An unowned verb answers `false` and is unreachable
    /// anyway -- `RefusePeer` above has already refused it, and requiring a credential
    /// for a verb nobody serves would tell a stranger that one exists.
    [[nodiscard]] bool AuthRequired(std::uint8_t opRaw) const noexcept override
    {
        auto const* const owner = OwnerOf(opRaw);
        return owner != nullptr && owner->AuthRequired(opRaw);
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// The scheduler's, because the credential is. A node with no scheduler has no
    /// policy to check -- and never reaches this, since `AUTH` is then an unowned verb
    /// refused at the door.
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        if (_scheduler == nullptr)
            return CredentialOutcome::NoPolicy;
        return _scheduler->CheckCredential(payload);
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
            return UnservedReply();
        return owner->RefusalReply(decision, opRaw, detail);
    }

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// Routed for the counter, exactly as `RefusalReply` is and for the same reason:
    /// the wording is the same either way, and a cache STORE that overran the byte
    /// budget counted against the scheduler names the wrong subsystem.
    ///
    /// A verb is always known here -- the endpoint decodes the header before it asks
    /// any of these -- so unlike `MaxRequestBytes` and its two siblings there is
    /// nothing to fold and no surface-wide answer to invent.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override
    {
        auto const* const owner = OwnerOf(opRaw);
        if (owner == nullptr)
            return UnservedReply();
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
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return Largest(&IFrameResponder::MaxOpenConnections);
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

  private:
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
    /// too large and sent to shrink one that was never going to be answered.
    /// @return The encoded refusal.
    [[nodiscard]] static std::vector<std::byte> UnservedReply()
    {
        return Cc::RefuseWithoutCounter({ .code = CompileCacheWire::UnimplementedVerb,
                                          .rationale =
                                              "an ANSWER healthy traffic produces continuously, not an event: a node runs "
                                              "its components independently, so one it does not hold refuses every "
                                              "exchange of a whole build and a port scan would be invisible inside it" },
                                        "this node serves no component for that verb family");
    }

    /// The largest value the present owners report for one ceiling.
    ///
    /// One helper rather than three near-identical folds: the three ceilings differ
    /// only in which member function they read, and copy-pasted branches that differ
    /// by a name are what this codebase treats as a defect.
    /// @param ceiling Which ceiling to read.
    /// @return The largest; 0 when no owner is present.
    [[nodiscard]] std::size_t Largest(std::size_t (IFrameResponder::*ceiling)() const noexcept) const noexcept
    {
        std::size_t out = 0;
        for (auto const* const owner: { _cache, _scheduler, _compile })
            if (owner != nullptr)
                out = std::max(out, (owner->*ceiling)());
        return out;
    }

    IFrameResponder* _cache;
    IFrameResponder* _scheduler;
    IFrameResponder* _compile;
};

} // namespace FastCache::Node
