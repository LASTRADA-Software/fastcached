// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"

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

/// @file FleetTextResponder.hpp
/// The fleet document, read once over `0xFC` (#1391): the `0xFC` component that answers
/// `Op::FleetText`.

/// Answers `FleetText` with the document `/fleet.txt` serves.
///
/// **Its own component rather than a verb of `LiveStatsResponder`**, whose every answer is a
/// stream and whose hooks are sized for one: a subscription holds its own byte budget and is never
/// watched as an answer, and a request answered in microseconds wants the endpoint's ordinary
/// accounting instead. What the two share is the part that must not diverge -- who may read the
/// fleet (`DecideFleetRead`) -- and the sources the document is read from.
///
/// **The body is `ILiveStatsSources::FleetText`'s**, which on a node is `AnswerFleetText`, the
/// function `/fleet.txt` answers from; this component renders nothing, so the verb and the route
/// cannot be two renderers.
class FleetTextResponder final: public IFrameResponder
{
  public:
    /// @param sources Where the fleet and its leadership are read; must outlive this. The node's
    ///        slot, since the fleet is built after this surface is listening.
    /// @param membership Who may ask; must outlive this. Bound once, by reference, the way every
    ///        surface binds it.
    /// @param dashboard The dashboard credential, or a default one when no token file is named --
    ///        and then the fleet is served to loopback peers only.
    /// @param metrics Where a refusal is counted; must outlive this.
    FleetTextResponder(ILiveStatsSources const& sources,
                       Distributed::IMembershipOracle const& membership,
                       AdminCredential dashboard,
                       IMetricsSink& metrics) noexcept;

    /// @copydoc IFrameResponder::Answer
    [[nodiscard]] Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// The one implementation of the membership rule, called by `Answer` as well as by the door,
    /// so the counter moves exactly once per refused request whichever path reached it.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// **No**, for `LiveStatsResponder::AuthRequired`'s reason: the credential on this listener is
    /// the scheduler's, and a surface must not require a secret it cannot verify. The fleet's own
    /// secret is the dashboard credential, which the request carries.
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
    /// The header window: a control-sized request, answered by one render.
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
    /// `NodeStatusResponder`'s modest figure, for its reason: a terminal opens one connection and
    /// closes it. ADDED by `MergedResponder` to the other operator families', for that responder's
    /// reason too: the three coexist on every port.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 32;
    }

    /// @copydoc IFrameResponder::MaxInFlightBytes
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload * 16;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// **No**, for `NodeStatusResponder`'s reason: nothing here reserves a footprint per request, so
    /// the endpoint keeps the account.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// **Not watched**: one render of a document measured in kilobytes, answered before a client
    /// could leave in a way worth counting.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// **Not pulsed**: there is no silence to interpret.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Not a stream**: the same document as a series is the fleet subject of `Op::Subscribe`.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None.** The credential this surface reads is the dashboard token, which is deliberately
    /// not `--requirepass` and not the cluster key. The proof reaches its membership half through
    /// `RefusePeer`.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    /// Gate a decoded request from a member, and answer it.
    /// @param request What was asked for.
    /// @param peer Who asked.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> Read(CompileCacheWire::FleetTextRequest const& request,
                                              std::string_view peer) const;

    /// Answer an admitted request with the document, or with why there is none.
    /// @param request What was asked for.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> Render(CompileCacheWire::FleetTextRequest const& request) const;

    ILiveStatsSources const& _sources;
    Distributed::IMembershipOracle const& _membership;
    AdminCredential _dashboard;
    IMetricsSink& _metrics;
};

} // namespace FastCache::Node
