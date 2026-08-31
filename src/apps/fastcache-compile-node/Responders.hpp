// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>

namespace FastCache::Node
{

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
                                                      std::uint8_t /*opRaw*/) const override
    {
        if (decision == CompileCacheWire::PrePayloadDecision::Unauthenticated)
            _metrics.Increment(IMetricsSink::Counter::SchedulerRequestsRefusedUnauthenticated);

        // No detail. What a message could add is which verb the caller failed to
        // reach, and an unauthenticated peer learns nothing from being told that.
        return CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCodeFor(decision), {});
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
/// somebody widens `--listen-cache` -- or, once the cache, scheduler and compile
/// surfaces share one wildcard listener, the moment they stop being separately
/// bindable at all. Making locality a property of the **verb** is what survives that
/// merge; "it is only bound to loopback" does not.
///
/// Membership is *not* consulted here, and its absence is the fix. A
/// `--fleet-member` names a machine that may spend this node's CPU and be leased its
/// slots; it does not name a machine that may read this node's whole build output,
/// which is what a cache tier is. Those were one list answering two questions, and
/// the second answer was wrong: a peer could `FETCH` every object this machine had
/// ever compiled. The compile surface still asks membership, because "may you run a
/// job here" is the question membership is actually about.
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
        _metrics.Increment(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal);
        return CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCode::NotAMember,
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
    /// Nothing to count: this surface requires no credential, so it can never
    /// produce the one refusal that carries a counter. The size and opcode arms are
    /// framing errors and are already visible as such to the peer.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t /*opRaw*/) const override
    {
        return CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCodeFor(decision), {});
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

  private:
    CacheProxy& _proxy;
    ILocalityOracle const& _locality;
    IMetricsSink& _metrics;
};

} // namespace FastCache::Node
