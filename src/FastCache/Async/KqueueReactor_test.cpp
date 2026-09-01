// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/KqueueReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(__APPLE__)

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <memory>

    #include <unistd.h>

using namespace FastCache;

namespace
{

/// One attached descriptor plus the handler the reactor holds a pointer to.
/// Heap-allocated so a callback can destroy it, which is what a resumed
/// coroutine dropping its socket does.
///
/// A pipe rather than an eventfd, which is Linux-only: writing one byte makes
/// the read end readable, which is all this needs.
struct BatchPeer
{
    KqueueFdHandler handler {};
    int readFd { -1 };
    int writeFd { -1 };
    KqueueReactor* reactor { nullptr };
    std::unique_ptr<BatchPeer>* other { nullptr };
    bool* actedAlready { nullptr };

    BatchPeer() = default;

    ~BatchPeer()
    {
        if (readFd >= 0)
            ::close(readFd);
        if (writeFd >= 0)
            ::close(writeFd);
    }

    BatchPeer(BatchPeer const&) = delete;
    BatchPeer& operator=(BatchPeer const&) = delete;
    BatchPeer(BatchPeer&&) = delete;
    BatchPeer& operator=(BatchPeer&&) = delete;
};

/// Destroy the OTHER peer, exactly as a resumed coroutine dropping a socket
/// would: detach first -- which is all any owner here can do -- then free.
void DestroyTheOtherPeer(KqueueFdHandler* self)
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
// dispatched on. The twin of the EpollReactor case of the same name, and the
// same defect: `EV_DELETE` stops future reports and does not retract entries
// `kevent()` has already written into the caller's array, so the entry for the
// freed peer is still sitting there when the loop reaches it.
//
// The `serviced` array in Run() does not cover this -- it dedupes one handler
// reported under two filters, which is a different case. Issue #475.
//
// Measured on epoll under ASan; this half was written from the source, since the
// lane that wrote it had no macOS machine. It is here so the platform's own CI
// leg answers the question rather than leaving it inferred.
TEST_CASE("A handler freed earlier in the same batch is not dispatched", "[kqueue][reactor]")
{
    SteadyClock clock;
    KqueueReactor reactor { clock };

    auto first = std::make_unique<BatchPeer>();
    auto second = std::make_unique<BatchPeer>();

    for (auto* peer: { first.get(), second.get() })
    {
        std::array<int, 2> fds { -1, -1 };
        REQUIRE(::pipe(fds.data()) == 0);
        peer->readFd = fds[0];
        peer->writeFd = fds[1];

        peer->reactor = &reactor;
        peer->handler.fd = peer->readFd;
        peer->handler.owner = peer;
        peer->handler.onReadable = &DestroyTheOtherPeer;
        REQUIRE(reactor.Attach(&peer->handler));
        REQUIRE(reactor.UpdateInterest(&peer->handler, true, false));
    }

    bool actedAlready = false;
    first->actedAlready = &actedAlready;
    second->actedAlready = &actedAlready;
    first->other = &second;
    second->other = &first;

    // Both readable BEFORE the wait, so one kevent() returns both in a single
    // batch. Without this the case proves nothing -- it would be two batches and
    // the window would never open.
    char const byte = 'x';
    REQUIRE(::write(first->writeFd, &byte, 1) == 1);
    REQUIRE(::write(second->writeFd, &byte, 1) == 1);

    reactor.Run();

    // Exactly one of them acted, and the other was destroyed from inside the
    // batch rather than dispatched.
    REQUIRE(actedAlready);
    REQUIRE(((first == nullptr) != (second == nullptr)));
}

#endif // __APPLE__
