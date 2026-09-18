// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Distributed/RosterStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;

namespace
{

/// A kept roster with nothing in it but its identity.
[[nodiscard]] Cluster::PersistedRoster Sample()
{
    auto certified = Cluster::CertifiedRoster {
        .clusterId = "fleet", .version = 3, .roster = Cluster::EncodeRoster(Cluster::Roster {}), .endorsements = {}
    };
    return Cluster::PersistedRoster { .certificate = std::move(certified),
                                      .certifiedUntil =
                                          std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } } };
}

} // namespace

TEST_CASE("A kept roster is read back as it was saved, and no file is no roster", "[distributed][roster]")
{
    auto const scratch = Testing::ScratchDirectory { "roster-store" };
    auto const path = scratch / RosterFileName;

    auto const absent = LoadPersistedRoster(path);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());

    auto store = FileRosterStore { path };
    REQUIRE(store.Save(Sample()).has_value());
    auto const loaded = LoadPersistedRoster(path);
    REQUIRE(loaded.has_value());
    REQUIRE(Testing::Unwrap(loaded).has_value());
    CHECK(Testing::Unwrap(Testing::Unwrap(loaded)) == Sample());
}

TEST_CASE("A kept roster that cannot be used refuses, never reads as none kept", "[distributed][roster]")
{
    // The distinction is the whole contract: "none kept" starts a worker from its anchors, which
    // may name a voter revoked since, so a file this machine wrote and cannot now use must not
    // answer the same.
    auto const scratch = Testing::ScratchDirectory { "roster-store-bad" };
    auto const path = scratch / RosterFileName;

    SECTION("an empty file")
    {
        scratch.Write(RosterFileName);
    }
    SECTION("bytes that are not a roster")
    {
        scratch.Write(RosterFileName, "not a roster");
    }
    SECTION("another build's layout")
    {
        auto bytes = Cluster::EncodePersistedRoster(Sample());
        auto const split = WireFields::SplitAll(bytes);
        REQUIRE(split.has_value());
        auto const& fields = Testing::Unwrap(split);
        REQUIRE(fields[0].size() == 1);
        auto const offset = static_cast<std::size_t>(fields[0].data() - bytes.data());
        bytes[offset] = std::byte { Cluster::PersistedRosterFormatVersion + 1 };
        scratch.Write(RosterFileName, WireFields::AsStringView(bytes));
    }

    auto const loaded = LoadPersistedRoster(path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().contains(path.string()));
}
