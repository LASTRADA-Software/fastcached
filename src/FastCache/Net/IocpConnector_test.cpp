// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/IocpConnector.hpp>

#if defined(_WIN32)

    #include <FastCache/Async/DeadlineTimer.hpp>
    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/ResumeOn.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Clock.hpp>
    #include <FastCache/Net/IocpSocket.hpp>
    #include <FastCache/Net/TcpClient.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <optional>
    #include <span>
    #include <string>
    #include <vector>

    #include <tests/Unwrap.hpp>

using namespace std::chrono_literals;

namespace
{

/// Read `want` bytes and record them.
///
/// Its own task so it can PARK on an empty socket while the caller goes on to
/// send. A read that finds data or EOF already waiting proves nothing about the
/// completion port; only one with nothing to return does.
FastCache::DetachedTask ReadInto(
    FastCache::ISocket* client, std::size_t want, std::vector<std::byte>* seen, bool* done, std::string* why)
{
    std::vector<std::byte> buffer(want);
    auto const read = co_await client->Read(buffer);
    if (read.has_value())
    {
        buffer.resize(*read);
        *seen = buffer;
        *why = "read " + std::to_string(*read);
    }
    else
        *why = read.error().ToString();
    *done = true;
    co_return;
}

/// Accept one connection and send the payload on it.
///
/// A task of its own, started BEFORE the dial. On IOCP an accept must be awaited
/// by somebody while it is outstanding: `IocpListener::Accept` issues AcceptEx
/// immediately but its op records the awaitable only in the suspend callback, so
/// a completion that arrives before anyone awaits is dropped and the accept never
/// resolves. Creating the awaitable early and awaiting it after the dial -- which
/// reads naturally and works on epoll -- deadlocks here.
FastCache::DetachedTask AcceptAndSend(FastCache::IocpListener* server, std::span<std::byte const> payload, std::string* why)
{
    auto accepted = co_await server->Accept();
    if (!accepted.has_value())
    {
        *why = "accept failed: " + accepted.error().ToString();
        co_return;
    }
    if (!co_await FastCache::SendAll(accepted.value().get(), payload))
        *why = "send failed";
    accepted.value()->Close();
    co_return;
}

/// Dial through ConnectEx and read what the acceptor sends.
///
/// A free function taking raw pointers rather than a capturing lambda: a
/// coroutine closure outlives the expression that created it.
FastCache::DetachedTask DriveExchange(FastCache::IocpReactor* loop,
                                      FastCache::IocpConnector* dialer,
                                      FastCache::IocpListener* server,
                                      std::uint16_t port,
                                      std::optional<FastCache::SocketResult>* out,
                                      std::vector<std::byte>* seen,
                                      std::string* why)
{
    *out = co_await dialer->Connect("127.0.0.1", port, 5s);
    auto& dialResult = out->value();
    if (dialResult.has_value() && dialResult.value() != nullptr)
    {
        auto readDone = false;
        ReadInto(dialResult.value().get(), 4, seen, &readDone, why);

        while (!readDone)
            co_await FastCache::ResumeOn { *loop };

        dialResult.value()->Close();
    }
    server->Close();
    loop->Stop();
    co_return;
}

} // namespace

TEST_CASE("A ConnectEx dial connects and then actually transfers bytes", "[net][iocpconnector]")
{
    // Not "did it connect": ConnectEx leaves the handle's context unset until
    // SO_UPDATE_CONNECT_CONTEXT is applied, so a socket that skipped that step
    // reports success and then fails every ordinary call made on it. Only moving
    // bytes through the returned socket distinguishes the two -- and the read is
    // arranged to park, so it also proves the handle really is on the port.
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };

    auto listener = FastCache::IocpListener::Bind(reactor, "127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        SKIP("no loopback listener available on this host");

    FastCache::InlineAddressResolver resolver;
    FastCache::IocpConnector connector { reactor, resolver, clock };

    std::optional<FastCache::SocketResult> dialed;
    std::vector<std::byte> echoed;
    std::string why;

    constexpr std::array<std::byte, 4> payload {
        std::byte { 'p' }, std::byte { 'i' }, std::byte { 'n' }, std::byte { 'g' }
    };
    AcceptAndSend(listener.get(), payload, &why);
    DriveExchange(&reactor, &connector, listener.get(), listener->BoundPort(), &dialed, &echoed, &why);

    // Bounded: a completion that never arrives -- the shape every mistake here
    // produces -- would otherwise report as a suite timeout naming nothing.
    FastCache::DeadlineTimer const watchdog {
        reactor, clock.Now() + 15s, [](void* state) { static_cast<FastCache::IocpReactor*>(state)->Stop(); }, &reactor
    };
    reactor.Run();

    REQUIRE(dialed.has_value());
    auto const& outcome = FastCache::Testing::Unwrap(dialed);
    INFO("dial outcome: " << (outcome.has_value() ? std::string { "connected" } : outcome.error().ToString()));
    REQUIRE(outcome.has_value());
    CHECK(outcome.value() != nullptr);
    INFO("exchange: " << why);
    CHECK(echoed.size() == 4);
}

#endif // _WIN32
