// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "Responders.hpp"
#include "SharedCacheDirectory.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

namespace Detail
{
    /// What the SHARED-CACHE component does about each endpoint-decided refusal.
    ///
    /// Its own rows rather than the private tier's, although the shape is theirs: a byte budget a
    /// fleet's STOREs run into on the named machine is a different event from one a local build
    /// runs into on its own, and summed into one series an operator could not tell which machine's
    /// builds are being refused. The credential and proof rows are the private tier's arguments,
    /// word for word, because they are about which component owns `AUTH` and the proof, not about
    /// which tier this is.
    inline constexpr EnumTable<EndpointRefusal, CacheEndpointRefusal> SharedCacheEndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          .policy = { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedEndpointBusy, .rationale = {} } },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSessionsRationale } },
        { .refusal = EndpointRefusal::CredentialRejected,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSessionsRationale } },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .policy = { .counter = std::nullopt, .rationale = AnswerDeadlineIsTheEndpointsRationale } },
        { .refusal = EndpointRefusal::NodeProofUnchallenged,
          .policy = { .counter = std::nullopt, .rationale = NodeProofIsTheProversRationale } },
    } };

    // The private tier's two guards, for its reasons: an appended enumerator leaves a row stating
    // neither claim, and a row stating both is an author who could not choose.
    static_assert(RowsInEnumeratorOrder(SharedCacheEndpointRefusals, &CacheEndpointRefusal::refusal),
                  "SharedCacheEndpointRefusals must hold one row per EndpointRefusal, in enumerator order");
    static_assert(Cc::RowsStateOneRefusalClaim(SharedCacheEndpointRefusals,
                                               [](CacheEndpointRefusal const& row) {
                                                   return Cc::RefusalClaim { .counted = row.policy.counter.has_value(),
                                                                             .rationale = row.policy.rationale };
                                               }),
                  "every shared-cache endpoint refusal must state either a counter or a rationale, and not both");

    /// What the SHARED-CACHE component does about each pre-payload decision.
    ///
    /// A total switch rather than a table, for the reason `CachePrePayloadPolicy` gives: the enum is
    /// a wire enum with no `Last`, and `-Werror=switch` is the guard.
    /// @param decision A decision other than `Serve`.
    /// @return What this component does about it.
    [[nodiscard]] constexpr CacheRefusalPolicy SharedCachePrePayloadPolicy(
        CompileCacheWire::PrePayloadDecision decision) noexcept
    {
        switch (decision)
        {
            case CompileCacheWire::PrePayloadDecision::PayloadTooLarge:
                // The private tier's argument, on the fleet's own series: twenty-four bytes and no
                // body is the cheapest probe there is, and a node the fleet names takes every
                // machine's STOREs.
                return { .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedPayloadTooLarge, .rationale = {} };
            case CompileCacheWire::PrePayloadDecision::UnknownOpcode:
                return { .counter = std::nullopt,
                         .rationale = "MergedResponder owns a verb only when FamilyOf names its family, and an opcode "
                                      "with no OpTable row is Unset -- so an unknown one is answered UnservedReply at "
                                      "the door and never reaches this component" };
            case CompileCacheWire::PrePayloadDecision::Unauthenticated:
                return { .counter = std::nullopt, .rationale = NodeChecksNoPasswordRationale };
            case CompileCacheWire::PrePayloadDecision::Serve:
                break;
        }
        return { .counter = std::nullopt, .rationale = "Serve is not a refusal and the endpoint never asks about it" };
    }

    static_assert(std::ranges::all_of(std::array { CompileCacheWire::PrePayloadDecision::Serve,
                                                   CompileCacheWire::PrePayloadDecision::UnknownOpcode,
                                                   CompileCacheWire::PrePayloadDecision::PayloadTooLarge,
                                                   CompileCacheWire::PrePayloadDecision::Unauthenticated },
                                      [](CompileCacheWire::PrePayloadDecision decision) {
                                          return StatesOneClaim(SharedCachePrePayloadPolicy(decision));
                                      }),
                  "every shared-cache pre-payload decision must state either a counter or a rationale, and not both");
} // namespace Detail

/// A caller the shared tier does not admit: counted apart from the private tier's locality
/// refusal, which answers a different question about a different caller.
inline constexpr Cc::SurfaceRefusal SharedStranger {
    .code = CompileCacheWire::ErrorCode::NotAMember,
    .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember,
};

/// What a caller the shared tier does not admit is told.
inline constexpr std::string_view SharedStrangerWhy =
    "the fleet's shared cache serves machines that prove a key the fleet holds, or present a ticket it verifies";

/// This machine is not the fleet's shared cache right now.
///
/// Its own code rather than `NotAMember`, because the caller IS admitted -- telling a member it is
/// not one is a confident wrong signal -- and rather than `UnimplementedVerb`, because every node
/// implements the family.
inline constexpr Cc::SurfaceRefusal NotServingSharedCache {
    .code = CompileCacheWire::ErrorCode::NotSharedCache,
    .counter = IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotServing,
};

/// What an admitted caller is told on a node that is not serving the shared tier.
inline constexpr std::string_view NotServingSharedCacheWhy =
    "the fleet's shared-cache setting does not name this machine now, or names it and its tier is unavailable; "
    "see --node-status";

/// Serves `SharedFetch` and `SharedStore` on the node's merged listener: the fleet's shared cache.
///
/// **Built on every node.** A node the `shared-cache` setting does not name answers an admitted
/// caller `NotSharedCache` through its own counted row, so the family is never unrouted and a node
/// named at runtime needs no component it did not already have.
///
/// **Admission is the node's one fold** (`Distributed::ExplainConnection`), never a list of its own
/// -- and then a narrower bar: a live key the connection PROVED or a ticket it presented
/// (`Distributed::RestsOnMachineKey`), never loopback alone and never `--fleet-open` alone. An open
/// fleet must not turn the fleet's shared objects into anyone's, and the named machine's own builds
/// do not need loopback here: they read the tier in process. A revoked key is `Forgotten` through
/// the fold, from any address, and refused as the forgotten machine.
///
/// Mirrors `CacheResponder` member for member otherwise, and the private tier's locality rule is
/// untouched: `Fetch` and `Store` never reach this component, and these verbs never reach that one.
class SharedCacheResponder final: public IFrameResponder
{
  public:
    /// @param host Whether this node serves, and the tier it serves from; must outlive this.
    /// @param membership The node's oracle, bound once; must outlive this.
    /// @param metrics Where a refusal is counted; must outlive this.
    SharedCacheResponder(SharedCacheHost const& host,
                         Distributed::IMembershipOracle const& membership,
                         IMetricsSink& metrics) noexcept:
        _host { host },
        _membership { membership },
        _metrics { metrics }
    {
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Re-asks its own gate, for `CacheResponder::Answer`'s reason: `Answer` is reachable directly.
    /// The tier is held for the whole answer, so a move of the setting mid-request retires it and
    /// never frees it under the reply.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// The one implementation of the admission rule; `Answer` calls it rather than repeating it, so
    /// the counter moves exactly once per refused request whichever path reached it. Admission
    /// first, then whether this node serves: a dormant node refuses an admitted caller at the door,
    /// so the endpoint steps over the body instead of reading it.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`, as the private tier answers: AUTH is the Session family's, and this surface is
    /// never routed one. The fold is the gate, and a credential every fleet node would need to
    /// hold is a shared secret -- which #178 removed from this fleet.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> payload) const override;

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// Which arm each decision takes is `Detail::SharedCachePrePayloadPolicy`.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// Which arm each refusal takes is `Detail::SharedCacheEndpointRefusals`.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// A round trip: the shared tier answers from its own store and reads through to nothing.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// The private tier's number, because the objects are the same objects.
    ///
    /// Folded into the listener's ceiling on EVERY node, a dormant one included, because any node
    /// may be named at runtime and must then take a STORE of the size a private tier would. The
    /// verbs' own `SessionCapGoverns` column is what keeps it off every control verb.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return SharedCacheMaxRequestBytes;
    }

    /// One kept session per fleet node, plus bursts.
    ///
    /// `MergedResponder` SUMS every-node owners' connection allowances, so this adds to the
    /// listener's descriptor ceiling on every node and must stay a fleet-sized number.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return SharedCacheMaxOpenConnections;
    }

    /// One request's worth, the private tier's reasoning: ordinary objects run in parallel, and a
    /// single largest one cannot be joined at all.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return SharedCacheMaxRequestBytes;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No: an answer is a lookup in one store, and the endpoint's budget is what accounts for it.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// None, for the private tier's reason: the interval in which a client could vanish unnoticed
    /// is the interval before the next statement.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// None: `Status::Progress` is not legal on these verbs.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// Not a stream: one request, one reply.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None.** The merged listener's node-proof component is the prover; this component READS
    /// what a connection proved, through the fold.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

    /// The largest request, and the whole in-flight budget: `CacheResponder`'s 256 MiB.
    static constexpr std::size_t SharedCacheMaxRequestBytes = 256ULL * 1024ULL * 1024ULL;

    /// Connections the component allows, added to the listener's on every node.
    static constexpr std::size_t SharedCacheMaxOpenConnections = 128;

  private:
    SharedCacheHost const& _host;
    Distributed::IMembershipOracle const& _membership;
    IMetricsSink& _metrics;
};

/// What every node builds for the fleet's shared cache: the directory that says where the shared
/// cache is, the disk opener over this node's configuration, the host that decides whether this
/// machine serves, and the component the merged listener routes the family to.
///
/// **One object because it is the production composition, and a test holds it to that.** It is
/// what builds a tier with `SharedTierProfile` at all, and `main.cpp` is in no test target: the
/// shared tier's counters were attributed to that profile before anything constructed one, so a
/// composition that stopped building it -- or built it with the private profile -- would leave
/// every one of them reading zero with nothing red. Built as a whole, it is testable as a whole.
///
/// Declared before the listener that routes to `Responder()`, before the consensus tier that feeds
/// `Directory()` and `Host()`, and before the private cache tier whose upstream reads both -- so it
/// is destroyed after all three: the host's thread stops and the tiers close once nothing can answer
/// from them or read them. The last of those is not left to the order alone: an in-process upstream
/// borrows the host (`SharedCacheHost::Borrow`), whose destructor ends the process by name if one
/// outlives it.
class SharedCacheService
{
  public:
    /// @param cfg The configuration: this node's id, the store's place and its ceiling; must outlive this.
    /// @param membership The node's oracle, bound once; must outlive this.
    /// @param clock Handed to every tier the host opens; must outlive this.
    /// @param metrics Where the tier and the component count; must outlive this.
    /// @param logger Where a transition or an unenforceable claim is reported; must outlive this.
    /// @param conditions Where `SharedCacheUnavailable` is answered, or nullptr on a node that runs
    ///        no consensus -- no cluster state reaches it, so it is never named.
    /// @param on Where the host reconciles: its own thread in production.
    SharedCacheService(NodeConfig const& cfg,
                       Distributed::IMembershipOracle const& membership,
                       core::platform::IClock& clock,
                       IMetricsSink& metrics,
                       ILogger& logger,
                       NodeConditions* conditions,
                       ReconcileOn on);

    SharedCacheService(SharedCacheService const&) = delete;
    SharedCacheService& operator=(SharedCacheService const&) = delete;
    SharedCacheService(SharedCacheService&&) = delete;
    SharedCacheService& operator=(SharedCacheService&&) = delete;
    ~SharedCacheService() = default;

    /// Where the shared cache is, as the applied state says; what the private tier's upstream reads.
    /// @return The directory.
    [[nodiscard]] SharedCacheDirectory& Directory() noexcept
    {
        return _directory;
    }

    /// What the consensus tier tells every applied state.
    /// @return The host.
    [[nodiscard]] SharedCacheHost& Host() noexcept
    {
        return _host;
    }

    /// What the merged listener routes `VerbFamily::SharedCache` to.
    /// @return The component.
    [[nodiscard]] IFrameResponder& Responder() noexcept
    {
        return _responder;
    }

  private:
    // Declaration order IS construction order: the host borrows the opener, the component the host.
    SharedCacheDirectory _directory;
    DiskSharedTierOpener _opener;
    SharedCacheHost _host;
    SharedCacheResponder _responder;
};

} // namespace FastCache::Node
