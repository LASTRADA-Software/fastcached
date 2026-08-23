// SPDX-License-Identifier: Apache-2.0
#include "SchedulerEndpoint.hpp"

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
    NullLogger logger;
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
    auto socket = connector.Connect("127.0.0.1", port, 5s);
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
    auto const started = SchedulerEndpoint::Start("not-a-port", "127.0.0.1", fleet.protocol, fleet.membership, fleet.logger);

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
    auto const started = SchedulerEndpoint::Start(unreachable, "127.0.0.1", fleet.protocol, fleet.membership, fleet.logger);

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

    // Destroyed on another thread and waited for with a deadline, deliberately: a test
    // that HANGS when the order is wrong reports a defect as a suite timeout naming
    // nothing, which this repository has already paid for once.
    auto stopped = std::async(std::launch::async, [&] {
        auto started =
            SchedulerEndpoint::Start(std::to_string(port), "127.0.0.1", fleet.protocol, fleet.membership, fleet.logger);
        return started.has_value();
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
        SchedulerEndpoint::Start(std::to_string(port), "127.0.0.1", fleet.protocol, fleet.membership, fleet.logger);
    REQUIRE(started.has_value());

    // The endpoint reports what it actually bound, which is what makes `0` usable
    // elsewhere and what a log line has to say to be worth printing.
    CHECK((*started)->BoundEndpoint() == std::format("127.0.0.1:{}", port));

    auto const frame = Wire::EncodeRegister(
        Wire::RegisterRequest { .fingerprint = "gcc-14", .endpoint = "127.0.0.1:7100", .slots = 2, .acceptedCodecs = {} });
    auto const reply = Exchange(port, frame);

    REQUIRE_FALSE(reply.empty());
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    CHECK(header->status == Wire::Status::Ok);
}

TEST_CASE("A stranger is refused the fleet over a real socket", "[node][scheduler]")
{
    // The anti-leeching rule reaching the wire. The oracle here admits only
    // 10.0.0.1, so a loopback connection is a non-member -- which is the same
    // classification a machine outside the cluster would get, expressed in the one
    // way a test can actually arrange.
    Fleet fleet;
    fleet.membership.Publish({ "10.0.0.1:7000" });

    auto const port = FreePort();
    auto started =
        SchedulerEndpoint::Start(std::to_string(port), "127.0.0.1", fleet.protocol, fleet.membership, fleet.logger);
    REQUIRE(started.has_value());

    // The endpoint reports what it actually bound, which is what makes `0` usable
    // elsewhere and what a log line has to say to be worth printing.
    CHECK((*started)->BoundEndpoint() == std::format("127.0.0.1:{}", port));

    auto const frame = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
    CHECK(ErrorOf(Exchange(port, frame)) == Wire::ErrorCode::NotAMember);
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
        SchedulerEndpoint::Start(std::to_string(port), "127.0.0.1", fleet.protocol, fleet.membership, fleet.logger);
    REQUIRE(started.has_value());

    // The endpoint reports what it actually bound, which is what makes `0` usable
    // elsewhere and what a log line has to say to be worth printing.
    CHECK((*started)->BoundEndpoint() == std::format("127.0.0.1:{}", port));

    // A header alone, declaring far more payload than it will ever send.
    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame,
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::Register),
                         static_cast<std::uint32_t>(SchedulerServer::MaxRequestBytes + 1));

    auto const reply = Exchange(port, frame);
    REQUIRE(ErrorOf(reply) == Wire::ErrorCode::PayloadTooLarge);

    auto const payload = std::span<std::byte const> { reply }.subspan(Wire::ReplyHeaderSize + 1);
    auto const text = std::string { reinterpret_cast<char const*>(payload.data()), payload.size() };
    CHECK(text.contains(std::to_string(SchedulerServer::MaxRequestBytes)));
}
