// SPDX-License-Identifier: Apache-2.0
#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Bytes.hpp>
    #include <FastCache/Core/Clock.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/IocpSocket.hpp>

    #include <winsock2.h>

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <atomic>
    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <span>
    #include <string>
    #include <string_view>
    #include <thread>
    #include <vector>

    #include <ws2tcpip.h>

namespace
{

/// Connect a fresh client socket to localhost:port using blocking
/// Winsock so the test can drive it from its own thread.
std::uintptr_t ConnectClient(std::uint16_t port)
{
    auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock != INVALID_SOCKET);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    auto const rc = ::connect(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr));
    REQUIRE(rc == 0);
    return static_cast<std::uintptr_t>(sock);
}

/// Find a free ephemeral port by binding a probe socket, reading the
/// assigned port, and closing.
std::uint16_t FindFreePort()
{
    auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock != INVALID_SOCKET);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::bind(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) == 0);
    int len = sizeof(addr);
    REQUIRE(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    auto const port = ntohs(addr.sin_port);
    ::closesocket(sock);
    return port;
}

FastCache::DetachedTask Echo(FastCache::IocpReactor& reactor, FastCache::IocpListener& listener)
{
    auto accept = co_await listener.Accept();
    if (!accept.has_value())
    {
        reactor.Stop();
        co_return;
    }
    auto socket = std::move(*accept);

    // Read up to 64 bytes, then echo them back.
    std::array<std::byte, 64> buf {};
    auto const r = co_await socket->Read(std::span<std::byte> { buf.data(), buf.size() });
    if (r.has_value() && *r > 0)
        (void) co_await socket->Write(std::span<std::byte const> { buf.data(), *r });
    socket->Close();
    reactor.Stop();
    co_return;
}

} // namespace

TEST_CASE("IocpReactor + IocpListener + IocpSocket round-trip", "[reactor][iocp][socket]")
{
    FastCache::Detail::EnsureNetworkInitialised();

    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    auto const port = FindFreePort();
    auto listener = FastCache::IocpListener::Bind(reactor, "127.0.0.1", port);
    REQUIRE(listener);
    REQUIRE(listener->IsBound());

    Echo(reactor, *listener);

    // Client lives on a separate thread so the reactor thread (this one)
    // can drive the accept + read + write.
    std::string response;
    std::jthread client { [port, &response] {
        auto const sock = ConnectClient(port);
        std::string_view const msg = "ping!";
        (void) ::send(static_cast<SOCKET>(sock), msg.data(), static_cast<int>(msg.size()), 0);
        std::array<char, 64> buf {};
        auto const got = ::recv(static_cast<SOCKET>(sock), buf.data(), static_cast<int>(buf.size()), 0);
        if (got > 0)
            response.assign(buf.data(), buf.data() + got);
        ::closesocket(static_cast<SOCKET>(sock));
    } };

    reactor.Run();
    client.join();
    REQUIRE(response == "ping!");
}

#endif // _WIN32
