// SPDX-License-Identifier: Apache-2.0
#include "FleetSummaryResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Cluster/FleetSummarySignature.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using FastCache::Testing::ErrorOf;
using FastCache::Testing::StatusOf;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// Answer @p frame the way the endpoint would, synchronously.
/// @param responder Who answers.
/// @param frame The request.
/// @param peer Who asked.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> AnswerSync(IFrameResponder& responder,
                                                std::vector<std::byte> const& frame,
                                                PeerIdentity const& peer)
{
    return core::async::syncRun(responder.Answer(frame, peer)).bytes;
}

/// The payload of an `Ok` reply, refusing any other.
/// @param bytes The reply.
/// @return What follows the header.
[[nodiscard]] std::span<std::byte const> OkPayloadOf(std::vector<std::byte> const& bytes)
{
    REQUIRE(StatusOf(bytes) == std::optional { Wire::Status::Ok });
    return std::span { bytes }.subspan(Wire::ReplyHeaderSize);
}

/// A nonce of @p fill, as an asker sends it.
/// @param fill Every byte of it.
/// @return The nonce.
[[nodiscard]] std::array<std::byte, Wire::NodeChallengeBytes> NonceOf(std::byte fill)
{
    auto nonce = std::array<std::byte, Wire::NodeChallengeBytes> {};
    nonce.fill(fill);
    return nonce;
}

/// A stranger: on no list, holding no credential, which is who a seed answers.
/// @return Its identity.
[[nodiscard]] PeerIdentity Stranger()
{
    return PeerIdentity { .host = "203.0.113.9" };
}
} // namespace

TEST_CASE("Any caller is answered a signed fleet summary", "[node][formation][summary]")
{
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office",
                                                         .state = FleetState::Established,
                                                         .leaderNodeEndpoint = "office:6674",
                                                         .nodeId = "n-office" } };
    auto const identity = Testing::TestKeyPair("n-office");
    FleetSummaryResponder responder { self, identity };

    CHECK_FALSE(responder.RefusePeer(Stranger(), static_cast<std::uint8_t>(Wire::Op::FleetSummary)).has_value());
    // Answered before authentication: the verb's own `OpTable` row, which the endpoint reads.
    CHECK(Wire::IsPreAuthAllowed(static_cast<std::uint8_t>(Wire::Op::FleetSummary)));

    auto fresh = Testing::IssuedNonceOf(std::byte { 9 });
    auto const nonce = fresh.wire;
    auto const reply = AnswerSync(responder, Wire::EncodeFleetSummaryRequest(nonce), Stranger());
    auto const decoded = Wire::DecodeFleetSummaryReply(OkPayloadOf(reply));
    REQUIRE(decoded.has_value());
    auto const proven =
        Cluster::ProvenFleet::FromSeedAnswer(std::move(fresh.held), Unwrap(decoded), Cluster::SeedSource::FleetSeedFlag);
    REQUIRE(proven.has_value());
    CHECK(Unwrap(proven).Summary() == self.Current());
    CHECK(Unwrap(proven).Key() == identity.PublicKey());

    // Read afresh per answer: a node whose fleet changed answers the change.
    self.Set(FleetSummary { .clusterId = "c-other", .state = FleetState::Solitary, .nodeId = "n-office" });
    auto const again = Wire::DecodeFleetSummaryReply(
        OkPayloadOf(AnswerSync(responder, Wire::EncodeFleetSummaryRequest(nonce), Stranger())));
    REQUIRE(again.has_value());
    CHECK(Unwrap(again).summary.clusterId == "c-other");
}

TEST_CASE("A node with no fleet to offer refuses NoCluster rather than signing one", "[node][formation][summary]")
{
    auto const identity = Testing::TestKeyPair("n-worker");
    auto const nonce = NonceOf(std::byte { 5 });

    // A node that runs no consensus belongs to no cluster.
    Testing::ScriptedSummarySource worker { FleetSummary {} };
    FleetSummaryResponder noCluster { worker, identity };
    CHECK(ErrorOf(AnswerSync(noCluster, Wire::EncodeFleetSummaryRequest(nonce), Stranger()))
          == std::optional { Wire::ErrorCode::NoCluster });

    // One whose consensus address reaches only itself offers its fleet to nobody: the asker would
    // dial itself.
    Testing::ScriptedSummarySource local { FleetSummary {
        .clusterId = "c-local", .state = FleetState::Solitary, .nodeId = "n-local", .raftEndpoint = "127.0.0.1:6680" } };
    FleetSummaryResponder loopback { local, identity };
    CHECK(ErrorOf(AnswerSync(loopback, Wire::EncodeFleetSummaryRequest(nonce), Stranger()))
          == std::optional { Wire::ErrorCode::NoCluster });

    // The control: the same node at an address others reach is answered.
    local.Set(FleetSummary {
        .clusterId = "c-local", .state = FleetState::Solitary, .nodeId = "n-local", .raftEndpoint = "10.0.0.7:6680" });
    CHECK(StatusOf(AnswerSync(loopback, Wire::EncodeFleetSummaryRequest(nonce), Stranger()))
          == std::optional { Wire::Status::Ok });
}

TEST_CASE("A fleet-summary question that is not one nonce is refused as malformed", "[node][formation][summary]")
{
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .nodeId = "n-office" } };
    auto const identity = Testing::TestKeyPair("n-office");
    FleetSummaryResponder responder { self, identity };

    auto const shortNonce = std::array<std::byte, Wire::NodeChallengeBytes - 1> {};
    auto const frame = Wire::Detail::EncodeRequest(
        Wire::CurrentVersion, Wire::Op::FleetSummary, { std::span<std::byte const> { shortNonce } });
    CHECK(ErrorOf(AnswerSync(responder, frame, Stranger())) == std::optional { Wire::ErrorCode::MalformedFrame });
}

TEST_CASE("The merged listener routes FleetSummary to the formation component on every built node",
          "[node][formation][summary][merged-responder]")
{
    // W2 left the family ownerless, so every node answered it UnimplementedVerb. It is now an
    // every-node family: a node with a formation component answers, whatever else it runs.
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .nodeId = "n-office" } };
    auto const identity = Testing::TestKeyPair("n-office");
    FleetSummaryResponder formation { self, identity };

    CHECK(FamilyOwner(SurfaceComponents { .formation = &formation }, Wire::VerbFamily::Formation) == &formation);
    CHECK(AnswersAnyFamily(SurfaceComponents { .formation = &formation }));

    MergedResponder merged { SurfaceComponents { .formation = &formation } };
    auto const nonce = NonceOf(std::byte { 7 });
    auto const reply = AnswerSync(merged, Wire::EncodeFleetSummaryRequest(nonce), Stranger());
    CHECK(StatusOf(reply) == std::optional { Wire::Status::Ok });
    CHECK_FALSE(merged.RefusePeer(Stranger(), static_cast<std::uint8_t>(Wire::Op::FleetSummary)).has_value());

    // Its connection allowance ADDS to the other every-node owners', as theirs do.
    CHECK(merged.MaxOpenConnections() == FleetSummaryResponder::OpenConnections);
}
