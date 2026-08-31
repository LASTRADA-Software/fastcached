// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "LocalCache.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <CacheProtocol.hpp>
#include <tests/ScriptedSocket.hpp>
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

/// What a scheduler that does not implement AUTH answers one with.
///
/// **Hand-written since #289, and the reversal needs its reasoning kept.** This was
/// produced by the production `SchedulerProtocol` on the argument that a literal
/// would pass while the server sent something else. That argument was right for as
/// long as this server was the one that sent it -- and #289 ended that: the scheduler
/// surface now terminates `AUTH` in `FrameServer`'s loop, so `SchedulerProtocol`
/// never sees the verb in production and answers `DispatchNotPermitted` when asked
/// directly, which `SchedulerAnswersAuthNotPermitted` below pins separately.
///
/// So the fixture had to become a literal or the case had to go, and the case is
/// worth keeping: what it regresses is a property of the **client**, not of this
/// server. `Cc::CacheProtocol::Exchange` must step over `UnimplementedVerb` and
/// proceed, and it must keep doing so for every scheduler that predates #289 --
/// which is every launcher and every node an operator has not upgraded yet.
///
/// Written as the byte and not only the symbol, per the rulebook: a wire constant has
/// two facts, and a spelling both ends share can only test the first.
/// @return The refusal frame a pre-#289 scheduler sends.
[[nodiscard]] std::vector<std::byte> SchedulerAuthRefusal()
{
    static_assert(static_cast<std::uint8_t>(Wire::UnimplementedVerb) == 0x02,
                  "a deployed launcher tolerates 0x02 and cannot be recompiled from here");
    return Wire::EncodeErrorReply(Wire::UnimplementedVerb, "this endpoint schedules and checks no credential");
}

/// The other half: what THIS scheduler answers, asked at the layer that no longer
/// serves the verb.
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
    ManualClock clock;
    ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    // No `SetRole`: the refusal is answered before `Route`, so before any `Gate()`.
    // Leadership is not part of this contract, and a line setting it would tell a
    // reader it is.
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, {} };
    Distributed::SchedulerProtocol protocol { service };

    auto const auth = Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" });
    return protocol.Answer(auth, Distributed::CallerContext { .peerId = "127.0.0.1" });
}

} // namespace

TEST_CASE("A credentialled client reaches a scheduler that has no AUTH and still gets its answer", "[node][auth-contract]")
{
    // **The acceptance of #340, and deliberately not "the refusal code changed".**
    // That assertion passes the moment a constant is edited; this one fails unless
    // the two binaries actually agree, because the bytes come out of the real
    // `SchedulerProtocol` and go into the real `Cc::CacheProtocol`.
    //
    // The two link nothing in common -- `fastcache-cc` compiles `CompileCacheWire.hpp`
    // in and links none of `FastCache` -- so the enumerator each names is the only
    // thing holding them together, and this is the only place both are present.
    //
    // What it regresses: with `FASTCACHE_TOKEN` set, a scheduler answering AUTH with
    // `DispatchNotPermitted` had that refusal returned in place of the answer to the
    // request the client actually sent. Every LEASE was declined, every compile
    // happened locally, and the build went green while the fleet distributed nothing.
    auto const stored = std::vector<std::byte> { std::byte { 0x42 } };
    Testing::ScriptedSocket socket { Testing::Replies(
        { SchedulerAuthRefusal(), Wire::EncodeReply(Wire::Status::Ok, stored) }) };

    std::vector<std::string> said;
    Cc::CredentialNotice notice { [&said](std::string_view text) { said.emplace_back(text); } };

    auto const outcome =
        SyncRun(Cc::CacheFetch(&socket, &notice, "k", Cc::Credential { .username = {}, .secret = "s3cret" }));

    // The command behind the credential is served. This is the half that was broken.
    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);

    // And the operator is still told their token went unchecked. A surface that
    // silently does less than it was configured to is the failure this codebase keeps
    // a list about -- restoring the answer must not also swallow that.
    CHECK(outcome.credentialIgnored);
    // Said once, through the notice the exchange carries -- the property #363 adds.
    CHECK(said.size() == 1);
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
    ManualClock clock;
    AtomicMetricsSink metrics;
    Node::LocalCache cache { local, upstream, clock, metrics };
    Node::CacheProxy proxy { cache };

    auto const refusal = SyncRun(proxy.Answer(Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" })));
    REQUIRE_FALSE(refusal.empty());

    auto const stored = std::vector<std::byte> { std::byte { 0x9 } };
    Testing::ScriptedSocket socket { Testing::Replies({ refusal, Wire::EncodeReply(Wire::Status::Ok, stored) }) };

    std::vector<std::string> said;
    Cc::CredentialNotice notice { [&said](std::string_view text) { said.emplace_back(text); } };

    auto const outcome =
        SyncRun(Cc::CacheFetch(&socket, &notice, "k", Cc::Credential { .username = {}, .secret = "s3cret" }));

    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);
    CHECK(outcome.credentialIgnored);
    CHECK(said.size() == 1);
}
