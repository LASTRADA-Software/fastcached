// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "Responders.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheResponder.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/platform/Clock.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SharedTierFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::AStoredObject;
using FastCache::Testing::ErrorOf;
using FastCache::Testing::MemoryOpener;
using FastCache::Testing::NamingSharedCache;
using FastCache::Testing::StatusOf;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A caller from a NON-loopback address that proved @p machine's key: the only kind of caller
/// whose admission says anything about the shared tier's own bar.
/// @param machine Whose key the connection proved.
/// @param host Where it dialled from.
/// @return The caller.
[[nodiscard]] PeerIdentity Proven(std::string const& machine, std::string host)
{
    return PeerIdentity { .host = std::move(host), .proven = Testing::IdentityOf(machine), .authenticatedMachine = {} };
}

/// A shared STORE of @p key.
/// @param key The key.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> SharedStoreOf(std::string const& key)
{
    return Wire::EncodeStoreAs(
        Wire::FleetSharedCacheVerbs,
        Wire::StoreRequest { .key = key, .prefetchGroup = {}, .srcRoot = {}, .buildTree = {}, .value = AStoredObject() });
}

struct Rig
{
    MemoryOpener opener;
    NullLogger logger;
    AtomicMetricsSink metrics;
    /// This machine and a key roster, the fold a node that is not open composes, over real oracles.
    Testing::RosterFold roster { { "pc-7", "cache-c" }, { "pc-gone" } };
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    SharedCacheResponder responder { host, roster.admitted, metrics };

    void Name(std::string const& machine)
    {
        host.Applied(NamingSharedCache(machine));
        host.Reconcile();
    }
};

constexpr auto SharedFetchByte = static_cast<std::uint8_t>(Wire::Op::SharedFetch);

} // namespace

TEST_CASE("The shared tier admits a proven fleet key and refuses an unproven caller", "[node][shared-cache][admission]")
{
    Rig rig;
    rig.Name("cache-c"); // serving, so the door's answer is admission's alone
    CHECK_FALSE(rig.responder.RefusePeer(Proven("pc-7", "10.0.0.7"), SharedFetchByte).has_value());

    auto const stranger = rig.responder.RefusePeer(
        PeerIdentity { .host = "10.0.0.8", .proven = {}, .authenticatedMachine = {} }, SharedFetchByte);
    REQUIRE(stranger.has_value());
    CHECK(ErrorOf(Unwrap(stranger)) == Wire::ErrorCode::NotAMember);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember) == 1);

    // A revoked key is refused as the forgotten machine, from any address, on its own counter.
    auto const gone = rig.responder.RefusePeer(Proven("pc-gone", "10.0.0.9"), SharedFetchByte);
    REQUIRE(gone.has_value());
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked) == 1);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember) == 1);

    // A verified TICKET is the other door: the key the machine presented rather than proved.
    auto const ticketed =
        PeerIdentity { .host = "10.0.0.7", .proven = {}, .authenticatedMachine = Testing::IdentityOf("pc-7") };
    CHECK_FALSE(rig.responder.RefusePeer(ticketed, SharedFetchByte).has_value());

    // And none of it is the private tier's series.
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 0);
}

TEST_CASE("The shared tier admits nobody on an open fleet or on loopback alone", "[node][shared-cache][admission]")
{
    // The fold says Member for both -- an open fleet, a local caller -- and the shared tier's bar is
    // higher: a proven key or a ticket. An open fleet must not turn the fleet's shared objects into
    // anyone's.
    AtomicMetricsSink metrics;
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };

    Testing::FixedMembership const open { Distributed::Membership::Member, Distributed::MembershipParticipant::OpenPolicy };
    SharedCacheResponder const openFleet { host, open, metrics };
    REQUIRE(
        openFleet.RefusePeer(PeerIdentity { .host = "10.0.0.8", .proven = {}, .authenticatedMachine = {} }, SharedFetchByte)
            .has_value());

    // A LISTED fake, because loopback admits this machine alone: a fixed `Member` labelled
    // `Loopback` would claim that route admits every host (`RequireHonestLabel`).
    Testing::ListedMembership const local { { "127.0.0.1" }, Distributed::MembershipParticipant::Loopback };
    SharedCacheResponder const loopback { host, local, metrics };
    REQUIRE(
        loopback.RefusePeer(PeerIdentity { .host = "127.0.0.1", .proven = {}, .authenticatedMachine = {} }, SharedFetchByte)
            .has_value());

    CHECK(metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember) == 2);
}

TEST_CASE("A dormant node answers a proven member's fleet verb not-shared-cache", "[node][shared-cache][admission]")
{
    Rig rig;
    rig.Name("cache-d"); // somebody else is the shared cache
    auto const reply = core::async::syncRun(
        rig.responder.Answer(Wire::EncodeFetchAs(Wire::FleetSharedCacheVerbs, "k"), Proven("pc-7", "10.0.0.7")));
    CHECK(ErrorOf(reply.bytes) == Wire::ErrorCode::NotSharedCache);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotServing) == 1);
    // Not a membership refusal: the caller IS a member, and being told otherwise is a confident wrong signal.
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember) == 0);

    // And at the DOOR, before a payload is read: the endpoint steps over an object this node will
    // not keep rather than buffering it.
    auto const door = rig.responder.RefusePeer(Proven("pc-7", "10.0.0.7"), static_cast<std::uint8_t>(Wire::Op::SharedStore));
    REQUIRE(door.has_value());
    CHECK(ErrorOf(Unwrap(door)) == Wire::ErrorCode::NotSharedCache);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotServing) == 2);
}

TEST_CASE("The named node stores and answers the fleet verbs on its shared tier", "[node][shared-cache][admission]")
{
    Rig rig;
    rig.Name("cache-c");
    auto const peer = Proven("pc-7", "10.0.0.7");
    auto const stored = core::async::syncRun(rig.responder.Answer(SharedStoreOf("k"), peer));
    CHECK(StatusOf(stored.bytes) == Wire::Status::Ok);
    auto const fetched =
        core::async::syncRun(rig.responder.Answer(Wire::EncodeFetchAs(Wire::FleetSharedCacheVerbs, "k"), peer));
    CHECK(StatusOf(fetched.bytes) == Wire::Status::Ok);
    CHECK(rig.opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 1);
}

TEST_CASE("The merged listener routes the fleet verbs to the shared component and FETCH to nobody else's",
          "[node][shared-cache][admission]")
{
    // The router, not the component: a stranger's SharedFetch is refused by the SHARED component's
    // fold and counted there, while a FETCH on a node whose private tier is absent reaches no shared
    // code at all -- the verb is the policy, and one opcode is never routed two ways.
    Rig rig;
    MergedResponder merged { SurfaceComponents { .sharedCache = &rig.responder } };
    auto const stranger = PeerIdentity { .host = "10.0.0.8", .proven = {}, .authenticatedMachine = {} };

    REQUIRE(merged.RefusePeer(stranger, SharedFetchByte).has_value());
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember) == 1);

    std::ignore = merged.RefusePeer(stranger, static_cast<std::uint8_t>(Wire::Op::Fetch));
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember) == 1);

    // And every built node has the component: the family's presence is every node, not "when it runs".
    CHECK(FamilyRoutes[static_cast<std::size_t>(Wire::VerbFamily::SharedCache)].presence
          == FamilyPresence::OnEveryBuiltNode);
}

TEST_CASE("The shared component counts its own frame ceiling and byte budget and never the private tier's",
          "[node][shared-cache][admission]")
{
    // Which ROW moved, for each refusal the endpoint decides: a fleet's STOREs running into the
    // named machine's budget are a different event from a local build running into its own.
    Rig rig;
    auto const tooLarge = rig.responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, SharedFetchByte, {});
    CHECK(ErrorOf(tooLarge) == Wire::ErrorCodeFor(Wire::PrePayloadDecision::PayloadTooLarge));
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedPayloadTooLarge) == 1);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedPayloadTooLarge) == 0);

    auto const busy = rig.responder.EndpointRefusalReply(EndpointRefusal::InFlightBudget, SharedFetchByte, {});
    CHECK(ErrorOf(busy) == ErrorCodeFor(EndpointRefusal::InFlightBudget));
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedEndpointBusy) == 1);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedEndpointBusy) == 0);

    // The listener's ceilings do not move where a private tier is served: the same objects.
    CHECK(rig.responder.MaxRequestBytes() == 256ULL * 1024ULL * 1024ULL);
    CHECK(rig.responder.MaxInFlightBytes() == 256ULL * 1024ULL * 1024ULL);
}

TEST_CASE("Every node's shared-cache service serves the fleet verbs from a disk tier counted as the shared tier",
          "[node][shared-cache][wiring]")
{
    // The PRODUCTION composition, which `main` builds on every node: a disk opener over this node's
    // configuration, the host and the component. It is what constructs a tier with the shared
    // profile at all -- the shared counters were attributed to that profile before anything built
    // one -- so a composition that stopped building it, or built it with the private profile,
    // fails here rather than in an operator's graph.
    Testing::ScratchDirectory const scratch { "node-shared-cache-service" };
    NodeConfig cfg;
    cfg.nodeId = "cache-c";
    cfg.clusterDir = scratch.Path();
    core::platform::ManualClock clock {};
    AtomicMetricsSink metrics;
    NullLogger logger;
    Testing::RosterFold roster { { "pc-7", "cache-c" } };
    SharedCacheService service { cfg, roster.admitted, clock, metrics, logger, nullptr, ReconcileOn::Caller };
    auto const peer = Proven("pc-7", "10.0.0.7");

    // Built dormant: an admitted caller is told this machine is not the shared cache.
    auto const dormant = core::async::syncRun(service.Responder().Answer(SharedStoreOf("k"), peer));
    CHECK(ErrorOf(dormant.bytes) == Wire::ErrorCode::NotSharedCache);
    CHECK_FALSE(std::filesystem::exists(SharedCacheStorePath(cfg)));

    service.Host().Applied(NamingSharedCache("cache-c"));
    service.Host().Reconcile();
    REQUIRE(service.Host().Status().serving);
    CHECK(std::filesystem::exists(SharedCacheStorePath(cfg)));

    auto const stored = core::async::syncRun(service.Responder().Answer(SharedStoreOf("k"), peer));
    CHECK(StatusOf(stored.bytes) == Wire::Status::Ok);
    auto const fetched =
        core::async::syncRun(service.Responder().Answer(Wire::EncodeFetchAs(Wire::FleetSharedCacheVerbs, "k"), peer));
    CHECK(StatusOf(fetched.bytes) == Wire::Status::Ok);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheHits) == 0);

    // The shared profile answers only its own verbs: the private tier's FETCH, handed to this tier,
    // is refused as served by the other one.
    auto const twin = core::async::syncRun(service.Responder().Answer(Wire::EncodeFetch("k"), peer));
    CHECK(ErrorOf(twin.bytes) == Wire::ErrorCode::DispatchNotPermitted);
}
