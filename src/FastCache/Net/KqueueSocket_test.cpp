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

#endif // __APPLE__
