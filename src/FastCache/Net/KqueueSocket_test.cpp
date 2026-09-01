// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/KqueueReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/KqueueSocket.hpp>

#if defined(__APPLE__)

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
/// a KqueueSocket driven by the reactor under test; the second is read with a
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
        // Unqualified: macOS defines htonl as a macro, so ::htonl does not parse.
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
        // reactorSide is owned by the KqueueSocket once wrapped.
    }

    LoopbackPair(LoopbackPair const&) = delete;
    LoopbackPair& operator=(LoopbackPair const&) = delete;
    LoopbackPair(LoopbackPair&&) = delete;
    LoopbackPair& operator=(LoopbackPair&&) = delete;
};

/// Shrink both kernel buffers so a large payload cannot be accepted in one
/// syscall, forcing EAGAIN mid-write and thus the park-and-re-arm path.
/// @param pair The loopback pair to constrain.
void ForcePartialWrites(LoopbackPair const& pair)
{
    int const smallSnd = 8192;
    ::setsockopt(pair.reactorSide, SOL_SOCKET, SO_SNDBUF, &smallSnd, sizeof(smallSnd));
    int const smallRcv = 8192;
    ::setsockopt(pair.peerSide, SOL_SOCKET, SO_RCVBUF, &smallRcv, sizeof(smallRcv));

    // Bound the drain. The failure these tests guard against is a write that
    // parks and never resumes, which would otherwise block the test thread in
    // recv() until CTest's timeout killed the whole run. With a deadline the
    // drain returns short instead and the byte-count assertion reports the
    // truncation directly. Generous: 4 MiB over loopback takes milliseconds.
    timeval tv {};
    tv.tv_sec = 15;
    ::setsockopt(pair.peerSide, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/// Read exactly `expected` bytes from a blocking fd into a vector.
/// @param fd Blocking socket to drain.
/// @param expected Byte count to read before returning.
/// @return The bytes read; shorter than `expected` if the peer closed early.
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

/// A payload whose every byte is position-dependent, so a truncation or a
/// mis-advanced cursor shows up as a content mismatch rather than by luck.
/// @param count Number of bytes to generate.
/// @return The generated buffer.
std::vector<std::byte> CounterPattern(std::size_t count)
{
    std::vector<std::byte> value(count);
    for (std::size_t i = 0; i < count; ++i)
        value[i] = static_cast<std::byte>(i & 0xFF);
    return value;
}

/// Drive a single scalar Write over a KqueueSocket and publish the reported
/// byte count, then stop the reactor. A free coroutine (not a capturing lambda)
/// so its frame holds the arguments by value — no dangling captures across the
/// suspend.
/// @param reactor Reactor driving the socket.
/// @param fd Connected socket fd to wrap and own.
/// @param payload Bytes to write (points into caller-owned storage).
/// @param reported Out: total bytes the write reported on success.
FastCache::DetachedTask DriveScalarWrite(FastCache::KqueueReactor* reactor,
                                         int fd,
                                         std::span<std::byte const> payload,
                                         std::atomic<std::size_t>* reported)
{
    FastCache::KqueueSocket socket { *reactor, fd };
    auto const r = co_await socket.Write(payload);
    if (r.has_value())
        reported->store(*r);
    socket.Close();
    reactor->Stop();
    co_return;
}

/// Drive a vectored write of [header][value][trailer] over a KqueueSocket.
/// @param reactor Reactor driving the socket.
/// @param fd Connected socket fd to wrap and own.
/// @param header Leading segment.
/// @param value Payload segment (points into caller-owned storage).
/// @param trailer Trailing segment.
/// @param reported Out: total bytes the write reported on success.
FastCache::DetachedTask DriveVectoredWrite(FastCache::KqueueReactor* reactor,
                                           int fd,
                                           std::string_view header,
                                           std::span<std::byte const> value,
                                           std::string_view trailer,
                                           std::atomic<std::size_t>* reported)
{
    FastCache::KqueueSocket socket { *reactor, fd };
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
/// one over: blocking, and NOT close-on-exec. A supervisor passes both that way
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
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral, so the test cannot collide with a live port
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(fd, 8) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port = ::ntohs(addr.sin_port);
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
/// shape the accept case has to be able to fail in. macOS has no MSG_NOSIGNAL,
/// so the send passes no flags; SIGPIPE is ignored process-wide here anyway
/// (`Detail::EnsureNetworkInitialised`), and nothing closes the peer early.
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
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = ::htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return {};
    }
    std::ignore = ::send(fd, greeting.data(), greeting.size(), 0);
    std::array<char, 64> buf {};
    auto const n = ::recv(fd, buf.data(), buf.size(), 0);
    ::close(fd);
    if (n <= 0)
        return {};
    return std::string { buf.data(), static_cast<std::size_t>(n) };
}

/// Await one accept on an adopted listener, answer the client's greeting, and
/// stop the reactor. A free coroutine (not a capturing lambda) so its frame
/// holds the arguments by value — no dangling captures across the suspend.
/// @param reactor Reactor driving the listener.
/// @param listener The adopted listener to accept on.
/// @param served Out: set when a connection was accepted and answered.
FastCache::DetachedTask DriveAdoptedAccept(FastCache::KqueueReactor* reactor,
                                           FastCache::KqueueListener* listener,
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

// This is the regression test for the reply-write stall. KqueueReactor's
// UpdateInterest used to submit both filter changes in one kevent() changelist
// with no eventlist, so the FIRST failing change aborted the rest. A scalar
// write that parks asks for (read=false, write=true): the EVFILT_READ delete
// ENOENTs on a socket that never armed a read, which silently swallowed the
// EVFILT_WRITE add. The write then parked forever and the peer waited for bytes
// that were sitting in daemon userspace. Any payload larger than the send
// buffer reproduces it; the small SO_SNDBUF just makes that cheap.
TEST_CASE("KqueueSocket::Write completes a payload larger than the send buffer", "[net][kqueue][large]")
{
    LoopbackPair pair;
    ForcePartialWrites(pair);

    constexpr std::size_t ValueByteCount = 4U * 1024U * 1024U; // 4 MiB
    auto const value = CounterPattern(ValueByteCount);

    FastCache::SteadyClock clock;
    FastCache::KqueueReactor reactor { clock };
    std::atomic<std::size_t> reported { 0 };

    DriveScalarWrite(&reactor, pair.reactorSide, std::span<std::byte const> { value.data(), value.size() }, &reported);
    std::jthread reactorThread { [&reactor] { reactor.Run(); } };

    // Drain on the test thread; the small SO_RCVBUF means the writer can only
    // make progress as we read, guaranteeing the park path is hit.
    auto const received = RecvExactly(pair.peerSide, ValueByteCount);
    // Stop explicitly rather than relying on the coroutine's own Stop(): if the
    // write never completed, the coroutine is still parked and would never
    // reach it, leaving this join to hang forever. Stopping twice is harmless.
    reactor.Stop();
    reactorThread.join();

    REQUIRE(reported.load() == ValueByteCount);
    REQUIRE(received.size() == ValueByteCount);
    bool intact = true;
    for (std::size_t i = 0; i < ValueByteCount && intact; ++i)
        intact = received[i] == static_cast<std::byte>(i & 0xFF);
    REQUIRE(intact);
}

TEST_CASE("KqueueSocket::WriteVectored streams a large value across partial writes", "[net][kqueue][large]")
{
    LoopbackPair pair;
    ForcePartialWrites(pair);

    constexpr std::size_t ValueByteCount = 4U * 1024U * 1024U; // 4 MiB
    auto const value = CounterPattern(ValueByteCount);
    std::string_view const header = "HDR:";
    std::string_view const trailer = ":END";
    auto const total = header.size() + ValueByteCount + trailer.size();

    FastCache::SteadyClock clock;
    FastCache::KqueueReactor reactor { clock };
    std::atomic<std::size_t> reported { 0 };

    DriveVectoredWrite(
        &reactor, pair.reactorSide, header, std::span<std::byte const> { value.data(), value.size() }, trailer, &reported);
    std::jthread reactorThread { [&reactor] { reactor.Run(); } };

    auto const received = RecvExactly(pair.peerSide, total);
    // Stop explicitly rather than relying on the coroutine's own Stop(): if the
    // write never completed, the coroutine is still parked and would never
    // reach it, leaving this join to hang forever. Stopping twice is harmless.
    reactor.Stop();
    reactorThread.join();

    REQUIRE(reported.load() == total);
    REQUIRE(received.size() == total);
    REQUIRE(std::string_view { reinterpret_cast<char const*>(received.data()), header.size() } == header);
    bool intact = true;
    for (std::size_t i = 0; i < ValueByteCount && intact; ++i)
        intact = received[header.size() + i] == static_cast<std::byte>(i & 0xFF);
    REQUIRE(intact);
    REQUIRE(
        std::string_view { reinterpret_cast<char const*>(received.data()) + header.size() + ValueByteCount, trailer.size() }
        == trailer);
}

TEST_CASE("KqueueListener::Adopt corrects the two descriptor properties a handoff omits", "[net][kqueue]")
{
    InheritedListenFd inherited;
    REQUIRE(inherited.fd >= 0);
    // The precondition this test rests on: a handed-over descriptor arrives with
    // neither property. `Bind` applies both through `PrepareOwnedFd` — macOS has
    // no SOCK_NONBLOCK socket-type flag — and `Adopt` has to do the same.
    REQUIRE((::fcntl(inherited.fd, F_GETFL, 0) & O_NONBLOCK) == 0);
    REQUIRE((::fcntl(inherited.fd, F_GETFD, 0) & FD_CLOEXEC) == 0);

    FastCache::SteadyClock clock;
    FastCache::KqueueReactor reactor { clock };
    auto const fd = inherited.Release();
    auto const listener = FastCache::KqueueListener::Adopt(reactor, fd);
    REQUIRE(listener);
    INFO("BindError: " << listener->BindError());
    REQUIRE(listener->IsBound());

    // Non-blocking is what keeps the reactor thread out of a synchronous accept;
    // close-on-exec is what keeps a compiler the worker spawns from holding the
    // port after the worker is gone.
    REQUIRE((::fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0);
    REQUIRE((::fcntl(fd, F_GETFD, 0) & FD_CLOEXEC) != 0);
}

TEST_CASE("KqueueListener::Adopt keeps the supervisor's port rather than binding one", "[net][kqueue]")
{
    InheritedListenFd inherited;
    auto const port = inherited.port;
    REQUIRE(port != 0);

    FastCache::SteadyClock clock;
    FastCache::KqueueReactor reactor { clock };
    auto const listener = FastCache::KqueueListener::Adopt(reactor, inherited.Release());
    REQUIRE(listener);
    INFO("BindError: " << listener->BindError());
    REQUIRE(listener->IsBound());
    REQUIRE(listener->BindError().empty());
    REQUIRE(listener->BoundPort() == port);
}

TEST_CASE("KqueueListener::Adopt accepts a connection through the reactor", "[net][kqueue]")
{
    InheritedListenFd inherited;
    auto const port = inherited.port;
    REQUIRE(port != 0);

    FastCache::SteadyClock clock;
    FastCache::KqueueReactor reactor { clock };
    auto const listener = FastCache::KqueueListener::Adopt(reactor, inherited.Release());
    REQUIRE(listener);
    INFO("BindError: " << listener->BindError());
    REQUIRE(listener->IsBound());

    std::atomic<bool> served { false };
    // The accept is parked BEFORE any client connects, which is the whole point
    // of the case. `Accept()`'s synchronous fast path would satisfy a connection
    // that was already pending, and that path needs neither the EVFILT_READ
    // registration nor a non-blocking descriptor — so an `Adopt` that skipped
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

TEST_CASE("An adopted KqueueListener closes its descriptor when it is destroyed", "[net][kqueue]")
{
    InheritedListenFd inherited;
    REQUIRE(inherited.fd >= 0);
    auto const fd = inherited.Release();

    FastCache::SteadyClock clock;
    FastCache::KqueueReactor reactor { clock };
    {
        auto const listener = FastCache::KqueueListener::Adopt(reactor, fd);
        REQUIRE(listener);
        INFO("BindError: " << listener->BindError());
        REQUIRE(listener->IsBound());
    }

    // Ownership transferred, so the descriptor went with the listener -- which is
    // what the factory's doc comment promises and what a caller handing over an
    // inherited fd is relying on. It was NOT true until the destructor stopped
    // being `= default`: the fd leaked, and the kqueue kept a `udata` into the
    // freed handler, so a connection arriving afterwards resumed through it. The
    // second half is a use-after-free nothing here can observe deterministically;
    // this half is exact, and the two have one cause.
    errno = 0;
    REQUIRE(::fcntl(fd, F_GETFD) == -1);
    REQUIRE(errno == EBADF);
}

TEST_CASE("KqueueListener::Adopt reports a descriptor it cannot take rather than throwing", "[net][kqueue]")
{
    FastCache::SteadyClock clock;
    FastCache::KqueueReactor reactor { clock };
    auto listener = FastCache::KqueueListener::Adopt(reactor, -1);
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

#endif // __APPLE__
