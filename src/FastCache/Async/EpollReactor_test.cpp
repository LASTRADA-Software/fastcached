// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/EpollReactor.hpp>

#if defined(__linux__)

    #include <catch2/catch_test_macros.hpp>

    #include <sys/epoll.h>

    #include <cstdint>

using namespace FastCache;

namespace
{

// Distinct no-op callbacks, so a case can assert WHICH one was selected rather
// than only that something was. Three near-identical bodies would let a
// selection that returns the wrong member pass.
void Readable(EpollFdHandler* /*self*/) {}
void Writable(EpollFdHandler* /*self*/) {}
void Errored(EpollFdHandler* /*self*/) {}

[[nodiscard]] std::uint32_t Bits(int events) noexcept
{
    return static_cast<std::uint32_t>(events);
}

} // namespace

TEST_CASE("A readable event selects the read callback", "[epoll][reactor]")
{
    EpollFdHandler const handler { .fd = 3, .onReadable = &Readable, .onWritable = &Writable };
    CHECK(SelectEpollCallback(handler, Bits(EPOLLIN)) == &Readable);
    CHECK(SelectEpollCallback(handler, Bits(EPOLLOUT)) == &Writable);
}

TEST_CASE("An error reaches onError even when no direction is signalled", "[epoll][reactor]")
{
    // The defect this exists for: a failed outbound connect can be reported with
    // EPOLLERR/EPOLLHUP and NEITHER EPOLLIN nor EPOLLOUT. Before onError existed
    // the loop matched no branch, and because the fd is level-triggered it was
    // re-reported immediately -- a dial that never completes and a reactor
    // spinning at 100% CPU, with nothing logged at either end.
    EpollFdHandler const handler { .fd = 3, .onReadable = &Readable, .onWritable = &Writable, .onError = &Errored };

    CHECK(SelectEpollCallback(handler, Bits(EPOLLERR)) == &Errored);
    CHECK(SelectEpollCallback(handler, Bits(EPOLLHUP)) == &Errored);
    CHECK(SelectEpollCallback(handler, Bits(EPOLLERR | EPOLLHUP)) == &Errored);
}

TEST_CASE("An error outranks a direction that is also signalled", "[epoll][reactor]")
{
    // A refused connect commonly reports EPOLLOUT alongside the error, because a
    // dead socket is trivially "writable". Taking the direction would send the
    // dial down its success path, where getsockopt(SO_ERROR) is the only thing
    // that would have caught it -- so the error has to win here, not merely be
    // available when nothing else is.
    EpollFdHandler const handler { .fd = 3, .onReadable = &Readable, .onWritable = &Writable, .onError = &Errored };

    CHECK(SelectEpollCallback(handler, Bits(EPOLLOUT | EPOLLERR)) == &Errored);
    CHECK(SelectEpollCallback(handler, Bits(EPOLLIN | EPOLLHUP)) == &Errored);
}

TEST_CASE("An error with no onError falls back to a watched direction", "[epoll][reactor]")
{
    // Neither EpollSocket nor EpollListener sets onError, and both must keep
    // behaving exactly as they did: a socket's parked operation fails with the
    // error, and a listener's next accept reports it. What must NOT happen is
    // the event being dropped, which is the spin above.
    EpollFdHandler const readerOnly { .fd = 3, .onReadable = &Readable };
    CHECK(SelectEpollCallback(readerOnly, Bits(EPOLLERR)) == &Readable);

    EpollFdHandler const writerOnly { .fd = 3, .onWritable = &Writable };
    CHECK(SelectEpollCallback(writerOnly, Bits(EPOLLHUP)) == &Writable);
}

TEST_CASE("A handler watching nothing selects no callback", "[epoll][reactor]")
{
    // Returning nullptr rather than dispatching something is what lets the loop
    // skip cleanly; the alternative would be a null call.
    EpollFdHandler const handler { .fd = 3 };
    CHECK(SelectEpollCallback(handler, Bits(EPOLLIN | EPOLLOUT | EPOLLERR)) == nullptr);

    EpollFdHandler const reader { .fd = 3, .onReadable = &Readable };
    CHECK(SelectEpollCallback(reader, Bits(EPOLLOUT)) == nullptr);
}

#endif // __linux__
