// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>
#include <FastCache/Server/AdminCredential.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// @file LiveStatsResponder.hpp
/// A compile node's live stats: the `0xFC` component that answers `Op::Subscribe` with a stream
/// (#1399). The loop is `LiveStream`'s, shared with the daemon; what is here is the node's GATE --
/// who may watch, and which subject -- and its place on the merged surface.

/// Answers `Op::Subscribe` with a stream.
///
/// **Gated on membership, as `NodeStatusResponder` is**, for its reasons: what streams is what
/// those verbs report, and an operator watching a fleet is by construction not sitting on every
/// node in it. The fleet subject adds two gates -- the dashboard credential, and leadership --
/// because the fleet map is behind the dashboard credential on `/fleet`, and a follower's registry
/// is a partial picture presented as the whole.
class LiveStatsResponder final: public IFrameResponder, public IFrameStream, private ILiveGate
{
  public:
    /// @param sources What streams read; must outlive this.
    /// @param membership Who may watch; must outlive this. Bound once, by reference, the way every
    ///        surface binds it -- a test that re-acquired it would pass under exactly the removal
    ///        defect re-gating exists for.
    /// @param dashboard The dashboard credential, or a default one when no token file is named --
    ///        and then the fleet is streamed to loopback peers only.
    /// @param reactor The loop streams sleep on; the node's `0xFC` reactor.
    /// @param metrics Where the stream's counters rise; must outlive this.
    LiveStatsResponder(ILiveStatsSources const& sources,
                       Distributed::IMembershipOracle const& membership,
                       AdminCredential dashboard,
                       IReactor& reactor,
                       IMetricsSink& metrics) noexcept;

    /// @copydoc IFrameResponder::Answer
    ///
    /// Reached only by a caller that does not stream, since the endpoint asks `StreamFor` first:
    /// such a caller cannot carry a subscription, so it is told the verb is not served that way.
    [[nodiscard]] Task<FrameReply> Answer(std::span<std::byte const> frame, std::string peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer, std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// **No**, for `NodeStatusResponder::AuthRequired`'s first reason: the credential on this
    /// listener is the scheduler's, and a surface must not require a secret it cannot verify. The
    /// fleet's own secret is the dashboard credential, which the request carries.
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::CheckCredential
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(nullptr, payload);
    }

    /// @copydoc IFrameResponder::RefusalReply
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// The header window: it covers reading a control-sized request, and the stream that follows
    /// is bounded per push by its hold rather than by a window over the whole subscription.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// @copydoc IFrameResponder::MaxRequestBytes
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload;
    }

    /// @copydoc IFrameResponder::MaxOpenConnections
    ///
    /// The subscription cap. ADDED by `MergedResponder` to the other operator families', so a node
    /// running nothing else still admits `--node-status` and a fleet read while every subscription is
    /// held; the cap that bites on subscriptions is the one this responder enforces itself, with a
    /// counted refusal.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return CompileCacheWire::MaxLiveSubscriptions;
    }

    /// @copydoc IFrameResponder::MaxInFlightBytes
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload * 16;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// **Yes**, and the accounting is the subscription cap: a request is released by the endpoint
    /// once READ, because a stream lasts hours and a budget charged for its whole length would
    /// count dashboards as load. What one subscription holds is bounded by `MaxRequestBytes`, and
    /// how many there are by `MaxLiveSubscriptions`.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return true;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// **Not watched as an answer**: the endpoint arms a stream its own consulted watch, because a
    /// subscriber leaving is the ordinary end of a stream and not a delivery abandoned mid-answer.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// **Not pulsed**: a stream pushes a snapshot every granted cadence, which is the liveness a
    /// pulse stands in for.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t opRaw) noexcept override;

    /// @copydoc IFrameStream::Serve
    [[nodiscard]] Task<std::vector<std::byte>> Serve(std::span<std::byte const> frame,
                                                     std::string peer,
                                                     IPushSink* sink) override;

    /// @return How many subscriptions are streaming right now. For tests.
    [[nodiscard]] std::size_t ActiveSubscriptions() const noexcept
    {
        return _stream.ActiveSubscriptions();
    }

  private:
    /// @copydoc ILiveGate::RefuseWatcher
    [[nodiscard]] std::optional<std::vector<std::byte>> RefuseWatcher(std::string_view peer) const override;

    /// @copydoc ILiveGate::Admit
    ///
    /// Every subject is served to a member. The fleet adds the dashboard credential -- or, with no
    /// token file, this machine only -- and leadership: a follower redirects, and a node running no
    /// scheduler says the fleet is served elsewhere.
    [[nodiscard]] std::optional<std::vector<std::byte>> Admit(CompileCacheWire::SubscribeRequest const& request,
                                                              std::string_view peer) const override;

    /// @copydoc ILiveGate::Recheck
    ///
    /// Membership again, of the oracle bound once; and for the fleet, leadership, so a demoted
    /// node ends its fleet streams naming the new leader.
    [[nodiscard]] std::optional<std::vector<std::byte>> Recheck(CompileCacheWire::LiveSubject subject,
                                                                std::string_view peer) const override;

    ILiveStatsSources const& _sources;
    Distributed::IMembershipOracle const& _membership;
    AdminCredential _dashboard;
    IReactor& _reactor;
    IMetricsSink& _metrics;
    LiveStream _stream;
};

} // namespace FastCache::Node
