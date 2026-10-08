// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/RouteProbe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string_view>

using namespace FastCache;

TEST_CASE("The route probe reaches loopback from loopback", "[platform][route]")
{
    auto const answer = MakeSystemRouteProbe()->SourceFor("127.0.0.1");
    REQUIRE(answer.has_value());
    CHECK(*answer == "127.0.0.1");
}

TEST_CASE("The route probe refuses a target that is not an address literal", "[platform][route]")
{
    auto const probe = MakeSystemRouteProbe();
    for (auto const target: std::array<std::string_view, 4> { "example.com", "", "10.0.0.1:80", "fe80::1%lo" })
    {
        auto const answer = probe->SourceFor(target);
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error() == RouteProbeError::InvalidTarget);
    }
}

TEST_CASE("The route probe answers in the spelling local addresses use", "[platform][route]")
{
    // IPv6 may be disabled on the machine running the suite: then the honest answers are the two
    // below, and a malformed-target verdict would be the probe misreading a valid literal.
    auto const answer = MakeSystemRouteProbe()->SourceFor("::1");
    if (answer.has_value())
        CHECK(*answer == "::1");
    else
        CHECK((answer.error() == RouteProbeError::NoRoute || answer.error() == RouteProbeError::Unsupported));
}

TEST_CASE("The default route probe targets are address literals the probe accepts", "[platform][route]")
{
    auto const probe = MakeSystemRouteProbe();
    for (auto const target: DefaultRouteProbeTargets)
    {
        auto const answer = probe->SourceFor(target);
        CHECK((answer.has_value() || answer.error() != RouteProbeError::InvalidTarget));
    }
}
