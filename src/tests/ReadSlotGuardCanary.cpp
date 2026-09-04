// SPDX-License-Identifier: Apache-2.0
//
// A program that MUST die.
//
// `Detail::ClaimReadSlot` (`FastCache/Net/ReadSlot.hpp`) is the tripwire for
// [#663](https://github.com/LASTRADA-Software/fastcached/issues/663): a socket has
// ONE read operation, `Read` and `WaitReadable` share it, and arming either while
// the other is parked drops the parked coroutine -- never resumed, never freed, no
// assertion, no error, no log. The whole point of the guard is to be loud about
// something nothing else in this tree can observe, and **a guard nobody has watched
// refuse is not a guard**.
//
// So this arms a `Read` over a parked `WaitReadable` on a REAL socket, through the
// real reactor. It is read by `scripts/read-slot-guard-gate.cmake`, which reports
// green exactly while the guard fires, and red the moment somebody deletes it,
// weakens it, or adds a seventh arm site that does not go through it.
//
// **The call site is what it drives, not the guard function.** Calling
// `ClaimReadSlot` directly would prove the `assert` works and say nothing about
// whether `EpollSocket::Read` reaches it -- which is the half that rots.
//
// Every self-diagnosed problem exits **0** and says which one it was, so the gate
// can tell "could not bind", "the client never arrived" and "the double-arm was not
// refused" apart from "the guard refused". None of them may read as the guard
// working: not having run is not a pass. That is also why the gate requires the
// assertion's own words rather than inverting an exit code -- a bare `WILL_FAIL`
// would accept a segfault, a missing shared library and a refused bind alike.
//
// Registered only for Debug configurations (`src/tests/CMakeLists.txt`), because
// `assert` compiles out everywhere else -- the same shape, and for the same reason,
// as `iterator-debug-canary` being guarded to MSVC Debug.

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/PlatformListener.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <print>
#include <span>
#include <thread>
#include <utility>

namespace
{

/// Park a `WaitReadable` on @p socket and stay there.
///
/// The client never sends anything, so this genuinely suspends: the awaitable is
/// recorded in the socket's read-op slot and stays there until something takes it
/// away -- which is exactly what the `Read` below does.
/// @param socket The accepted connection.
/// @return The detached task holding the parked wait.
FastCache::DetachedTask ParkOnReadable(FastCache::ISocket* socket)
{
    static_cast<void>(co_await socket->WaitReadable());
}

/// Accept one connection, park a wait on it, then arm a `Read` over the top.
/// @param reactor Stopped if we survive, so `Run()` returns and main can report.
/// @param listener Bound listener to accept on.
/// @param survived Set when the double-arm was NOT refused.
/// @return The detached task.
FastCache::DetachedTask DoubleArmTheReadSlot(FastCache::PlatformReactor* reactor,
                                             FastCache::IListener* listener,
                                             std::atomic<bool>* survived)
{
    auto accepted = co_await listener->Accept();
    if (!accepted.has_value())
    {
        reactor->Stop();
        co_return;
    }
    auto socket = std::move(*accepted);

    ParkOnReadable(socket.get());

    std::println(std::cerr, "read-slot-guard-canary: arming a Read over a parked WaitReadable");

    // The claim happens when `Read` is CALLED -- before any suspension -- so the
    // guard fires on this line in a build with assertions live. The awaitable is
    // deliberately never awaited: there is nothing to read and we do not intend to
    // get here at all.
    std::array<std::byte, 8> buffer {};
    auto const armed = socket->Read(std::span<std::byte> { buffer });
    static_cast<void>(armed);

    survived->store(true, std::memory_order_release);
    socket->Close();
    reactor->Stop();
}

} // namespace

int main()
{
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };

    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    if (!listener || !listener->IsBound() || listener->BoundPort() == 0)
    {
        std::println(std::cerr, "read-slot-guard-canary: could not bind a loopback listener; nothing was watched");
        return 0; // WILL_FAIL: not having run is not the guard working.
    }
    auto const port = listener->BoundPort();

    std::atomic<bool> survived { false };
    DoubleArmTheReadSlot(&reactor, listener.get(), &survived);

    std::jthread client { [port] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;
        // Silent, and held open: the server's `WaitReadable` must find nothing to
        // report and park, and its `Read` must find nothing and try to park too.
        std::this_thread::sleep_for(std::chrono::seconds { 2 });
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    if (survived.load(std::memory_order_acquire))
        std::println(std::cerr,
                     "read-slot-guard-canary: the double-arm was NOT refused -- either assertions are compiled "
                     "out of this build, or ClaimReadSlot no longer guards this arm site (#663)");
    else
        std::println(std::cerr, "read-slot-guard-canary: the connection never reached the double-arm");
    return 0;
}
