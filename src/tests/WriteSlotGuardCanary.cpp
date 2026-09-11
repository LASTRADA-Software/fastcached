// SPDX-License-Identifier: Apache-2.0
//
// A program that MUST die -- after first being watched NOT dying.
//
// `Detail::ClaimWriteSlot` (`FastCache/Net/WriteSlot.hpp`) is the write-side tripwire
// for [#893](https://github.com/LASTRADA-Software/fastcached/issues/893): every
// reactor socket keeps one `awaitable` pointer PER DIRECTION, so arming a write over
// a parked one drops that coroutine exactly as the read side's #663 does -- never
// resumed, never freed, no assertion, no error, no log.
//
// It was landed with its own header saying *"Watched refusing by
// `ctest -R write-slot-guard-canary`"*, and no such canary existed anywhere in the
// tree: one `git grep` hit, the sentence itself. A guard nobody has watched refuse is
// not a guard, and a header asserting otherwise is worse than silence because it
// retires the suspicion. This is that canary (#1218).
//
// **It watches the guard ACCEPT before it watches it refuse, and that ordering is the
// point.** A guard nobody has watched accept is not known to work either (#1031): a
// `ClaimWriteSlot` that fired on every write at all would still make the refusal case
// pass, and the whole suite would go red somewhere else entirely, days later, reading
// as a different defect. So this drives an ORDINARY sequential pair of writes first
// and prints a marker only if both are accepted; the gate requires that marker to
// appear BEFORE the abort. One program, two observations, and neither is inferred.
//
// **The call sites are what it drives, not the guard function.** Calling
// `ClaimWriteSlot` directly would prove the `assert` works and say nothing about
// whether `EpollSocket::Write` reaches it -- which is the half that rots when an
// eighth arm site is added.
//
// **How a write is made to park**, since that is the one thing harder here than on
// the read side: a read parks on silence, which costs nothing to arrange, while a
// write parks only once the kernel will take no more. So the client reads the control
// bytes and then stops reading, and the server writes a buffer far larger than any
// default socket buffer pair. `Write` sends what fits, then parks with the remainder.
//
// Every self-diagnosed problem exits **0** and says which one it was, so the gate can
// tell "could not bind", "the client never arrived" and "the double-arm was not
// refused" apart from "the guard refused". None of them may read as the guard working:
// not having run is not a pass.
//
// Registered only for Debug configurations (`src/tests/CMakeLists.txt`), because
// `assert` compiles out everywhere else.

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
#include <vector>

#include <tests/WindowsErrorPopups.hpp>

namespace
{

/// Far larger than any default send/receive buffer pair, so one `Write` cannot drain.
///
/// Sized rather than tuned: `SO_SNDBUF` is not reachable through `ISocket`, and a
/// canary that parked only on hosts with small buffers would report the guard
/// unwatched on the others -- which is the state this whole program exists to end.
constexpr std::size_t UnsendableBytes = 32UZ * 1024UZ * 1024UZ;

/// Park a `Write` on @p socket and stay there.
///
/// The client has stopped reading, so the kernel takes what fits and this genuinely
/// suspends: the awaitable is recorded in the socket's WRITE-op slot and stays there
/// until something takes it away -- which is exactly what the second `Write` does.
///
/// @param socket The accepted connection.
/// @param buffer Bytes to send; must outlive the parked awaitable.
/// @return The detached task holding the parked write.
FastCache::DetachedTask ParkOnWrite(FastCache::ISocket* socket, std::span<std::byte const> buffer)
{
    static_cast<void>(co_await socket->Write(buffer));
}

/// Read the control bytes the server sends, then stop reading.
///
/// The reads are what let the positive control complete; the SILENCE afterwards is what
/// makes the next write park, which is the whole arrangement this canary needs.
///
/// A free function taking a POINTER rather than a capturing lambda, for the reason
/// `BlockingSocket_test.cpp` states beside its own: a coroutine frame outlives the call
/// expression that created it, so a lambda coroutine capturing by reference can resume
/// after its closure object is gone. clang-tidy refuses it
/// (`cppcoreguidelines-avoid-capturing-lambda-coroutines`) and is right to.
///
/// @param socket The connected client socket.
/// @return The task, awaited synchronously on the client thread.
FastCache::Task<void> DrainControlBytes(FastCache::ISocket* socket)
{
    std::array<std::byte, 16> scratch {};
    std::size_t got = 0;
    while (got < 16)
    {
        auto const read = co_await socket->Read(std::span<std::byte> { scratch });
        if (!read.has_value() || *read == 0)
            co_return;
        got += *read;
    }
}

/// Accept one connection, watch the guard ACCEPT, then arm a write over a parked one.
/// @param reactor Stopped if we survive, so `Run()` returns and main can report.
/// @param listener Bound listener to accept on.
/// @param survived Set when the double-arm was NOT refused.
/// @param accepted Set once an ordinary sequential pair of writes has been accepted.
/// @return The detached task.
FastCache::DetachedTask DoubleArmTheWriteSlot(FastCache::PlatformReactor* reactor,
                                              FastCache::IListener* listener,
                                              std::atomic<bool>* survived,
                                              std::atomic<bool>* accepted)
{
    auto incoming = co_await listener->Accept();
    if (!incoming.has_value())
    {
        reactor->Stop();
        co_return;
    }
    auto socket = std::move(*incoming);

    // THE POSITIVE CONTROL, and it runs first on purpose. Two ordinary writes, each
    // awaited to completion, which is what every honest caller does. A guard that
    // refused these would be caught here rather than in production -- and its refusal
    // case would still have passed, which is what makes watching only the refusal
    // insufficient.
    std::array<std::byte, 8> const control {};
    for (auto const pass: { 1, 2 })
    {
        auto const wrote = co_await socket->Write(std::span<std::byte const> { control });
        if (!wrote.has_value())
        {
            std::println(std::cerr, "write-slot-guard-canary: the control write {} failed; nothing was watched", pass);
            reactor->Stop();
            co_return;
        }
    }
    accepted->store(true, std::memory_order_release);
    std::println(std::cerr, "write-slot-guard-canary: two sequential writes were ACCEPTED by the guard");

    // Now the refusal. The client has stopped reading by here, so this parks.
    static std::vector<std::byte> const unsendable(UnsendableBytes, std::byte { 0xAB });
    ParkOnWrite(socket.get(), std::span<std::byte const> { unsendable });

    std::println(std::cerr, "write-slot-guard-canary: arming a Write over a parked Write");

    // The claim happens when `Write` PARKS -- so the guard fires on this line only if
    // the first write is still parked, which is why the client must not be reading.
    // The awaitable is deliberately never awaited: we do not intend to get here.
    auto const armed = socket->Write(std::span<std::byte const> { unsendable });
    static_cast<void>(armed);

    survived->store(true, std::memory_order_release);
    socket->Close();
    reactor->Stop();
}

} // namespace

int main()
{
    // Several canaries here exist to be SEEN aborting, so on Windows the modal CRT
    // dialog is what happens on a successful run, not an edge case.
    FastCache::Testing::SuppressWindowsErrorPopups();

    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };

    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    if (!listener || !listener->IsBound() || listener->BoundPort() == 0)
    {
        std::println(std::cerr, "write-slot-guard-canary: could not bind a loopback listener; nothing was watched");
        return 0; // Not having run is not the guard working.
    }
    auto const port = listener->BoundPort();

    std::atomic<bool> survived { false };
    std::atomic<bool> accepted { false };
    DoubleArmTheWriteSlot(&reactor, listener.get(), &survived, &accepted);

    std::jthread client { [port] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;

        // Read exactly the control bytes, then stop. A client that kept reading would
        // drain 32 MiB and the canary would report "the double-arm was not refused" for
        // a reason that is nothing to do with the guard.
        //
        // `SyncRun` takes a `Task`, not a bare `IoAwaitable`, which is why the read goes
        // through a coroutine at all -- `DrainControlBytes` above, a free function rather
        // than a lambda because a coroutine must not capture by reference.
        FastCache::SyncRun(DrainControlBytes(socket->get()));

        std::this_thread::sleep_for(std::chrono::seconds { 3 });
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    if (!accepted.load(std::memory_order_acquire))
        std::println(std::cerr, "write-slot-guard-canary: the connection never reached the control writes");
    else if (survived.load(std::memory_order_acquire))
        std::println(std::cerr,
                     "write-slot-guard-canary: the double-arm was NOT refused -- either assertions are compiled "
                     "out of this build, or ClaimWriteSlot no longer guards this arm site (#893)");
    else
        std::println(std::cerr, "write-slot-guard-canary: the connection never reached the double-arm");
    return 0;
}
