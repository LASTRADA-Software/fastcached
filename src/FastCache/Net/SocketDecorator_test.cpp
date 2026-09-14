// SPDX-License-Identifier: Apache-2.0
//
// The shared socket fakes' own contract (#1430): a fake is held to the rule the sockets it
// stands for are held to, that retiring a parked operation completes it LAST (see
// `Testing::CompleteCancelled`). The cases below are the shape that makes the rule bite --
// the coroutine the completion resumes OWNS the socket and drops it before control returns
// -- and a fake that touched a member after completing would write through a freed object,
// which only a sanitizer build reports.
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <tuple>

#include <tests/SocketDecorator.hpp>

using namespace FastCache;

namespace
{

/// Await a readability watch on a socket the caller owns, and drop the socket the moment the
/// watch resumes -- what a connection does when its watch says the peer is gone.
/// @param owner The socket's only owner; reset from inside the resumption.
DetachedTask WatchThenDrop(std::unique_ptr<Testing::ParkingReadableSocket>& owner)
{
    std::ignore = co_await owner->WaitReadable();
    owner.reset();
}

/// Write to a socket the caller owns, and drop the socket the moment the write resumes.
/// @param owner The socket's only owner; reset from inside the resumption.
DetachedTask WriteThenDrop(std::unique_ptr<Testing::ParkingWritableSocket>& owner)
{
    auto const bytes = std::array { std::byte { 0x2A } };
    std::ignore = co_await owner->Write(bytes);
    owner.reset();
}

} // namespace

TEST_CASE("A parking fake lets the coroutine it resumes destroy it", "[net][socket][fake]")
{
    auto const pair = InMemorySocketPair::Create();

    // Each route resumes the awaiting coroutine inline, which destroys the socket before the
    // call returns. Nothing after the call may touch the raw pointer.
    SECTION("a readable watch retired by Close")
    {
        auto owner = std::make_unique<Testing::ParkingReadableSocket>(*pair.server);
        auto* const socket = owner.get();
        WatchThenDrop(owner);
        REQUIRE(socket->IsWatchParked());

        socket->Close();
        CHECK(owner == nullptr);
    }

    SECTION("a readable watch retired by CancelRead")
    {
        auto owner = std::make_unique<Testing::ParkingReadableSocket>(*pair.server);
        auto* const socket = owner.get();
        WatchThenDrop(owner);
        REQUIRE(socket->IsWatchParked());

        socket->CancelRead();
        CHECK(owner == nullptr);
    }

    SECTION("a parked write retired by Close")
    {
        auto owner = std::make_unique<Testing::ParkingWritableSocket>(*pair.server);
        auto* const socket = owner.get();
        socket->StopReading();
        WriteThenDrop(owner);
        REQUIRE(socket->IsWriteParked());

        socket->Close();
        CHECK(owner == nullptr);
    }
}
