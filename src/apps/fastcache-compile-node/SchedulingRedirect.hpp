// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"

#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/StateLeaseRoster.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// @file SchedulingRedirect.hpp
/// What a node that runs consensus but no scheduler -- a Raft learner -- answers the fleet's
/// scheduling verbs with: `NotLeader`, naming the leader's `0xFC` endpoint (#1639).
///
/// Without it the merged listener has no owner for `VerbFamily::Scheduler` on such a node and
/// answers `UnimplementedVerb`, which a launcher reads as *this build is too old* -- so a client
/// configured with this machine's own address as its scheduler never reaches the fleet at all,
/// although the node beside it knows exactly where the leader is.

/// Where the fleet's leader answers its scheduling verbs, as this node last learned it.
///
/// A seam rather than a reach into the consensus tier, so the responder reads one string and the
/// tier that learns it can be faked: a case states the leader, never elects one.
class ISchedulingLeaderSource
{
  public:
    virtual ~ISchedulingLeaderSource() = default;

    ISchedulingLeaderSource() = default;
    ISchedulingLeaderSource(ISchedulingLeaderSource const&) = default;
    ISchedulingLeaderSource& operator=(ISchedulingLeaderSource const&) = default;
    ISchedulingLeaderSource(ISchedulingLeaderSource&&) = default;
    ISchedulingLeaderSource& operator=(ISchedulingLeaderSource&&) = default;

    /// The current leader's `0xFC` (scheduling) endpoint -- the member record's, never its Raft
    /// endpoint.
    /// @return The endpoint, or empty when no leader is known.
    [[nodiscard]] virtual std::string LeaderSchedulingEndpoint() const = 0;
};

/// The leader's scheduling endpoint, held for the responder that names it.
///
/// Thread-safe: published from the consensus thread whenever the role observer is told who leads,
/// and read on the I/O reactor once per refused verb -- the shape `AppliedSchedulers` has for the
/// same two threads. It stores what it is given and decides nothing: whether a silent leader is
/// still named is the publisher's question.
class KnownSchedulingLeader final: public ISchedulingLeaderSource
{
  public:
    /// Take @p endpoint as the leader's scheduling endpoint, replacing the previous one.
    /// @param endpoint The endpoint; empty says no leader is known.
    void Publish(std::string_view endpoint);

    /// @copydoc ISchedulingLeaderSource::LeaderSchedulingEndpoint
    [[nodiscard]] std::string LeaderSchedulingEndpoint() const override;

  private:
    mutable std::mutex _lock; ///< Guards `_endpoint`.
    std::string _endpoint;    ///< What `Publish` was last given; empty until it is.
};

/// Which leader endpoint a node names to a redirected client, given what consensus last said.
///
/// **A leader silent for longer than @p bound is named by nobody**: a redirect is a promise that the
/// endpoint answers, and a launcher sent to a leader the whole fleet has stopped hearing spends its
/// connect on a machine that is not scheduling. Empty is the ordinary answer of an election in
/// progress, so the launcher declines the lease with `NoLeader` and compiles locally.
///
/// The bound is the consensus tier's own Raft `electionTimeoutMax` -- the silence after which a
/// follower VOTER would stand for election and stop naming the leader, which a learner, having no
/// election timer, never does by itself. It is injected (`ConsensusTier::LeaderContactObserver`),
/// never a constant here, and it is NOT `Distributed::LeaderSilenceBound`: that bounds how long a
/// worker trusts the state it applied (`consensus-leader-silent`), a different question with an
/// answer an hour long. Wrong in both directions at some value:
/// - **too long**: an unreachable leader -- a laptop off the VPN -- goes on being named, and every
///   redirected miss pays the launcher's connect before it compiles locally;
/// - **too short**: on a lossy link a learner briefly names nobody, and a TU compiles locally that a
///   worker could have built -- fail-safe, and healed at the next pass that hears the leader.
///
/// The reading arrives once per reconcile pass, so the effective bound is @p bound plus up to one
/// pass (`ReconcileInterval`). Nothing heard yet is not silence: the role observer names a leader
/// only once the driver has heard from it, so an absent reading is a pass that has not happened,
/// never one that measured forever.
/// @param leaderEndpoint The leader's `0xFC` endpoint as the role observer last gave it; empty while
///        nobody leads, or while this node does.
/// @param silentFor How long ago the leader last spoke, as the last consensus pass read it.
/// @param bound The silence past which the leader is no longer named.
/// @return @p leaderEndpoint, or empty when it is silent past @p bound.
[[nodiscard]] std::string_view SchedulingEndpointToPublish(
    std::string_view leaderEndpoint,
    std::optional<core::platform::SteadyTimePoint::duration> silentFor,
    core::platform::SteadyTimePoint::duration bound) noexcept;

/// The writer side of `KnownSchedulingLeader`: what the consensus tier tells this node, folded into
/// the one endpoint the redirect names.
///
/// Two observers feed it, on whichever threads the tier calls them from -- the role observer with
/// the leader's endpoint, and every reconcile pass with how long that leader has been silent and the
/// bound to judge it by -- so it keeps the last of each under one lock and republishes through ONE
/// decision, `SchedulingEndpointToPublish`, whichever of the two moved. Neither observer can then
/// publish an answer the other would have overruled.
class SchedulingLeaderPublisher
{
  public:
    /// @param holder Where the decided endpoint is published; must outlive this.
    explicit SchedulingLeaderPublisher(KnownSchedulingLeader& holder) noexcept;

    /// The role observer's half: who leads, as an endpoint.
    /// @param leaderEndpoint The leader's `0xFC` endpoint; empty while nobody leads, or this node does.
    void LeaderChanged(std::string_view leaderEndpoint);

    /// The reconcile pass's half: how long the leader has been silent, and how long is too long.
    /// @param reading The pass's reading; only its `silentFor` is read.
    /// @param bound The tier's `electionTimeoutMax`; see `SchedulingEndpointToPublish`.
    void LeaderContact(Distributed::LeaderReading const& reading, core::platform::SteadyTimePoint::duration bound);

  private:
    /// Publish what `SchedulingEndpointToPublish` decides from the two halves. Called under `_lock`.
    void RepublishLocked();

    KnownSchedulingLeader& _holder; ///< Where the decision goes.
    std::mutex _lock;               ///< Guards the two halves, and orders their publications.
    std::string _leaderEndpoint;    ///< What `LeaderChanged` was last given.
    /// What `LeaderContact` was last given; nothing before the first pass.
    std::optional<core::platform::SteadyTimePoint::duration> _silentFor;
    /// The bound `LeaderContact` was last given; read only once `_silentFor` holds a reading.
    core::platform::SteadyTimePoint::duration _bound {};
};

/// Answers every scheduling verb on a node that runs no scheduler: a member is sent to the leader.
///
/// **Admission first, and through the fold the scheduler uses** (`Distributed::CallerContextOf`): a
/// caller the node's oracle does not admit is answered `NotAMember` exactly as a scheduler answers
/// it, and is never told where the leader is. Every admitted caller is then told `NotLeader` with
/// the leader's endpoint -- what a follower scheduler says -- except for RELEASE, which settles a
/// lease with whoever granted it, and nothing here granted one.
///
/// **No identity check**, deliberately: a verb that needs a proven node (`ProvenNodeOnly`) is
/// refused or served by the leader the caller is redirected to, on the connection that request
/// arrives on. Refusing it here would only make the learner a second, stricter door.
///
/// `RefusePeer` and `Answer` reach ONE decision (`Decide`), so a caller refused before its payload
/// is read and one refused after it receive the same bytes. Nothing is counted: each answer's
/// constant states why.
class SchedulingRedirectResponder final: public IFrameResponder
{
  public:
    /// Kilobytes, the scheduler surface's own request ceiling: a client sends this node exactly
    /// what it would have sent the leader.
    static constexpr std::size_t RequestBytes = 64ULL * 1024ULL;

    /// The scheduler surface's connection allowance: a fleet's worth of launchers.
    static constexpr std::size_t OpenConnections = 256;

    /// @param membership Decides who is told where the leader is; must outlive this.
    /// @param leader Where the leader answers; read afresh per answer, must outlive this.
    SchedulingRedirectResponder(Distributed::IMembershipOracle const& membership,
                                ISchedulingLeaderSource const& leader) noexcept;

    /// @copydoc IFrameResponder::Answer
    ///
    /// Never suspends: the answer is a membership fold and one string.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// Every scheduling verb is refused here, a member's included: there is nothing to read a
    /// payload FOR, so the answer is given at the header and the payload is stepped over.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`: AUTH is the Session family's; this surface is never routed one.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return NotTheSessionSurface();
    }

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// Uncounted, as the scheduler surface's: a size or opcode refusal says the peer is confused.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// The scheduler surface's rows (`Detail::SchedulerEndpointRefusals`), every one uncounted.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// The header window: nothing here waits on anything.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// `RequestBytes`.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return RequestBytes;
    }

    /// `OpenConnections`.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return OpenConnections;
    }

    /// The connection cap times the request cap, so the byte budget never refuses what the
    /// connection cap would allow -- the scheduler surface's 16 MiB.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return OpenConnections * RequestBytes;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No: answered from memory, so the endpoint's reservation suffices.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// None: nothing here waits, so a peer has no window to vanish in.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// None: there is no silence to interpret.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Not a stream**: one refusal per verb.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None**: an identity is the leader's question, asked on the redirected request.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    /// The one answer for @p opRaw from @p peer, shared by `RefusePeer` and `Answer`.
    /// @param peer Who is asking.
    /// @param opRaw The verb, as received.
    /// @return The encoded refusal, or nullopt for a verb outside the scheduler family -- which a
    ///         member is then refused by whoever asked: the endpoint's opcode check, or `Answer`.
    [[nodiscard]] std::optional<std::vector<std::byte>> Decide(PeerIdentity const& peer, std::uint8_t opRaw) const;

    Distributed::IMembershipOracle const& _membership;
    ISchedulingLeaderSource const& _leader;
};

} // namespace FastCache::Node
