// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/EpollReactor.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>

#if defined(__linux__)

    #include <catch2/catch_test_macros.hpp>

    #include <sys/epoll.h>
    #include <sys/eventfd.h>

    #include <cerrno>
    #include <chrono>
    #include <cstdint>
    #include <memory>
    #include <utility>

    #include <unistd.h>

using namespace FastCache;
using namespace std::chrono_literals;

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
//
// NO PRODUCTION PATH REACHED THIS WHEN IT WAS FIXED, and that is a bounded
// statement rather than a reason to doubt the case. It was a search over call
// sites, not a proof: every reactor socket was owned by exactly one coroutine
// frame -- `Connection`, `ServeAdminConnection`, `FrameEndpoint`,
// `WorkerServer::Serve`, `RaftPeerServer::ServePeer` all take a
// `unique_ptr<ISocket>` -- with no shared container to reach into, and listeners
// outlived `Run()`.
//
// One connection registry undoes all of that. An admin verb that closes other
// clients' connections, or a peer directory that drops a socket on demotion, are
// ordinary things to want and each makes this live on the day it lands. So the
// absence of a caller is not evidence this was never a real defect, and it is
// specifically not a reason to delete this case as testing something that cannot
// happen. See issue #475, where the search and its limits are recorded in full.
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

namespace
{

/// What one probe frame reported about the reactor that was freeing it.
///
/// Four fields rather than one boolean, because a probe that COULD NOT ASK reads
/// exactly like a probe that asked and was told no. `attempted` says a destructor ran
/// at all, `hadDescriptor` says it held a descriptor of its own to ask with, and only
/// with both of those does `sawLiveEpoll` mean anything.
struct ProbeRecord
{
    int attempted { 0 };     ///< Probe frames destroyed.
    int hadDescriptor { 0 }; ///< ... of which held a descriptor of their own to ask with.
    int sawLiveEpoll { 0 };  ///< ... of which found this reactor's epoll descriptor open.
    int refusedWith { 0 };   ///< `epoll_ctl`'s errno from the last probe it refused.
};

/// A coroutine-frame member that asks the reactor freeing it whether its epoll
/// descriptor is still open, and records the answer.
///
/// **The recorded fact is `Attach()`'s own answer, never the absence of a crash.**
/// `EpollReactor::Attach` is `epoll_ctl(_epollFd, EPOLL_CTL_ADD, ...)` -- the same
/// call `~EpollSocket` reaches through `Detach()` on its way down -- so it answers
/// true exactly while that descriptor is open and false once `~EpollReactor` has
/// closed it. Watching for a crash instead would prove nothing, which is the whole
/// reason [#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054) is its
/// own ticket: `epoll_ctl` on a descriptor number another thread has since reused can
/// silently succeed, so in production the defect is invisible by construction.
///
/// The errno is recorded for that same reason. `EBADF` says the descriptor was
/// CLOSED; `EINVAL` would say its number had been taken by something that is not an
/// epoll descriptor, and `EEXIST` that this probe had attached before. Nothing here
/// opens a descriptor between the closes in `~EpollReactor` and the member
/// destruction that follows them, so no number can be reused -- and recording the
/// code is what makes that an observation rather than an assumption.
class EpollLivenessProbe
{
  public:
    /// @param reactor The reactor to ask as this dies; never null.
    /// @param record  Where the answer is tallied; never null.
    EpollLivenessProbe(EpollReactor* reactor, ProbeRecord* record) noexcept:
        _reactor { reactor },
        _record { record },
        _fd { ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) }
    {
        _handler.fd = _fd;
    }

    EpollLivenessProbe(EpollLivenessProbe&& other) noexcept:
        _reactor { std::exchange(other._reactor, nullptr) },
        _record { other._record },
        _fd { std::exchange(other._fd, -1) }
    {
        _handler.fd = _fd;
    }

    EpollLivenessProbe(EpollLivenessProbe const&) = delete;
    EpollLivenessProbe& operator=(EpollLivenessProbe const&) = delete;
    EpollLivenessProbe& operator=(EpollLivenessProbe&&) = delete;

    ~EpollLivenessProbe()
    {
        if (_reactor != nullptr)
        {
            ++_record->attempted;
            if (_fd >= 0)
            {
                ++_record->hadDescriptor;
                errno = 0;
                if (_reactor->Attach(&_handler))
                    ++_record->sawLiveEpoll;
                else
                    _record->refusedWith = errno;
            }
        }
        if (_fd >= 0)
            ::close(_fd);
    }

  private:
    EpollReactor* _reactor;
    ProbeRecord* _record;
    int _fd;
    EpollFdHandler _handler {};
};

/// An abandoned chain carrying one probe, parked on the submit side.
///
/// Detached, so nothing owns its frame and `Detail::ParkedWorkFor` gives the reactor
/// an `abandon` root -- which is what makes the reactor, rather than some caller, the
/// thing that frees it.
/// @param reactor The reactor to park on.
/// @param probe   Asks that reactor about its descriptor when this frame dies.
DetachedTask ProbeChainOnSubmit(EpollReactor* reactor, EpollLivenessProbe probe)
{
    (void) probe;
    co_await ResumeOn { *reactor };
    co_return;
}

/// A frame member that parks a fresh abandoned chain on the reactor as it is freed.
///
/// It stands in for the production shape the drain loop exists for: freeing a chain
/// runs arbitrary destructors, and `AsyncQueue::Close()` resuming a waiter reaches
/// `IExecutor::Submit`. So a chain freed out of the TIMER heap can put a new entry
/// into the submit queue after that queue has already been drained once.
class ReentrantPark
{
  public:
    /// @param reactor The reactor to park the new chain on; never null.
    /// @param record  Where that chain's probe reports; never null.
    ReentrantPark(EpollReactor* reactor, ProbeRecord* record) noexcept:
        _reactor { reactor },
        _record { record }
    {
    }

    ReentrantPark(ReentrantPark&& other) noexcept:
        _reactor { std::exchange(other._reactor, nullptr) },
        _record { other._record }
    {
    }

    ReentrantPark(ReentrantPark const&) = delete;
    ReentrantPark& operator=(ReentrantPark const&) = delete;
    ReentrantPark& operator=(ReentrantPark&&) = delete;

    ~ReentrantPark()
    {
        if (_reactor == nullptr)
            return;
        ProbeChainOnSubmit(_reactor, EpollLivenessProbe { _reactor, _record });
    }

  private:
    EpollReactor* _reactor;
    ProbeRecord* _record;
};

/// An abandoned chain parked in the TIMER heap whose frame re-parks as it is freed.
/// @param reactor The reactor to park on.
/// @param park    Parks a probe chain on that reactor when this frame dies.
DetachedTask ParkOnTimerThenRepark(EpollReactor* reactor, ReentrantPark park)
{
    (void) park;
    co_await SleepUntil { .reactor = reactor, .deadline = reactor->Clock().Now() + 1h };
    co_return;
}

} // namespace

// `AbandonParkedWork()` loops, and this is what the loop is for: a chain freed out of
// the timer heap can park a NEW one on the submit side, and only a second pass frees
// that before `~EpollReactor` closes its descriptors. A single pass leaves it for
// MEMBER destruction, which runs after `::close(_epollFd)` -- and the entry freed
// there runs `~EpollSocket` -> `Close()` -> `Detach()` -> `epoll_ctl` on a closed
// descriptor whose number another thread may by then have reused
// ([#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054), the residual
// of [#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
//
// **A real reactor, because the double cannot express this.** On `TestReactor` the
// single-pass version is genuinely benign -- it owns no descriptors, so there is no
// later moment that differs from the earlier one, and a case written there passes with
// the loop and without it. That case existed and was DELETED rather than kept; this one
// replaces it. What makes the difference observable here is that `EpollReactor` holds
// two descriptors it closes in its destructor BODY, so "freed in the body" and "freed
// at member destruction" are two different answers to one question a probe can ask.
//
// **Shown red, in the direction that matters**: with the `while` removed from
// `AbandonParkedWork` and nothing else changed, `reparked.sawLiveEpoll` is 0 with
// `refusedWith == EBADF`, while `control` is unmoved at 1. The control is not
// decoration -- without it a probe that always answered no would look exactly like the
// defect.
TEST_CASE("A chain re-parked during teardown is freed before the reactor's descriptors close", "[epoll][reactor][teardown]")
{
    SteadyClock clock;
    ProbeRecord control {};
    ProbeRecord reparked {};

    {
        EpollReactor reactor { clock };

        // The control. Parked before teardown begins, so the FIRST pass frees it --
        // inside the destructor body, with both descriptors still open. It answers yes
        // in every build, including the broken one, which is what says the probe works.
        ProbeChainOnSubmit(&reactor, EpollLivenessProbe { &reactor, &control });

        // The subject. Parked in the TIMER heap, which `AbandonParkedWork` frees after
        // the submit queue, and its frame parks a probe chain on the way down.
        ParkOnTimerThenRepark(&reactor, ReentrantPark { &reactor, &reparked });
    }

    // Both probes ran, and both held a descriptor of their own to ask with. Without
    // this, a probe that could not ask would report the defect's answer for its own
    // reason -- and the re-parked one is the case's whole subject, so a destructor that
    // never ran must not read as a pass.
    REQUIRE(control.attempted == 1);
    REQUIRE(control.hadDescriptor == 1);
    REQUIRE(reparked.attempted == 1);
    REQUIRE(reparked.hadDescriptor == 1);

    // Unmoved between the two builds: the reactor's epoll descriptor is open when the
    // first pass frees a chain, whether or not there is ever a second pass.
    CHECK(control.sawLiveEpoll == 1);
    CHECK(control.refusedWith == 0);

    // And this is the pair that distinguishes them.
    CHECK(reparked.sawLiveEpoll == 1);
    CHECK(reparked.refusedWith == 0);
}

#endif // __linux__
