// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "CacheTier.hpp"
#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "DiscoveryTier.hpp"
#include "EndpointDialer.hpp"
#include "EnrollmentResponder.hpp"
#include "EnrollmentWindow.hpp"
#include "FleetProbe.hpp"
#include "FleetSummaryResponder.hpp"
#include "FleetTextResponder.hpp"
#include "LiveStatsResponder.hpp"
#include "MachineStandingTestUtils.hpp"
#include "MembershipGate.hpp"
#include "NodeAnnounce.hpp"
#include "NodeAudience.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeFormation.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIoLoop.hpp"
#include "NodeMembership.hpp"
#include "NodeProofResponder.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeSurfaces.hpp"
#include "PrivateTierProfile.hpp"
#include "Responders.hpp"
#include "SchedulerTier.hpp"
#include "SessionResponder.hpp"
#include "SharedCacheResponder.hpp"
#include "WorkerTierTestFixture.hpp"

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>
#include <FastCache/Protocol/SessionContext.hpp>
#include <FastCache/Server/AdminCredential.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/net/TcpClient.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/platform/Clock.hpp>

#if !defined(_WIN32)
    #include <sys/socket.h>

    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/HalfClose.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/LocalityFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/SurfaceOwnerFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

// Shared rather than spelled here: this was one of five private copies of the same
// reply readers, byte-identical once the `Wire` alias is expanded. `MessageOf` below
// stays local -- it reads the error payload's TEXT, which is a different question the
// shared header deliberately does not answer.
using FastCache::Testing::ErrorOf;
using FastCache::Testing::StatusOf;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

using SurfaceFakes::EveryNodeOwners;
using SurfaceFakes::NamedResponder;

/// The message an error reply carries, or nothing when the frame is not one.
///
/// An error payload is one code byte and then the message, which is what makes both
/// of these two-line readers rather than a decoder call.
/// @param reply An encoded reply frame.
/// @return Its message.
[[nodiscard]] std::string MessageOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != Wire::Status::Error || header->payloadLength < 2)
        return {};
    auto const text = reply.subspan(Wire::ReplyHeaderSize + 1, header->payloadLength - 1);
    return std::string { reinterpret_cast<char const*>(text.data()), text.size() };
}

/// Send one request on a real connection and read its whole reply.
/// @param socket The connection.
/// @param request The encoded request.
/// @return The reply frame, header included.
[[nodiscard]] std::vector<std::byte> Exchange(core::net::ISocket* socket, std::span<std::byte const> request)
{
    REQUIRE(core::async::syncRun(core::net::sendAll(socket, request)));
    auto const head = core::async::syncRun(core::net::receiveExactly(socket, Wire::ReplyHeaderSize));
    REQUIRE(head.has_value());
    auto const header = Wire::DecodeReplyHeader(Unwrap(head));
    REQUIRE(header.has_value());
    auto reply = Unwrap(head);
    if (Unwrap(header).payloadLength > 0)
    {
        auto const payload = core::async::syncRun(core::net::receiveExactly(socket, Unwrap(header).payloadLength));
        REQUIRE(payload.has_value());
        reply.insert(reply.end(), Unwrap(payload).begin(), Unwrap(payload).end());
    }
    return reply;
}

/// One request header with no payload.
/// @param op The verb.
/// @return The encoded frame.
[[nodiscard]] std::vector<std::byte> HeaderFor(Wire::Op op)
{
    std::vector<std::byte> frame(Wire::RequestHeaderSize);
    WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(op), 0);
    return frame;
}

/// Drive one `Answer` to completion; the fakes never suspend.
/// @param responder Who to ask.
/// @param frame The request.
/// @return The reply.
[[nodiscard]] std::vector<std::byte> AnswerNow(MergedResponder& responder, std::span<std::byte const> frame)
{
    return core::async::syncRun(responder.Answer(frame, PeerIdentity { .host = "127.0.0.1" })).bytes;
}

/// A config naming a free loopback port, and that port.
///
/// Per run rather than fixed: `catch_discover_tests` gives every case its own process
/// and the suite runs in parallel, so a chosen number is a failure that appears only
/// under `ctest -j`. The port is returned beside the config because two cases have to
/// take it before the surface does.
/// @return The config and the port it names.
[[nodiscard]] std::pair<NodeConfig, std::uint16_t> BaseConfig()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    // Asked of the SOCKET, not of the pointer: `Bind` returns a listener in an errored
    // state rather than nothing, so a null check passes on a bind that failed and the
    // port below comes back 0.
    REQUIRE(probe->IsBound());
    auto const port = probe->boundPort();
    probe.reset();

    NodeConfig cfg;
    cfg.nodeListen = std::format("127.0.0.1:{}", port);
    return { cfg, port };
}

/// Whether any captured line contains @p needle.
/// @param logger Where the surface reported.
/// @param needle Text to look for.
/// @return True when some line contains it.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view needle)
{
    auto const records = logger.Snapshot();
    return std::ranges::any_of(records, [needle](CapturingLogger::Record const& r) { return r.message.contains(needle); });
}

} // namespace

TEST_CASE("Each verb family reaches the component that owns it", "[node][merged-responder]")
{
    // The whole of what the merge replaced. The listener a frame arrived on used to BE
    // the routing decision -- a frame on the cache port was a cache frame -- and one
    // listener cannot decide that by existing.
    NamedResponder cache { "cache" };
    NamedResponder scheduler { "scheduler" };
    NamedResponder session { "session" };
    MergedResponder responder { SurfaceComponents { .cache = &cache, .scheduler = &scheduler, .session = &session } };

    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Fetch))) == "cache");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Store))) == "cache");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Lease))) == "scheduler");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Register))) == "scheduler");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::ClusterStatus))) == "scheduler");

    // AUTH is the Session family's, which is its own component on every node: the node holds
    // no password, and what an AUTH establishes must not depend on whether it schedules.
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Auth))) == "session");

    CHECK(cache.Answered().size() == 2);
    CHECK(scheduler.Answered().size() == 3);
    CHECK(session.Answered().size() == 1);
}

TEST_CASE("A progress cadence is routed to the surface that does the slow work", "[node][merged-responder][progress]")
{
    // Assert the wiring, for the reason the peer-watch case below states. Whether a verb
    // is slow enough to owe its client a liveness signal is a property of the VERB and
    // of the surface doing its work -- and a merged listener that folded or hard-coded
    // the answer would either pulse at a client reading a cache reply, which is a frame
    // that verb's status table does not even admit, or pulse at nobody on the one
    // surface that needs it (#245).
    NamedResponder cache { "cache" };
    NamedResponder scheduler { "scheduler" };
    NamedResponder compile { "compile" };
    compile.SetProgressInterval(std::chrono::milliseconds { 250 });

    MergedResponder responder { SurfaceComponents { .cache = &cache, .scheduler = &scheduler, .compile = &compile } };

    CHECK(responder.ProgressInterval(static_cast<std::uint8_t>(Wire::Op::Compile))
          == std::optional { std::chrono::milliseconds { 250 } });

    // And nowhere else. Both directions, because a router that answered the compile
    // surface's cadence for every verb would pass an assertion about `Op::Compile`
    // alone.
    CHECK_FALSE(responder.ProgressInterval(static_cast<std::uint8_t>(Wire::Op::Fetch)).has_value());
    CHECK_FALSE(responder.ProgressInterval(static_cast<std::uint8_t>(Wire::Op::Store)).has_value());
    CHECK_FALSE(responder.ProgressInterval(static_cast<std::uint8_t>(Wire::Op::Lease)).has_value());

    // A verb nobody owns is not pulsed. Unreachable -- `RefusePeer` has already refused
    // it -- and not-pulsing is the answer that changes nothing.
    CHECK_FALSE(responder.ProgressInterval(0xEE).has_value());
}

TEST_CASE("A peer watch is routed to the surface whose work it would abandon", "[node][merged-responder][peerwatch]")
{
    // Assert the wiring. The counter a watch raises belongs to the surface whose work
    // was abandoned, and nothing but this router carries it there -- a fold, or a
    // constant, would file every surface's abandoned deliveries under whichever one
    // happened to arm a watch, and the endpoint could not tell.
    NamedResponder cache { "cache" };
    NamedResponder scheduler { "scheduler" };
    NamedResponder compile { "compile" };
    compile.SetPeerWatchCounter(IMetricsSink::Counter::WorkerJobsAbandonedClientGone);
    MergedResponder responder { SurfaceComponents { .cache = &cache, .scheduler = &scheduler, .compile = &compile } };

    CHECK(responder.PeerWatchCounter(static_cast<std::uint8_t>(Wire::Op::Compile))
          == std::optional { IMetricsSink::Counter::WorkerJobsAbandonedClientGone });

    // And the surfaces that answer from memory are not watched: a watch that is armed
    // and does not fire ends the connection, which is what the cache surface must not
    // pay (#176).
    CHECK_FALSE(responder.PeerWatchCounter(static_cast<std::uint8_t>(Wire::Op::Fetch)).has_value());
    CHECK_FALSE(responder.PeerWatchCounter(static_cast<std::uint8_t>(Wire::Op::Lease)).has_value());

    // A verb nobody owns is not watched. It is unreachable -- `RefusePeer` has already
    // refused it -- and not-watching is the answer that changes nothing.
    CHECK_FALSE(responder.PeerWatchCounter(0xEE).has_value());
}

TEST_CASE("(#290) one peer on one listener has a FETCH refused and a COMPILE admitted",
          "[node][merged-responder][cache-locality]")
{
    // **#290's acceptance criterion.** The ticket states it as
    //
    //     a cache FETCH from another machine is refused on the merged wildcard port
    //     while a compile from that same peer succeeds
    //
    // -- same peer, same listener, two verbs, two answers. That is the property the
    // merge could silently break, because before it the LISTENER was the policy: a
    // frame on the cache port was a cache frame, and "who may do this" was answered by
    // which socket it arrived on. One socket cannot answer that by existing.
    //
    // **Both components are the production ones.** The pieces are proven separately --
    // `CacheProxy_test` pins the locality refusal and its counter, `CompileResponder_test`
    // the admission -- and what no other case has is the CONTRAST: the two rules
    // disagreeing about one peer, reached through the router that has to keep them
    // apart. A fake responder cannot show it; `One peer is refused one verb and served
    // another on the same listener` uses `RefuseOnlyVerb` and says so, which proves the
    // seam can carry the question rather than that the real rules produce it.
    //
    // The peer is `10.0.0.1`, admitted by a verified ticket for a key the roster holds, and
    // the two things that make this case mean anything are asserted rather than assumed: it
    // IS an admitted member, so the FETCH refusal cannot be membership, and it is NOT this
    // machine, so the refusal is locality.
    // Never invoked. This case stops at the peer gate, which is decided from the
    // caller's host before a payload byte is read -- so no compiler is spawned and a
    // runner that refuses to spawn is the honest stand-in for one that is not asked.
    struct NeverSpawns final: Cc::IProcessRunner
    {
        Cc::CompileRun RunCaptureCombined(std::span<std::string const> /*argv*/) override
        {
            return Cc::CompileRun { .exitCode = Cc::NotSpawned, .out = {}, .err = {} };
        }
        Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
        {
            return RunCaptureCombined(argv);
        }
    };

    InMemoryLruStorage local { 64 * 1024 };
    NoUpstream upstream;
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    LocalCache cache { local, upstream, clock, metrics, PrivateTierProfile };
    CacheProxy proxy { cache, metrics };

    // This machine answers on 10.0.0.7, so 10.0.0.1 is somebody else. Injected because
    // the question is ambient: `ILocalityOracle` exists so a test can say which
    // addresses are this host's without the host having to have them.
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    CachedLocalityOracle const locality { machine, clock };

    // Admitted. Without this the FETCH would be refused for membership and the case
    // would pass having tested nothing about the merge -- and the compile would be
    // refused too, so there would be no contrast at all.
    Testing::RosterFold const fold { { "pc-01" } };
    auto const& membership = fold.admitted;
    auto const ticketed = ConnectionFacts { .host = "10.0.0.1", .authenticatedMachine = Testing::IdentityOf("pc-01") };
    REQUIRE(Distributed::ExplainConnection(membership, ticketed).verdict == Distributed::Membership::Member);
    REQUIRE_FALSE(locality.IsThisMachine("10.0.0.1"));

    NodeIoLoop io;
    CapturingLogger logger;
    NeverSpawns runner;
    FastCache::Testing::ScratchDirectory const scratch { "fc-290-acceptance" };
    Cc::CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, Cc::ToolchainSurvey::Completed() };
    Cc::WorkerProtocol protocol { jobs, Cc::UncheckedLeaseValidator(), { Wire::IdentityCodec }, metrics };
    core::async::ThreadPoolExecutor pool { 1 };
    CompileCapacity capacity { 1, WorkerMaxRequestBytes, std::chrono::seconds { 5 }, logger };

    CacheResponder cacheResponder { proxy, locality, metrics };
    CompileResponder compileResponder { protocol, capacity, membership, locality, pool, io.Reactor(), metrics, logger };
    MergedResponder responder { SurfaceComponents { .cache = &cacheResponder, .compile = &compileResponder } };

    // --- the cache verb: refused, and refused FOR LOCALITY ------------------------
    //
    // The REASON is asserted, not merely that it was refused: "was it refused" is
    // satisfied by any refusal at all, so a routing bug or a plain failure would pass
    // a weaker check.
    //
    // **And the code is not the reason either, on this surface specifically.** The two
    // rules this case exists to contrast BOTH answer `NotAMember` -- the cache refusing
    // a caller that is not this machine, and `RefuseUnlessMember` refusing a caller
    // with no claim on this machine's CPU. One code because a launcher steps over both
    // identically; two counters because an operator does not. Since #290 they arrive on
    // one socket, so the counter is the only thing here that says WHICH rule fired:
    // delete the increment in `CacheResponder::RefusePeer` and the code assertion below
    // still passes, with only the counter going red.
    auto const fetchRefusal = responder.RefusePeer(ticketed, static_cast<std::uint8_t>(Wire::Op::Fetch));
    REQUIRE(fetchRefusal.has_value());
    CHECK(ErrorOf(Unwrap(fetchRefusal)) == Wire::ErrorCode::NotAMember);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 1);

    // --- the compile verb: the SAME peer is admitted ------------------------------
    //
    // No refusal at the peer gate, which is as far as this layer decides: a lease and
    // a compiler are the next questions and belong to the fixture that has both.
    CHECK_FALSE(responder.RefusePeer(ticketed, static_cast<std::uint8_t>(Wire::Op::Compile)).has_value());

    // And the cache tier still answers THIS machine, which is the other direction of
    // the same rule and the one a widened bind is most likely to break in silence.
    CHECK_FALSE(
        responder
            .RefusePeer(PeerIdentity { .host = std::string { "127.0.0.1" } }, static_cast<std::uint8_t>(Wire::Op::Fetch))
            .has_value());
}

TEST_CASE("A verb no component serves is refused as unimplemented", "[node][merged-responder]")
{
    // Legitimate and common: a worker that neither caches nor schedules, and a
    // scheduler with no cache tier. `UnimplementedVerb` is the honest code -- this
    // endpoint really does not implement it -- and it is the one refusal
    // `Cc::CacheProtocol` steps over rather than treating as fatal, so a launcher that
    // meets it carries on and compiles.
    NamedResponder scheduler { "scheduler" };
    MergedResponder schedulerOnly { SurfaceComponents { .scheduler = &scheduler } };

    auto const fetch = AnswerNow(schedulerOnly, HeaderFor(Wire::Op::Fetch));
    CHECK(ErrorOf(fetch) == Wire::UnimplementedVerb);
    CHECK(scheduler.Answered().empty());

    // COMPILE is NOT refused that way, since #206 gave a node the means to run no worker
    // (`--slots=0`). The verb is not unimplemented on such a node, it is served by the
    // nodes that run one -- and `UnimplementedVerb` would tell whoever sent `--cordon`
    // here that this build is too old to know the verb. It gets the code the daemon
    // answers a cordon with for the same fact, so the two endpoints give one remedy.
    // Both verbs of the family, because the cordon is the one an operator actually sends.
    NamedResponder cache { "cache" };
    MergedResponder both { SurfaceComponents { .cache = &cache, .scheduler = &scheduler } };
    CHECK(ErrorOf(AnswerNow(both, HeaderFor(Wire::Op::Compile))) == Wire::ErrorCode::DispatchNotPermitted);
    CHECK(ErrorOf(AnswerNow(both, HeaderFor(Wire::Op::Cordon))) == Wire::ErrorCode::DispatchNotPermitted);

    // **And none of them is counted, which was decided rather than left out** (#447).
    // Every other refusal on this listener is an event; this one is the answer ordinary
    // traffic gets. A worker with no scheduler refuses every `AUTH` a `FASTCACHE_TOKEN`
    // launcher sends, once per exchange for a whole build, and a node with no tier
    // refuses every local `FETCH` -- so a counter here would be dominated by a healthy
    // build and a port scan would be invisible inside it. That is this ticket's own
    // failure reached from the other side: a series nothing can be read out of is no
    // better than one that never moves.
}

TEST_CASE("The daemon and a node running no worker refuse a cordon with one code and one fact", "[node][merged-responder]")
{
    // #206. Two endpoints meet the same question -- a `--cordon` aimed at a machine that
    // compiles nothing -- and one condition must not reach a client as two codes, or as
    // two different facts. Both tables are checked against `Wire::NoCompileWorker` when
    // they compile; this asks the SURFACES, on the wire, because a refusal is decided by
    // the call that sends it and a table nothing routes to asserts nothing.
    auto const cordon = Wire::EncodeCordonRequest(Wire::CordonAction::Cordon);

    // The daemon, which is a cache.
    core::platform::ManualClock clock;
    InMemoryLruStorage storage { 0 };
    CacheEngine engine { storage, clock };
    auto const pair = core::net::testing::InMemorySocketPair::create();
    CompileCacheHandler daemon;
    REQUIRE(core::async::syncRun(core::net::sendAll(pair.client.get(), cordon)));
    REQUIRE(FastCache::Testing::ShutdownWrite(*pair.client).has_value());
    core::async::syncRun(daemon.Run(pair.server.get(), &engine, {}, SessionContext {}));
    // One framed reply, read as the header declares it: the header, then its payload.
    auto const daemonHead = core::async::syncRun(core::net::receiveExactly(pair.client.get(), Wire::ReplyHeaderSize));
    REQUIRE(daemonHead.has_value());
    auto const daemonHeader = Wire::DecodeReplyHeader(Unwrap(daemonHead));
    REQUIRE(daemonHeader.has_value());
    auto const daemonPayload =
        core::async::syncRun(core::net::receiveExactly(pair.client.get(), Unwrap(daemonHeader).payloadLength));
    REQUIRE(daemonPayload.has_value());
    auto daemonReply = Unwrap(daemonHead);
    daemonReply.insert(daemonReply.end(), Unwrap(daemonPayload).begin(), Unwrap(daemonPayload).end());

    // A node started with `--slots=0`, which builds no compile component.
    NamedResponder cache { "cache" };
    NamedResponder scheduler { "scheduler" };
    MergedResponder node { SurfaceComponents { .cache = &cache, .scheduler = &scheduler } };
    auto const nodeReply = AnswerNow(node, cordon);

    auto const daemonCode = ErrorOf(daemonReply);
    auto const nodeCode = ErrorOf(nodeReply);
    REQUIRE(daemonCode.has_value());
    REQUIRE(nodeCode.has_value());
    CHECK(daemonCode == nodeCode);

    // And the same FACT in words, each finished with its own endpoint's remedy.
    CHECK(MessageOf(daemonReply).starts_with(Wire::NoCompileWorker::Stem));
    CHECK(MessageOf(nodeReply).starts_with(Wire::NoCompileWorker::Stem));
    CHECK(MessageOf(daemonReply) != MessageOf(nodeReply));
}

TEST_CASE("An unowned verb is refused before its payload is read", "[node][merged-responder]")
{
    // At the door rather than in `Answer`, which is what keeps a verb this node serves
    // nowhere from costing the surface a buffer -- the property #285 is about, held for
    // the new refusal as well as for the old ones.
    NamedResponder scheduler { "scheduler" };
    MergedResponder schedulerOnly { SurfaceComponents { .scheduler = &scheduler } };

    auto const refusal = schedulerOnly.RefusePeer(PeerIdentity { .host = std::string { "10.0.0.1" } },
                                                  static_cast<std::uint8_t>(Wire::Op::Fetch));
    REQUIRE(refusal.has_value());
    CHECK(ErrorOf(Unwrap(refusal)) == Wire::UnimplementedVerb);

    // And an owned verb is still the owner's question to answer, not this one's.
    CHECK_FALSE(
        schedulerOnly
            .RefusePeer(PeerIdentity { .host = std::string { "10.0.0.1" } }, static_cast<std::uint8_t>(Wire::Op::Lease))
            .has_value());
    REQUIRE(scheduler.Admitted().size() == 1);
    CHECK(scheduler.Admitted().front() == static_cast<std::uint8_t>(Wire::Op::Lease));

    // The fourth route, and the one #447 added: an endpoint-decided refusal about a
    // verb nobody owns. One sentence however the question arrived -- a router asked
    // about a verb it cannot place has only the one honest answer, and giving it here
    // is what keeps a peer from being told its frame was too large for a verb that was
    // never going to be answered at all.
    auto const deadline =
        schedulerOnly.EndpointRefusalReply(EndpointRefusal::AnswerDeadline, static_cast<std::uint8_t>(Wire::Op::Fetch), {});
    CHECK(ErrorOf(deadline) == Wire::UnimplementedVerb);
    CHECK(scheduler.Refusals().empty());

    // Except the in-flight budget, the one endpoint refusal decided from a header nobody
    // verified: a SEALED frame over budget is refused before its tag is read, so its verb is
    // a byte that may name anything. The refusal is certain and the verb is not, so it is
    // answered by the owner whose budget ran out and counted on that owner's row -- the
    // unserved answer moved no counter at all. Busy is also the kinder wrong answer: it sends
    // a peer to retry, where it meets the unserved answer once the verb can be believed.
    auto const budget =
        schedulerOnly.EndpointRefusalReply(EndpointRefusal::InFlightBudget, static_cast<std::uint8_t>(Wire::Op::Fetch), {});
    CHECK(ErrorOf(budget) == Wire::ErrorCode::EndpointBusy);
    CHECK(scheduler.Refusals() == std::vector<std::string> { "scheduler" });
}

TEST_CASE("A refusal is counted against the component that owned the verb", "[node][merged-responder]")
{
    // A cache STORE that overran its ceiling counted against the scheduler names the
    // wrong subsystem, and naming the subsystem is what these counters are read for.
    NamedResponder cache { "cache" };
    NamedResponder scheduler { "scheduler" };
    MergedResponder responder { SurfaceComponents { .cache = &cache, .scheduler = &scheduler } };

    (void) responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, static_cast<std::uint8_t>(Wire::Op::Store), {});
    (void) responder.RefusalReply(Wire::PrePayloadDecision::Unauthenticated, static_cast<std::uint8_t>(Wire::Op::Lease), {});

    CHECK(cache.Refusals() == std::vector<std::string> { "cache" });
    CHECK(scheduler.Refusals() == std::vector<std::string> { "scheduler" });

    // An unowned verb still gets a reply, and it is no component's: there is nobody
    // whose refusal it would be, so neither fake sees it.
    //
    // **It says the verb is unserved rather than repeating the decision** (#447). This
    // arm is reachable -- the endpoint weighs its surface-wide frame ceiling before it
    // asks `RefusePeer`, so a header naming a verb nothing here serves and declaring a
    // gigabyte arrives at exactly this call -- and it used to answer
    // `payload-too-large`, which sends that peer to shrink a frame that was never going
    // to be answered at all.
    //
    // It moves no counter, and that is deliberate: see `UnservedReply`, and the case
    // above for why counting an answer ordinary traffic produces continuously would
    // bury the thing a counter here would be read for.
    //
    // The compile family's unserved answer is `DispatchNotPermitted` since #206 -- a node
    // running no worker serves those verbs elsewhere rather than not at all -- and it is
    // still the unserved answer, never the pre-payload decision this call was handed.
    auto const orphan =
        responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, static_cast<std::uint8_t>(Wire::Op::Compile), {});
    CHECK(ErrorOf(orphan) == Wire::ErrorCode::DispatchNotPermitted);
    CHECK(cache.Refusals().size() == 1);
    CHECK(scheduler.Refusals().size() == 1);
}

TEST_CASE("The session ceilings are the largest of the components present", "[node][merged-responder]")
{
    // Safe only because #284 made the per-verb ceiling a property of the wire table:
    // this is the SESSION cap, and the session cap governs exactly the three
    // payload-bearing verbs. Every scheduler verb declares its own kilobyte bound and
    // stays bounded on a surface whose session cap is the cache's megabytes.
    constexpr std::size_t CacheRequest = 256ULL * 1024ULL * 1024ULL;
    constexpr std::size_t CacheOpen = 512;
    constexpr std::size_t CacheInFlight = 64ULL * 1024ULL * 1024ULL;
    constexpr std::size_t SchedulerRequest = 64ULL * 1024ULL;
    constexpr std::size_t SchedulerOpen = 256;
    constexpr std::size_t SchedulerInFlight = 1024ULL * 1024ULL;

    NamedResponder cache { "cache" };
    cache.PlaceCeilings(CacheRequest, CacheOpen, CacheInFlight);
    NamedResponder scheduler { "scheduler" };
    scheduler.PlaceCeilings(SchedulerRequest, SchedulerOpen, SchedulerInFlight);

    MergedResponder both { SurfaceComponents { .cache = &cache, .scheduler = &scheduler } };
    CHECK(both.MaxRequestBytes() == CacheRequest);
    CHECK(both.MaxInFlightBytes() == CacheInFlight);
    // The largest, not the smallest: this one surface carries both populations, and
    // the smaller ceiling would close the port to one because the other exists.
    CHECK(both.MaxOpenConnections() == CacheOpen);

    // A surface with one component reports that component's, never a fold over a
    // null one.
    MergedResponder schedulerOnly { SurfaceComponents { .scheduler = &scheduler } };
    CHECK(schedulerOnly.MaxRequestBytes() == SchedulerRequest);
    CHECK(schedulerOnly.MaxOpenConnections() == SchedulerOpen);
    CHECK(schedulerOnly.MaxInFlightBytes() == SchedulerInFlight);

    // And the budget that folds to has an OWNER: the component whose ceiling it is answers an
    // in-flight refusal for a verb nobody here owns -- the cache, not the scheduler listed
    // after it and not whichever comes first.
    std::ignore =
        both.EndpointRefusalReply(EndpointRefusal::InFlightBudget, static_cast<std::uint8_t>(Wire::Op::Compile), {});
    CHECK(cache.Refusals() == std::vector<std::string> { "cache" });
    CHECK(scheduler.Refusals().empty());
}

TEST_CASE("A surface serving only the node families folds their ceilings rather than zero", "[node][merged-responder]")
{
    // #1338's shape, and #206 made it reachable: a `--slots=0` node running only consensus builds
    // the operator components and nothing else. While the fold covered only the cache,
    // scheduler and compile owners, every ceiling came out ZERO -- and zero is every payload
    // refused for the request cap (`FrameEndpoint.hpp`) -- so that node bound its port and closed
    // every connection, `--node-status` included. WHAT DISTINGUISHES: the answers come from three
    // different operator owners, so dropping any one of them from the fold moves a number, and the
    // enrollment owner, sized larger than all three, stays out.
    NamedResponder node { "node" };
    node.PlaceCeilings(4096, 32, 65536);
    NamedResponder live { "live" };
    live.PlaceCeilings(2048, 64, 32768);
    NamedResponder fleet { "fleet" };
    fleet.PlaceCeilings(1024, 16, 98304);
    NamedResponder enrollment { "enrollment" };
    enrollment.PlaceCeilings(8192, 128, 131072);

    MergedResponder watched { SurfaceComponents {
        .node = &node, .enrollment = &enrollment, .live = &live, .fleet = &fleet } };
    CHECK(watched.MaxRequestBytes() == 4096);
    // Connections ADD for the families that coexist on every port; see the case holding them.
    CHECK(watched.MaxOpenConnections() == 32 + 64 + 16);
    CHECK(watched.MaxInFlightBytes() == 98304);

    // The control: a worker beside them, larger than all, is the fold -- the operator families are
    // sized as the SMALL owners and change no number where anything else runs.
    NamedResponder compile { "compile" };
    compile.PlaceCeilings(1U << 20U, 256, 1U << 22U);
    MergedResponder worker { SurfaceComponents {
        .compile = &compile, .node = &node, .enrollment = &enrollment, .live = &live, .fleet = &fleet } };
    CHECK(worker.MaxRequestBytes() == 1U << 20U);
    CHECK(worker.MaxOpenConnections() == 256);
    CHECK(worker.MaxInFlightBytes() == 1U << 22U);
}

TEST_CASE("The listener binds for a surface holding any one component, the fleet document included", "[node][node-surface]")
{
    // #206: the bind predicate is asked of the routing table, never of a list of members, so a
    // component that owns some verb answers yes whichever it is. One member at a time, because a
    // surface holding several passes whichever one a list forgot.
    NamedResponder owner { "owner" };
    CHECK_FALSE(AnswersAnyFamily(SurfaceComponents {}));
    CHECK(AnswersAnyFamily(SurfaceComponents { .cache = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .scheduler = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .compile = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .node = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .enrollment = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .live = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .fleet = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .nodeProof = &owner }));
    CHECK(AnswersAnyFamily(SurfaceComponents { .formation = &owner }));
}

TEST_CASE("A node with no component at all opens no 0xFC port", "[node][node-surface]")
{
    // Not an error and not a silence: a listener with no component for any verb family
    // would answer `UnimplementedVerb` to everything. Unreachable in the production
    // binary, where the operator verbs and live stats are always passed.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    auto const [cfg, port] = BaseConfig();

    auto surface = StartNodeSurfaceOrExplain(io, cfg, SurfaceComponents {}, std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    CHECK(*surface == nullptr);
    CHECK(Logged(logger, "serving no 0xFC port"));
}

TEST_CASE("A node whose only component is its worker opens the 0xFC port", "[node][node-surface]")
{
    // **Inverted by #290 stage 3, deliberately.** Stage 2 gave the compile verbs a
    // component here and did NOT give this node a port, because a worker still had a
    // compile port of its own. Stage 3 retires that port, so a worker with no tier and
    // no scheduler now binds this one -- it is the only place its compiles can arrive.
    //
    // The row is still what decides, and this asserts the surface follows it. What
    // used to be the second reason for answering nothing -- "no cache tier and no
    // scheduler" -- is gone rather than untested: no configuration reaches it, and a
    // branch that said "compiles are still served on the compile port" would name a
    // port that no longer exists.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder compile { "compile" };
    auto [cfg, port] = BaseConfig();
    cfg.cacheMemoryBytes = 0; // nowhere to keep objects, so no tier is built
    cfg.cacheDir.clear();
    REQUIRE_FALSE(ServesScheduler(cfg));
    REQUIRE_FALSE(cfg.nodeListen.empty());

    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .compile = &compile }), std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    CHECK(*surface != nullptr);

    // Neither sentence is said, and both matter. The old reason is unreachable, and
    // "--listen-node is empty" about a flag left at its default would send an operator
    // looking for a configuration problem that is not there.
    CHECK_FALSE(Logged(logger, "no cache tier and no scheduler"));
    CHECK_FALSE(Logged(logger, "--listen-node is empty"));
}

TEST_CASE("A node running only consensus opens the 0xFC port it is watched through", "[node][node-surface]")
{
    // #206 review. No worker, no cache tier, no scheduler: the components such a node
    // builds are the operator verbs, live stats and the fleet document, and a predicate that read only the
    // other three bound nothing -- so `--node-status` had nowhere to connect while
    // `--print-surfaces` named the port. Both halves are asserted: the row names it, and
    // the listener binds it and answers there.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder node { "node" };
    NamedResponder live { "live" };
    NamedResponder fleet { "fleet" };
    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();
    REQUIRE_FALSE(ServesScheduler(cfg));

    // What `--print-surfaces` names for this configuration.
    auto const named = RowFor(NodeSurface::Node).Resolve(cfg);
    REQUIRE(named.size() == 1);
    CHECK(named.front().port == port);

    EveryNodeOwners owners;
    auto surface =
        StartNodeSurfaceOrExplain(io,
                                  cfg,
                                  owners.Around(SurfaceComponents { .node = &node, .live = &live, .fleet = &fleet }),
                                  std::nullopt,
                                  metrics,
                                  logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    CHECK_FALSE(Logged(logger, "serving no 0xFC port"));
    io.Start();

    // And it answers there: a NodeStatus frame reaches the operator verbs.
    core::net::BlockingConnector connector;
    auto socket = core::async::syncRun(
        connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());
    auto const request = HeaderFor(Wire::Op::NodeStatus);
    REQUIRE(core::async::syncRun(core::net::sendAll(socket->get(), request)));
    auto const head = core::async::syncRun(core::net::receiveExactly(socket->get(), Wire::ReplyHeaderSize));
    REQUIRE(head.has_value());
    auto const header = Wire::DecodeReplyHeader(Unwrap(head));
    REQUIRE(header.has_value());
    auto const payload = core::async::syncRun(core::net::receiveExactly(socket->get(), Unwrap(header).payloadLength));
    REQUIRE(payload.has_value());
    auto reply = Unwrap(head);
    reply.insert(reply.end(), Unwrap(payload).begin(), Unwrap(payload).end());
    CHECK(MessageOf(reply) == "node");
}

TEST_CASE("An AUTH establishes a machine for the connection, and a refused one clears it", "[node][node-surface][session]")
{
    // Over a real listener, because what is under test is the ENDPOINT's per-connection state: the
    // verdict is ASSIGNED on every AUTH rather than merged, so a connection cannot keep a machine it
    // can no longer vouch for by presenting something worse afterwards. The operator verbs stand in
    // for any surface asked `RefusePeer` -- what they were handed is what admission folds.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder node { "node" };
    NamedResponder session { "session" };
    auto const machine = Testing::IdentityOf("pc-07");
    session.AnswerAuthWith({
        CredentialVerdict { .outcome = CredentialOutcome::Accepted, .machine = machine },
        // A refusal the surface encoded and counted itself: written as it stands.
        CredentialVerdict { .outcome = CredentialOutcome::Rejected,
                            .machine = machine,
                            .refusalReply = Wire::EncodeErrorReply(Wire::ErrorCode::Unauthenticated, "scripted refusal") },
        CredentialVerdict { .outcome = CredentialOutcome::Accepted, .machine = machine },
        // A refusal the surface left to its endpoint row.
        CredentialVerdict { .outcome = CredentialOutcome::Rejected },
        CredentialVerdict { .outcome = CredentialOutcome::Accepted, .machine = machine },
    });
    // A surface ceiling above `AUTH`'s own, so an oversize `AUTH` is refused by its verb's cap and
    // stepped over rather than closing the connection.
    node.PlaceCeilings(64ULL * 1024ULL, 8, 1024ULL * 1024ULL);
    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();

    // Every other every-node owner a stand-in, as `main` always has them (`MissingEveryNodeOwner`).
    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .node = &node, .session = &session }), std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    core::net::BlockingConnector connector;
    auto socket = core::async::syncRun(
        connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());
    auto const exchange = [&socket](std::span<std::byte const> request) {
        return Exchange(socket->get(), request);
    };
    auto const auth =
        Wire::EncodeAuth(Wire::AuthRequest { .kind = Wire::AuthKind::MachineTicket, .username = {}, .secret = "t" });
    auto const status = HeaderFor(Wire::Op::NodeStatus);

    // Before any AUTH, nothing is established -- and a verb that is NOT pre-auth is served all
    // the same: the node checks no password, so no surface waits for one.
    REQUIRE_FALSE(Wire::IsPreAuthAllowed(static_cast<std::uint8_t>(Wire::Op::NodeStatus)));
    (void) exchange(status);
    // ANSWERED, not merely replied to: this fake names itself in its refusals too, so only its
    // `Answer` having run separates "served" from "refused Unauthenticated at the door".
    CHECK(node.Answered().size() == 1);
    REQUIRE(node.Peers().size() == 1);
    CHECK_FALSE(node.Peers().back().authenticatedMachine.has_value());

    // Accepted: the machine rides every later frame on this connection.
    CHECK(StatusOf(exchange(auth)) == Wire::Status::Ok);
    (void) exchange(status);
    REQUIRE(node.Peers().size() == 2);
    CHECK(node.Peers().back().authenticatedMachine == std::optional { machine });

    // Refused, with the surface's own reply -- written verbatim, and the machine is GONE although
    // the refusing verdict carried one: only `Accepted` establishes anything.
    CHECK(MessageOf(exchange(auth)) == "scripted refusal");
    (void) exchange(status);
    REQUIRE(node.Peers().size() == 3);
    CHECK_FALSE(node.Peers().back().authenticatedMachine.has_value());
    CHECK(session.Refusals().empty());

    // And refused with no reply of its own, after being re-established: the endpoint row answers,
    // routed to the session component, and the machine is gone again.
    CHECK(StatusOf(exchange(auth)) == Wire::Status::Ok);
    CHECK(MessageOf(exchange(auth)) == "session");
    (void) exchange(status);
    REQUIRE(node.Peers().size() == 4);
    CHECK_FALSE(node.Peers().back().authenticatedMachine.has_value());
    CHECK(session.Refusals().size() == 1);

    // And refused at the HEADER, which never reaches the verdict: an `AUTH` over its own ceiling is
    // answered `PayloadTooLarge` and stepped over, and the machine established before it is gone
    // all the same.
    CHECK(StatusOf(exchange(auth)) == Wire::Status::Ok);
    (void) exchange(status);
    REQUIRE(node.Peers().size() == 5);
    REQUIRE(node.Peers().back().authenticatedMachine == std::optional { machine });
    std::vector<std::byte> oversize(Wire::RequestHeaderSize + Wire::MaxAuthPayload + 16);
    WireFrame::PutHeader(std::span { oversize }.first(Wire::RequestHeaderSize),
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::Auth),
                         static_cast<std::uint32_t>(Wire::MaxAuthPayload + 16));
    CHECK(ErrorOf(exchange(oversize)) == Wire::ErrorCode::PayloadTooLarge);
    (void) exchange(status);
    REQUIRE(node.Peers().size() == 6);
    CHECK_FALSE(node.Peers().back().authenticatedMachine.has_value());
}

TEST_CASE("A forgotten machine's tickets are refused, and the connection presenting one speaks for nobody",
          "[node][node-surface][session][ticket]")
{
    // The real session component behind a real endpoint, verifying against a roster the case then
    // revokes the machine from, as an applied forget does (#1555). What is under test is that the
    // roster is asked PER TICKET: a machine admitted a minute ago is refused the moment it is not,
    // by name and counted, and the connection that presented the refused ticket holds no machine.
    using namespace std::chrono_literals;
    constexpr auto noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };
    constexpr std::string_view office = "office.corp:6674";

    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder node { "node" };
    core::platform::ManualWallClock const wallClock { noon };
    Testing::FixedLeaseRoster roster { { "office" } };
    roster.AdmitMachine("pc-07");
    Testing::ThisMachineIs const locality { "127.0.0.1" };
    AnnouncedEndpoint const announced { office };
    NodeAudience const audience { announced, {}, {}, locality };
    Distributed::SpentTickets spent;
    Distributed::TicketVerifier const verifier { &roster, audience, spent };
    Testing::ScriptedSecureRandom random;
    SessionResponder session { verifier, SessionKeys {}, random, wallClock, metrics };

    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();
    // Every other every-node owner a stand-in, as `main` always has them (`MissingEveryNodeOwner`).
    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .node = &node, .session = &session }), std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    core::net::BlockingConnector connector;
    auto socket = core::async::syncRun(
        connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());

    // pc-07's own tickets for this node, one per AUTH: a ticket is spent once, so the second must
    // differ, and it differs in its nonce alone.
    auto const ticketAuth = [office, noon](std::uint8_t nonce) {
        auto claims = Distributed::MachineTicketClaims {
            .machineId = "pc-07",
            .audience = std::string { office },
            .expiresAtUnixSeconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>((noon + 30s).time_since_epoch()).count()),
            .nonce = {}
        };
        claims.nonce.front() = std::byte { nonce };
        auto const ticket = Distributed::MintMachineTicket(Testing::TestKeyPair("pc-07"), claims);
        return Wire::EncodeAuth(
            Wire::AuthRequest { .kind = Wire::AuthKind::MachineTicket, .username = {}, .secret = ticket.View() });
    };
    auto const status = HeaderFor(Wire::Op::NodeStatus);

    // Admitted: the ticket establishes pc-07 on this connection.
    CHECK(StatusOf(Exchange(socket->get(), ticketAuth(1))) == Wire::Status::Ok);
    (void) Exchange(socket->get(), status);
    REQUIRE(node.Peers().size() == 1);
    REQUIRE(node.Peers().back().authenticatedMachine.has_value());
    CHECK(Unwrap(node.Peers().back().authenticatedMachine).id == "pc-07");
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 1);

    // Forgotten: its next ticket, good in every other respect, is refused and counted as revoked --
    // told only that it is not admitted, as a stranger is -- and the machine the first one
    // established is gone from the connection.
    roster.Revoke("pc-07");
    auto const refused = Exchange(socket->get(), ticketAuth(2));
    CHECK(ErrorOf(refused) == Wire::ErrorCode::TicketRefused);
    CHECK(MessageOf(refused) == Distributed::TicketNotAdmittedMessage);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedRevoked) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 1);
    (void) Exchange(socket->get(), status);
    REQUIRE(node.Peers().size() == 2);
    CHECK_FALSE(node.Peers().back().authenticatedMachine.has_value());
}

TEST_CASE("On an open fleet, a forgotten machine's revoked ticket refuses the verb pipelined behind it",
          "[node][node-surface][session][ticket][forget]")
{
    // The reviewer's measurement at the production seam: a `--fleet-open` node, whose address route
    // admits anybody, and pc-07 -- forgotten, its key revoked -- sending AUTH with its ticket and a
    // command behind it in ONE write, as a launcher does. The AUTH is refused `revoked`; what this
    // case is about is the COMMAND, which must be refused as the forgotten machine's rather than
    // admitted by the open address route.
    //
    // The fold's key roster is deliberately BEHIND the lease roster here -- it still holds pc-07
    // live -- because the two are published at different moments: the refusal must come from the
    // evidence the ticket carried, which can only ever answer `Forgotten`, and not from asking a
    // roster that has not heard of the forget yet.
    using namespace std::chrono_literals;
    constexpr auto noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };
    constexpr std::string_view office = "office.corp:6674";

    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    core::platform::ManualWallClock const wallClock { noon };
    Testing::FixedLeaseRoster roster { { "office" } };
    roster.Revoke("pc-07");
    Testing::ThisMachineIs const locality { "127.0.0.1" };
    AnnouncedEndpoint const announced { office };
    NodeAudience const audience { announced, {}, {}, locality };
    Distributed::SpentTickets spent;
    Distributed::TicketVerifier const verifier { &roster, audience, spent };
    Testing::ScriptedSecureRandom random;
    SessionResponder session { verifier, SessionKeys {}, random, wallClock, metrics };

    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();
    cfg.fleetOpen = true;
    NodeMembership membership { cfg, logger };
    auto state = Cluster::ClusterState {};
    Cluster::Apply(state,
                   Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                      .key = "pc-07",
                                      .value = "10.0.0.7:6676",
                                      .schedulerEndpoint = {},
                                      .publicKey = Testing::TestKeyPair("pc-07").PublicKey(),
                                      .role = std::nullopt });
    membership.PublishCluster(state);

    NamedResponder node { "node" };
    node.GateBy(membership.Oracle(), metrics, IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember);
    // Every other every-node owner a stand-in, as `main` always has them (`MissingEveryNodeOwner`).
    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .node = &node, .session = &session }), std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    core::net::BlockingConnector connector;
    auto socket = core::async::syncRun(
        connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());

    auto const ticketAuth = [office, noon](std::string const& machine, std::uint8_t nonce) {
        auto claims = Distributed::MachineTicketClaims {
            .machineId = machine,
            .audience = std::string { office },
            .expiresAtUnixSeconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>((noon + 30s).time_since_epoch()).count()),
            .nonce = {}
        };
        claims.nonce.front() = std::byte { nonce };
        auto const ticket = Distributed::MintMachineTicket(Testing::TestKeyPair(machine), claims);
        return Wire::EncodeAuth(
            Wire::AuthRequest { .kind = Wire::AuthKind::MachineTicket, .username = {}, .secret = ticket.View() });
    };
    auto const status = HeaderFor(Wire::Op::NodeStatus);

    // The control that makes the refusal below about the TICKET: before it, this connection's
    // command is served -- the open address route admits it.
    (void) Exchange(socket->get(), status);
    REQUIRE(node.Answered().size() == 1);

    // AUTH and the command in ONE write, then both replies.
    auto pipelined = ticketAuth("pc-07", 1);
    pipelined.insert(pipelined.end(), status.begin(), status.end());
    REQUIRE(core::async::syncRun(core::net::sendAll(socket->get(), pipelined)));
    auto const readReply = [&socket] {
        auto const head = core::async::syncRun(core::net::receiveExactly(socket->get(), Wire::ReplyHeaderSize));
        REQUIRE(head.has_value());
        auto const header = Wire::DecodeReplyHeader(Unwrap(head));
        REQUIRE(header.has_value());
        auto reply = Unwrap(head);
        auto const payload = core::async::syncRun(core::net::receiveExactly(socket->get(), Unwrap(header).payloadLength));
        REQUIRE(payload.has_value());
        reply.insert(reply.end(), Unwrap(payload).begin(), Unwrap(payload).end());
        return reply;
    };
    auto const authReply = readReply();
    CHECK(ErrorOf(authReply) == Wire::ErrorCode::TicketRefused);
    CHECK(MessageOf(authReply) == Distributed::TicketNotAdmittedMessage);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedRevoked) == 1);

    // The command: refused as the FORGOTTEN machine's, counted on that row and not as a stranger's,
    // and never answered.
    auto const commandReply = readReply();
    CHECK(ErrorOf(commandReply) == Wire::ErrorCode::NotAMember);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 0);
    CHECK(node.Answered().size() == 1);

    // And no later AUTH lifts it: a genuine ticket for another admitted machine on the same
    // connection is accepted, and the connection is still the forgotten machine's.
    roster.AdmitMachine("pc-08");
    CHECK(StatusOf(Exchange(socket->get(), ticketAuth("pc-08", 2))) == Wire::Status::Ok);
    CHECK(ErrorOf(Exchange(socket->get(), status)) == Wire::ErrorCode::NotAMember);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked) == 2);
    CHECK(node.Answered().size() == 1);
}

namespace
{

/// Readings nobody reads, for the same reason.
class NoReadings final: public ILiveStatsSources
{
  public:
    [[nodiscard]] std::optional<LiveCapture> Capture(Wire::LiveSubject /*subject*/) const override
    {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return std::nullopt;
    }
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return {};
    }
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view /*section*/,
                                                                                std::string_view /*range*/) const override
    {
        return std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet, .detail = {} });
    }
};

/// @param reply A reply.
/// @return Whether its bytes spell `revoked` in any case.
[[nodiscard]] bool SaysRevoked(std::span<std::byte const> reply)
{
    auto text = std::string {};
    for (auto const byte: reply)
        text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(byte))));
    return text.contains("revoked");
}

} // namespace

TEST_CASE("A captured ticket of a forgotten machine is answered as a stranger's at AUTH, at a gated verb and at "
          "explain-admission",
          "[node][node-surface][session][ticket][forget][admission]")
{
    // The three roster-dependent refusals end to end, through the production session, gate and
    // responder, one connection each: a genuine ticket of `gone`, whose key the cluster revoked (a
    // captured ticket); a ticket for `nobody`, signed by the prober's own key (unknown machine); and
    // a ticket for `pc-07`, a LIVE machine, signed by the prober's own key (forged). A stranger must
    // not tell them apart -- not at AUTH, not at the gated `node-status` behind it, not at the self
    // form of `explain-admission` -- while the node counts each on its own row.
    //
    // The oracle is the key roster ALONE: over loopback, `Loopback` would admit B and refuse A, a
    // difference that is this machine's and not the one under test.
    using namespace std::chrono_literals;
    constexpr auto noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };
    constexpr std::string_view office = "office.corp:6674";

    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    core::platform::ManualWallClock const wallClock { noon };
    Testing::FixedLeaseRoster roster { { "office" } };
    roster.AdmitMachine("gone");
    roster.Revoke("gone");
    roster.AdmitMachine("pc-07");
    Testing::ThisMachineIs const locality { "127.0.0.1" };
    AnnouncedEndpoint const announced { office };
    NodeAudience const audience { announced, {}, {}, locality };
    Distributed::SpentTickets spent;
    Distributed::TicketVerifier const verifier { &roster, audience, spent };
    Testing::ScriptedSecureRandom random;
    SessionResponder session { verifier, SessionKeys {}, random, wallClock, metrics };

    Distributed::KeyRosterMembership keys;
    keys.Publish({}, { Testing::TestKeyPair("gone").PublicKey() });
    Distributed::AnyOfMembership const byKeyAlone { { &keys } };
    Testing::SilentNodeStatus const identity;
    NoReadings const readings;
    Testing::FixedStanding const standing {};
    NodeStatusResponder node { identity, readings, byKeyAlone, standing, metrics };

    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();
    // Every other every-node owner a stand-in, as `main` always has them (`MissingEveryNodeOwner`).
    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .node = &node, .session = &session }), std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    auto const ticketAuth = [office, noon](std::string const& claimed, std::string const& signer) {
        auto const claims = Distributed::MachineTicketClaims {
            .machineId = claimed,
            .audience = std::string { office },
            .expiresAtUnixSeconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>((noon + 30s).time_since_epoch()).count()),
            .nonce = {}
        };
        auto const ticket = Distributed::MintMachineTicket(Testing::TestKeyPair(signer), claims);
        return Wire::EncodeAuth(
            Wire::AuthRequest { .kind = Wire::AuthKind::MachineTicket, .username = {}, .secret = ticket.View() });
    };

    /// AUTH with one ticket, then `node-status`, then the self form, on one connection.
    auto const probe = [&](std::string const& claimed, std::string const& signer) {
        core::net::BlockingConnector connector;
        auto socket = core::async::syncRun(
            connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        REQUIRE(socket.has_value());
        auto answers = std::array<std::vector<std::byte>, 3> {};
        answers[0] = Exchange(socket->get(), ticketAuth(claimed, signer));
        answers[1] = Exchange(socket->get(), HeaderFor(Wire::Op::NodeStatus));
        answers[2] = Exchange(socket->get(), Wire::EncodeExplainAdmissionRequest(""));
        return answers;
    };

    // Each probe moves its own ticket row, and the gated verb behind it the row its connection
    // earned: the forgotten machine's for the revoked ticket, the stranger's for the other two.
    auto const captured = probe("gone", "gone");
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedRevoked) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 0);

    auto const unknown = probe("nobody", "prober");
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedUnknownMachine) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 1);

    auto const forged = probe("pc-07", "prober");
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedForged) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 2);

    // And no probe moved another's row.
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedRevoked) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedUnknownMachine) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked) == 1);

    // What each connection was told, answer by answer.
    CHECK(ErrorOf(captured[0]) == Wire::ErrorCode::TicketRefused);
    CHECK(ErrorOf(captured[1]) == Wire::ErrorCode::NotAMember);
    auto const self = Wire::DecodeAdmissionExplanation(Testing::PayloadOf(captured[2]));
    REQUIRE(self.has_value());
    CHECK(Unwrap(self).verdict == Wire::WireMembership::Outsider);
    CHECK(Unwrap(self).decidedBy == 0);
    CHECK(Unwrap(self).subject == "127.0.0.1");
    for (auto const index: std::views::iota(std::size_t { 0 }, captured.size()))
    {
        INFO("answer " << index);
        CHECK_FALSE(SaysRevoked(captured[index]));
        // And byte for byte what the other two were told, which is the claim itself.
        CHECK(captured[index] == unknown[index]);
        CHECK(forged[index] == unknown[index]);
    }
}

TEST_CASE("An explain-admission above its own ceiling is refused before any component reads it",
          "[node][node-surface][admission]")
{
    // The verb is reachable before admission, so its own ceiling is the only thing bounding what a
    // stranger makes the node read: an oversized request is refused `PayloadTooLarge` by the
    // endpoint, stepped over, and never reaches the component that owns the verb.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder node { "node" };
    node.PlaceCeilings(64ULL * 1024ULL, 8, 1024ULL * 1024ULL);
    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();
    // Every other every-node owner a stand-in, as `main` always has them (`MissingEveryNodeOwner`).
    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .node = &node }), std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    core::net::BlockingConnector connector;
    auto socket = core::async::syncRun(
        connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());

    std::vector<std::byte> oversize(Wire::RequestHeaderSize + Wire::MaxExplainAdmissionPayload + 1);
    WireFrame::PutHeader(std::span { oversize }.first(Wire::RequestHeaderSize),
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::ExplainAdmission),
                         static_cast<std::uint32_t>(Wire::MaxExplainAdmissionPayload + 1));
    CHECK(ErrorOf(Exchange(socket->get(), oversize)) == Wire::ErrorCode::PayloadTooLarge);
    CHECK(node.Answered().empty());

    // The control: at its ceiling it is read and reaches the component, on the same connection.
    auto atCeiling = Wire::EncodeExplainAdmissionRequest(std::string(Wire::MaxExplainAdmissionPayload - 4, 'x'));
    REQUIRE(atCeiling.size() == Wire::RequestHeaderSize + Wire::MaxExplainAdmissionPayload);
    (void) Exchange(socket->get(), atCeiling);
    CHECK(node.Answered().size() == 1);
}

TEST_CASE("A node running only consensus still answers its status with every live subscription held", "[node][node-surface]")
{
    // #206 review. The operator families coexist on one port, so their connection
    // allowances add: folded as a maximum, a node running only consensus had exactly the
    // live cap, and `--node-status` could not connect once every subscription was held.
    // Every connection is held after a frame on it was ANSWERED, so each is known to have
    // been accepted and counted before the next exchange is tried. The node family's
    // allowance is then filled as well, so the fleet read connects only if ITS allowance
    // was added too.
    constexpr std::size_t NodeOpen = 32;
    constexpr std::size_t LiveOpen = 64;
    constexpr std::size_t FleetOpen = 32;
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder node { "node" };
    node.PlaceCeilings(CompileCacheWire::MaxControlPayload, NodeOpen, 16 * CompileCacheWire::MaxControlPayload);
    NamedResponder live { "live" };
    live.PlaceCeilings(CompileCacheWire::MaxControlPayload, LiveOpen, 16 * CompileCacheWire::MaxControlPayload);
    NamedResponder fleet { "fleet" };
    fleet.PlaceCeilings(CompileCacheWire::MaxControlPayload, FleetOpen, 16 * CompileCacheWire::MaxControlPayload);
    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();

    EveryNodeOwners owners;
    auto surface =
        StartNodeSurfaceOrExplain(io,
                                  cfg,
                                  owners.Around(SurfaceComponents { .node = &node, .live = &live, .fleet = &fleet }),
                                  std::nullopt,
                                  metrics,
                                  logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    core::net::BlockingConnector connector;
    auto const listenPort = port;
    auto const exchange = [&connector, listenPort](std::vector<std::unique_ptr<core::net::ISocket>>& held, Wire::Op op) {
        auto socket = core::async::syncRun(connector.connect(
            "127.0.0.1", listenPort, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return std::string {};
        auto const request = HeaderFor(op);
        if (!core::async::syncRun(core::net::sendAll(socket->get(), request)))
            return std::string {};
        auto head = core::async::syncRun(core::net::receiveExactly(socket->get(), Wire::ReplyHeaderSize));
        if (!head.has_value())
            return std::string {};
        auto const header = Wire::DecodeReplyHeader(Unwrap(head));
        if (!header.has_value())
            return std::string {};
        auto const payload = core::async::syncRun(core::net::receiveExactly(socket->get(), Unwrap(header).payloadLength));
        if (!payload.has_value())
            return std::string {};
        auto reply = Unwrap(head);
        reply.insert(reply.end(), Unwrap(payload).begin(), Unwrap(payload).end());
        held.push_back(std::move(*socket));
        return MessageOf(reply);
    };

    std::vector<std::unique_ptr<core::net::ISocket>> subscriptions;
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, LiveOpen))
        REQUIRE(exchange(subscriptions, Wire::Op::Subscribe) == "live");

    std::vector<std::unique_ptr<core::net::ISocket>> status;
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, NodeOpen))
        REQUIRE(exchange(status, Wire::Op::NodeStatus) == "node");

    std::vector<std::unique_ptr<core::net::ISocket>> reads;
    CHECK(exchange(reads, Wire::Op::FleetText) == "fleet");
}

namespace
{
/// The four responders every node builds, over nothing wired: what `main` passes
/// `ComposeSurfaceComponents` for the families no node may lack.
struct EveryNodeResponders
{
    /// @param cfg The node's configuration; must outlive this.
    /// @param io The loop the live-stats stream runs on; must outlive this.
    /// @param metrics Where a refusal is counted; must outlive this.
    EveryNodeResponders(NodeConfig const& cfg, NodeIoLoop& io, IMetricsSink& metrics):
        status { cfg, clock, clock.now(), "test", "n-office", NodeComponents {} },
        node { status, sources, membership, standing, metrics },
        live { sources, membership, AdminCredential {}, io.Reactor(), metrics },
        fleet { sources, membership, AdminCredential {}, metrics },
        session { verifier, SessionKeys {}, random, wallClock, metrics },
        sharedCache { cfg, membership, clock, metrics, logger, nullptr, ReconcileOn::Caller }
    {
    }

    core::platform::ManualClock clock;
    LiveStatsSourceSlot sources;
    Testing::ListedMembership membership { { "127.0.0.1" }, Distributed::MembershipParticipant::Loopback };
    ConfiguredNodeStatus status;
    Testing::FixedStanding const standing {};
    NodeStatusResponder node;
    LiveStatsResponder live;
    FleetTextResponder fleet;
    // The session component over a node that holds no roster: every ticket it is shown is refused,
    // which is all a routing case needs of it.
    AnnouncedEndpoint const announced { "office.corp:6674" };
    Testing::ThisMachineIs const locality { "127.0.0.1" };
    NodeAudience const audience { announced, {}, {}, locality };
    Distributed::SpentTickets spent;
    Distributed::TicketVerifier const verifier { nullptr, audience, spent };
    Testing::ScriptedSecureRandom random;
    core::platform::ManualWallClock wallClock;
    SessionResponder session;
    // The fleet's shared cache every node builds, dormant: nothing names this machine, so nothing
    // opens, and a routing case needs no more of it.
    NullLogger logger;
    SharedCacheService sharedCache;
};
} // namespace

TEST_CASE("The surface main composes routes each family to the component it was handed, and nowhere else",
          "[node][node-surface][summary]")
{
    // `ComposeSurfaceComponents` is the one place `main` names each family's owner, so a member it
    // drops is a family refused at the door as served nowhere on every node. Asked of the router's
    // own table: each component the function was handed owns exactly its family, and a component
    // it was handed as absent owns none. The tiers and the two consensus components are passed
    // absent here -- building them is each one's own test's business -- so what this case holds is
    // the every-node families and `formation`.
    NodeIoLoop io;
    AtomicMetricsSink metrics;
    auto const cfg = BaseConfig().first;
    FixedFleetSummary const answered { Wire::FleetSummary { .clusterId = "c-office", .nodeId = "n-office" } };
    auto const identity = Testing::TestKeyPair("n-office");
    FleetSummaryResponder formation { answered, identity };
    EveryNodeResponders every { cfg, io, metrics };

    auto const components = ComposeSurfaceComponents(nullptr,
                                                     nullptr,
                                                     nullptr,
                                                     every.node,
                                                     nullptr,
                                                     every.live,
                                                     every.fleet,
                                                     nullptr,
                                                     &formation,
                                                     every.session,
                                                     every.sharedCache);

    struct Expected
    {
        Wire::VerbFamily family;
        IFrameResponder const* owner;
    };
    auto const expected = std::array {
        Expected { .family = Wire::VerbFamily::Session, .owner = &every.session },
        Expected { .family = Wire::VerbFamily::Cache, .owner = nullptr },
        Expected { .family = Wire::VerbFamily::Scheduler, .owner = nullptr },
        Expected { .family = Wire::VerbFamily::Compile, .owner = nullptr },
        Expected { .family = Wire::VerbFamily::Node, .owner = &every.node },
        Expected { .family = Wire::VerbFamily::Enrollment, .owner = nullptr },
        Expected { .family = Wire::VerbFamily::Live, .owner = &every.live },
        Expected { .family = Wire::VerbFamily::Fleet, .owner = &every.fleet },
        Expected { .family = Wire::VerbFamily::NodeProof, .owner = nullptr },
        Expected { .family = Wire::VerbFamily::Formation, .owner = &formation },
        Expected { .family = Wire::VerbFamily::SharedCache, .owner = &every.sharedCache.Responder() },
    };
    for (auto const& row: expected)
    {
        INFO("family " << static_cast<int>(row.family));
        CHECK(FamilyOwner(components, row.family) == row.owner);
    }
}

TEST_CASE("With every component present, the surface main composes routes each family to the one it was handed",
          "[node][node-surface][summary]")
{
    // The half the case above cannot show: there the tiers and the two consensus-only components
    // are absent, so a pass-through `ComposeSurfaceComponents` dropped for one of them -- or filled
    // from the wrong argument -- would still read as null. Here every one exists, each started the
    // way its own tests start it, so a dropped member is a family owned by NOBODY and a crossed one a
    // family owned by somebody else.
    WorkerTierTesting::WorkerTierFixture fix;
    auto worker = fix.Start();
    REQUIRE(worker.has_value());
    REQUIRE(*worker != nullptr);

    auto const nodeCfg = BaseConfig().first;
    ConfiguredCredential const upstreamCredential { nodeCfg, nullptr };
    // Reading through to nothing: no --upstream, a fleet setting naming no machine, and no
    // identity to prove with -- this case is about which component owns each family.
    SharedCacheDirectory const directory { "n1", {} };
    UpstreamParts const parts { .upstream = nodeCfg.upstream,
                                .credential = upstreamCredential,
                                .directory = directory,
                                .prover = nullptr,
                                .io = fix.io,
                                .clock = fix.clock,
                                .metrics = fix.metrics,
                                .conditions = nullptr,
                                .logger = fix.logger,
                                .host = nullptr };
    auto cache = StartCacheTierOrExplain(nodeCfg, parts, fix.locality, fix.clock, fix.metrics, fix.logger);
    REQUIRE(cache.has_value());
    REQUIRE(*cache != nullptr);

    auto schedulerCfg = Testing::FirstStart(NodeConfig {});
    schedulerCfg.schedulers = { "127.0.0.1:6675" };
    schedulerCfg.nodeId = "n1";
    schedulerCfg.raftListen = "127.0.0.1:6680";
    core::platform::ManualWallClock wallClock;
    std::optional<Ed25519KeyPair> const identityKey { Testing::TestKeyPair("n1") };
    auto scheduler =
        SchedulerTier::Start(schedulerCfg, fix.membership, fix.clock, wallClock, fix.metrics, fix.logger, identityKey);
    REQUIRE(scheduler.has_value());
    REQUIRE(*scheduler != nullptr);

    EnrollmentWindow window { fix.clock };
    EnrollmentResponder enrollment { window, (*scheduler)->ServiceForSurfaces(), fix.membership, fix.metrics, fix.logger };
    SystemSecureRandom random;
    NodeProofResponder nodeProof { "n1", *identityKey, fix.membership, random, fix.metrics, fix.logger };
    FixedFleetSummary const answered { Wire::FleetSummary { .clusterId = "c-office", .nodeId = "n1" } };
    FleetSummaryResponder formation { answered, *identityKey };
    EveryNodeResponders every { nodeCfg, fix.io, fix.metrics };

    auto const components = ComposeSurfaceComponents(cache->get(),
                                                     scheduler->get(),
                                                     worker->get(),
                                                     every.node,
                                                     &enrollment,
                                                     every.live,
                                                     every.fleet,
                                                     &nodeProof,
                                                     &formation,
                                                     every.session,
                                                     every.sharedCache);

    struct Expected
    {
        Wire::VerbFamily family;
        IFrameResponder const* owner;
    };
    IFrameResponder const* const schedulerResponder = &(*scheduler)->Responder();
    auto const expected = std::array {
        Expected { .family = Wire::VerbFamily::Session, .owner = &every.session },
        Expected { .family = Wire::VerbFamily::Cache, .owner = &(*cache)->Responder() },
        Expected { .family = Wire::VerbFamily::Scheduler, .owner = schedulerResponder },
        Expected { .family = Wire::VerbFamily::Compile, .owner = &(*worker)->Responder() },
        Expected { .family = Wire::VerbFamily::Node, .owner = &every.node },
        Expected { .family = Wire::VerbFamily::Enrollment, .owner = &enrollment },
        Expected { .family = Wire::VerbFamily::Live, .owner = &every.live },
        Expected { .family = Wire::VerbFamily::Fleet, .owner = &every.fleet },
        Expected { .family = Wire::VerbFamily::NodeProof, .owner = &nodeProof },
        Expected { .family = Wire::VerbFamily::Formation, .owner = &formation },
        Expected { .family = Wire::VerbFamily::SharedCache, .owner = &every.sharedCache.Responder() },
    };
    for (auto const& row: expected)
    {
        INFO("family " << static_cast<int>(row.family));
        REQUIRE(row.owner != nullptr);
        CHECK(FamilyOwner(components, row.family) == row.owner);
    }
}

TEST_CASE("A node's port answers FLEET-SUMMARY over the probe's own nonce and only once per connection",
          "[node][node-surface][summary]")
{
    // The wiring, asserted rather than checked live: the formation component, placed where `main`
    // places it, behind a REAL bound port, asked by the production probe over the production
    // dialer -- and the answer verifies over the nonce THAT probe drew. Then the M-1 half: two
    // questions pipelined on one connection are answered once, and the connection closes, so a
    // stranger's second signature costs it a second handshake.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    auto [cfg, port] = BaseConfig();
    cfg.slots = 0;
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir.clear();

    FixedFleetSummary const answered { Wire::FleetSummary { .clusterId = "c-office",
                                                            .state = Wire::FleetState::Established,
                                                            .nodeId = "n-office",
                                                            .raftEndpoint = "office.example:6680" } };
    auto const identity = Testing::TestKeyPair("n-office");
    FleetSummaryResponder formation { answered, identity };
    // Beside the families every node builds, composed by the function `main` composes with.
    EveryNodeResponders every { cfg, io, metrics };
    auto surface = StartNodeSurfaceOrExplain(io,
                                             cfg,
                                             ComposeSurfaceComponents(nullptr,
                                                                      nullptr,
                                                                      nullptr,
                                                                      every.node,
                                                                      nullptr,
                                                                      every.live,
                                                                      every.fleet,
                                                                      nullptr,
                                                                      &formation,
                                                                      every.session,
                                                                      every.sharedCache),
                                             std::nullopt,
                                             metrics,
                                             logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);
    io.Start();

    BlockingEndpointDialer dialer { std::chrono::seconds { 5 } };
    SystemSecureRandom random;
    core::platform::SteadyClock clock;
    DialledFleetProbe probe { dialer, random, clock };
    auto const fleet =
        probe.Ask({ .endpoint = std::format("127.0.0.1:{}", port), .source = Cluster::SeedSource::FleetSeedFlag });
    INFO((fleet.has_value() ? std::string {} : fleet.error()));
    REQUIRE(fleet.has_value());
    CHECK(fleet->Summary() == answered.Current());
    CHECK(fleet->Key() == identity.PublicKey());

    // Two questions, pipelined, then this side's write half closed: one answer comes back, and then
    // the end of the stream -- no second signature.
    core::net::BlockingConnector connector;
    auto socket = core::async::syncRun(
        connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());
    auto const question = Wire::EncodeFleetSummaryRequest(std::array<std::byte, Wire::NodeChallengeBytes> {});
    auto twice = question;
    twice.insert(twice.end(), question.begin(), question.end());
    REQUIRE(core::async::syncRun(core::net::sendAll(socket->get(), twice)));
    REQUIRE(Testing::ShutdownWrite(**socket).has_value());

    auto const head = core::async::syncRun(core::net::receiveExactly(socket->get(), Wire::ReplyHeaderSize));
    REQUIRE(head.has_value());
    auto const header = Wire::DecodeReplyHeader(Unwrap(head));
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Wire::Status::Ok);
    REQUIRE(core::async::syncRun(core::net::receiveExactly(socket->get(), Unwrap(header).payloadLength)).has_value());
    CHECK_FALSE(core::async::syncRun(core::net::receiveExactly(socket->get(), Wire::ReplyHeaderSize)).has_value());
}

TEST_CASE("An emptied --listen-node closes the port and says so", "[node][node-surface]")
{
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder cache { "cache" };
    NodeConfig cfg;
    cfg.nodeListen.clear();

    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .cache = &cache }), std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    CHECK(*surface == nullptr);
    CHECK(Logged(logger, "--listen-node is empty"));
}

#if !defined(_WIN32)

/// A descriptor in the state a supervisor hands one over in: bound and listening,
/// and deliberately NOT non-blocking and NOT close-on-exec, because systemd passes
/// them without either and correcting them is part of what adoption is for.
struct HandedOverListenFd
{
    /// In the default member initializer rather than the body, because
    /// `cppcoreguidelines-prefer-member-initializer` is an error here and the body
    /// still has to REQUIRE the result.
    int fd { ::socket(AF_INET, SOCK_STREAM, 0) };
    std::uint16_t port { 0 };

    HandedOverListenFd()
    {
        REQUIRE(fd >= 0);
        if (fd < 0)
            return; // constrains the fd for the static analyzer on the ::bind path below
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        // The byte-order calls are UNQUALIFIED while every syscall around them is
        // `::`-prefixed, and that asymmetry is deliberate: macOS defines htonl and
        // ntohs as macros, so a scope qualifier in front of one does not parse.
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral, so this cannot collide with a live port
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(fd, 8) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);
    }

    HandedOverListenFd(HandedOverListenFd const&) = delete;
    HandedOverListenFd& operator=(HandedOverListenFd const&) = delete;
    HandedOverListenFd(HandedOverListenFd&&) = delete;
    HandedOverListenFd& operator=(HandedOverListenFd&&) = delete;

    ~HandedOverListenFd()
    {
        if (fd >= 0)
            ::close(fd);
    }

    /// Hand the descriptor to something that takes ownership of it.
    /// @return The descriptor; this fixture no longer closes it.
    [[nodiscard]] int Release() noexcept
    {
        auto const released = fd;
        fd = -1;
        return released;
    }
};

TEST_CASE("A socket-activated node serves the descriptor it was handed", "[node][node-surface]")
{
    // **The last stitch of #290 stage 3.** For as long as the surfaces were merged
    // and the reactor listeners could not adopt, an activated worker was refused at
    // startup: the merged 0xFC surface runs on the reactor and socket activation
    // handed back a BLOCKING listener, so there was nothing that could join the two.
    // #464 added `AdoptInheritedListener` and this closes over it.
    //
    // The two facts asserted here are the ones no unit of either change can see on
    // its own: that the node reaches `Adopt` at all rather than binding, and that the
    // endpoint it then reports is the SUPERVISOR's port rather than anything read out
    // of the configuration.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder cache { "cache" };
    HandedOverListenFd handed;
    auto const supervisorPort = handed.port;

    NodeConfig cfg;
    // Emptied deliberately, and it is the load-bearing part of this case. Under
    // activation the unit owns the address, so this flag configures nothing -- and a
    // node that consulted the row would find it resolves to nothing and decline a
    // handoff that has already happened, leaving the descriptor unserved.
    cfg.nodeListen.clear();
    // The only thing that can say where clients go, which is why activation makes it
    // mandatory. The port here is deliberately NOT the supervisor's: a node that
    // echoed this value back instead of asking the socket would pass a weaker version
    // of this case, so the two must differ.
    cfg.advertise = "worker-01.internal:1";

    EveryNodeOwners owners;
    auto surface = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .cache = &cache }), std::optional { handed.Release() }, metrics, logger);
    REQUIRE(surface.has_value());
    REQUIRE(*surface != nullptr);

    // The host from --advertise, the port from the SOCKET. Asked of the descriptor
    // because the unit never tells this process which port it chose, so `BoundPort()`
    // is the only thing that knows -- and the advertised `:1` above proves the answer
    // was not simply copied out of the configuration.
    CHECK((*surface)->BoundEndpoint() == std::format("worker-01.internal:{}", supervisorPort));
    CHECK(Logged(logger, "socket-activated"));

    // And neither sentence from the ordinary path, both of which would mean the row
    // had been consulted after all.
    CHECK_FALSE(Logged(logger, "--listen-node is empty"));
    CHECK_FALSE(Logged(logger, "listening on"));
}

#endif

/// Why each platform's listener cannot serve the `-1` the case below hands over.
///
/// Per platform because the REASON differs while the refusal must not, and the reason is
/// what distinguishes: `socket-activated` is in the refusal everywhere, so a case asserting
/// only that would pass on Windows for the POSIX reason. Windows has no socket activation,
/// so its listener refuses before it looks at the descriptor at all (#1347).
#if defined(_WIN32)
constexpr std::string_view UnservableDescriptorCause = "socket activation is not available on this platform";
#else
constexpr std::string_view UnservableDescriptorCause = "adopt: not a descriptor";
#endif

TEST_CASE("A socket-activated descriptor that cannot be served is fatal", "[node][node-surface]")
{
    // The same answer a failed bind gets, and for the same reason: an activated node
    // that cannot serve its descriptor still has --scheduler, so it would register,
    // advertise an address nothing answers, and be leased to clients that each fail
    // to reach it and compile locally in silence. `Adopt` reports through
    // `IsBound()`/`BindError()` rather than throwing, so this is a path a caller has
    // to actively route into the refusal -- it does not arrive as an exception.
    //
    // A packaged Linux install enables the worker THROUGH the socket unit -- the
    // `.service` deliberately carries no `[Install]` section -- so this is the
    // ordinary deployment rather than an exotic one.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder cache { "cache" };

    NodeConfig cfg;
    cfg.nodeListen.clear();
    cfg.advertise = "worker-01.internal:6676";

    // Not a descriptor. `Adopt` answers this without touching it, which is also why
    // there is nothing here to close: ownership passes on every path, including the
    // ones that fail.
    EveryNodeOwners owners;
    auto refused = StartNodeSurfaceOrExplain(
        io, cfg, owners.Around(SurfaceComponents { .cache = &cache }), std::optional { -1 }, metrics, logger);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("socket-activated"));
    CHECK(refused.error().contains(UnservableDescriptorCause));
}

TEST_CASE("A node port that cannot be bound is fatal however it was configured", "[node][node-surface]")
{
    // **The provenance rule stopped applying here at #290 stage 3, and the reason is
    // that its premise went away rather than that it was wrong.** A taken DEFAULT port
    // was tolerated because the launcher reaches whatever else holds it -- a
    // `fastcached` on this machine, almost always -- so the build still worked and
    // what was lost was a cache tier nobody asked for. That rested on the worker
    // having a compile port of its OWN. It has none now: one 0xFC port, and without it
    // nowhere for a dispatched compile to arrive.
    //
    // Continuing would be invisible rather than merely degraded. `--scheduler` is
    // required, so every node registers, and the registrars are built from
    // `AdvertisedEndpoint(cfg)` -- the CONFIGURATION, not the listener -- so the bind
    // failing does not reach them. The node advertises an address nothing answers and
    // every client meets a failed connection and compiles locally, which is silent by
    // design.
    //
    // Driven as a table over both things that used to decide it -- whether the address was
    // named, and whether the node serves a scheduler (its MODE, since the flag went) -- because
    // the claim is that NEITHER does any more and a case per combination would be the same
    // assertion written four times. `nodeListenExplicit` is still live elsewhere --
    // `--install-service` emits on it (#286) -- so this is the bit ceasing to decide
    // one thing, not the bit going away.
    struct Shape
    {
        bool explicitAddress;
        bool servesScheduler;
        std::string_view what;
    };
    static constexpr auto shapes = std::to_array<Shape>({
        { .explicitAddress = false, .servesScheduler = false, .what = "a defaulted address on a plain worker" },
        { .explicitAddress = true, .servesScheduler = false, .what = "an address the operator named" },
        { .explicitAddress = false, .servesScheduler = true, .what = "a defaulted address on a scheduler" },
        { .explicitAddress = true, .servesScheduler = true, .what = "a named address on a scheduler" },
    });

    for (auto const& shape: shapes)
    {
        CAPTURE(shape.what);

        NodeIoLoop io;
        CapturingLogger logger;
        AtomicMetricsSink metrics;
        NamedResponder cache { "cache" };
        NamedResponder scheduler { "scheduler" };

        auto [cfg, port] = BaseConfig();
        cfg.nodeListenExplicit = shape.explicitAddress;
        // A scheduler is a node the formation makes one: a solitary node running consensus.
        if (shape.servesScheduler)
            Testing::ShapeAsFirstStart(cfg);
        REQUIRE(ServesScheduler(cfg) == shape.servesScheduler);

        auto holder = BlockingListener::Bind("127.0.0.1", port);
        REQUIRE(holder);
        REQUIRE(holder->IsBound());

        EveryNodeOwners owners;
        auto refused = StartNodeSurfaceOrExplain(
            io,
            cfg,
            owners.Around(SurfaceComponents { .cache = &cache, .scheduler = shape.servesScheduler ? &scheduler : nullptr }),
            std::nullopt,
            metrics,
            logger);
        REQUIRE_FALSE(refused.has_value());

        // The flag, so an operator knows what to edit.
        CHECK(refused.error().contains("--listen-node"));

        // And the REMEDY, not merely the diagnosis. "cannot bind" is a wall; what an
        // operator needs is what is almost certainly holding the port and that this
        // node does not need it. Asserted because a message is the entire user
        // interface of a startup refusal, and a diagnosis-only one passes every test
        // that checks the refusal happened.
        CHECK(refused.error().contains("fastcached"));
        CHECK(refused.error().contains("stop it, or give"));

        // The tolerated outcome is gone, and named so a reinstated warning fails here
        // rather than passing as "it refused for some reason".
        CHECK_FALSE(Logged(logger, "continuing without a 0xFC port"));
    }
}

TEST_CASE("A listener missing a component every built node serves is refused by that component's name",
          "[node][node-surface][shared-cache]")
{
    // `SurfaceComponents` says these owners are never null on a built node, and this is what makes
    // that true: `main` is in no test target, and a node that dropped one would answer its whole
    // family `UnimplementedVerb` -- *this node is too old* -- with nothing red anywhere. One owner
    // at a time, each refused by its own name; the control, every owner present, starts.
    auto const [cfg, port] = BaseConfig();
    std::size_t asked = 0;
    for (auto const& row: FamilyRoutes)
    {
        if (row.presence != FamilyPresence::OnEveryBuiltNode)
            continue;
        ++asked;
        INFO("left out: " << row.component);
        NodeIoLoop io;
        CapturingLogger logger;
        AtomicMetricsSink metrics;
        EveryNodeOwners owners;
        auto components = owners.Around(SurfaceComponents {});
        components.*row.owner = nullptr;
        auto const refused = StartNodeSurfaceOrExplain(io, cfg, components, std::nullopt, metrics, logger);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains(std::format("without its {} component", row.component)));
    }
    // Session, node, live stats, the fleet document, formation and the fleet's shared cache.
    CHECK(asked == 6);

    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    EveryNodeOwners owners;
    auto const started =
        StartNodeSurfaceOrExplain(io, cfg, owners.Around(SurfaceComponents {}), std::nullopt, metrics, logger);
    REQUIRE(started.has_value());
    CHECK(*started != nullptr);
}
