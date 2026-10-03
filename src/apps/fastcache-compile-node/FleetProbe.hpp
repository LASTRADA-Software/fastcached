// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"

#include <FastCache/Cluster/ChallengeIssuer.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// @file FleetProbe.hpp
/// Asking a seed which fleet it is in: the client half of `Op::FleetSummary`.

/// Asks one endpoint which fleet it is in, and answers only with what its signature proves.
///
/// A seam, so formation decides from a scripted answer in a test and from a dialled one in
/// production.
class IFleetProbe
{
  public:
    IFleetProbe() = default;
    IFleetProbe(IFleetProbe const&) = delete;
    IFleetProbe& operator=(IFleetProbe const&) = delete;
    IFleetProbe(IFleetProbe&&) = delete;
    IFleetProbe& operator=(IFleetProbe&&) = delete;
    virtual ~IFleetProbe() = default;

    /// Ask a seed which fleet it is in.
    /// @param seed Its `host:port`, and which source named it -- which decides the answer's origin
    ///        (`Cluster::SeedOriginTable`).
    /// @return The fleet its signature proves, over a nonce drawn for this question; or a sentence
    ///         naming the endpoint and what was wrong.
    [[nodiscard]] virtual std::expected<Cluster::ProvenFleet, std::string> Ask(Cluster::SeedCandidate const& seed) = 0;

    /// Ask an endpoint for the summary of the fleet it is in, claiming no ORIGIN for the answer.
    ///
    /// The same exchange as `Ask`, for a reader that is not joining through a seed: a pending node
    /// proving the key an endpoint it is about to poll answers under -- the leader a summary named, or
    /// one a redirect named -- and a solitary node following a pending one's pointer. Stamping a seed's
    /// origin on it would be a confident wrong claim about how it arrived, so it returns the proven
    /// summary and nothing more.
    /// @param endpoint Its `host:port`.
    /// @return The summary its signature proves, over a nonce drawn for this question; or a sentence
    ///         naming the endpoint and what was wrong.
    [[nodiscard]] virtual std::expected<Cluster::ProvenFleetSummary, std::string> AskSummary(std::string_view endpoint) = 0;
};

/// `IFleetProbe` over a dialled connection: one question, one answer, one connection.
class DialledFleetProbe final: public IFleetProbe
{
  public:
    /// How long reaching a seed may take. A seed that does not answer in a second is one of several
    /// a joiner may try, and the next is cheaper than waiting.
    static constexpr std::chrono::milliseconds ConnectTimeout { 1'000 };

    /// How long one question may take in ALL, connect included -- measured on the injected clock.
    ///
    /// **One deadline over the whole exchange, never a per-call timeout**: the seed is a stranger by
    /// construction (a typed seed, a DNS SRV answer, a remembered address, or anything on-path), and
    /// a peer dribbling one byte inside a per-call bound holds a per-call reader forever. Every read
    /// is bounded by what is left of this (`TotalDeadlineSocket`). A few seconds, because an honest
    /// seed answers from a table and a formation walk has other seeds to try.
    static constexpr std::chrono::milliseconds ExchangeDeadline { 5'000 };
    static_assert(ConnectTimeout < ExchangeDeadline, "the connect is part of the exchange, so it must leave time for it");

    /// The most of a seed's refusal sentence a probe's error carries, once escaped.
    ///
    /// The sentence is the seed's, unauthenticated and of any length and any bytes, and the error
    /// goes into log lines and condition details: it is made text (`BoundedPeerText`) and cut here.
    static constexpr std::size_t MaxRefusalText = 256;

    /// @param dialer How a seed is reached; must outlive this.
    /// @param random Where each question's nonce comes from; must outlive this.
    /// @param clock What `ExchangeDeadline` is measured on; must outlive this.
    DialledFleetProbe(IEndpointDialer& dialer, ISecureRandom& random, core::platform::IClock const& clock) noexcept;

    /// @copydoc IFleetProbe::Ask
    [[nodiscard]] std::expected<Cluster::ProvenFleet, std::string> Ask(Cluster::SeedCandidate const& seed) override;

    /// @copydoc IFleetProbe::AskSummary
    [[nodiscard]] std::expected<Cluster::ProvenFleetSummary, std::string> AskSummary(std::string_view endpoint) override;

  private:
    /// An answer as it arrived, with the nonce it must be signed over: what `Ask` and `AskSummary`
    /// each prove in their own way.
    struct Answered
    {
        Cluster::IssuedNonce held;                 ///< The nonce this question drew, to verify against.
        CompileCacheWire::FleetSummaryReply reply; ///< What came back, decoded and not yet verified.
    };

    /// Ask @p endpoint the question, under one deadline, and read the reply.
    /// @param endpoint Its `host:port`.
    /// @return The answer and its nonce, or a sentence naming the endpoint and what was wrong.
    [[nodiscard]] std::expected<Answered, std::string> Exchange(std::string_view endpoint);

    IEndpointDialer& _dialer;
    core::platform::IClock const& _clock;
    Cluster::ChallengeIssuer _issuer; ///< Each question's one-use nonce (`IssueNonce`).
};

} // namespace FastCache::Node
