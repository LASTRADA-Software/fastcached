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
#include <utility>

#include <tests/SocketDecorator.hpp>

using namespace FastCache;

namespace
{

/// A parking fake over @p inner, held by exactly one `shared_ptr` in an allocation of its own.
///
/// Not `make_shared`: an object sharing its allocation with the control block stays mapped
/// while any `weak_ptr` to it lives, so ASan would see no freed object for a late member
/// access -- and a `weak_ptr` is how each case below observes the destruction.
/// @param inner The socket the fake decorates.
/// @return The only owning reference.
template <typename Fake>
std::shared_ptr<Fake> SoleOwner(ISocket& inner)
{
    return std::make_unique<Fake>(inner);
}

/// Await a readability watch on a socket this coroutine owns, and drop the socket the moment
/// the watch resumes -- what a connection does when its watch says the peer is gone.
/// @param owner The socket's only owning reference, moved in; reset from inside the resumption.
DetachedTask WatchThenDrop(std::shared_ptr<Testing::ParkingReadableSocket> owner)
{
    std::ignore = co_await owner->WaitReadable();
    owner.reset();
}

/// Write to a socket this coroutine owns, and drop the socket the moment the write resumes.
/// @param owner The socket's only owning reference, moved in; reset from inside the resumption.
DetachedTask WriteThenDrop(std::shared_ptr<Testing::ParkingWritableSocket> owner)
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
    // call returns. The coroutine holds the only owning reference, so the case watches it
    // through a `weak_ptr`, and nothing after the call may touch the raw pointer.
    SECTION("a readable watch retired by Close")
    {
        auto owner = SoleOwner<Testing::ParkingReadableSocket>(*pair.server);
        auto* const socket = owner.get();
        std::weak_ptr<Testing::ParkingReadableSocket> const watched = owner;
        WatchThenDrop(std::move(owner));
        REQUIRE(socket->IsWatchParked());
        REQUIRE_FALSE(watched.expired());

        socket->Close();
        CHECK(watched.expired());
    }

    SECTION("a readable watch retired by CancelRead")
    {
        auto owner = SoleOwner<Testing::ParkingReadableSocket>(*pair.server);
        auto* const socket = owner.get();
        std::weak_ptr<Testing::ParkingReadableSocket> const watched = owner;
        WatchThenDrop(std::move(owner));
        REQUIRE(socket->IsWatchParked());
        REQUIRE_FALSE(watched.expired());

        socket->CancelRead();
        CHECK(watched.expired());
    }

    SECTION("a parked write retired by Close")
    {
        auto owner = SoleOwner<Testing::ParkingWritableSocket>(*pair.server);
        auto* const socket = owner.get();
        std::weak_ptr<Testing::ParkingWritableSocket> const watched = owner;
        socket->StopReading();
        WriteThenDrop(std::move(owner));
        REQUIRE(socket->IsWriteParked());
        REQUIRE_FALSE(watched.expired());

        socket->Close();
        CHECK(watched.expired());
    }
}
