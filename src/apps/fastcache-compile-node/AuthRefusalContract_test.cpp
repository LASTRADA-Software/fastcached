// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "LocalCache.hpp"
#include "NodeAnnounce.hpp"
#include "NodeAudience.hpp"
#include "NodeConfig.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIoLoop.hpp"
#include "PrivateTierProfile.hpp"
#include "Responders.hpp"
#include "SessionResponder.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CacheProtocol.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/ISocket.hpp>
#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/SurfaceOwnerFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;

namespace
{
namespace Wire = CompileCacheWire;

// The scripted socket and `Replies` live in `src/tests/ScriptedSocket.hpp` (#362).
//
// The copy that used to sit here was deliberate, and its reason was that the point
// of this file is that these two binaries share NOTHING but the wire, so a fixture
// reaching across would be the coupling the test denies. That argument is about the
// two BINARIES and it still holds: the shared header is neither one's -- it is test
// infrastructure both borrow, exactly as they both borrow `Unwrap.hpp`, and it
// includes nothing from either app.

/// What THIS scheduler answers, asked at the layer that does not serve the verb.
///
/// `DispatchNotPermitted` rather than `UnimplementedVerb`, and the distinction is the
/// one the rulebook records twice (#283, #340). *Unimplemented* is not *served
/// elsewhere*: telling a client this verb is unknown would say the daemon is too OLD
/// when it is in fact too new, and the launcher would step over the refusal and
/// proceed unauthenticated -- holding a token it never presented, then refused every
/// gated verb behind a green build.
/// @return The refusal frame this build's `SchedulerProtocol` sends.
[[nodiscard]] std::vector<std::byte> SchedulerAnswersAuthDirectly()
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    // No `SetRole`: the refusal is answered before `Route`, so before any `Gate()`.
    // Leadership is not part of this contract, and a line setting it would tell a
    // reader it is.
    auto const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} };
    Distributed::SchedulerProtocol protocol { service, metrics };

    auto const auth = Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" });
    return protocol.Answer(auth, Distributed::CallerContext { .peerId = "127.0.0.1" });
}

} // namespace

TEST_CASE("A credentialled client reaches a node and still gets its answer", "[node][auth-contract]")
{
    // **The acceptance of #340, over the node as it is built**: a real listener, the real session
    // component answering `AUTH`, the real cache responder answering the command pipelined behind
    // it, and the real `Cc::CacheProtocol` on the other end of a real socket. The two binaries link
    // nothing in common -- `fastcache-cc` compiles `CompileCacheWire.hpp` in and links none of
    // `FastCache` -- so this is the one place both are present.
    //
    // The node checks no password, so the session component answers `AUTH` `NoPolicy`: `Ok`,
    // establishing nothing. The contract this file pins is the launcher's: a token-configured
    // client steps over whatever a node answers its credential with and is served the command it
    // actually sent -- never refused it, which is the green build distributing nothing.
    InMemoryLruStorage local { 64 * 1024 };
    Node::NoUpstream upstream;
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    Node::LocalCache cache { local, upstream, clock, metrics, Node::PrivateTierProfile };
    Node::CacheProxy proxy { cache, metrics };
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    CachedLocalityOracle const locality { machine, clock };
    Node::CacheResponder cacheResponder { proxy, locality, metrics };
    // The session component as `main` builds it, over a roster nobody is in: this case presents a
    // password, which the node holds none of, so what a ticket would establish is not its subject.
    Node::AnnouncedEndpoint const announced { "" };
    Node::NodeAudience const audience { announced, {}, {}, locality };
    Distributed::SpentTickets spent;
    Distributed::TicketVerifier const verifier { nullptr, audience, spent };
    Testing::ScriptedSecureRandom random;
    core::platform::ManualWallClock const wallClock;
    Node::SessionResponder session { verifier, Node::SessionKeys {}, random, wallClock, metrics };

    // Stored through the proxy, so the FETCH below has something to hit.
    auto const stored = std::vector<std::byte> { std::byte { 0x42 } };
    auto const store = core::async::syncRun(proxy.Answer(Wire::EncodeStore(
        Wire::StoreRequest { .key = "k", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = stored })));
    REQUIRE(Wire::DecodeReplyHeader(store).has_value());

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();
    Node::NodeConfig cfg;
    cfg.nodeListen = std::format("127.0.0.1:{}", port);
    cfg.slots = 0;

    Node::NodeIoLoop io;
    NullLogger logger;
    // Every other owner a built node has, as stand-ins: this case is about the cache verb and AUTH.
    Node::SurfaceFakes::EveryNodeOwners owners;
    auto surface = Node::StartNodeSurfaceOrExplain(
        io,
        cfg,
        owners.Around(Node::SurfaceComponents { .cache = &cacheResponder, .session = &session }),
        std::nullopt,
        metrics,
        logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    core::net::BlockingConnector connector;
    auto socket = core::async::syncRun(
        connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());

    std::vector<std::string> said;
    Cc::CredentialNotice notice { [&said](std::string_view text) { said.emplace_back(text); } };
    auto const outcome = core::async::syncRun(
        Cc::CacheFetch(socket->get(), &notice, "k", Cc::Credential { .username = {}, .secret = "s3cret" }));

    // The command behind the credential is served. This is the half that was broken.
    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);
    // `Ok` is what a node with no password answers, so nothing was ignored and nothing is said:
    // the notice is for a credential a server refused to check, which this one did not.
    CHECK_FALSE(outcome.credentialIgnored);
    CHECK(said.empty());
}

TEST_CASE("This scheduler refuses AUTH at the wrong layer without claiming it is unknown", "[node][auth-contract]")
{
    // The server half of the same contract, and the half that had no case at all
    // before #289 -- which is how the surface could have started serving `AUTH` while
    // still telling clients the verb was unknown, and nothing would have failed.
    //
    // `SchedulerProtocol` is asked directly here, which production never does: the
    // frame loop terminates `AUTH` because what it changes is per-connection state and
    // this class is deliberately stateless. So this pins the answer on a path only a
    // confused or older client takes, and the requirement is that it not LIE about
    // why -- `UnknownOpcode` would tell that client to give up on a daemon that is too
    // new rather than too old, and `UnimplementedVerb` would tell it to proceed
    // unauthenticated.
    auto const refusal = SchedulerAnswersAuthDirectly();
    auto const decoded = Wire::DecodeReplyHeader(refusal);
    REQUIRE(decoded.has_value());
    // `Unwrap`, not a bare `*decoded`: clang-tidy's optional analysis does not follow
    // Catch2's REQUIRE, so the deref reads as unchecked and the build fails.
    auto const header = Testing::Unwrap(decoded);
    REQUIRE(header.status == Wire::Status::Error);
    REQUIRE(header.payloadLength != 0);

    auto const code = static_cast<Wire::ErrorCode>(refusal[Wire::ReplyHeaderSize]);
    CHECK(code == Wire::ErrorCode::DispatchNotPermitted);

    // Stated as the byte too, because that is what a deployed launcher compares and
    // nobody here can recompile one. `UnimplementedVerb` is an alias for
    // `UnknownOpcode`, so asserting only the symbols would be a tautology the moment
    // somebody re-aliased it.
    CHECK(static_cast<std::uint8_t>(code) != 0x02);
}

TEST_CASE("A credentialled client reaches a cache tier that has no AUTH and still gets its answer", "[node][auth-contract]")
{
    // The third surface, and the one that started this. #283 corrected its refusal
    // code but asserted only the enumerator -- which is the assertion this file
    // exists to say is not enough, so leaving the cache tier without a behavioural
    // case would have shipped a rule with a counterexample beside it.
    //
    // Same contract, third server: the bytes come out of the real `CacheProxy` and go
    // into the real `Cc::CacheProtocol`.
    InMemoryLruStorage local { 64 * 1024 };
    Node::NoUpstream upstream;
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    Node::LocalCache cache { local, upstream, clock, metrics, Node::PrivateTierProfile };
    Node::CacheProxy proxy { cache, metrics };

    auto const refusal =
        core::async::syncRun(proxy.Answer(Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" })));
    REQUIRE_FALSE(refusal.empty());

    auto const stored = std::vector<std::byte> { std::byte { 0x9 } };
    Testing::ScriptedSocket socket { Testing::Replies({ refusal, Wire::EncodeReply(Wire::Status::Ok, stored) }) };

    std::vector<std::string> said;
    Cc::CredentialNotice notice { [&said](std::string_view text) { said.emplace_back(text); } };

    auto const outcome =
        core::async::syncRun(Cc::CacheFetch(&socket, &notice, "k", Cc::Credential { .username = {}, .secret = "s3cret" }));

    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);
    CHECK(outcome.credentialIgnored);
    CHECK(said.size() == 1);
}
