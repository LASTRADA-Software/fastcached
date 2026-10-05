// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/UnservedToolchains.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include <core/platform/Clock.hpp>

using namespace FastCache::Distributed;
using namespace std::chrono_literals;

namespace
{
/// Whether @p recent names @p fingerprint.
[[nodiscard]] bool Names(std::vector<UnservedToolchain> const& recent, std::string const& fingerprint)
{
    return std::ranges::contains(recent, fingerprint, &UnservedToolchain::fingerprint);
}
} // namespace

TEST_CASE("An unserved toolchain counts its refusals and keeps the label a client stated",
          "[distributed][scheduler][conditions]")
{
    core::platform::ManualClock clock;
    UnservedToolchains unserved { clock };

    unserved.Refused("fp", "cl 19.44.35207");
    // A client that states no label -- an operator's pinned fingerprint has no banner -- does not
    // erase what another client said about the same toolchain.
    unserved.Refused("fp", "");

    auto const recent = unserved.Recent();
    REQUIRE(recent.size() == 1);
    CHECK(recent.front().fingerprint == "fp");
    CHECK(recent.front().label == "cl 19.44.35207");
    CHECK(recent.front().refusals == 2);
}

TEST_CASE("An unserved toolchain is forgotten once nobody has asked for the window", "[distributed][scheduler][conditions]")
{
    core::platform::ManualClock clock;
    UnservedToolchains unserved { clock };
    unserved.Refused("fp", "g++ 14.2.0");

    // AT the window's edge it is still recent; one second past, it is gone -- the row clears by
    // time as well as by a worker arriving.
    clock.advance(UnservedToolchains::Window);
    CHECK(Names(unserved.Recent(), "fp"));
    clock.advance(1s);
    CHECK(unserved.Recent().empty());
}

TEST_CASE("The unserved list is bounded, and the toolchain asked for longest ago leaves first",
          "[distributed][scheduler][conditions]")
{
    core::platform::ManualClock clock;
    UnservedToolchains unserved { clock };
    for (auto const index: std::views::iota(std::size_t { 0 }, UnservedToolchains::Capacity))
    {
        unserved.Refused(std::format("fp-{}", index), {});
        clock.advance(1s);
    }
    // fp-0 is asked again, so fp-1 is now the one asked for longest ago.
    unserved.Refused("fp-0", {});
    clock.advance(1s);
    unserved.Refused("fp-new", {});

    auto const recent = unserved.Recent();
    CHECK(recent.size() == UnservedToolchains::Capacity);
    CHECK(Names(recent, "fp-new"));
    CHECK(Names(recent, "fp-0"));
    CHECK_FALSE(Names(recent, "fp-1"));
}
