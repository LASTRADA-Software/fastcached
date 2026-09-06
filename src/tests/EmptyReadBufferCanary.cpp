// SPDX-License-Identifier: Apache-2.0
//
// A program that MUST die.
//
// `Detail::RequireReadBuffer` (`FastCache/Net/ISocket.hpp`) is the tripwire for
// [#838](https://github.com/LASTRADA-Software/fastcached/issues/838): `Read` documents
// that its buffer must be non-empty, and every transport's receive primitive answers
// `0` for a zero-length request -- which on this interface means *the peer has finished
// sending*. So an empty span is answered EOF: the caller is handed a graceful close that
// never happened, which is precisely the false claim `0` is supposed to be reserved for.
//
// The whole point of the guard is to be loud about something the type cannot express,
// and **a guard nobody has watched refuse is not a guard**. So this hands a real
// transport an empty span. It is read by `scripts/empty-read-buffer-gate.cmake`, which
// reports green exactly while the guard fires, and red the moment somebody deletes it or
// weakens it.
//
// **The call site is what it drives, not the guard function.** Calling
// `RequireReadBuffer` directly would prove the `assert` works and say nothing about
// whether an `ISocket::Read` implementation reaches it -- which is the half that rots.
//
// **`InMemorySocket` and not a reactor socket, deliberately.** A canary aborts at the
// first violation, so it can only ever watch ONE site whatever it picks -- the same is
// true of `read-slot-guard-canary` next door. Given that, the site worth watching is the
// one with no environmental failure modes: no bind that can be refused, no client thread
// that can fail to arrive, no platform variation, and therefore a verdict that means the
// same thing on Linux, macOS and Windows. It is also the transport every Catch2 case in
// this tree reads through, which is the population #838's own census is about. The other
// five sites (`BlockingSocket`, `EpollSocket`, `KqueueSocket`, `IocpSocket`, `TlsSocket`)
// carry the identical one-line call as the first statement of `Read`; that they do is
// verified by reading, and is stated here rather than implied.
//
// Every self-diagnosed problem exits **0** and says which one it was, so the gate can
// tell "could not stage the read" apart from "the empty read was NOT refused" and from
// "the guard refused". Neither of the first two may read as the guard working: not having
// run is not a pass. That is also why the gate requires the assertion's own words rather
// than inverting an exit code -- a bare `WILL_FAIL` would accept a segfault or a missing
// shared library just as happily.
//
// Registered only for Debug configurations (`src/tests/CMakeLists.txt`), because `assert`
// compiles out everywhere else -- the same shape, and for the same reason, as
// `read-slot-guard-canary` and `iterator-debug-canary`.

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <cstddef>
#include <iostream>
#include <print>
#include <span>
#include <string_view>

namespace
{

/// Put bytes in flight so the empty read below takes the "data is waiting" path.
/// @param client The writing end of the pair.
/// @param bytes  Payload to send.
/// @return True when every byte was accepted.
FastCache::Task<bool> StagePendingBytes(FastCache::ISocket* client, std::span<std::byte const> bytes)
{
    auto const wrote = co_await client->Write(bytes);
    co_return wrote.has_value() && *wrote == bytes.size();
}

} // namespace

int main()
{
    auto pair = FastCache::InMemorySocketPair::Create();
    if (!pair.client || !pair.server)
    {
        std::println(std::cerr, "empty-read-buffer-canary: could not construct a socket pair; nothing was watched");
        return 0; // WILL_FAIL: not having run is not the guard working.
    }

    // Bytes ARE waiting, so without the guard `Read` takes its synchronous fast path,
    // pulls nothing into a zero-length span and resolves with `0` -- an EOF report on a
    // connection whose peer has said nothing of the sort. That is the defect, staged.
    static constexpr std::string_view payload = "not eof";
    auto const staged = FastCache::SyncRun(StagePendingBytes(
        pair.client.get(),
        std::span<std::byte const> { reinterpret_cast<std::byte const*>(payload.data()), payload.size() }));
    if (!staged)
    {
        std::println(std::cerr, "empty-read-buffer-canary: could not stage pending bytes; nothing was watched");
        return 0;
    }

    std::println(std::cerr, "empty-read-buffer-canary: reading into an empty span with bytes pending");

    // The check happens when `Read` is CALLED -- before any suspension -- so the guard
    // fires on this line in a build with assertions live. The awaitable is deliberately
    // never awaited: we do not intend to get here at all.
    auto armed = pair.server->Read(std::span<std::byte> {});
    auto const answered = armed.await_resume();

    std::println(std::cerr,
                 "empty-read-buffer-canary: the empty read was NOT refused -- it answered {} with {} bytes still "
                 "pending, which on this interface reads as the peer having finished sending. Either assertions "
                 "are compiled out of this build, or Detail::RequireReadBuffer no longer guards "
                 "InMemorySocket::Read (#838)",
                 answered.has_value() ? std::to_string(*answered) : answered.error().ToString(),
                 payload.size());
    return 0;
}
