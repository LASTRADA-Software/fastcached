// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/EpollReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(__linux__)

    #include <catch2/catch_test_macros.hpp>

    #include <sys/epoll.h>

    #include <cstdint>
    #include <memory>

    #include <sys/eventfd.h>
    #include <unistd.h>

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

namespace
{

/// One attached descriptor plus the handler the reactor holds a pointer to.
/// Heap-allocated so a callback can destroy it, which is what a resumed
/// coroutine dropping its socket does.
struct BatchPeer
{
    EpollFdHandler handler {};
    int fd { -1 };
    EpollReactor* reactor { nullptr };
    std::unique_ptr<BatchPeer>* other { nullptr };
    bool* actedAlready { nullptr };

    ~BatchPeer()
    {
        if (fd >= 0)
            ::close(fd);
    }

    BatchPeer() = default;
    BatchPeer(BatchPeer const&) = delete;
    BatchPeer& operator=(BatchPeer const&) = delete;
    BatchPeer(BatchPeer&&) = delete;
    BatchPeer& operator=(BatchPeer&&) = delete;
};

/// Destroy the OTHER peer, exactly as a resumed coroutine dropping a socket
/// would: detach first -- which is all any owner here can do -- then free.
void DestroyTheOtherPeer(EpollFdHandler* self)
{
    auto* peer = static_cast<BatchPeer*>(self->owner);
    if (*peer->actedAlready)
        return;
    *peer->actedAlready = true;

    if (peer->other && *peer->other)
    {
        peer->reactor->Detach(&(*peer->other)->handler);
        peer->other->reset();
    }
    peer->reactor->Stop();
}

} // namespace

// A handler freed by an earlier callback in the SAME dequeued batch must not be
// dispatched on. `epoll_ctl(EPOLL_CTL_DEL)` stops future reports and does not
// retract what `epoll_wait` already wrote into the local array, so the entry for
// the freed peer is still sitting there when the loop reaches it.
//
// Reproduced under ASan as a heap-use-after-free in `SelectEpollCallback`, read
// from `EpollReactor::Run`, freed from `EpollReactor::Run` one iteration earlier
// -- issue #475. Whichever peer the kernel reports first destroys the other, so
// the case does not depend on the order epoll happens to return them in.
//
// Without the fix this is a use-after-free rather than a failed assertion, so it
// reports as a crash under a sanitizer and can pass silently without one. That is
// the nature of the defect and is why the case exists.
TEST_CASE("A handler freed earlier in the same batch is not dispatched", "[epoll][reactor]")
{
    SteadyClock clock;
    EpollReactor reactor { clock };

    auto first = std::make_unique<BatchPeer>();
    auto second = std::make_unique<BatchPeer>();

    first->fd = ::eventfd(0, EFD_NONBLOCK);
    second->fd = ::eventfd(0, EFD_NONBLOCK);
    REQUIRE(first->fd >= 0);
    REQUIRE(second->fd >= 0);

    bool actedAlready = false;
    for (auto* peer: { first.get(), second.get() })
    {
        peer->reactor = &reactor;
        peer->actedAlready = &actedAlready;
        peer->handler.fd = peer->fd;
        peer->handler.owner = peer;
        peer->handler.onReadable = &DestroyTheOtherPeer;
        REQUIRE(reactor.Attach(&peer->handler));
        REQUIRE(reactor.UpdateInterest(&peer->handler, true, false));
    }
    first->other = &second;
    second->other = &first;

    // Both readable BEFORE the wait, so one epoll_wait returns both in a single
    // batch. Without this the case proves nothing -- it would be two batches and
    // the window would never open.
    std::uint64_t const one = 1;
    REQUIRE(::write(first->fd, &one, sizeof(one)) == sizeof(one));
    REQUIRE(::write(second->fd, &one, sizeof(one)) == sizeof(one));

    reactor.Run();

    // Exactly one of them acted, and the other was destroyed from inside the
    // batch rather than dispatched.
    REQUIRE(actedAlready);
    REQUIRE(((first == nullptr) != (second == nullptr)));
}

#endif // __linux__
