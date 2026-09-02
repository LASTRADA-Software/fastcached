// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "NodeConfig.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIoLoop.hpp"
#include "Responders.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#if !defined(_WIN32)
    #include <sys/socket.h>

    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

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

    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> /*frame*/, std::string /*peer*/) override
    {
        _answered.push_back(_name);
        co_return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedValue, _name);
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view /*peer*/,
                                                                   std::uint8_t opRaw) const override
    {
        _admitted.push_back(opRaw);
        return std::nullopt;
    }

    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _authRequired;
    }

    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return CredentialOutcome::Accepted;
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

    /// Claim, or stop claiming, that this fake accounts for its own request bytes.
    /// @param own What `HoldsOwnByteBudget` should answer.
    void ClaimOwnByteBudget(bool own) noexcept
    {
        _ownBudget = own;
    }

    /// Require a credential for every verb this fake is asked about.
    /// @param required What `AuthRequired` should answer.
    void RequireAuth(bool required) noexcept
    {
        _authRequired = required;
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

    /// @return The name recorded once per frame this fake answered.
    [[nodiscard]] std::vector<std::string> const& Answered() const noexcept
    {
        return _answered;
    }

  private:
    std::string _name;
    bool _authRequired { false };
    std::size_t _maxRequest { 1024 };
    std::size_t _maxOpen { 8 };
    std::size_t _maxInFlight { 4096 };
    bool _ownBudget { false };
    std::chrono::milliseconds _requestTimeout { FrameServer::HeaderTimeout };
    // Mutable because the three predicates recording into them are `const`: a
    // predicate that counted how often it was asked would otherwise have to look like
    // a mutator, which is the thing `RefusePeer`'s own signature refuses to do.
    mutable std::vector<std::string> _refusals;
    mutable std::vector<std::uint8_t> _admitted;
    mutable std::vector<std::string> _answered;
};

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

/// The error code an error reply carries.
/// @param reply An encoded reply frame.
/// @return Its code, or nullopt when it is not an error.
[[nodiscard]] std::optional<Wire::ErrorCode> ErrorOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != Wire::Status::Error || header->payloadLength == 0)
        return std::nullopt;
    return static_cast<Wire::ErrorCode>(reply[Wire::ReplyHeaderSize]);
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
    return SyncRun(responder.Answer(frame, std::string { "127.0.0.1" }));
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
    auto const port = probe->BoundPort();
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
    MergedResponder responder { &cache, &scheduler, nullptr };

    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Fetch))) == "cache");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Store))) == "cache");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Lease))) == "scheduler");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Register))) == "scheduler");
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::ClusterStatus))) == "scheduler");

    // AUTH follows the credential, which is the scheduler's: the cache requires none,
    // so an AUTH routed there would answer "no policy" and a peer holding the
    // scheduler's secret could never present it.
    CHECK(MessageOf(AnswerNow(responder, HeaderFor(Wire::Op::Auth))) == "scheduler");

    CHECK(cache.Answered().size() == 2);
    CHECK(scheduler.Answered().size() == 4);
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
    // The peer is `10.0.0.1`, and the two things that make this case mean anything are
    // asserted rather than assumed: it IS an admitted member, so the FETCH refusal
    // cannot be membership, and it is NOT this machine, so the refusal is locality.
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
    ManualClock clock;
    AtomicMetricsSink metrics;
    LocalCache cache { local, upstream, clock, metrics };
    CacheProxy proxy { cache, metrics };

    // This machine answers on 10.0.0.7, so 10.0.0.1 is somebody else. Injected because
    // the question is ambient: `ILocalityOracle` exists so a test can say which
    // addresses are this host's without the host having to have them.
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    CachedLocalityOracle const locality { machine, clock };

    // Admitted. Without this the FETCH would be refused for membership and the case
    // would pass having tested nothing about the merge -- and the compile would be
    // refused too, so there would be no contrast at all.
    Distributed::ClusterMembership const membership { { "10.0.0.1:7000" } };
    REQUIRE(membership.Classify("10.0.0.1") == Distributed::Membership::Member);
    REQUIRE_FALSE(locality.IsThisMachine("10.0.0.1"));

    NodeIoLoop io;
    CapturingLogger logger;
    NeverSpawns runner;
    FastCache::Testing::ScratchDirectory const scratch { "fc-290-acceptance" };
    Cc::CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, Cc::ToolchainSurvey::Completed() };
    Cc::WorkerProtocol protocol { jobs, Cc::UncheckedLeaseValidator(), { Wire::IdentityCodec }, metrics };
    ThreadPoolExecutor pool { 1 };
    CompileCapacity capacity { 1, WorkerMaxRequestBytes, std::chrono::seconds { 5 }, logger };

    CacheResponder cacheResponder { proxy, locality, metrics };
    CompileResponder compileResponder { protocol, capacity, membership, pool, io.Reactor(), metrics, logger };
    MergedResponder responder { &cacheResponder, nullptr, &compileResponder };

    constexpr auto* peer = "10.0.0.1";

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
    auto const fetchRefusal = responder.RefusePeer(peer, static_cast<std::uint8_t>(Wire::Op::Fetch));
    REQUIRE(fetchRefusal.has_value());
    CHECK(ErrorOf(Unwrap(fetchRefusal)) == Wire::ErrorCode::NotAMember);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 1);

    // --- the compile verb: the SAME peer is admitted ------------------------------
    //
    // No refusal at the peer gate, which is as far as this layer decides: a lease and
    // a compiler are the next questions and belong to the fixture that has both.
    CHECK_FALSE(responder.RefusePeer(peer, static_cast<std::uint8_t>(Wire::Op::Compile)).has_value());

    // And the cache tier still answers THIS machine, which is the other direction of
    // the same rule and the one a widened bind is most likely to break in silence.
    CHECK_FALSE(responder.RefusePeer("127.0.0.1", static_cast<std::uint8_t>(Wire::Op::Fetch)).has_value());
}

TEST_CASE("A verb no component serves is refused as unimplemented", "[node][merged-responder]")
{
    // Legitimate and common: a worker that neither caches nor schedules, and a
    // scheduler with no cache tier. `UnimplementedVerb` is the honest code -- this
    // endpoint really does not implement it -- and it is the one refusal
    // `Cc::CacheProtocol` steps over rather than treating as fatal, so a launcher that
    // meets it carries on and compiles.
    NamedResponder scheduler { "scheduler" };
    MergedResponder schedulerOnly { nullptr, &scheduler, nullptr };

    auto const fetch = AnswerNow(schedulerOnly, HeaderFor(Wire::Op::Fetch));
    CHECK(ErrorOf(fetch) == Wire::UnimplementedVerb);
    CHECK(scheduler.Answered().empty());

    // COMPILE is refused the same way when this node runs no worker to route it to.
    // It is not a family this listener cannot carry -- `CompileResponder` owns it since
    // #290's second half -- so the refusal is about a MISSING COMPONENT exactly as the
    // cache one above is, and a node passing a null one gets the honest code.
    NamedResponder cache { "cache" };
    MergedResponder both { &cache, &scheduler, nullptr };
    CHECK(ErrorOf(AnswerNow(both, HeaderFor(Wire::Op::Compile))) == Wire::UnimplementedVerb);

    // **And none of them is counted, which was decided rather than left out** (#447).
    // Every other refusal on this listener is an event; this one is the answer ordinary
    // traffic gets. A worker with no scheduler refuses every `AUTH` a `FASTCACHE_TOKEN`
    // launcher sends, once per exchange for a whole build, and a node with no tier
    // refuses every local `FETCH` -- so a counter here would be dominated by a healthy
    // build and a port scan would be invisible inside it. That is this ticket's own
    // failure reached from the other side: a series nothing can be read out of is no
    // better than one that never moves.
}

TEST_CASE("An unowned verb is refused before its payload is read", "[node][merged-responder]")
{
    // At the door rather than in `Answer`, which is what keeps a verb this node serves
    // nowhere from costing the surface a buffer -- the property #285 is about, held for
    // the new refusal as well as for the old ones.
    NamedResponder scheduler { "scheduler" };
    MergedResponder schedulerOnly { nullptr, &scheduler, nullptr };

    auto const refusal = schedulerOnly.RefusePeer("10.0.0.1", static_cast<std::uint8_t>(Wire::Op::Fetch));
    REQUIRE(refusal.has_value());
    CHECK(ErrorOf(Unwrap(refusal)) == Wire::UnimplementedVerb);

    // And an owned verb is still the owner's question to answer, not this one's.
    CHECK_FALSE(schedulerOnly.RefusePeer("10.0.0.1", static_cast<std::uint8_t>(Wire::Op::Lease)).has_value());
    REQUIRE(scheduler.Admitted().size() == 1);
    CHECK(scheduler.Admitted().front() == static_cast<std::uint8_t>(Wire::Op::Lease));

    // The fourth route, and the one #447 added: an endpoint-decided refusal about a
    // verb nobody owns. One sentence however the question arrived -- a router asked
    // about a verb it cannot place has only the one honest answer, and giving it here
    // is what keeps a peer from being told its frame was too large for a verb that was
    // never going to be answered at all.
    auto const budget =
        schedulerOnly.EndpointRefusalReply(EndpointRefusal::InFlightBudget, static_cast<std::uint8_t>(Wire::Op::Fetch), {});
    CHECK(ErrorOf(budget) == Wire::UnimplementedVerb);
}

TEST_CASE("The credential answer follows the verb, not the surface", "[node][merged-responder]")
{
    // The reason `AuthRequired` had to take the verb at all. The two components answer
    // it oppositely and both are right, so a surface-wide answer has no correct value:
    // `true` refuses every local fastcache-cc FETCH, `false` undoes #289.
    NamedResponder cache { "cache" };
    NamedResponder scheduler { "scheduler" };
    scheduler.RequireAuth(true);
    MergedResponder responder { &cache, &scheduler, nullptr };

    CHECK_FALSE(responder.AuthRequired(static_cast<std::uint8_t>(Wire::Op::Fetch)));
    CHECK(responder.AuthRequired(static_cast<std::uint8_t>(Wire::Op::Lease)));

    // Unowned -- this node runs no worker -- answers false and is unreachable anyway:
    // `RefusePeer` has already refused it, and requiring a credential for a verb nobody
    // serves would tell a stranger that one exists.
    CHECK_FALSE(responder.AuthRequired(static_cast<std::uint8_t>(Wire::Op::Compile)));
}

TEST_CASE("A refusal is counted against the component that owned the verb", "[node][merged-responder]")
{
    // A cache STORE that overran its ceiling counted against the scheduler names the
    // wrong subsystem, and naming the subsystem is what these counters are read for.
    NamedResponder cache { "cache" };
    NamedResponder scheduler { "scheduler" };
    MergedResponder responder { &cache, &scheduler, nullptr };

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
    auto const orphan =
        responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, static_cast<std::uint8_t>(Wire::Op::Compile), {});
    CHECK(ErrorOf(orphan) == Wire::UnimplementedVerb);
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

    MergedResponder both { &cache, &scheduler, nullptr };
    CHECK(both.MaxRequestBytes() == CacheRequest);
    CHECK(both.MaxInFlightBytes() == CacheInFlight);
    // The largest, not the smallest: this one surface carries both populations, and
    // the smaller ceiling would close the port to one because the other exists.
    CHECK(both.MaxOpenConnections() == CacheOpen);

    // A surface with one component reports that component's, never a fold over a
    // null one.
    MergedResponder schedulerOnly { nullptr, &scheduler, nullptr };
    CHECK(schedulerOnly.MaxRequestBytes() == SchedulerRequest);
    CHECK(schedulerOnly.MaxOpenConnections() == SchedulerOpen);
    CHECK(schedulerOnly.MaxInFlightBytes() == SchedulerInFlight);
}

TEST_CASE("A node with neither component opens no 0xFC port", "[node][node-surface]")
{
    // Not an error and not a silence: a node with no component for any verb family is
    // a supported shape, and a port opened for it would answer `UnimplementedVerb` to
    // everything. In the production binary it is now unreachable -- a node always runs
    // a worker, so a compile responder is always passed -- and the predicate stays
    // honest rather than being narrowed to the two components that can still be
    // absent.
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    auto const [cfg, port] = BaseConfig();

    auto surface = StartNodeSurfaceOrExplain(io, cfg, nullptr, nullptr, nullptr, std::nullopt, metrics, logger);
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
    REQUIRE_FALSE(cfg.serveScheduler);
    REQUIRE_FALSE(cfg.nodeListen.empty());

    auto surface = StartNodeSurfaceOrExplain(io, cfg, nullptr, nullptr, &compile, std::nullopt, metrics, logger);
    REQUIRE(surface.has_value());
    CHECK(*surface != nullptr);

    // Neither sentence is said, and both matter. The old reason is unreachable, and
    // "--listen-node is empty" about a flag left at its default would send an operator
    // looking for a configuration problem that is not there.
    CHECK_FALSE(Logged(logger, "no cache tier and no scheduler"));
    CHECK_FALSE(Logged(logger, "--listen-node is empty"));
}

TEST_CASE("An emptied --listen-node closes the port and says so", "[node][node-surface]")
{
    NodeIoLoop io;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NamedResponder cache { "cache" };
    NodeConfig cfg;
    cfg.nodeListen.clear();

    auto surface = StartNodeSurfaceOrExplain(io, cfg, &cache, nullptr, nullptr, std::nullopt, metrics, logger);
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
    // #464 added `PlatformListener::Adopt` and this closes over it.
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

    auto surface =
        StartNodeSurfaceOrExplain(io, cfg, &cache, nullptr, nullptr, std::optional { handed.Release() }, metrics, logger);
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
    auto refused = StartNodeSurfaceOrExplain(io, cfg, &cache, nullptr, nullptr, std::optional { -1 }, metrics, logger);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("socket-activated"));
}

#endif

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
    // Driven as a table over both flags that used to decide it, because the claim is
    // that NEITHER does any more and a case per combination would be the same
    // assertion written four times. `nodeListenExplicit` is still live elsewhere --
    // `--install-service` emits on it (#286) -- so this is the bit ceasing to decide
    // one thing, not the bit going away.
    struct Shape
    {
        bool explicitAddress;
        bool serveScheduler;
        std::string_view what;
    };
    static constexpr auto shapes = std::to_array<Shape>({
        { .explicitAddress = false, .serveScheduler = false, .what = "a defaulted address on a plain worker" },
        { .explicitAddress = true, .serveScheduler = false, .what = "an address the operator named" },
        { .explicitAddress = false, .serveScheduler = true, .what = "a defaulted address on a scheduler" },
        { .explicitAddress = true, .serveScheduler = true, .what = "a named address on a scheduler" },
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
        cfg.serveScheduler = shape.serveScheduler;

        auto holder = BlockingListener::Bind("127.0.0.1", port);
        REQUIRE(holder);
        REQUIRE(holder->IsBound());

        auto refused = StartNodeSurfaceOrExplain(
            io, cfg, &cache, shape.serveScheduler ? &scheduler : nullptr, nullptr, std::nullopt, metrics, logger);
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
