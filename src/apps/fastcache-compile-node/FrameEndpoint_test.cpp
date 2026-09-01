// SPDX-License-Identifier: Apache-2.0
#include "FrameEndpoint.hpp"
#include "NodeIoLoop.hpp"
#include "Responders.hpp"

#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// A scheduler that leads, its protocol, and an oracle admitting loopback.
///
/// Bundled because every case needs all four over one clock, and wiring them by
/// hand at each would spend more lines on setup than on the property asserted.
struct Fleet
{
    Fleet()
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    }

    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    ManualWallClock wallClock;
    Distributed::SchedulerService service { clock, wallClock, metrics, schedulerLogger, {}, {} };
    Distributed::SchedulerProtocol protocol { service };
    // Loopback, because that is the host a test connection arrives from. The
    // endpoint is given with a port so the constructor's host/endpoint collapse is
    // exercised rather than bypassed.
    Distributed::ClusterMembership membership { { "127.0.0.1:7000" } };
    SchedulerResponder responder { protocol, membership, metrics };
    NullLogger logger;

    /// The reactor every endpoint in a case accepts on.
    ///
    /// Declared LAST so it is destroyed FIRST, which is the wrong way round for a
    /// production wiring and exactly right here: a case owns its endpoint as a local
    /// that outlives the fixture, so the reactor must still be turning when that
    /// endpoint's destructor posts its closes onto it. `Serve()` below is what makes
    /// the ordering explicit at each use.
    NodeIoLoop io;

    /// Start the loop once every endpoint in this case has bound and adopted.
    void Serve()
    {
        io.Start();
    }
};

/// A port nothing is listening on right now.
///
/// `ParseTcpPort` rejects `0` -- correctly, since as a CLI value it names no port an
/// operator could dial -- so "let the OS choose" is not reachable through `Start`.
/// Binding a probe and releasing it is how the other endpoint's tests find one.
/// @return A free TCP port on loopback.
[[nodiscard]] std::uint16_t FreePort()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    auto const port = probe->BoundPort();
    probe.reset();
    return port;
}

/// Read until @p received holds at least @p count bytes.
///
/// A free function taking raw pointers rather than a lambda closing over the
/// accumulator: a capturing lambda coroutine outlives the expression that created
/// it, which `cppcoreguidelines-avoid-capturing-lambda-coroutines` rejects outright,
/// and a reference parameter is the same hazard spelled differently
/// (`...-avoid-reference-coroutine-parameters`). `ServeConnection` is shaped this way
/// for the same reason.
/// @param peer Connected socket.
/// @param received Accumulator, appended to; must outlive the awaiting caller.
/// @param count How many bytes must be present before this returns.
/// @return False when the peer closed before that many arrived.
[[nodiscard]] Task<bool> ReadAtLeast(ISocket* peer, std::vector<std::byte>* received, std::size_t count)
{
    while (received->size() < count)
    {
        std::array<std::byte, 4096> chunk {};
        auto const want = std::min(chunk.size(), count - received->size());
        auto const read = co_await peer->Read(std::span<std::byte> { chunk.data(), want });
        if (!read.has_value() || *read == 0)
            co_return false;
        received->insert(received->end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read));
    }
    co_return true;
}

/// Read exactly one framed reply: the header, then the payload it declares.
///
/// Reading to EOF instead would work only against a server that closes after
/// answering, which is what this endpoint used to do and no longer does -- against a
/// connection the server keeps, it would block until the sweeper closed the socket
/// and add `RequestTimeout` to every case. So this reads what the protocol says is
/// there, which is also what lets a caller send a second request afterwards.
/// @param peer Connected socket.
/// @return Header plus payload, or empty when the peer closed without answering.
[[nodiscard]] Task<std::vector<std::byte>> ReadOneReply(ISocket* peer)
{
    std::vector<std::byte> received;

    if (!co_await ReadAtLeast(peer, &received, Wire::ReplyHeaderSize))
        co_return std::vector<std::byte> {};

    auto const header = Wire::DecodeReplyHeader(received);
    if (!header.has_value())
        co_return received;

    if (!co_await ReadAtLeast(peer, &received, Wire::ReplyHeaderSize + header->payloadLength))
        co_return std::vector<std::byte> {};
    co_return received;
}

/// Send one frame to `port` and read the reply.
/// @param port Where the endpoint is listening.
/// @param frame The request, header included.
/// @return The reply bytes, empty when the peer closed without answering.
[[nodiscard]] std::vector<std::byte> Exchange(std::uint16_t port, std::span<std::byte const> frame)
{
    BlockingConnector connector;
    auto socket = SyncRun(connector.Connect("127.0.0.1", port, 5s));
    REQUIRE(socket.has_value());

    // One coroutine for the whole exchange: `SyncRun` drives a `Task`, so the awaits
    // have to live inside one rather than be called individually.
    auto reply = SyncRun([](ISocket* peer, std::vector<std::byte> request) -> Task<std::vector<std::byte>> {
        auto const written = co_await peer->Write(std::span<std::byte const> { request });
        if (!written.has_value())
            co_return std::vector<std::byte> {};
        co_return co_await ReadOneReply(peer);
    }((*socket).get(), std::vector<std::byte> { frame.begin(), frame.end() }));

    (*socket)->Close();
    return reply;
}

/// One connection, held open across several requests.
///
/// What `Exchange` cannot express: the heartbeat loop and `Cc::Exchange` both send
/// more than one frame down a connection they opened once, and that is the property
/// #176 was about.
class Conversation
{
  public:
    explicit Conversation(std::uint16_t port)
    {
        auto socket = SyncRun(_connector.Connect("127.0.0.1", port, 5s));
        REQUIRE(socket.has_value());
        _socket = std::move(*socket);
    }

    /// Send one frame and read one reply, leaving the connection open.
    /// @param frame The request, header included.
    /// @return The reply, or empty when the peer closed without answering.
    [[nodiscard]] std::vector<std::byte> Send(std::span<std::byte const> frame)
    {
        return SyncRun([](ISocket* peer, std::vector<std::byte> request) -> Task<std::vector<std::byte>> {
            auto const written = co_await peer->Write(std::span<std::byte const> { request });
            if (!written.has_value())
                co_return std::vector<std::byte> {};
            co_return co_await ReadOneReply(peer);
        }(_socket.get(), std::vector<std::byte> { frame.begin(), frame.end() }));
    }

    ~Conversation()
    {
        if (_socket)
            _socket->Close();
    }

    Conversation(Conversation const&) = delete;
    Conversation(Conversation&&) = delete;
    Conversation& operator=(Conversation const&) = delete;
    Conversation& operator=(Conversation&&) = delete;

  private:
    BlockingConnector _connector;
    std::unique_ptr<ISocket> _socket;
};

/// The error code of a refusal, or nullopt when the reply is not one.
[[nodiscard]] std::optional<Wire::ErrorCode> ErrorOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != Wire::Status::Error || header->payloadLength == 0)
        return std::nullopt;
    return static_cast<Wire::ErrorCode>(reply[Wire::ReplyHeaderSize]);
}
/// A configuration serving @p surface at @p spec.
///
/// Written THROUGH the surface's own row rather than by naming the config field, so
/// a test cannot reach a port by a route production code no longer has. The row
/// already knows where each surface's text lives; a helper that hard-coded
/// `cfg.nodeListen` would be a second copy of that mapping, which is the fifth
/// place again in test form.
/// @param surface Which surface to configure.
/// @param spec The address it should serve.
/// @return A configuration serving exactly that.
[[nodiscard]] NodeConfig ConfigFor(NodeSurface surface, std::string spec)
{
    auto const& row = RowFor(surface);
    // Every surface these cases drive carries spec text; the compile port is the one
    // that does not, and it is not served through `FrameEndpoint`.
    REQUIRE(row.spec != nullptr);

    NodeConfig cfg;
    cfg.*row.spec = std::move(spec);
    return cfg;
}

/// A configuration serving @p surface on loopback at @p port.
///
/// Loopback is spelled out rather than left to the surface's own default, and that
/// is deliberate: the node port's default host is the WILDCARD once the node
/// schedules, so a bare port here would bind every interface on a developer's machine
/// and on CI the moment a case sets `--serve-scheduler`.
/// @param surface Which surface to configure.
/// @param port The port it should serve.
/// @return A configuration serving that surface on loopback.
[[nodiscard]] NodeConfig LoopbackFor(NodeSurface surface, std::uint16_t port)
{
    return ConfigFor(surface, std::format("127.0.0.1:{}", port));
}
} // namespace

TEST_CASE("A surface with no address to bind is refused, not guessed at", "[node][scheduler]")
{
    // `Start` no longer parses -- it takes a surface and asks the row. A malformed
    // address is refused by `StartupPolicyRejection`, which walks the same rows and
    // echoes what the operator typed; that is asserted in `NodeConfig_test`, where
    // the message lives. What is left here is the shape this factory still has to
    // refuse: a surface it was asked to serve that resolves to no address at all.
    Fleet fleet;
    // Emptied rather than defaulted: `--listen-node` carries a non-empty default that
    // every ordinary node runs with, so a default-constructed config now resolves to
    // an address rather than to nothing.
    NodeConfig unserved;
    unserved.nodeListen.clear();
    auto const started = FrameEndpoint::Start(fleet.io, NodeSurface::Node, unserved, fleet.responder, fleet.logger);

    REQUIRE_FALSE(started.has_value());
    // Naming the flag, because an operator reading it has to know which surface went
    // unserved -- the endpoint knows, and a bare "no address" would not say.
    CHECK(started.error().contains("--listen-node"));
}

TEST_CASE("An endpoint that cannot bind reports why", "[node][scheduler]")
{
    // Provoked with an address this host does not hold -- 192.0.2.1 is RFC 5737
    // documentation space -- rather than by binding a port twice, which needs a
    // second listener kept alive for the duration and asserts nothing this does not.
    // That a port already being served is refused belongs where the option refusing
    // it is set, and is asserted there (SocketAddress_test.cpp, issue #85).
    Fleet fleet;
    auto const unreachable = std::string { "192.0.2.1:6674" };
    auto const started = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, ConfigFor(NodeSurface::Node, unreachable), fleet.responder, fleet.logger);

    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains(unreachable));
}

TEST_CASE("Destroying the endpoint stops it, with nothing to remember", "[node][scheduler]")
{
    // The reason this is a class rather than three locals in main(): the listener must
    // be closed before the serving thread can be joined, and a jthread whose loop is
    // still parked in accept() joins never.
    Fleet fleet;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    auto const port = probe->BoundPort();
    probe.reset();

    auto started = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), fleet.responder, fleet.logger);
    REQUIRE(started.has_value());
    fleet.Serve();

    // Destroyed on ANOTHER thread and waited for with a deadline, deliberately, and
    // that is sharper now than it was: shutdown posts its closes onto the reactor and
    // then waits for the connections to end, so a stop issued from a thread that is
    // not the reactor's is the ordinary case and the one that must not hang. A test
    // that hangs when the order is wrong reports a defect as a suite timeout naming
    // nothing, which this repository has already paid for once.
    auto stopped = std::async(std::launch::async, [&] {
        started->reset();
        return true;
    });

    REQUIRE(stopped.wait_for(15s) == std::future_status::ready);
    REQUIRE(stopped.get());

    // And the port is free again, which is only true if the listener was really closed
    // rather than leaked with its thread still parked on it.
    auto again = BlockingListener::Bind("127.0.0.1", port);
    REQUIRE(again);
    CHECK(again->IsBound());
}

TEST_CASE("A member registers over a real socket", "[node][scheduler]")
{
    // End to end through the listener, because everything below it is already covered
    // by `SchedulerProtocol_test` and what this adds is the wiring: that the peer's
    // host reaches the oracle, and that a reply comes back framed.
    Fleet fleet;
    auto const port = FreePort();
    auto started = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), fleet.responder, fleet.logger);
    REQUIRE(started.has_value());

    // Bound and adopted; now let the loop accept. Separating the two is the ordering
    // production uses, and it is what lets several endpoints share one reactor.
    fleet.Serve();

    // The endpoint reports what it actually bound, which is what makes `0` usable
    // elsewhere and what a log line has to say to be worth printing.
    CHECK((*started)->BoundEndpoint() == std::format("127.0.0.1:{}", port));

    auto const frame = Wire::EncodeRegister(
        Wire::RegisterRequest { .fingerprint = "gcc-14", .endpoint = "127.0.0.1:7100", .slots = 2, .acceptedCodecs = {} });
    auto const reply = Exchange(port, frame);

    REQUIRE_FALSE(reply.empty());
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Wire::Status::Ok);
}

TEST_CASE("This machine is admitted whatever the member list says", "[node][scheduler]")
{
    // The rule that makes an unconfigured node useful and still closed. Anti-leeching
    // exists to stop OTHER machines spending capacity they do not contribute; a
    // process on this host already has this host's CPU, and the `fastcache-cc` a
    // developer runs against their own node is the whole reason the node is there.
    //
    // The member list names only a remote host, so before this rule a node whose
    // operator had listed their peers would have refused their own builds -- a fleet
    // that looks configured and serves nobody locally.
    Fleet fleet;
    fleet.membership.Publish({ "10.0.0.1:7000" });

    auto const port = FreePort();
    auto started = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), fleet.responder, fleet.logger);
    REQUIRE(started.has_value());

    // Bound and adopted; now let the loop accept. Separating the two is the ordering
    // production uses, and it is what lets several endpoints share one reactor.
    fleet.Serve();

    // The endpoint reports what it actually bound, which is what makes `0` usable
    // elsewhere and what a log line has to say to be worth printing.
    CHECK((*started)->BoundEndpoint() == std::format("127.0.0.1:{}", port));

    // Refused for having no worker, which is the fleet answering the question rather
    // than the gate refusing to hear it. `NotAMember` here would be the regression.
    auto const frame = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
    CHECK(ErrorOf(Exchange(port, frame)) == Wire::ErrorCode::NoWorker);
}

TEST_CASE("A stranger is refused the fleet", "[node][scheduler]")
{
    // Not over a socket, and that is a consequence of the rule above rather than a
    // weaker test: every connection a test can make to itself arrives from loopback,
    // and loopback is now a member by construction. Naming the peer directly is the
    // only way left to express "a different machine" -- and it is the same string the
    // transport would have handed over, so nothing is being simulated away.
    Fleet fleet;
    fleet.membership.Publish({ "10.0.0.1:7000" });

    auto const frame = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });

    CHECK(ErrorOf(SyncRun(fleet.responder.Answer(frame, "10.9.9.9"))) == Wire::ErrorCode::NotAMember);
    // And a listed peer gets past the gate to the fleet's own answer.
    CHECK(ErrorOf(SyncRun(fleet.responder.Answer(frame, "10.0.0.1"))) == Wire::ErrorCode::NoWorker);
}

TEST_CASE("An oversize frame is refused with both numbers, and never buffered", "[node][scheduler]")
{
    // The refusal names the ceiling, because "too large" without it tells an operator
    // nothing about a 64 KiB limit -- and the check is on the DECLARED length, so the
    // bytes are never taken. (The small ceiling used to double as the bound on what a
    // stranger could make this endpoint allocate, because membership was checked after
    // the frame was read. `RefusePeer` closes that directly now; see below.)
    Fleet fleet;
    auto const port = FreePort();
    auto started = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), fleet.responder, fleet.logger);
    REQUIRE(started.has_value());

    // Bound and adopted; now let the loop accept. Separating the two is the ordering
    // production uses, and it is what lets several endpoints share one reactor.
    fleet.Serve();

    // The endpoint reports what it actually bound, which is what makes `0` usable
    // elsewhere and what a log line has to say to be worth printing.
    CHECK((*started)->BoundEndpoint() == std::format("127.0.0.1:{}", port));

    // A header alone, declaring far more payload than it will ever send.
    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame,
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::Register),
                         static_cast<std::uint32_t>(fleet.responder.MaxRequestBytes() + 1));

    auto const reply = Exchange(port, frame);
    REQUIRE(ErrorOf(reply) == Wire::ErrorCode::PayloadTooLarge);

    auto const payload = std::span<std::byte const> { reply }.subspan(Wire::ReplyHeaderSize + 1);
    auto const text = std::string { reinterpret_cast<char const*>(payload.data()), payload.size() };
    CHECK(text.contains(std::to_string(fleet.responder.MaxRequestBytes())));
}

namespace
{

/// A responder that can be held mid-answer, so a case can be inside the window
/// where one client is being served and another arrives.
///
/// Held with a latch rather than a sleep: a sleep makes the case a race against
/// the machine, and the property here is ordering rather than timing.
class HoldableResponder final: public IFrameResponder
{
  public:
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> /*frame*/, std::string /*peer*/) override
    {
        _entered.fetch_add(1, std::memory_order_acq_rel);
        while (_held.load(std::memory_order_acquire))
            co_await FastCache::SleepFor(*_reactor, std::chrono::milliseconds { 1 });
        _answered.fetch_add(1, std::memory_order_acq_rel);
        co_return Wire::EncodeReply(Wire::Status::Miss, {});
    }

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// Admits by default and counts every call, so a case can assert both that the
    /// refusal fired and that it fired exactly ONCE -- an uncounted refusal and a
    /// double-counted one are both worse than none, because the count is what an
    /// operator reads to know the gate works.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view /*peer*/,
                                                                   std::uint8_t opRaw) const override
    {
        _peerChecks.fetch_add(1, std::memory_order_acq_rel);
        if (auto const only = _refusedVerb.load(std::memory_order_acquire);
            only >= 0 && opRaw == static_cast<std::uint8_t>(only))
        {
            _peerRefusals.fetch_add(1, std::memory_order_acq_rel);
            return Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "not admitted for this verb");
        }
        if (!_refusePeers.load(std::memory_order_acquire))
            return std::nullopt;
        _peerRefusals.fetch_add(1, std::memory_order_acq_rel);
        return Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "not admitted");
    }

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// Off by default, so every case written before #289 keeps asserting what it did.
    /// A case that turns it on is asking about the credential gate specifically.
    ///
    /// Answers per verb when a case named one (#290), because that is the only shape
    /// a merged listener can have: the same connection must be able to carry an
    /// unauthenticated cache FETCH and a scheduler verb that is refused without a
    /// credential. `AuthRequired(true)` on such a surface locks out every local build;
    /// `false` undoes #289.
    [[nodiscard]] bool AuthRequired(std::uint8_t opRaw) const noexcept override
    {
        if (auto const only = _gatedVerb.load(std::memory_order_acquire); only >= 0)
            return opRaw == static_cast<std::uint8_t>(only);
        return _authRequired.load(std::memory_order_acquire);
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// Answers whatever the case placed, and counts the calls. The count is not
    /// decoration: what separates a working gate from a door that is simply shut is
    /// that the ACCEPTED path is reached at all, so a case has to be able to say the
    /// credential was consulted rather than bypassed.
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        _credentialChecks.fetch_add(1, std::memory_order_acq_rel);
        return _outcome;
    }

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// Counts only the unauthenticated arm, mirroring `SchedulerResponder`: a size or
    /// opcode refusal says the caller is confused, an unauthenticated one says
    /// somebody is reaching for verbs they hold no secret for, and summing them would
    /// hide the second in the first.
    ///
    /// Records the verb as well, which is what a merged listener has to get right: the
    /// refusal is counted against the surface that OWNED the verb, so a case can assert
    /// the attribution rather than only the reply (#290).
    [[nodiscard]] std::vector<std::byte> RefusalReply(Wire::PrePayloadDecision decision, std::uint8_t opRaw) const override
    {
        _refusedOp.store(static_cast<int>(opRaw), std::memory_order_release);
        if (decision == Wire::PrePayloadDecision::Unauthenticated)
            _unauthenticatedRefusals.fetch_add(1, std::memory_order_acq_rel);
        return Wire::EncodeErrorReply(Wire::ErrorCodeFor(decision), {});
    }

    /// @return The verb of the last refusal, or nullopt if nothing was refused.
    [[nodiscard]] std::optional<std::uint8_t> LastRefusedOp() const noexcept
    {
        auto const raw = _refusedOp.load(std::memory_order_acquire);
        if (raw < 0)
            return std::nullopt;
        return static_cast<std::uint8_t>(raw);
    }

    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return 64ULL * 1024ULL;
    }

    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _requestTimeout;
    }

    /// How long this fake claims its answers may take.
    ///
    /// Settable because the endpoint now asks, and because a surface whose answer
    /// outlives the endpoint's header window is exactly what the deadline cases are
    /// about: a compile takes minutes, and five seconds used to close the socket
    /// underneath it without waking anything.
    /// @param window The window to report.
    void PlaceRequestTimeout(std::chrono::milliseconds window) noexcept
    {
        _requestTimeout = window;
    }

    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return _concurrent;
    }

    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return _budget;
    }

    void UseReactor(FastCache::IReactor& reactor) noexcept
    {
        _reactor = &reactor;
    }

    void Hold(bool held) noexcept
    {
        _held.store(held, std::memory_order_release);
    }

    void Limit(std::size_t concurrent, std::size_t budget) noexcept
    {
        _concurrent = concurrent;
        _budget = budget;
    }

    /// Refuse every peer before its payload is read.
    void RefuseEveryPeer(bool refuse) noexcept
    {
        _refusePeers.store(refuse, std::memory_order_release);
    }

    /// Refuse exactly one verb, admitting the rest.
    ///
    /// The shape a merged 0xFC listener needs: on three listeners the surface IS the
    /// policy, so a peer-only answer suffices; on one, the same peer must be refused
    /// a cache FETCH and served a COMPILE (#290).
    /// @param op The verb to refuse, or nullopt to refuse none.
    void RefuseOnlyVerb(std::optional<Wire::Op> op) noexcept
    {
        _refusedVerb.store(op.has_value() ? static_cast<int>(*op) : -1, std::memory_order_release);
    }

    /// Require a credential before every gated verb.
    void RequireAuth(bool required) noexcept
    {
        _authRequired.store(required, std::memory_order_release);
    }

    /// Require a credential before exactly one verb, leaving the rest open.
    ///
    /// The merged-listener shape (#290): a surface serving the cache AND the scheduler
    /// has no surface-wide answer that is right, because the cache's `false` is a
    /// property of its verbs -- a credential every local build can read is not a
    /// credential -- and not of the port they arrive on. Overrides `RequireAuth`.
    /// @param op The verb to gate, or nullopt to go back to the surface-wide answer.
    void RequireAuthOnlyFor(std::optional<Wire::Op> op) noexcept
    {
        _gatedVerb.store(op.has_value() ? static_cast<int>(*op) : -1, std::memory_order_release);
    }

    /// What the next `CheckCredential` will answer.
    void CredentialAnswers(CredentialOutcome outcome) noexcept
    {
        _outcome = outcome;
    }

    /// @return How many times the credential was actually consulted.
    [[nodiscard]] std::size_t CredentialChecks() const noexcept
    {
        return _credentialChecks.load(std::memory_order_acquire);
    }

    /// @return How many frames were refused for holding no accepted credential.
    [[nodiscard]] std::size_t UnauthenticatedRefusals() const noexcept
    {
        return _unauthenticatedRefusals.load(std::memory_order_acquire);
    }

    /// @return How many requests were refused at the door.
    [[nodiscard]] std::size_t PeerRefusals() const noexcept
    {
        return _peerRefusals.load(std::memory_order_acquire);
    }

    /// @return How many times the peer predicate was consulted at all.
    [[nodiscard]] std::size_t PeerChecks() const noexcept
    {
        return _peerChecks.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t Entered() const noexcept
    {
        return _entered.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t Answered() const noexcept
    {
        return _answered.load(std::memory_order_acquire);
    }

  private:
    FastCache::IReactor* _reactor { nullptr };
    std::atomic<bool> _held { false };
    std::atomic<bool> _refusePeers { false };
    /// The one verb to refuse, or -1. An `int` because `std::atomic<std::optional<>>`
    /// is not lock-free and this is read on the accept path.
    std::atomic<int> _refusedVerb { -1 };
    // Mutable because `RefusePeer` is `const` -- it is a predicate, and counting how
    // often it was asked must not make it look like a mutator.
    mutable std::atomic<std::size_t> _peerRefusals { 0 };
    mutable std::atomic<std::size_t> _peerChecks { 0 };
    std::atomic<bool> _authRequired { false };
    /// The one verb to gate, or -1 for the surface-wide answer. An `int` for the same
    /// reason `_refusedVerb` is one.
    std::atomic<int> _gatedVerb { -1 };
    CredentialOutcome _outcome { CredentialOutcome::NoPolicy };
    mutable std::atomic<std::size_t> _credentialChecks { 0 };
    mutable std::atomic<std::size_t> _unauthenticatedRefusals { 0 };
    /// The verb of the last refusal, or -1. An `int` for the same reason the two
    /// verb selectors above are.
    mutable std::atomic<int> _refusedOp { -1 };
    std::atomic<std::size_t> _entered { 0 };
    std::atomic<std::size_t> _answered { 0 };
    std::chrono::milliseconds _requestTimeout { FrameServer::HeaderTimeout };
    std::size_t _concurrent { 8 };
    std::size_t _budget { 0 };
};

/// A FETCH frame, the smallest thing the cache surface answers.
[[nodiscard]] std::vector<std::byte> Fetch(std::string_view key)
{
    return Wire::EncodeFetch(key);
}

} // namespace

TEST_CASE("A peer the surface refuses never gets its payload read", "[node][frame]")
{
    // #285 and #377. The acceptance is deliberately NOT "the stranger is refused" --
    // that passed while the bug was live, because the refusal happened either way,
    // just after the frame had been read. What separates the two states is whether
    // the bytes were taken first, so that is what this has to observe.
    //
    // **The instrument: declare a payload and send none of it.** With the gate ahead
    // of the read, the refusal is decided from the header alone and comes back
    // without a body. Without it, the server sits in `ReadExactly` waiting for bytes
    // that will never arrive, answers nothing, and is eventually swept by its own
    // `RequestTimeout` -- so the reply is empty. One state can answer; the other
    // cannot answer at all. No timing threshold is involved in telling them apart.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RefuseEveryPeer(true);
    // A budget the declared length exceeds, which pins the ORDER against the other
    // pre-payload gate as well: if the peer check ran after the byte budget this
    // would come back `EndpointBusy`, and a refusable peer would still be able to
    // push the surface into answering `EndpointBusy` to the peers it does serve.
    responder.Limit(8, 1024);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    // Declared, never sent. Comfortably inside `MaxRequestBytes()`, so the size
    // ceiling cannot be what refuses this -- the peer gate has to be.
    constexpr std::uint32_t Declared = 32ULL * 1024ULL;
    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Fetch), Declared);

    auto const startedAt = std::chrono::steady_clock::now();
    auto const reply = Exchange(port, frame);
    auto const waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

    // Says WHICH failure it is when it fails. A refusal decided from the header
    // requires no work at all, so a run that takes anything near the deadline
    // was blocked reading a payload -- whereas a slow machine still answers, just
    // later. Reporting the number is what lets a reader tell those apart instead of
    // guessing at a bare "no reply".
    INFO("waited " << waited.count() << "ms for a refusal that costs no work; this surface's deadline is "
                   << FrameServer::HeaderTimeout.count() << "ms. An empty reply at roughly that figure is the "
                   << "server blocked in ReadExactly for a payload it should never have asked for; an empty reply "
                   << "well under it is something else, and a late but PRESENT reply is only a slow machine.");
    REQUIRE_FALSE(reply.empty());
    REQUIRE(ErrorOf(reply) == Wire::ErrorCode::NotAMember);

    // The allocation claim rather than the refusal claim: the responder was never
    // entered, so nothing downstream of the read ran.
    CHECK(responder.Entered() == 0);
    // Exactly one, per the counter rule: the surfaces that increment a metric inside
    // this predicate would double-count every refusal if it were consulted twice.
    CHECK(responder.PeerChecks() == 1);
    CHECK(responder.PeerRefusals() == 1);
}

TEST_CASE("A peer refused before admission never gets the served window", "[node][frame]")
{
    // **The window a verb asks for is earned by being SERVED, never by being named.**
    //
    // `RequestTimeout` is per verb so a compile can run for minutes (#223, #290). Armed
    // where it first was -- immediately after the header decoded -- a stranger who
    // merely writes `Op::Compile` into a seven-byte header would have had the compile
    // window armed on their connection *before* `RefusePeer` was asked whether they are
    // a member at all. Every refusal branch ends in `reader.Skip(declaredLength)`, a
    // read from a peer that may dribble, so that is a hundred-and-twentyfold longer
    // pre-admission hold on a surface whose whole purpose here was to be harder to
    // exhaust. The comment that used to sit above the re-arm reasoned the refusals were
    // "all fast": true of the `WriteAll`, false of the skip after it.
    //
    // **The instrument is a SHORT window, and that is deliberate.** What is under test
    // is *which* deadline governs a refused peer, and the dangerous value -- ten minutes
    // -- cannot be waited out. A short one answers the same question in the opposite
    // direction and in two seconds: if the responder's window were in force here, this
    // connection would be swept almost at once; under `HeaderTimeout` it is not swept at
    // all within the observation. A functional test cannot see any of this, because the
    // refusal itself is correct either way and only the deadline differs.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RefuseEveryPeer(true);
    responder.PlaceRequestTimeout(std::chrono::milliseconds { 200 });
    // A budget the declaration fits inside, so the peer gate is unambiguously what
    // refuses: this case is about the deadline, not about which gate fired.
    responder.Limit(8, 64ULL * 1024ULL);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    BlockingConnector connector;
    auto socket = SyncRun(connector.Connect("127.0.0.1", port, 5s));
    REQUIRE(socket.has_value());
    auto* const peer = (*socket).get();

    // Declared, never sent -- so the server answers the refusal and then sits in
    // `Skip`, which is the state whose deadline this case is about.
    constexpr std::uint32_t Declared = 32ULL * 1024ULL;
    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Fetch), Declared);
    REQUIRE(SyncRun([](ISocket* sock, std::vector<std::byte> bytes) -> Task<bool> {
        auto const written = co_await sock->Write(std::span<std::byte const> { bytes });
        co_return written.has_value();
    }(peer, std::vector<std::byte> { frame.begin(), frame.end() })));

    // The refusal arrives first. Asserted so the observation below cannot pass because
    // the connection was closed for some entirely different reason.
    auto const refusal = SyncRun(ReadOneReply(peer));
    REQUIRE_FALSE(refusal.empty());
    REQUIRE(ErrorOf(refusal) == Wire::ErrorCode::NotAMember);

    // Now: is this connection still held? A read returns only when the server closes,
    // so "still pending" IS "still open". Bounded, and released by hand afterwards on
    // BOTH paths -- an unbounded wait here would turn a regression into a suite that
    // hangs rather than one that fails.
    // `ReadAtLeast` rather than a raw `Read`: `SyncRun` drives a `Task`, and the file's
    // existing helper is already the free-function-with-pointers shape the coroutine
    // lint rules require. It returns false exactly when the peer closed first.
    auto lingering = std::async(std::launch::async, [peer] {
        std::vector<std::byte> received;
        return !SyncRun(ReadAtLeast(peer, &received, 1));
    });

    // Two sweeps: past any deadline the responder asked for, and comfortably inside
    // `HeaderTimeout`. Derived from the constants rather than written as a number, so it
    // cannot silently stop covering the sweep if the cadence moves.
    auto const stillOpen = lingering.wait_for(FrameServer::SweepInterval * 2) == std::future_status::timeout;

    // Released by stopping the SERVER, never by closing this socket from here.
    //
    // Closing it would unblock the reader -- and `BlockingSocket::Close` writes members
    // that `Read` is concurrently reading on the other thread, which is a data race on
    // the socket object itself. Not theoretical: ThreadSanitizer reported exactly this,
    // three times, against the first version of this case.
    //
    // `~FrameEndpoint` posts its closes onto the reactor, which ends the connection from
    // the thread that owns it and makes the read return. That is the production shutdown
    // path, it costs the case nothing, and it leaves this socket touched by exactly one
    // thread. The close below is then ordinary cleanup, after the reader has finished.
    endpoint->reset();
    (void) lingering.get();
    (*socket)->Close();

    INFO("the responder asked for 200ms and HeaderTimeout is "
         << FrameServer::HeaderTimeout.count() << "ms; observed for " << (FrameServer::SweepInterval * 2).count()
         << "ms. A closed connection here is the responder's window governing a peer this surface had not yet "
            "admitted, which is the pre-admission hold this case exists to refuse.");
    CHECK(stillOpen);
}

TEST_CASE("An unauthenticated peer never gets its payload read either", "[node][frame]")
{
    // #289, and the same instrument as the peer gate above for the same reason: "the
    // stranger is refused" passes while the bug is live, because the refusal happens
    // either way -- just after the frame has been read. Declare a payload, send none
    // of it, and only a header-decided refusal can answer at all.
    //
    // Separate from the peer case rather than folded into it: this gate reads the
    // VERB and per-connection state, the other reads only the peer, and a case that
    // could not tell them apart would pass if one were deleted.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RequireAuth(true);
    // The peer gate must NOT be what refuses this, or the case proves nothing about
    // the credential.
    responder.RefuseEveryPeer(false);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    // Comfortably inside `MaxRequestBytes()`, so the size ceiling cannot be what
    // refuses it -- and `Fetch` is a gated verb, so the credential has to be.
    constexpr std::uint32_t Declared = 32ULL * 1024ULL;
    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Fetch), Declared);

    auto const startedAt = std::chrono::steady_clock::now();
    auto const reply = Exchange(port, frame);
    auto const waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);

    INFO("waited " << waited.count() << "ms for a refusal that costs no work; this surface's deadline is "
                   << FrameServer::HeaderTimeout.count() << "ms. An EMPTY reply near that figure is the server "
                   << "blocked in ReadExactly for a payload it should never have asked for; an empty reply well "
                   << "under it is something else, and a late but PRESENT reply is only a slow machine.");
    REQUIRE_FALSE(reply.empty());
    CHECK(ErrorOf(reply) == Wire::ErrorCode::Unauthenticated);

    // The allocation claim rather than the refusal claim.
    CHECK(responder.Entered() == 0);
    // Counted once. An uncounted refusal and a double-counted one are both worse than
    // the bug, because this number is what tells an operator the gate is working.
    CHECK(responder.UnauthenticatedRefusals() == 1);
}

TEST_CASE("An authenticated peer is served the same verb", "[node][frame]")
{
    // The control, and the half that a gate refusing EVERYTHING would fail. Without
    // it the case above is satisfied by a surface that serves nobody.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RequireAuth(true);
    responder.CredentialAnswers(CredentialOutcome::Accepted);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    // AUTH first -- itself reachable before a credential exists, or the gate would be
    // a deadlock -- then the gated verb, on the SAME connection, because that is
    // where the accepted credential lives.
    Conversation conversation { port };

    auto const authReply = conversation.Send(Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" }));
    REQUIRE_FALSE(authReply.empty());
    CHECK(ErrorOf(authReply) == std::nullopt);

    // The gated verb, on the SAME connection, because that is where the accepted
    // credential lives -- a second connection would start unauthenticated again,
    // which is itself the property that keeps one client's secret from blessing
    // everybody else's connection to a shared responder.
    auto const fetchReply = conversation.Send(Wire::EncodeFetch("k"));
    REQUIRE_FALSE(fetchReply.empty());
    CHECK(ErrorOf(fetchReply) == std::nullopt);
    CHECK(responder.CredentialChecks() == 1);
    CHECK(responder.UnauthenticatedRefusals() == 0);
    // Reached, which is the whole point: the gate let a credentialled caller through.
    CHECK(responder.Entered() == 1);
}

TEST_CASE("One peer is refused one verb and served another on the same listener", "[node][frame]")
{
    // #290's acceptance criterion, made expressible. The merge's own test is
    //
    //     a cache FETCH from another machine is refused on the merged wildcard port
    //     while a compile from that same peer succeeds
    //
    // -- same peer, two verbs, two answers. That could not be written at all while
    // `RefusePeer` took the peer and nothing else: on three listeners the SURFACE is
    // the policy, so a peer-only answer is complete; on one listener the policy
    // belongs to the verb, and a peer-only predicate has nowhere to put the
    // difference.
    //
    // No merge here. This asserts the seam can carry the question, which is the part
    // worth landing separately: the bind change that follows looks harmless in review
    // and this one does not.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RefuseOnlyVerb(Wire::Op::Fetch);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    // A COMPLETE frame, unlike the pre-payload cases above. Those declare a payload
    // and send none of it, which is what proves a refusal was decided from the header
    // -- but it also leaves the server draining bytes that never arrive, so nothing
    // else can travel on that connection. This case needs the connection afterwards,
    // and what it asserts is the discrimination rather than the ordering, which the
    // cases above already pin.
    auto const refused = conversation.Send(Wire::EncodeFetch("k"));
    REQUIRE_FALSE(refused.empty());
    CHECK(ErrorOf(refused) == Wire::ErrorCode::NotAMember);
    CHECK(responder.Entered() == 0);

    // The SAME connection, the same peer, a different verb: served. Without this the
    // case above is satisfied by a responder that refuses everything.
    std::array<std::byte, Wire::RequestHeaderSize> storeHeader {};
    WireFrame::PutHeader(storeHeader, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Store), 0);
    auto const served = conversation.Send(storeHeader);
    REQUIRE_FALSE(served.empty());
    CHECK(ErrorOf(served) == std::nullopt);
    CHECK(responder.Entered() == 1);
}

TEST_CASE("One verb needs a credential and another does not on the same listener", "[node][frame]")
{
    // The second half of what a merged listener needs, and the half that is easy to
    // land wrong. `RefusePeer` decides admission; this decides the CREDENTIAL, and on
    // one surface the two production answers are opposite and both correct: the
    // scheduler requires a credential when one is configured, the cache requires none
    // because a credential every local build can read is not a credential.
    //
    // A surface-wide answer therefore has no right value once they merge. `true`
    // refuses every local `fastcache-cc` FETCH with `Unauthenticated` -- a total
    // outage that looks like a permissions bug -- and `false` silently undoes #289,
    // leaving the scheduler verbs open on a port that faces the network, which is
    // exactly the hole #289 closed and which nothing would fail to notice.
    //
    // So, as with the verb-aware `RefusePeer` above: no merge here, just the seam
    // proven able to carry the distinction before the bind changes underneath it.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RequireAuthOnlyFor(Wire::Op::Lease);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    // A scheduler verb, unauthenticated: refused. Both verbs are `RequiresAuth` in the
    // wire table, so nothing here is decided by `PreAuth` -- if it were, this case
    // would pass with `AuthRequired` ignoring its argument entirely.
    std::array<std::byte, Wire::RequestHeaderSize> leaseHeader {};
    WireFrame::PutHeader(leaseHeader, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Lease), 0);
    auto const refused = conversation.Send(leaseHeader);
    REQUIRE_FALSE(refused.empty());
    CHECK(ErrorOf(refused) == Wire::ErrorCode::Unauthenticated);
    // Counted as the credential arm specifically -- the operator reading this counter
    // is asking whether somebody is reaching for verbs they hold no secret for, which
    // a size or opcode refusal does not answer.
    CHECK(responder.UnauthenticatedRefusals() == 1);
    CHECK(responder.Entered() == 0);

    // The same connection, still unauthenticated, a cache verb: served.
    auto const served = conversation.Send(Wire::EncodeFetch("k"));
    REQUIRE_FALSE(served.empty());
    CHECK(ErrorOf(served) == std::nullopt);
    CHECK(responder.Entered() == 1);
}

TEST_CASE("A refusal is reported with the verb that caused it", "[node][frame]")
{
    // The third and last seam #290 has to widen, and the only one that is about the
    // COUNTER rather than the decision. `RefusalReply` both encodes the refusal and
    // tallies it, so on a merged listener it has to be told which surface's verb was
    // refused -- a cache STORE that overran its ceiling counted against the scheduler
    // names the wrong subsystem, and naming the subsystem is the entire job of these
    // counters.
    //
    // The wording stays verb-blind, which is a separate decision and still the right
    // one: a peer that failed to authenticate learns nothing from being told which
    // verb it failed to reach.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RequireAuth(true);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    CHECK(responder.LastRefusedOp() == std::nullopt);

    std::array<std::byte, Wire::RequestHeaderSize> leaseHeader {};
    WireFrame::PutHeader(leaseHeader, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Lease), 0);
    Conversation conversation { port };
    auto const refused = conversation.Send(leaseHeader);
    REQUIRE_FALSE(refused.empty());
    CHECK(ErrorOf(refused) == Wire::ErrorCode::Unauthenticated);

    // The verb, not merely that something was refused. Without this the widening is
    // satisfied by a parameter nothing reads.
    CHECK(responder.LastRefusedOp() == static_cast<std::uint8_t>(Wire::Op::Lease));
}

TEST_CASE("An admitted peer is asked once and served normally", "[node][frame]")
{
    // The control. Without it a surface that refused EVERYTHING would satisfy the
    // case above while serving nobody, and the gate would look like it worked.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.RefuseEveryPeer(false);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    auto const reply = Exchange(port, Fetch("admitted"));
    REQUIRE_FALSE(reply.empty());

    CHECK(responder.PeerRefusals() == 0);
    CHECK(responder.PeerChecks() == 1);
    CHECK(responder.Entered() == 1);
    CHECK(responder.Answered() == 1);
}

TEST_CASE("A held answer does not stop another client being served", "[node][frame]")
{
    // THE regression case for the defect this whole migration exists to remove.
    // Answering used to happen inline in the accept loop, so a cache answer that
    // consulted a slow upstream held every other local client behind it -- one
    // unreachable shared cache made the node's own port unusable.
    //
    // Verified by serving connections inline again and watching only this fail.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());
    responder.Hold(true);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    // First client: reaches the responder and is held there.
    auto first = std::async(std::launch::async, [port] { return Exchange(port, Fetch("first")); });
    for (auto spin = 0; spin < 2000 && responder.Entered() == 0; ++spin)
        std::this_thread::sleep_for(1ms);
    REQUIRE(responder.Entered() == 1);
    REQUIRE(responder.Answered() == 0);

    // Second client, while the first is still held. It must reach the responder --
    // which is what serialization made impossible.
    auto second = std::async(std::launch::async, [port] { return Exchange(port, Fetch("second")); });
    for (auto spin = 0; spin < 2000 && responder.Entered() < 2; ++spin)
        std::this_thread::sleep_for(1ms);
    CHECK(responder.Entered() == 2);

    responder.Hold(false);

    // Bounded, so a regression reports as this assertion rather than as a suite
    // timeout naming nothing.
    REQUIRE(first.wait_for(15s) == std::future_status::ready);
    REQUIRE(second.wait_for(15s) == std::future_status::ready);
    CHECK_FALSE(first.get().empty());
    CHECK_FALSE(second.get().empty());
}

TEST_CASE("Two requests on one connection are both answered", "[node][frame]")
{
    // Issue #176. `ServeConnection` read one frame, answered it and closed, while two
    // callers in this tree send more than one down a connection they opened once:
    // the heartbeat loop dials once per round and then registers or heartbeats EVERY
    // toolchain over it (`main.cpp:577`), and `Cc::Exchange` writes AUTH and the
    // command back to back and reads two replies (`CacheProtocol.cpp:136`). The first
    // cost a two-toolchain worker half its fleet presence, permanently and silently;
    // the second made a credential impossible to present at all.
    //
    // The daemon serving the identical wire has always looped
    // (`CompileCacheHandler.cpp:490`), so this asserts the contract the node was the
    // only implementation to break.
    Fleet fleet;
    auto const port = FreePort();
    auto endpoint = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), fleet.responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    auto const first = conversation.Send(Wire::EncodeFetch("first"));
    REQUIRE_FALSE(first.empty());
    CHECK(Wire::DecodeReplyHeader(first).has_value());

    // The one that never arrived. Before the fix this is empty, because the server
    // closed the connection after answering the frame above.
    auto const second = conversation.Send(Wire::EncodeFetch("second"));
    REQUIRE_FALSE(second.empty());
    CHECK(Wire::DecodeReplyHeader(second).has_value());
}

TEST_CASE("A connection survives a recoverable refusal", "[node][frame]")
{
    // The framing invariant, from `.agent/rules/wire-and-protocol.md`: a frame
    // declares its own length, so a rejection is a REPLY and a resynchronization --
    // never a close. The endpoint wrote the reply and then closed anyway, which meant
    // the length the header carries bought nothing.
    Fleet fleet;
    auto const port = FreePort();
    auto endpoint = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), fleet.responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    // A COMPLETE oversize frame -- header plus the bytes it declares. Sending the
    // header alone would be a peer that broke its own framing, and there is nothing
    // for the endpoint to resynchronize to in that case; the separate scheduler case
    // covers that shape and asserts only that the refusal still arrives.
    auto const declared = fleet.responder.MaxRequestBytes() + 1;
    std::vector<std::byte> oversize(Wire::RequestHeaderSize + declared, std::byte { 0 });
    WireFrame::PutHeader(std::span<std::byte> { oversize }.first(Wire::RequestHeaderSize),
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::Register),
                         static_cast<std::uint32_t>(declared));

    auto const refusal = conversation.Send(oversize);
    REQUIRE_FALSE(refusal.empty());
    CHECK(ErrorOf(refusal) == Wire::ErrorCode::PayloadTooLarge);

    // And the connection is still usable, which is the half that was missing.
    auto const after = conversation.Send(Wire::EncodeFetch("after-the-refusal"));
    REQUIRE_FALSE(after.empty());
    CHECK(Wire::DecodeReplyHeader(after).has_value());
}

TEST_CASE("The capacity cap counts connections, not requests", "[node][frame]")
{
    // What the loop for #176 changed about the cap, made explicit so it cannot drift
    // back. While a connection was one request the two numbers were the same, and the
    // cache surface picked its value for the expensive thing -- eight object files at
    // once. Held for a whole connection instead, eight would have been eight ATTACHED
    // PEERS, so a wide build's ninth launcher, or eight peers sending almost nothing,
    // would have closed the surface to everyone else.
    //
    // Both halves are asserted here: one connection is one slot however many requests
    // it serves, and the slot is what a second connection is refused for.
    Fleet fleet;
    HoldableResponder responder;
    responder.Limit(/*concurrent*/ 1, /*budget*/ 0);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    // Three requests down the one connection. If the cap counted requests, the second
    // would be refused; it holds one slot for all three.
    for (auto const* const attempt: { "first", "second", "third" })
    {
        auto const reply = conversation.Send(Wire::EncodeFetch(attempt));
        REQUIRE_FALSE(reply.empty());
        CHECK(ErrorOf(reply) != Wire::ErrorCode::EndpointBusy);
    }

    // And the slot IS taken, so a second connection is refused by name rather than
    // admitted -- the cap still bounds something.
    //
    // Read without writing first, which keeps this case about the CAP: a client that
    // sends its request before reading exercises whether the refusal survives the
    // close as well, and that is a separate defect with a case of its own below.
    BlockingConnector connector;
    auto second = SyncRun(connector.Connect("127.0.0.1", port, 5s));
    REQUIRE(second.has_value());
    auto const refusal = SyncRun(ReadOneReply((*second).get()));
    (*second)->Close();
    CHECK(ErrorOf(refusal) == Wire::ErrorCode::EndpointBusy);
}

TEST_CASE("A capacity refusal survives the close that follows it", "[node][frame]")
{
    // The half the case above deliberately does not cover, and the half every real
    // client is on: it writes its request and THEN reads. The endpoint wrote the
    // refusal and closed without reading, so the request sat unread in the receive
    // queue -- and a close with bytes still queued is a reset, which takes the refusal
    // back out of the client's own receive buffer before it can be read.
    //
    // So the surface at capacity looked to every caller like a connection reset, which
    // names neither the surface nor the reason, while the reply that did name both had
    // been written and thrown away. The case above reads without writing and always
    // saw it, which is what isolated this from "the refusal is never written".
    Fleet fleet;
    HoldableResponder responder;
    responder.Limit(/*concurrent*/ 1, /*budget*/ 0);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    // One completed round trip before the second client arrives, so the slot is
    // provably taken rather than probably taken: the accept that takes it is on the
    // reactor thread and would otherwise be racing this one.
    Conversation holder { port };
    REQUIRE_FALSE(holder.Send(Fetch("attached")).empty());

    auto const refusal = Exchange(port, Fetch("while-attached"));
    REQUIRE_FALSE(refusal.empty());
    CHECK(ErrorOf(refusal) == Wire::ErrorCode::EndpointBusy);
}

TEST_CASE("The byte budget refuses on a connection it keeps", "[node][frame]")
{
    // `MaxInFlightBytes` is what actually bounds this surface's memory, and since the
    // connection cap stopped being a request cap it is the only thing that bounds
    // concurrent work either. It had no coverage at all -- every case ran with the
    // budget disabled -- while being the branch the loop rewrote most.
    //
    // The property is that it refuses like every other recoverable refusal: a REPLY
    // naming the budget, on a connection that is still usable afterwards. A close here
    // would make a busy moment cost every launcher a reconnect.
    Fleet fleet;
    HoldableResponder responder;
    responder.UseReactor(fleet.io.Reactor());

    // One byte of budget, so any declared payload exceeds it. Small enough to be
    // unambiguous: this is the declared length being weighed, not the bytes read.
    responder.Limit(/*concurrent*/ 0, /*budget*/ 1);

    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    auto const refusal = conversation.Send(Fetch("over-budget"));
    REQUIRE_FALSE(refusal.empty());
    CHECK(ErrorOf(refusal) == Wire::ErrorCode::EndpointBusy);

    // Never reached the responder: the budget is weighed on the DECLARED length,
    // before a payload byte is read, which is the whole reason it can bound anything.
    CHECK(responder.Entered() == 0);

    // And the connection survives it, so the peer may retry rather than reconnect.
    CHECK_FALSE(conversation.Send(Fetch("again")).empty());
}

TEST_CASE("A foreign magic still closes the connection", "[node][frame]")
{
    // The one case that must NOT resynchronize: with no recognisable header there is
    // no declared length, so there is nowhere to skip to and no framing in which a
    // reply would mean anything. Kept as a case so the loop above cannot quietly turn
    // this into an infinite one.
    Fleet fleet;
    auto const port = FreePort();
    auto endpoint = FrameEndpoint::Start(
        fleet.io, NodeSurface::Node, LoopbackFor(NodeSurface::Node, port), fleet.responder, fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    std::array<std::byte, Wire::RequestHeaderSize> foreign {};
    foreign.fill(std::byte { 0x7F });

    CHECK(conversation.Send(foreign).empty());
}
