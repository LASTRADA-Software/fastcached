// SPDX-License-Identifier: Apache-2.0
#include "CacheDecision.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <iterator>
#include <string_view>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// Every observation, so a case can walk the enum rather than list what somebody
/// remembered. `Last` is the length and is not one of them.
constexpr FetchObservation AllObservations[] = {
    FetchObservation::NotServing, FetchObservation::Miss,        FetchObservation::HitUndecodable,
    FetchObservation::HitServed,  FetchObservation::HitUnusable, FetchObservation::HitStale,
};

static_assert(std::size(AllObservations) == static_cast<std::size_t>(FetchObservation::Last),
              "AllObservations must name every FetchObservation -- a case that walks a short list is a case that "
              "passes over the state somebody just added");

} // namespace

// THE ONE THAT COSTS EVERY KEY IN EVERY FLEET CACHE AT ONCE.
//
// A `CompileValueVersion` bump does not move a key, so a value of the previous
// generation sits under the key this build computes. Answering it without a STORE
// leaves it there to be fetched and refused on every later build, for every key,
// forever -- a permanently dead cache rather than the one cold cache a generation
// bump promises.
//
// Asserted on the ACTION and not merely on "it did not serve", because both correct
// and catastrophic behaviours agree that an undecodable value is not served. What
// separates them is whether the compile that follows stores.
TEST_CASE("an undecodable fetched value is recompiled AND STORED OVER", "[cache-decision]")
{
    auto const action = DecideCacheAction(FetchObservation::HitUndecodable);

    CHECK(action == CacheAction::CompileAndStore);
    CHECK(CompilesForReal(action));
    CHECK(StoresResult(action));

    // The distinguishing half, spelled out: the neighbouring action also compiles,
    // and it is the one that leaves the stale value in place.
    CHECK(action != CacheAction::CompileWithoutStoring);
}

TEST_CASE("a served hit is the only observation that compiles nothing", "[cache-decision]")
{
    for (auto const observation: AllObservations)
    {
        auto const action = DecideCacheAction(observation);
        if (observation == FetchObservation::HitServed)
        {
            CHECK(action == CacheAction::ServeFromCache);
            CHECK_FALSE(CompilesForReal(action));
            CHECK_FALSE(StoresResult(action));
        }
        else
            CHECK(CompilesForReal(action));
    }
}

// The only state that must NOT store, and the reason it is one state rather than
// being folded in with the others: a machine that could not write the object will
// not be able to write a store either.
TEST_CASE("an unusable hit compiles without storing", "[cache-decision]")
{
    CHECK(DecideCacheAction(FetchObservation::HitUnusable) == CacheAction::CompileWithoutStoring);
    CHECK(CompilesForReal(CacheAction::CompileWithoutStoring));
    CHECK_FALSE(StoresResult(CacheAction::CompileWithoutStoring));
}

TEST_CASE("a stale hit repairs its own key", "[cache-decision]")
{
    CHECK(DecideCacheAction(FetchObservation::HitStale) == CacheAction::CompileAndStore);
}

TEST_CASE("a miss and a daemon that never answered both store", "[cache-decision]")
{
    CHECK(DecideCacheAction(FetchObservation::Miss) == CacheAction::CompileAndStore);
    CHECK(DecideCacheAction(FetchObservation::NotServing) == CacheAction::CompileAndStore);
}

// Exactly one state may skip the store, and it is named. A test that only asserted
// the six rows one at a time would pass a seventh state defaulting to whatever the
// author of that state chose; this one says how many there may be.
TEST_CASE("only one observation skips the store", "[cache-decision]")
{
    int skipping = 0;
    for (auto const observation: AllObservations)
        if (!StoresResult(DecideCacheAction(observation)) && CompilesForReal(DecideCacheAction(observation)))
            ++skipping;

    CHECK(skipping == 1);
}

// Every row carries a reason, and the reason is the half a reader of a failing test
// needs. An empty one is a row somebody added without saying why.
TEST_CASE("every observation states why", "[cache-decision]")
{
    for (auto const observation: AllObservations)
    {
        INFO("observation index " << static_cast<int>(observation));
        CHECK_FALSE(CacheActionReason(observation).empty());
    }
}

// ---------------------------------------------------------------------------
// ObserveFetch: the fold from raw facts to a state.

TEST_CASE("a non-serving exchange is NotServing whatever else is true", "[cache-decision]")
{
    // Both non-serving kinds, and with the later facts set to values that would
    // otherwise select a hit -- the point being that they are not consulted.
    for (auto const kind: { CacheOutcomeKind::Rejected, CacheOutcomeKind::Transport })
    {
        CHECK(ObserveFetch(kind, true, true, HitDisposition::Served) == FetchObservation::NotServing);
        CHECK(ObserveFetch(kind, false, false, HitDisposition::Unusable) == FetchObservation::NotServing);
    }
}

TEST_CASE("a serving exchange with no value is a miss", "[cache-decision]")
{
    CHECK(ObserveFetch(CacheOutcomeKind::Miss, false, false, HitDisposition::Unusable) == FetchObservation::Miss);
    // `decoded` and `disposition` are not read on a miss, so a caller passing
    // anything for them must still get Miss.
    CHECK(ObserveFetch(CacheOutcomeKind::Miss, false, true, HitDisposition::Served) == FetchObservation::Miss);
}

TEST_CASE("a hit that did not decode is HitUndecodable whatever its disposition", "[cache-decision]")
{
    for (auto const disposition: { HitDisposition::Served, HitDisposition::Stale, HitDisposition::Unusable })
        CHECK(ObserveFetch(CacheOutcomeKind::Hit, true, false, disposition) == FetchObservation::HitUndecodable);
}

TEST_CASE("a decoded hit takes its state from the disposition", "[cache-decision]")
{
    CHECK(ObserveFetch(CacheOutcomeKind::Hit, true, true, HitDisposition::Served) == FetchObservation::HitServed);
    CHECK(ObserveFetch(CacheOutcomeKind::Hit, true, true, HitDisposition::Stale) == FetchObservation::HitStale);
    CHECK(ObserveFetch(CacheOutcomeKind::Hit, true, true, HitDisposition::Unusable) == FetchObservation::HitUnusable);
}

// The whole path, end to end, in the one arrangement the ticket is about: a daemon
// that answered, a value that came back, and a generation this build cannot read.
TEST_CASE("the generation-bump path stores, from the raw facts", "[cache-decision]")
{
    auto const observation = ObserveFetch(CacheOutcomeKind::Hit,
                                          /*isHit=*/true,
                                          /*decoded=*/false,
                                          /*disposition=*/HitDisposition::Unusable);

    REQUIRE(observation == FetchObservation::HitUndecodable);
    CHECK(StoresResult(DecideCacheAction(observation)));
}
