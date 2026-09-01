// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIoLoop.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
                                                      std::uint8_t /*opRaw*/) const override
    {
        _refusals.push_back(_name);
        return Wire::EncodeErrorReply(Wire::ErrorCodeFor(decision), _name);
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

    (void) responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, static_cast<std::uint8_t>(Wire::Op::Store));
    (void) responder.RefusalReply(Wire::PrePayloadDecision::Unauthenticated, static_cast<std::uint8_t>(Wire::Op::Lease));

    CHECK(cache.Refusals() == std::vector<std::string> { "cache" });
    CHECK(scheduler.Refusals() == std::vector<std::string> { "scheduler" });

    // An unowned verb still gets a reply, and moves nobody's counter: there is no
    // component whose refusal it would be.
    auto const orphan =
        responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, static_cast<std::uint8_t>(Wire::Op::Compile));
    CHECK(ErrorOf(orphan) == Wire::ErrorCode::PayloadTooLarge);
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
    auto const [cfg, port] = BaseConfig();

    auto surface = StartNodeSurfaceOrExplain(io, cfg, nullptr, nullptr, nullptr, logger);
    REQUIRE(surface.has_value());
    CHECK(*surface == nullptr);
    CHECK(Logged(logger, "serving no 0xFC port"));
}

TEST_CASE("A node whose only component is its worker opens no 0xFC port", "[node][node-surface]")
{
    // #290's second half gave the compile verbs a component here, and it deliberately
    // did NOT give this node a port. The row is what decides whether the surface is
    // served, and it still answers on the two components with nowhere else to go -- so
    // a worker with no tier and no scheduler binds nothing, and its compiles arrive on
    // the compile port exactly as they always have. Opening one anyway would put a
    // socket on the machine that `--print-surfaces` never printed.
    //
    // The sentence matters as much as the outcome: an operator reading "--listen-node
    // is empty" about a flag they left at its default would go looking for a
    // configuration problem that is not there.
    NodeIoLoop io;
    CapturingLogger logger;
    NamedResponder compile { "compile" };
    auto [cfg, port] = BaseConfig();
    cfg.cacheMemoryBytes = 0; // nowhere to keep objects, so no tier is built
    cfg.cacheDir.clear();
    REQUIRE_FALSE(cfg.serveScheduler);
    REQUIRE_FALSE(cfg.nodeListen.empty());

    auto surface = StartNodeSurfaceOrExplain(io, cfg, nullptr, nullptr, &compile, logger);
    REQUIRE(surface.has_value());
    CHECK(*surface == nullptr);
    CHECK(Logged(logger, "no cache tier and no scheduler"));
    CHECK_FALSE(Logged(logger, "--listen-node is empty"));
}

TEST_CASE("An emptied --listen-node closes the port and says so", "[node][node-surface]")
{
    NodeIoLoop io;
    CapturingLogger logger;
    NamedResponder cache { "cache" };
    NodeConfig cfg;
    cfg.nodeListen.clear();

    auto surface = StartNodeSurfaceOrExplain(io, cfg, &cache, nullptr, nullptr, logger);
    REQUIRE(surface.has_value());
    CHECK(*surface == nullptr);
    CHECK(Logged(logger, "--listen-node is empty"));
}

TEST_CASE("A taken DEFAULT node port is a warning and a taken NAMED one is fatal", "[node][node-surface]")
{
    // The provenance rule, unchanged in substance by the merge (#286): a named address
    // is a promise and a broken promise is fatal, while a default one an operator never
    // asked for must not stop a node whose launcher will simply reach whatever else
    // holds the port.
    NodeIoLoop io;
    CapturingLogger logger;
    NamedResponder cache { "cache" };

    auto const [cfg, port] = BaseConfig();
    auto holder = BlockingListener::Bind("127.0.0.1", port);
    REQUIRE(holder);
    REQUIRE(holder->IsBound());

    auto defaulted = cfg;
    defaulted.nodeListenExplicit = false;
    auto tolerated = StartNodeSurfaceOrExplain(io, defaulted, &cache, nullptr, nullptr, logger);
    REQUIRE(tolerated.has_value());
    CHECK(*tolerated == nullptr);
    CHECK(Logged(logger, "continuing without a 0xFC port"));

    auto named = cfg;
    named.nodeListenExplicit = true;
    auto refused = StartNodeSurfaceOrExplain(io, named, &cache, nullptr, nullptr, logger);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("--listen-node"));
}

TEST_CASE("A scheduler that cannot bind is fatal even on a defaulted address", "[node][node-surface]")
{
    // The one thing the merge CHANGED about the provenance rule, and the reason it is
    // stated rather than discovered. A default cache port already held by a fastcached
    // stays a warning -- the launcher reaches that daemon and the build works. The same
    // listener now also carries the scheduler verbs, and a scheduler that is not
    // listening is the "silently cannot work" shape a fleet never recovers from.
    NodeIoLoop io;
    CapturingLogger logger;
    NamedResponder scheduler { "scheduler" };

    auto [cfg, port] = BaseConfig();
    cfg.nodeListenExplicit = false;
    cfg.serveScheduler = true;

    auto holder = BlockingListener::Bind("127.0.0.1", port);
    REQUIRE(holder);
    REQUIRE(holder->IsBound());

    auto refused = StartNodeSurfaceOrExplain(io, cfg, nullptr, &scheduler, nullptr, logger);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("--serve-scheduler"));
}
