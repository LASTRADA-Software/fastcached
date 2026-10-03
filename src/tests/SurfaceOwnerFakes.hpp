// SPDX-License-Identifier: Apache-2.0
#pragma once

// A test fake shared by the node's surface tests, for the rulebook's reason: a fake is a helper
// too, and a copy's cost grows with every fact it has to get right. In `src/tests/` beside the
// other shared fakes (`MembershipFakes.hpp`), for `FleetHarness.hpp`'s reason. `NamedResponder` stands in for
// any component on the merged listener, and `EveryNodeOwners` fills in the owners every built node
// has, walking the same `FamilyRoutes` column `StartNodeSurfaceOrExplain` asks.

#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/FrameEndpoint.hpp>
#include <apps/fastcache-compile-node/MembershipGate.hpp>
#include <apps/fastcache-compile-node/Responders.hpp>
#include <core/async/Task.hpp>

namespace FastCache::Node::SurfaceFakes
{

namespace Wire = FastCache::CompileCacheWire;

/// A responder that records what it was asked and answers by name.
///
/// Named rather than counted: what a router has to get right is WHICH component was
/// reached, and two counters that both read 1 cannot say that a frame went to the
/// right one. The reply carries the name, so a case reads the answer rather than
/// inferring it.
class NamedResponder final: public IFrameResponder
{
  public:
    explicit NamedResponder(std::string name):
        _name { std::move(name) }
    {
    }

    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> /*frame*/, PeerIdentity /*peer*/) override
    {
        _answered.push_back(_name);
        co_return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedValue, _name);
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override
    {
        _admitted.push_back(opRaw);
        _peers.push_back(peer);
        if (_gate.membership == nullptr)
            return std::nullopt;
        return RefuseUnlessMember(*_gate.membership,
                                  *_gate.metrics,
                                  peer,
                                  { .code = Wire::ErrorCode::NotAMember, .counter = _gate.strangerCounter },
                                  _name);
    }

    /// The next scripted verdict, or `NoPolicy` once the script is spent.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        if (_verdicts.empty())
            return CredentialVerdict { .outcome = CredentialOutcome::NoPolicy };
        auto verdict = std::move(_verdicts.front());
        _verdicts.pop_front();
        return verdict;
    }

    [[nodiscard]] std::vector<std::byte> RefusalReply(Wire::PrePayloadDecision decision,
                                                      std::uint8_t /*opRaw*/,
                                                      std::string_view /*detail*/) const override
    {
        _refusals.push_back(_name);
        return Wire::EncodeErrorReply(Wire::ErrorCodeFor(decision), _name);
    }

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// Records the same name, so the routing cases below assert the attribution of an
    /// endpoint-decided refusal exactly as they do a pre-payload one.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal /*refusal*/,
                                                              std::uint8_t /*opRaw*/,
                                                              std::string_view /*detail*/) const override
    {
        _refusals.push_back(_name);
        return Wire::EncodeErrorReply(Wire::ErrorCode::EndpointBusy, _name);
    }

    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return _maxRequest;
    }

    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _requestTimeout;
    }

    /// How long this fake claims its answers may take.
    /// @param window The window to report.
    void PlaceRequestTimeout(std::chrono::milliseconds window) noexcept
    {
        _requestTimeout = window;
    }

    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return _maxOpen;
    }

    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return _maxInFlight;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// Settable per fake, because what `MergedResponder` must do with this is ROUTE
    /// it: the three ceilings above fold with `Largest`, and folding this one either
    /// way is a defect (#448).
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _ownBudget;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// Settable per fake, because what `MergedResponder` must do with this is ROUTE
    /// it -- the same reason `HoldsOwnByteBudget` above is settable.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _watchPeer;
    }

    /// Name the counter this fake's abandoned deliveries belong to, or none.
    /// @param counter What `PeerWatchCounter` should answer.
    void SetPeerWatchCounter(std::optional<IMetricsSink::Counter> counter) noexcept
    {
        _watchPeer = counter;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// Settable per fake, for the reason `PeerWatchCounter` above is: what
    /// `MergedResponder` must do with this is ROUTE it, and a routing test needs the
    /// fakes to answer differently from each other.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _progress;
    }

    /// @copydoc IFrameResponder::StreamFor
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return _stream;
    }
    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None.** This fake stands in for a surface, not for the node prover; a case that
    /// needs one builds a `NodeProofResponder`.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

    /// Say which stream this fake answers with, or that it answers none.
    /// @param stream What `StreamFor` should answer.
    void SetStream(IFrameStream* stream) noexcept
    {
        _stream = stream;
    }

    /// Say how often this fake pulses, or that it does not.
    /// @param interval What `ProgressInterval` should answer.
    void SetProgressInterval(std::optional<std::chrono::milliseconds> interval) noexcept
    {
        _progress = interval;
    }

    /// Claim, or stop claiming, that this fake accounts for its own request bytes.
    /// @param own What `HoldsOwnByteBudget` should answer.
    void ClaimOwnByteBudget(bool own) noexcept
    {
        _ownBudget = own;
    }

    /// Admit callers as every production surface does, through `RefuseUnlessMember` over @p membership,
    /// rather than admitting everybody.
    /// @param membership The node's oracle; must outlive this.
    /// @param metrics Where a refusal is counted; must outlive this.
    /// @param strangerCounter The row a caller nothing admits is counted on.
    void GateBy(Distributed::IMembershipOracle const& membership,
                IMetricsSink& metrics,
                IMetricsSink::Counter strangerCounter) noexcept
    {
        _gate = Gate { .membership = &membership, .metrics = &metrics, .strangerCounter = strangerCounter };
    }

    /// Script the verdicts `CheckCredential` answers, one per `AUTH`, in order.
    /// @param verdicts What the next checks establish.
    void AnswerAuthWith(std::vector<CredentialVerdict> verdicts)
    {
        for (auto& verdict: verdicts)
            _verdicts.push_back(std::move(verdict));
    }

    /// Place the three session ceilings this fake reports.
    /// @param request Largest request it will buffer.
    /// @param open Largest number of connections.
    /// @param inFlight Largest number of bytes in flight.
    void PlaceCeilings(std::size_t request, std::size_t open, std::size_t inFlight) noexcept
    {
        _maxRequest = request;
        _maxOpen = open;
        _maxInFlight = inFlight;
    }

    /// @return The name recorded once per refusal this fake encoded.
    [[nodiscard]] std::vector<std::string> const& Refusals() const noexcept
    {
        return _refusals;
    }

    /// @return The verbs this fake was asked to admit, in order.
    [[nodiscard]] std::vector<std::uint8_t> const& Admitted() const noexcept
    {
        return _admitted;
    }

    /// @return What each connection had established when this fake was asked to admit it, in order.
    [[nodiscard]] std::vector<PeerIdentity> const& Peers() const noexcept
    {
        return _peers;
    }

    /// @return The name recorded once per frame this fake answered.
    [[nodiscard]] std::vector<std::string> const& Answered() const noexcept
    {
        return _answered;
    }

  private:
    /// The membership gate `GateBy` placed; none admits everybody.
    struct Gate
    {
        Distributed::IMembershipOracle const* membership { nullptr };
        IMetricsSink* metrics { nullptr };
        IMetricsSink::Counter strangerCounter { IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember };
    };

    std::string _name;
    Gate _gate {};
    std::size_t _maxRequest { 1024 };
    std::size_t _maxOpen { 8 };
    std::size_t _maxInFlight { 4096 };
    bool _ownBudget { false };
    std::optional<IMetricsSink::Counter> _watchPeer {};
    std::optional<std::chrono::milliseconds> _progress {};
    IFrameStream* _stream { nullptr };
    std::chrono::milliseconds _requestTimeout { FrameServer::HeaderTimeout };
    // Mutable because the three predicates recording into them are `const`: a
    // predicate that counted how often it was asked would otherwise have to look like
    // a mutator, which is the thing `RefusePeer`'s own signature refuses to do.
    mutable std::vector<std::string> _refusals;
    mutable std::vector<std::uint8_t> _admitted;
    mutable std::vector<PeerIdentity> _peers;
    mutable std::vector<std::string> _answered;
    mutable std::deque<CredentialVerdict> _verdicts;
};

/// Stand-ins for the owners every built node has, filled into whichever a case left null.
///
/// A case about something else -- the bind, activation, a worker-only port -- names the components
/// it is about, and the start refuses a listener missing an every-node owner
/// (`MissingEveryNodeOwner`). So this fills in the rest, as `main` always has them, walking the same
/// `FamilyRoutes` column the start asks rather than a list of members.
class EveryNodeOwners
{
  public:
    /// @param components What the case routes to.
    /// @return The same, with a stand-in in every every-node owner it left null.
    [[nodiscard]] SurfaceComponents Around(SurfaceComponents components)
    {
        for (auto const& row: FamilyRoutes)
            if (row.presence == FamilyPresence::OnEveryBuiltNode && components.*row.owner == nullptr)
                components.*row.owner = &_standIns.emplace_back(std::string { row.component });
        return components;
    }

  private:
    std::deque<NamedResponder> _standIns; ///< A deque, so each stand-in keeps its address.
};

} // namespace FastCache::Node::SurfaceFakes
