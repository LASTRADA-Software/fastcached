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
        service.SetRole(Distributed::SchedulerRole::Leader, {});
    }

    ManualClock clock;
    AtomicMetricsSink metrics;
    Distributed::SchedulerService service { clock, metrics };
    Distributed::SchedulerProtocol protocol { service };
    // Loopback, because that is the host a test connection arrives from. The
    // endpoint is given with a port so the constructor's host/endpoint collapse is
    // exercised rather than bypassed.
    Distributed::ClusterMembership membership { { "127.0.0.1:7000" } };
    SchedulerResponder responder { protocol, membership };
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
} // namespace

TEST_CASE("An unparseable --listen-scheduler is refused, not guessed at", "[node][scheduler]")
{
    Fleet fleet;
    auto const started =
        FrameEndpoint::Start(fleet.io, "not-a-port", "127.0.0.1", fleet.responder, "scheduler", fleet.logger);

    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains("not-a-port"));
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
    auto const started =
        FrameEndpoint::Start(fleet.io, unreachable, "127.0.0.1", fleet.responder, "scheduler", fleet.logger);

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

    auto started =
        FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", fleet.responder, "scheduler", fleet.logger);
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
    auto started =
        FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", fleet.responder, "scheduler", fleet.logger);
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
    auto started =
        FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", fleet.responder, "scheduler", fleet.logger);
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
    // Membership is checked inside the service, i.e. AFTER the frame is read, so an
    // unauthenticated peer can make this endpoint buffer whatever it declares. That is
    // the hole `OpDescriptor::maxPayload` closes for AUTH on the cache port, and it is
    // closed the same way here. The refusal names the ceiling, because "too large"
    // without it tells an operator nothing about a 64 KiB limit -- and the check is on
    // the DECLARED length, so the bytes are never taken.
    Fleet fleet;
    auto const port = FreePort();
    auto started =
        FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", fleet.responder, "scheduler", fleet.logger);
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

    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return 64ULL * 1024ULL;
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
    std::atomic<std::size_t> _entered { 0 };
    std::atomic<std::size_t> _answered { 0 };
    std::size_t _concurrent { 8 };
    std::size_t _budget { 0 };
};

/// A FETCH frame, the smallest thing the cache surface answers.
[[nodiscard]] std::vector<std::byte> Fetch(std::string_view key)
{
    return Wire::EncodeFetch(key);
}

} // namespace

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
    auto endpoint = FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", responder, "cache", fleet.logger);
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
    auto endpoint =
        FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", fleet.responder, "scheduler", fleet.logger);
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
    auto endpoint =
        FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", fleet.responder, "scheduler", fleet.logger);
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
    auto endpoint = FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", responder, "cache", fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    // Three requests down the one connection. If the cap counted requests, the second
    // would be refused; it holds one slot for all three.
    for (auto const attempt: { "first", "second", "third" })
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
    auto endpoint = FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", responder, "cache", fleet.logger);
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

TEST_CASE("A foreign magic still closes the connection", "[node][frame]")
{
    // The one case that must NOT resynchronize: with no recognisable header there is
    // no declared length, so there is nowhere to skip to and no framing in which a
    // reply would mean anything. Kept as a case so the loop above cannot quietly turn
    // this into an infinite one.
    Fleet fleet;
    auto const port = FreePort();
    auto endpoint =
        FrameEndpoint::Start(fleet.io, std::to_string(port), "127.0.0.1", fleet.responder, "scheduler", fleet.logger);
    REQUIRE(endpoint.has_value());
    fleet.Serve();

    Conversation conversation { port };

    std::array<std::byte, Wire::RequestHeaderSize> foreign {};
    foreign.fill(std::byte { 0x7F });

    CHECK(conversation.Send(foreign).empty());
}
