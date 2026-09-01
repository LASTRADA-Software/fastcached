// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/EpollReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/EpollSocket.hpp>

#if defined(__linux__)

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>
    #include <sys/time.h>

    #include <array>
    #include <atomic>
    #include <cerrno>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <span>
    #include <string>
    #include <string_view>
    #include <thread>
    #include <tuple>
    #include <utility>
    #include <vector>

    #include <fcntl.h>
    #include <unistd.h>

    #include <arpa/inet.h>
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
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

/// A bound, listening loopback descriptor prepared the way a supervisor hands
/// one over: blocking, and NOT close-on-exec. systemd passes both that way
/// deliberately, and correcting them is part of what `Adopt` is for, so a
/// fixture that pre-corrected them would leave that half untested.
struct InheritedListenFd
{
    /// Created in the default member initializer rather than assigned in the
    /// body, because `cppcoreguidelines-prefer-member-initializer` is an error
    /// here and the body still has to REQUIRE the result.
    int fd { ::socket(AF_INET, SOCK_STREAM, 0) };
    std::uint16_t port { 0 };

    InheritedListenFd()
    {
        REQUIRE(fd >= 0);
        if (fd < 0)
            return; // constrains the fd for the static analyzer on the ::bind path below
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral, so the test cannot collide with a live port
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(fd, 8) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);
    }

    /// Hand the descriptor to something that takes ownership of it.
    /// @return The descriptor; this fixture no longer closes it.
    [[nodiscard]] int Release() noexcept
    {
        auto const released = fd;
        fd = -1;
        return released;
    }

    ~InheritedListenFd()
    {
        if (fd >= 0)
            ::close(fd);
    }

    InheritedListenFd(InheritedListenFd const&) = delete;
    InheritedListenFd& operator=(InheritedListenFd const&) = delete;
    InheritedListenFd(InheritedListenFd&&) = delete;
    InheritedListenFd& operator=(InheritedListenFd&&) = delete;
};

/// Connect to 127.0.0.1:`port`, send `greeting`, and read whatever comes back.
///
/// Every wait here is bounded by `budget` (SO_SNDTIMEO / SO_RCVTIMEO), so a
/// listener that adopted a descriptor and then never accepts on it fails the
/// test by returning nothing rather than hanging the suite -- which is the
/// shape the accept case has to be able to fail in.
/// @param port Loopback port to dial.
/// @param greeting Bytes to send once connected.
/// @param budget Ceiling on the connect-send-receive exchange.
/// @return The reply bytes, or an empty string when nothing arrived in time.
std::string ExchangeOverLoopback(std::uint16_t port, std::string_view greeting, std::chrono::seconds budget)
{
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return {};
    timeval const timeout { .tv_sec = static_cast<time_t>(budget.count()), .tv_usec = 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return {};
    }
    std::ignore = ::send(fd, greeting.data(), greeting.size(), MSG_NOSIGNAL);
    std::array<char, 64> buf {};
    auto const n = ::recv(fd, buf.data(), buf.size(), 0);
    ::close(fd);
    if (n <= 0)
        return {};
    return std::string { buf.data(), static_cast<std::size_t>(n) };
}

/// Await one accept on an adopted listener, answer the client's greeting, and
/// stop the reactor. A free coroutine (not a capturing lambda) so its frame
/// holds the arguments by value -- no dangling captures across the suspend.
/// @param reactor Reactor driving the listener.
/// @param listener The adopted listener to accept on.
/// @param served Out: set when a connection was accepted and answered.
FastCache::DetachedTask DriveAdoptedAccept(FastCache::EpollReactor* reactor,
                                           FastCache::EpollListener* listener,
                                           std::atomic<bool>* served)
{
    auto accepted = co_await listener->Accept();
    if (accepted.has_value())
    {
        auto socket = std::move(*accepted);
        std::array<std::byte, 16> buf {};
        auto const read = co_await socket->Read(std::span<std::byte> { buf });
        if (read.has_value() && *read > 0)
        {
            auto const written = co_await socket->Write(FastCache::AsBytes(std::string_view { "PONG" }));
            served->store(written.has_value() && *written == 4);
        }
        socket->Close();
    }
    listener->Close();
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
    std::jthread reactorThread { [&reactor] { reactor.Run(); } };

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

    constexpr std::size_t ValueByteCount = 4U * 1024U * 1024U; // 4 MiB
    std::vector<std::byte> value(ValueByteCount);
    for (std::size_t i = 0; i < ValueByteCount; ++i)
        value[i] = static_cast<std::byte>(i & 0xFF);
    std::string_view const header = "HDR:";
    std::string_view const trailer = ":END";
    auto const total = header.size() + ValueByteCount + trailer.size();

    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    std::atomic<std::size_t> reported { 0 };

    DriveVectoredWrite(
        &reactor, pair.reactorSide, header, std::span<std::byte const> { value.data(), value.size() }, trailer, &reported);
    std::jthread reactorThread { [&reactor] { reactor.Run(); } };

    // Drain on the test thread; the small SO_RCVBUF means the writer can only
    // make progress as we read, guaranteeing the partial-write path is hit.
    auto const received = RecvExactly(pair.peerSide, total);
    reactorThread.join();

    REQUIRE(reported.load() == total);
    REQUIRE(received.size() == total);
    std::string const head { reinterpret_cast<char const*>(received.data()), header.size() };
    REQUIRE(head == "HDR:");
    std::string const tail { reinterpret_cast<char const*>(received.data()) + header.size() + ValueByteCount,
                             trailer.size() };
    REQUIRE(tail == ":END");
    bool intact = true;
    for (std::size_t i = 0; i < ValueByteCount && intact; ++i)
        intact = received[header.size() + i] == static_cast<std::byte>(i & 0xFF);
    REQUIRE(intact);
}

TEST_CASE("EpollListener::Adopt corrects the two descriptor properties a handoff omits", "[net][epoll]")
{
    InheritedListenFd inherited;
    REQUIRE(inherited.fd >= 0);
    // The precondition this test rests on: a handed-over descriptor arrives with
    // neither property, which is what `Bind` gets for free from
    // SOCK_NONBLOCK | SOCK_CLOEXEC and `Adopt` has to apply for itself.
    REQUIRE((::fcntl(inherited.fd, F_GETFL, 0) & O_NONBLOCK) == 0);
    REQUIRE((::fcntl(inherited.fd, F_GETFD, 0) & FD_CLOEXEC) == 0);

    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    auto const fd = inherited.Release();
    auto const listener = FastCache::EpollListener::Adopt(reactor, fd);
    REQUIRE(listener);
    INFO("BindError: " << listener->BindError());
    REQUIRE(listener->IsBound());

    // Non-blocking is what keeps the reactor thread out of a synchronous accept;
    // close-on-exec is what keeps a compiler the worker spawns from holding the
    // port after the worker is gone.
    REQUIRE((::fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0);
    REQUIRE((::fcntl(fd, F_GETFD, 0) & FD_CLOEXEC) != 0);
}

TEST_CASE("EpollListener::Adopt keeps the supervisor's port rather than binding one", "[net][epoll]")
{
    InheritedListenFd inherited;
    auto const port = inherited.port;
    REQUIRE(port != 0);

    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    auto const listener = FastCache::EpollListener::Adopt(reactor, inherited.Release());
    REQUIRE(listener);
    INFO("BindError: " << listener->BindError());
    REQUIRE(listener->IsBound());
    REQUIRE(listener->BindError().empty());
    REQUIRE(listener->BoundPort() == port);
}

TEST_CASE("EpollListener::Adopt accepts a connection through the reactor", "[net][epoll]")
{
    InheritedListenFd inherited;
    auto const port = inherited.port;
    REQUIRE(port != 0);

    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    auto const listener = FastCache::EpollListener::Adopt(reactor, inherited.Release());
    REQUIRE(listener);
    INFO("BindError: " << listener->BindError());
    REQUIRE(listener->IsBound());

    std::atomic<bool> served { false };
    // The accept is parked BEFORE any client connects, which is the whole point
    // of the case. `Accept()`'s synchronous fast path would satisfy a connection
    // that was already pending, and that path needs neither the epoll
    // registration nor a non-blocking descriptor -- so an `Adopt` that skipped
    // either would still pass a test that connected first.
    DriveAdoptedAccept(&reactor, listener.get(), &served);
    std::jthread reactorThread { [&reactor] { reactor.Run(); } };

    auto const reply = ExchangeOverLoopback(port, "PING", std::chrono::seconds { 10 });

    // Stop before joining: on the failing path the coroutine is still parked in
    // Accept(), so nothing else would ever end reactor.Run() and the join would
    // hang instead of reporting.
    reactor.Stop();
    reactorThread.join();

    INFO("reply from the adopted listener: '" << reply << "'");
    REQUIRE(served.load());
    REQUIRE(reply == "PONG");
}

TEST_CASE("An adopted EpollListener closes its descriptor when it is destroyed", "[net][epoll]")
{
    InheritedListenFd inherited;
    REQUIRE(inherited.fd >= 0);
    auto const fd = inherited.Release();

    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    {
        auto const listener = FastCache::EpollListener::Adopt(reactor, fd);
        REQUIRE(listener);
        INFO("BindError: " << listener->BindError());
        REQUIRE(listener->IsBound());
    }

    // Ownership transferred, so the descriptor went with the listener -- which is
    // what the factory's doc comment promises and what a caller handing over an
    // inherited fd is relying on. It was NOT true until the destructor stopped
    // being `= default`: the fd leaked, and the epoll set kept a `data.ptr` into
    // the freed handler, so a connection arriving afterwards resumed through it.
    // The second half is a use-after-free nothing here can observe deterministically;
    // this half is exact, and the two have one cause.
    errno = 0;
    REQUIRE(::fcntl(fd, F_GETFD) == -1);
    REQUIRE(errno == EBADF);
}

TEST_CASE("EpollListener::Adopt reports a descriptor it cannot take rather than throwing", "[net][epoll]")
{
    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };
    auto listener = FastCache::EpollListener::Adopt(reactor, -1);
    REQUIRE(listener);
    REQUIRE_FALSE(listener->IsBound());
    REQUIRE_FALSE(listener->BindError().empty());
    REQUIRE(listener->BoundPort() == 0);

    // Same convention as a failed Bind: the reason surfaces on the first accept.
    auto accept = listener->Accept();
    REQUIRE(accept.await_ready());
    auto const result = accept.await_resume();
    REQUIRE_FALSE(result.has_value());
}

#endif // __linux__
