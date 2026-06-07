// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/HealthProbe.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <thread>

using namespace FastCache;

namespace
{

/// Accept exactly one connection on `listener` and return whether it succeeded.
/// A free function (not a capturing lambda) so it is a safe coroutine body.
/// @param listener The bound listener to accept on.
[[nodiscard]] Task<bool> AcceptOne(IListener* listener)
{
    auto r = co_await listener->Accept();
    co_return r.has_value();
}

} // namespace

TEST_CASE("HttpHealthProbe succeeds against a live /healthz and fails otherwise", "[net][health]")
{
    constexpr std::uint16_t Port = 19287;
    auto listener = BlockingListener::Bind("127.0.0.1", Port);
    if (!listener || !listener->IsBound())
    {
        SUCCEED("could not bind test port; skipping");
        return;
    }
    // Poll accept() so Shutdown() is observed and the server jthread joins on
    // every platform — POSIX does not unblock a parked accept() on Close(), so
    // without this the test would hang on Linux at scope exit.
    listener->SetTimeouts(std::chrono::milliseconds { 100 }, std::chrono::seconds { 1 });

    NullLogger logger;
    AtomicMetricsSink metrics;
    AdminHttpServer server { *listener, metrics, [] { return MetricsSnapshot {}; }, logger };

    std::jthread serverThread { [&server] { FastCache::SyncRun(server.Run()); } };

    // Give the accept loop a moment to park on Accept().
    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });

    CHECK(HttpHealthProbe("127.0.0.1", Port, "/healthz"));
    // A path that 404s is not "200", so the probe must report unhealthy.
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/nope"));

    server.Shutdown();
}

TEST_CASE("HttpHealthProbe fails when nothing is listening", "[net][health]")
{
    // An unbound port yields a connection refused -> unhealthy, not a hang.
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", 19288, "/healthz"));
}

TEST_CASE("HttpHealthProbe rejects a non-200 response whose body contains \" 200 \"", "[net][health]")
{
    // A previous implementation declared the peer healthy whenever the
    // substring " 200 " appeared anywhere in the first 256 bytes — including
    // inside a 5xx error page body. The probe must parse the HTTP status line
    // strictly: "HTTP/1.x 200 ...".
    constexpr std::uint16_t Port = 19290;
    auto listener = BlockingListener::Bind("127.0.0.1", Port);
    if (!listener || !listener->IsBound())
    {
        SUCCEED("could not bind test port; skipping");
        return;
    }
    listener->SetTimeouts(std::chrono::milliseconds { 100 }, std::chrono::seconds { 5 });

    auto const respond500 = [](IListener* l) -> Task<void> {
        auto accepted = co_await l->Accept();
        if (!accepted.has_value())
            co_return;
        std::array<char, 256> req {};
        (void) co_await (*accepted)->Read(std::span<std::byte> { reinterpret_cast<std::byte*>(req.data()), req.size() });
        constexpr std::string_view Reply { "HTTP/1.1 500 Internal Server Error\r\n"
                                           "Content-Type: text/plain\r\n"
                                           "Content-Length: 27\r\n"
                                           "\r\n"
                                           "expected 200 OK got 5xx err" };
        (void) co_await (*accepted)->Write(
            std::span<std::byte const> { reinterpret_cast<std::byte const*>(Reply.data()), Reply.size() });
        (*accepted)->Close();
    };
    std::jthread acceptor { [&listener, &respond500](std::stop_token const& stop) {
        while (!stop.stop_requested())
            FastCache::SyncRun(respond500(listener.get()));
    } };

    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/healthz"));

    acceptor.request_stop();
    listener->Close();
}

TEST_CASE("HttpHealthProbe times out (not hangs) against an accept-but-silent peer", "[net][health]")
{
    // A listener that accepts the TCP connection but never sends a response: the
    // probe's recv timeout must make it return unhealthy within a few seconds
    // rather than blocking forever. We accept exactly one connection and hold it
    // open without replying.
    constexpr std::uint16_t Port = 19289;
    auto listener = BlockingListener::Bind("127.0.0.1", Port);
    if (!listener || !listener->IsBound())
    {
        SUCCEED("could not bind test port; skipping");
        return;
    }
    listener->SetTimeouts(std::chrono::milliseconds { 100 }, std::chrono::seconds { 5 });

    std::jthread acceptor { [&listener](std::stop_token const& stop) {
        // Accept and then sit on the connection (never write) until asked to stop.
        auto accepted = FastCache::SyncRun(AcceptOne(listener.get()));
        static_cast<void>(accepted);
        while (!stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    } };

    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });

    auto const start = std::chrono::steady_clock::now();
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/healthz"));
    auto const elapsed = std::chrono::steady_clock::now() - start;
    // It must return on the bounded probe timeout (~3s), not hang indefinitely.
    CHECK(elapsed < std::chrono::seconds { 10 });

    acceptor.request_stop();
    listener->Close();
}
