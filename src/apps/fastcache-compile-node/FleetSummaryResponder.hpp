// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"

#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// @file FleetSummaryResponder.hpp
/// Which fleet this node is in, answered to anybody who asks: the `0xFC` component that answers
/// `Op::FleetSummary`.

/// Answers `FleetSummary` with this node's summary, signed over the asker's nonce.
///
/// **A seed answers strangers**: the machine asking is, by construction, not in this fleet yet --
/// it is deciding whether to join it. So nobody is refused at the door, no credential is required,
/// and what stands between a stranger and a lie is the SIGNATURE: the summary is signed under this
/// node's identity key over the asker's own nonce (`Cluster::FleetSummaryMessage`), so a recorded
/// answer verifies for nobody else. The fields are the beacon's, which the segment already hears.
///
/// **It announces nothing a peer must not be told**: a summary with no cluster, or one naming an
/// endpoint every peer resolves to itself (`Cluster::AnnouncesOnlyThisMachine`), is refused rather
/// than signed -- the rule discovery holds at its own door.
class FleetSummaryResponder final: public IFrameResponder
{
  public:
    /// What this family ADDS to its port's connection pool: a joiner asks once and closes.
    ///
    /// **A contribution, never a cap on this family.** The endpoint admits a connection at ACCEPT,
    /// before any verb is read, against ONE port-wide number -- the largest owner's allowance or
    /// the sum of the every-node owners', whichever is greater -- so nothing reserves these eight
    /// for fleet-summary questions or stops a stranger's connections at eight. A flood of
    /// connections can take the whole pool with any bytes or none, as it could before this family
    /// existed; what bounds it is the port's pool, and this only enlarges that pool.
    static constexpr std::size_t OpenConnections = 8;

    /// No metrics sink and no logger, deliberately: every refusal here is uncounted, each row's
    /// `rationale` saying why, and every answer is provokable by anybody who can route to the
    /// port, so a line per answer would be a log a stranger can grow.
    /// @param self What this node says about itself, read afresh per answer; must outlive this.
    /// @param identity This node's identity key pair, read once at startup; must outlive this.
    FleetSummaryResponder(Cluster::IFleetSummarySource const& self, Ed25519KeyPair const& identity) noexcept;

    /// @copydoc IFrameResponder::Answer
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// **Nobody**: the asker is on no list, which is the question it is asking about.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`: AUTH is the Session family's; this surface is never routed one. The verb is
    /// `OpenBeforeAuth` besides, since a joiner holds no credential of this fleet's.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return NotTheSessionSurface();
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
    /// The header window: one fixed-width nonce, answered with one signature.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// The verb's own ceiling, `MaxFleetSummaryPayload`.
    ///
    /// **Not read on the merged listener**: the `Formation` row says `SessionCeilings::NotRead`,
    /// and what bounds a caller is the verb's own `OpTable` row, which `DecidePrePayload` takes.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return CompileCacheWire::MaxFleetSummaryPayload;
    }

    /// `OpenConnections`: ADDED by `MergedResponder` to the other every-node families' allowances,
    /// since they coexist on every port -- a contribution to the port-wide pool, not a cap.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return OpenConnections;
    }

    /// The connection cap times the request cap, so the byte budget never refuses what the
    /// connection cap would allow; unread for `MaxRequestBytes`' reason.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return OpenConnections * CompileCacheWire::MaxFleetSummaryPayload;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No: answered from memory with one signature, so the endpoint's reservation suffices.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// None: nothing here waits, so a peer has no window to vanish in that is worth counting.
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
    /// **Not a stream**: one question, one answer.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None**: this surface proves THIS node to the asker, never the asker to this node.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    /// Answer one fleet-summary question: refuse a malformed one, or sign.
    /// @param frame The whole request, header included; its verb is FLEET-SUMMARY.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerOnce(std::span<std::byte const> frame) const;

    /// Sign this node's summary over @p nonce, or say why it is not offered.
    /// @param nonce The asker's nonce.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> Sign(std::span<std::byte const> nonce) const;

    Cluster::IFleetSummarySource const& _self;
    Ed25519KeyPair const& _identity;
};

} // namespace FastCache::Node
