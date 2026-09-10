// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Server/Connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <type_traits>

using namespace FastCache;

TEST_CASE("What a connection is handed is destructible without touching anything", "[server][connection][teardown]")
{
    // **The rule `ReactorServerLoop` used to state in a comment nothing could read**
    // ([#1051](https://github.com/LASTRADA-Software/fastcached/issues/1051)). A reactor
    // that stops with a connection chain parked on it frees that chain from its own
    // destructor (#1025), and a destructor body runs before the members declared beside
    // it -- so an abandoned connection frame unwinds AFTER the listeners, the servers,
    // the expiry pool, the reaper and the per-bind `TlsContext` are gone. A destroyed
    // coroutine frame runs no user code except destructors, which is what makes trivial
    // destructibility the exact test rather than a stand-in for one.
    //
    // The enforcement is the `static_assert` in `Connection.hpp`, not this case: a
    // planted member fails the BUILD, which no test could report. This case exists for
    // the direction a `static_assert` cannot show, which is that the predicate is
    // capable of refusing at all.
    STATIC_REQUIRE(std::is_trivially_destructible_v<ConnectionHoldings>);

    // The control. A guard nobody has watched refuse is not known to be a guard, and a
    // `static_assert` that holds is silent about whether it COULD fail -- exactly the
    // shape where a predicate that can only answer one way reads as coverage. This is
    // `ConnectionHoldings` with one collaborator that owns something, which is what a
    // future change hands a connection when it hands it something scope-owned.
    struct HoldingsWithAnOwner
    {
        CacheEngine& engine;
        ILogger& logger;
        SessionContext session {};
        LogSource logSource { LogSource::No };
        std::shared_ptr<int> owned {};
    };
    STATIC_REQUIRE_FALSE(std::is_trivially_destructible_v<HoldingsWithAnOwner>);

    // And the same question asked through `SessionContext`, because that is the field
    // most likely to grow one: it is copied BY VALUE into the frame and it is where a
    // per-connection collaborator naturally goes. A control on `ConnectionHoldings`
    // alone would leave the reader believing the check reaches only its own four
    // fields.
    STATIC_REQUIRE(std::is_trivially_destructible_v<SessionContext>);
}
