// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <future>
#include <optional>
#include <string>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;

namespace
{
/// A snapshot provider standing in for the worker's, with no worker behind it.
/// @return A worker-shaped snapshot: no storage, a machine, an uptime.
AdminHttpServer::SnapshotProvider WorkerShapedSnapshot()
{
    return [] {
        return MetricsSnapshot { .storage = std::nullopt,
                                 .host = HostCapacity { .logicalCores = 4,
                                                        .configuredSlots = 4,
                                                        .totalMemoryBytes = 8589934592ULL,
                                                        .diskCapacityBytes = 100,
                                                        .diskFreeBytes = 50,
                                                        .busySlots = 1 },
                                 .uptime = Uptime { 1s } };
    };
}
} // namespace

TEST_CASE("An unparseable --admin-listen is refused, not guessed at", "[node][admin]")
{
    // A worker that started without the endpoint an operator asked for looks
    // healthy to the very probe that would have reported it was not, so this is
    // fatal at startup rather than a warning. The message names what was rejected,
    // because "invalid" tells an operator nothing about what to type instead.
    AtomicMetricsSink metrics;
    NullLogger logger;

    for (auto const* const spelling: { "", "not-a-port", "127.0.0.1", "6674x", "0", "70000", "[::1]6674" })
    {
        INFO("spelling '" << spelling << "'");
        auto const started = AdminEndpoint::Start(spelling, "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().contains(spelling));
    }
}

TEST_CASE("A bare port binds loopback rather than the wildcard", "[node][admin]")
{
    // The default is the security-relevant half of this flag: a scrape surface on a
    // public interface must be something an operator wrote down, not something they
    // got by typing a port. Asserted through the address the endpoint reports,
    // since that is what an operator reads back off the log line.
    AtomicMetricsSink metrics;
    NullLogger logger;

    auto const started = AdminEndpoint::Start("0", "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
    // Port 0 is refused outright -- "any free port" is an address nobody can be
    // told in advance -- so bind an ephemeral one the ordinary way instead.
    REQUIRE_FALSE(started.has_value());

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();

    auto const bare = AdminEndpoint::Start(std::to_string(port), "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
    REQUIRE(bare.has_value());
    CHECK((*bare)->BoundEndpoint() == std::format("127.0.0.1:{}", port));
}

TEST_CASE("An endpoint that cannot bind reports why", "[node][admin]")
{
    // The operator needs the address in the message, because the flag they typed
    // may have been a bare port: a failure that does not say what that resolved to
    // cannot be acted on.
    //
    // Provoked with an address this host does not hold -- 192.0.2.1 is RFC 5737
    // TEST-NET-1, reserved for documentation and assigned to no interface -- rather
    // than by taking a port twice. The obvious version of this test binds a port and
    // then asks the endpoint for the same one, and it PASSES ON POSIX AND FAILS ON
    // WINDOWS: `SocketAddress.cpp` sets SO_REUSEADDR unconditionally, which on POSIX
    // only skips TIME_WAIT but on Windows lets a second socket bind an address that
    // is already in use. An unassigned address is refused by both.
    AtomicMetricsSink metrics;
    NullLogger logger;

    auto const unreachable = std::string { "192.0.2.1:6674" };
    auto const started = AdminEndpoint::Start(unreachable, "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().contains(unreachable));
}

TEST_CASE("Destroying the endpoint stops it, with nothing to remember", "[node][admin]")
{
    // The reason this is a class rather than three locals in main(): the listener
    // must be closed before the serving thread can be joined, and a jthread whose
    // loop is still parked in accept() joins never. This case hangs rather than
    // failing if that order is ever reversed -- which is exactly the shape of the
    // `systemctl stop` defect this repository already records, so it is worth
    // having somewhere it can be reached in seconds instead of in a supervisor.
    AtomicMetricsSink metrics;
    NullLogger logger;

    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    auto const port = probe->BoundPort();
    probe.reset();

    // Destroyed on another thread and waited for with a deadline, deliberately: a
    // test that HANGS when the order is wrong reports a defect as a suite timeout
    // naming nothing, which this repository has already paid for once. It fails in
    // seconds instead, saying what it waited for.
    auto stopped = std::async(std::launch::async, [&] {
        auto started = AdminEndpoint::Start(std::to_string(port), "127.0.0.1", metrics, WorkerShapedSnapshot(), logger);
        return started.has_value();
    });

    REQUIRE(stopped.wait_for(15s) == std::future_status::ready);
    REQUIRE(stopped.get());

    // And the port is free again, which is only true if the listener was really
    // closed rather than leaked with its thread still parked on it.
    auto again = BlockingListener::Bind("127.0.0.1", port);
    REQUIRE(again);
    CHECK(again->IsBound());
}
