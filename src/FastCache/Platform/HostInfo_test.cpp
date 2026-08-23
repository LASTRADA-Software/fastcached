// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostInfo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>

using namespace FastCache;

TEST_CASE("The host names itself", "[platform][hostinfo]")
{
    auto const& facts = QueryHostFacts();

    // Asserted as properties rather than against literals, because the values are
    // whatever this machine is and a test that pinned them would only pass on the
    // machine that wrote it.
    CHECK_FALSE(facts.osName.empty());
    CHECK_FALSE(facts.architecture.empty());

#if defined(_WIN32)
    CHECK(facts.osName == "Windows");
#elif defined(__APPLE__)
    // "macOS" rather than the kernel's "Darwin": accurate is not the same as what
    // an operator comparing a fleet listing expects to read.
    CHECK(facts.osName == "macOS");
#elif defined(__linux__)
    CHECK(facts.osName == "Linux");
#endif

    // The architecture comes from the compiler, so it is what this binary runs as
    // — which under emulation is deliberately not what the kernel would say.
    CHECK(facts.architecture != "unknown");
}

TEST_CASE("The facts are memoised, so the same object comes back", "[platform][hostinfo]")
{
    // Not an optimisation detail: callers hold the reference, and a second call
    // returning a different object would leave one of them reading a dangling one.
    CHECK(&QueryHostFacts() == &QueryHostFacts());
}

TEST_CASE("A filesystem reports capacity and free space", "[platform][hostinfo]")
{
    auto const space = QueryDiskSpace(std::filesystem::temp_directory_path());

    CHECK(space.capacityBytes > 0);
    CHECK(space.freeBytes <= space.capacityBytes);
}

TEST_CASE("An unqueryable path answers zero rather than throwing", "[platform][hostinfo]")
{
    // The failure shape this feeds: a scheduling weight and a metric. A node that
    // cannot report its disk should be scheduled on its other properties, not
    // refuse to start — and both fields at zero is a value no real filesystem
    // reports, so a caller that needs to tell "no space" from "no answer" can.
    auto const space = QueryDiskSpace("\\?\this-volume-does-not-exist\nor-does-this");

    CHECK(space.capacityBytes == 0);
    CHECK(space.freeBytes == 0);
}
