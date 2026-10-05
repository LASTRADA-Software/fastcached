// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// @file SessionResponder.hpp
/// The `0xFC` component that owns the `Session` verb family: `AUTH`, and what a verified
/// ticket establishes on a connection; and `MINT-TICKET`, which signs one for this machine.

/// What this node mints tickets as.
struct SessionKeys
{
    Ed25519KeyPair const* identityKey { nullptr }; ///< This node's; null on a node that holds none.
    std::string_view machineId;                    ///< The id that key is recorded under (`cfg.nodeId`).
};

/// Answers the `Session` family on every built node.
///
/// **The node checks no password.** `--scheduler-token-file` is gone, and so is the gate it fed:
/// admission is `RefusePeer`'s, decided from what a connection established. What an `AUTH` can
/// still establish is which MACHINE a verified ticket speaks for, and that is a question every
/// node answers whatever components it runs -- which is why this is a component of its own
/// rather than a verb of the scheduler, and why it is never null on a built node.
///
/// **`AUTH` is terminated by the endpoint**, which asks `CheckCredential` and records the verdict
/// on the connection; it never reaches `Answer`. A password `AUTH` is answered `NoPolicy`: `Ok`,
/// establishing nothing, so a token-configured launcher is not broken by a node that requires
/// none. A machine ticket is verified against this node's roster, and an accepted one says WHICH
/// machine the connection speaks for.
///
/// **`MINT-TICKET` is served on LOOPBACK only, judged per connection from the kernel's peer address
/// and never from a cache.** A ticket is this machine vouching for its caller, so the one caller it
/// must never vouch for is on another machine -- and the interval-refreshed `ILocalityOracle` the
/// cache tier trusts fails OPEN for up to one refresh after this machine loses an address. For
/// FETCH that window leaks this machine's own objects; for a credential it is a wrong answer that
/// looks right, since every node in the fleet then admits the ticket's bearer as this machine. So
/// there is no cache and no oracle here, and loopback is the whole rule: a launcher mints from its
/// OWN node over loopback, which the default bind serves, whatever endpoint it goes on to present
/// the ticket at. One that dials its own node by a LAN address is refused `NotAMember`.
///
/// **One misconfiguration defeats it, and it is named here because nothing can detect it: a relay
/// in front of the node surface.** A reverse proxy or port forwarder on this machine, or a NAT that
/// hairpins remote clients onto the node over loopback, makes every client it relays "this
/// machine", and each would be minted tickets that speak for it. Never put one in front of the
/// `0xFC` port.
class SessionResponder final: public IFrameResponder
{
  public:
    /// @param verifier Decides whether a presented ticket speaks for a machine; must outlive this.
    /// @param keys What this node mints tickets as. The key must outlive this.
    /// @param random Where a minted ticket's nonce comes from; must outlive this.
    /// @param wallClock What a ticket's expiry is compared with and stamped from.
    /// @param metrics Where this surface's outcomes are recorded; must outlive this.
    SessionResponder(Distributed::TicketVerifier const& verifier,
                     SessionKeys keys,
                     ISecureRandom& random,
                     core::platform::WallClockRef wallClock,
                     IMetricsSink& metrics) noexcept;

    /// @copydoc IFrameResponder::Answer
    ///
    /// `MINT-TICKET`. `AUTH` never arrives here -- the endpoint terminates it -- and any other
    /// `Session` verb is refused by name.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// **`AUTH` is admitted from any address, before membership is asked**, exactly as `ENROLL`
    /// is: it is how a caller establishes what membership then folds, so refusing it until the
    /// caller is a member would refuse the population the verb is for. It is pre-auth and
    /// bounded to `MaxAuthPayload` by its own `OpTable` row, so admitting it costs a stranger
    /// four kilobytes at most.
    ///
    /// **`MINT-TICKET` is admitted from loopback only**, counted when it is not.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// A machine ticket is caught BEFORE the password rule, which would answer it `NoPolicy`: it is
    /// verified, and answered `Accepted` with its machine or `Rejected` with the reason already
    /// encoded and counted. A password is `NoPolicy` -- the node holds none -- and a payload that
    /// will not decode is `Malformed`.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> payload) const override;

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
    /// The header window: an `AUTH` is a few hundred bytes answered without a suspension.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// @copydoc IFrameResponder::MaxRequestBytes
    ///
    /// The larger of the two verbs' own ceilings, each of which its `OpTable` row states.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return std::max(CompileCacheWire::MaxAuthPayload, CompileCacheWire::MaxMintTicketPayload);
    }

    /// @copydoc IFrameResponder::MaxOpenConnections
    ///
    /// **Not read**: the `Session` row of `FamilyRoutes` is `SessionCeilings::NotRead`, because an
    /// `AUTH` rides a connection some other family's verbs are the reason for. Answered rather
    /// than inherited, for the reason the interface is pure virtual.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 32;
    }

    /// @copydoc IFrameResponder::MaxInFlightBytes
    ///
    /// **Not read**, for `MaxOpenConnections`' reason.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return CompileCacheWire::MaxAuthPayload * 32;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// **No**: nothing here reserves a footprint per request, so the endpoint keeps the account.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// **Not watched**: an `AUTH` is answered before a client could leave in a way worth counting.
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
    /// **Not a stream.**
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None.** A session proof is the `NodeProof` family's; this component checks credentials.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    /// Sign a ticket for a process on this machine, or say why not.
    /// @param payload The request payload.
    /// @return The reply.
    [[nodiscard]] std::vector<std::byte> Mint(std::span<std::byte const> payload) const;

    Distributed::TicketVerifier const& _verifier;
    Ed25519KeyPair const* _identityKey;
    std::string _machineId;
    ISecureRandom& _random;
    core::platform::WallClockRef _wallClock;
    IMetricsSink& _metrics;
};

} // namespace FastCache::Node
