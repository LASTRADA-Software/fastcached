// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/EpollReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/EpollSocket.hpp>

#if defined(__linux__)

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>

    #include <array>
    #include <atomic>
    #include <cstddef>
    #include <span>
    #include <string>
    #include <string_view>
    #include <thread>
    #include <vector>

    #include <unistd.h>

    #include <netinet/in.h>

namespace
{

/// A connected loopback TCP socket pair (two raw fds). The first is wrapped in
/// an EpollSocket driven by the reactor under test; the second is read with a
/// plain blocking recv on the test thread to verify the bytes on the wire.
struct LoopbackPair
{
    int reactorSide { -1 };
    int peerSide { -1 };

    LoopbackPair()
    {
        int const listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listenFd >= 0);
        if (listenFd < 0)
            return; // constrains the fd for the static analyzer on the ::bind path below
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral
        auto const bound = ::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        REQUIRE(bound == 0);
        REQUIRE(::listen(listenFd, 1) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);

        peerSide = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(peerSide >= 0);
        if (peerSide < 0)
        {
            ::close(listenFd);
            return; // constrains the fd for the static analyzer on the ::connect path below
        }
        auto const connected = ::connect(peerSide, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        REQUIRE(connected == 0);
        reactorSide = ::accept(listenFd, nullptr, nullptr);
        REQUIRE(reactorSide >= 0);
        ::close(listenFd);
    }

    ~LoopbackPair()
    {
        if (peerSide >= 0)
            ::close(peerSide);
        // reactorSide is owned by the EpollSocket once wrapped.
    }

    LoopbackPair(LoopbackPair const&) = delete;
    LoopbackPair& operator=(LoopbackPair const&) = delete;
    LoopbackPair(LoopbackPair&&) = delete;
    LoopbackPair& operator=(LoopbackPair&&) = delete;
};

/// Read exactly `expected` bytes from a blocking fd into a vector.
std::vector<std::byte> RecvExactly(int fd, std::size_t expected)
{
    std::vector<std::byte> out;
    out.reserve(expected);
    std::array<std::byte, 4096> buf {};
    while (out.size() < expected)
    {
        auto const n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0)
            break;
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    return out;
}

/// Drive a vectored write of [header][value][trailer] over an EpollSocket and
/// publish the reported byte count, then stop the reactor. A free coroutine
/// (not a capturing lambda) so its frame holds the arguments by value — no
/// dangling captures across the suspend.
/// @param reactor Reactor driving the socket.
/// @param fd Connected socket fd to wrap and own.
/// @param header Leading segment.
/// @param value Payload segment (points into caller-owned storage).
/// @param trailer Trailing segment.
/// @param reported Out: total bytes the write reported on success.
FastCache::DetachedTask DriveVectoredWrite(FastCache::EpollReactor* reactor,
                                           int fd,
                                           std::string_view header,
                                           std::span<std::byte const> value,
                                           std::string_view trailer,
                                           std::atomic<std::size_t>* reported)
{
    FastCache::EpollSocket socket { *reactor, fd };
    std::array<std::span<std::byte const>, 3> const segments {
        FastCache::AsBytes(header),
        value,
        FastCache::AsBytes(trailer),
    };
    auto const r = co_await socket.WriteVectored(segments);
    if (r.has_value())
        reported->store(*r);
    socket.Close();
    reactor->Stop();
    co_return;
}

} // namespace

TEST_CASE("EpollSocket::WriteVectored round-trips a small gathered reply", "[net][epoll]")
{
    LoopbackPair pair;
    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    std::atomic<std::size_t> reported { 0 };

    std::string_view const value = "hello";
    DriveVectoredWrite(&reactor, pair.reactorSide, "VALUE k 0 5\r\n", FastCache::AsBytes(value), "\r\n", &reported);
    std::jthread reactorThread { [&reactor] {
        reactor.Run();
    } };

    auto const received = RecvExactly(pair.peerSide, 20);
    reactorThread.join();

    REQUIRE(reported.load() == 20);
    std::string const text { reinterpret_cast<char const*>(received.data()), received.size() };
    REQUIRE(text == "VALUE k 0 5\r\nhello\r\n");
}

TEST_CASE("EpollSocket::WriteVectored streams a large value across partial writes", "[net][epoll]")
{
    LoopbackPair pair;
    // Shrink the kernel buffers so a multi-megabyte gather cannot be accepted
    // in one syscall: this forces EAGAIN mid-write and exercises the partial-
    // write cursor (advance past sent segments + offset into the partially-
    // sent one) as the reactor re-arms EPOLLOUT.
    int const smallSnd = 8192;
    ::setsockopt(pair.reactorSide, SOL_SOCKET, SO_SNDBUF, &smallSnd, sizeof(smallSnd));
    int const smallRcv = 8192;
    ::setsockopt(pair.peerSide, SOL_SOCKET, SO_RCVBUF, &smallRcv, sizeof(smallRcv));

    constexpr std::size_t kValueBytes = 4U * 1024U * 1024U; // 4 MiB
    std::vector<std::byte> value(kValueBytes);
    for (std::size_t i = 0; i < kValueBytes; ++i)
        value[i] = static_cast<std::byte>(i & 0xFF);
    std::string_view const header = "HDR:";
    std::string_view const trailer = ":END";
    auto const total = header.size() + kValueBytes + trailer.size();

    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    std::atomic<std::size_t> reported { 0 };

    DriveVectoredWrite(
        &reactor, pair.reactorSide, header, std::span<std::byte const> { value.data(), value.size() }, trailer, &reported);
    std::jthread reactorThread { [&reactor] {
        reactor.Run();
    } };

    // Drain on the test thread; the small SO_RCVBUF means the writer can only
    // make progress as we read, guaranteeing the partial-write path is hit.
    auto const received = RecvExactly(pair.peerSide, total);
    reactorThread.join();

    REQUIRE(reported.load() == total);
    REQUIRE(received.size() == total);
    std::string const head { reinterpret_cast<char const*>(received.data()), header.size() };
    REQUIRE(head == "HDR:");
    std::string const tail { reinterpret_cast<char const*>(received.data()) + header.size() + kValueBytes, trailer.size() };
    REQUIRE(tail == ":END");
    bool intact = true;
    for (std::size_t i = 0; i < kValueBytes && intact; ++i)
        intact = received[header.size() + i] == static_cast<std::byte>(i & 0xFF);
    REQUIRE(intact);
}

#endif // __linux__
