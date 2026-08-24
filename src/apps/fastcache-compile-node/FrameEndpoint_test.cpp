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

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <future>
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

        std::vector<std::byte> received;
        std::array<std::byte, 4096> chunk {};
        while (true)
        {
            auto const read = co_await peer->Read(std::span<std::byte> { chunk });
            if (!read.has_value() || *read == 0)
                break;
            received.insert(received.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read));
        }
        co_return received;
    }((*socket).get(), std::vector<std::byte> { frame.begin(), frame.end() }));

    (*socket)->Close();
    return reply;
}

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
    // documentation space -- rather than by binding a port twice. `SocketAddress.cpp`
    // sets SO_REUSEADDR unconditionally, which on POSIX only skips TIME_WAIT but on
    // Windows lets a second socket bind an address already in use, so the obvious
    // test passes on Linux and macOS and fails on Windows (issue #85).
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

    [[nodiscard]] std::size_t MaxConcurrentRequests() const noexcept override
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
