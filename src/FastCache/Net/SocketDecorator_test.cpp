// SPDX-License-Identifier: Apache-2.0
//
// The shared socket fakes' own contract (#1430).
//
// `src/tests/SocketDecorator.hpp` holds fakes other suites build on, and a fake is held to
// the rule the sockets it stands for are held to: `Close()` -- and `CancelRead()`, which
// retires a parked watch the same way -- can be the last thing that runs on a socket, so it
// touches no member after it completes an awaitable. The case below is the shape that
// makes the rule bite: the coroutine the completion resumes OWNS the socket and drops it
// before control returns. A fake that counted or forwarded after completing would then
// write through a freed object, which a build without a sanitizer reports as a pass.
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <tuple>

#include <tests/SocketDecorator.hpp>

using namespace FastCache;

namespace
{

/// Await a readability watch on a socket this coroutine's caller owns, and drop the socket
/// the moment the watch resumes -- what a connection does when its watch says the peer is
/// gone and the connection ends.
/// @param owner The socket's only owner; reset from inside the resumption.
/// @param resumed Set once the watch has resumed.
DetachedTask AwaitThenDrop(std::unique_ptr<Testing::ParkingReadableSocket>& owner, bool& resumed)
{
    std::ignore = co_await owner->WaitReadable();
    resumed = true;
    owner.reset();
}

} // namespace

TEST_CASE("A parking fake lets the watcher it resumes destroy it", "[net][socket][fake]")
{
    auto const pair = InMemorySocketPair::Create();
    auto owner = std::make_unique<Testing::ParkingReadableSocket>(*pair.server);
    auto* const socket = owner.get();
    auto resumed = false;

    AwaitThenDrop(owner, resumed);
    REQUIRE(socket->IsWatchParked());
    REQUIRE_FALSE(resumed);

    // Each retirement route resumes the watcher inline, which destroys the socket before
    // the call returns. Nothing below may touch `socket`.
    SECTION("retired by Close")
    {
        socket->Close();
    }

    SECTION("retired by CancelRead")
    {
        socket->CancelRead();
    }

    CHECK(resumed);
    CHECK(owner == nullptr);
}
