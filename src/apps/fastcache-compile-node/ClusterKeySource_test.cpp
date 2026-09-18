// SPDX-License-Identifier: Apache-2.0
#include "ClusterKeySource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Node;

TEST_CASE("A key file is read, trimmed, and refused when it is too short", "[node][cluster-key]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcache-cluster-key-test" };
    auto const& dir = scratch.Path();

    SECTION("a generated key, with the newline an editor left on it")
    {
        // The overwhelmingly common way to produce one of these ends the file with a
        // newline, and a key one byte different from its peers' fails to authenticate
        // with a message about a bad proof rather than about a newline.
        auto const path = dir / "good";
        {
            std::ofstream out { path, std::ios::binary };
            out << "0123456789abcdefghij\n";
        }

        auto const key = ReadClusterKey(path);
        REQUIRE(key.has_value());
        CHECK(key->size() == 20);
    }

    SECTION("a key short enough to guess is refused, and the message says how to make one")
    {
        auto const path = dir / "short";
        {
            std::ofstream out { path, std::ios::binary };
            out << "hunter2\n";
        }

        auto const key = ReadClusterKey(path);
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().contains("urandom"));
    }

    SECTION("a file that is not there is a refusal rather than an empty key")
    {
        // An empty key would authenticate every node on the segment against every
        // other, which is the one failure this whole layer exists to prevent.
        CHECK_FALSE(ReadClusterKey(dir / "absent").has_value());
    }

    std::filesystem::remove_all(dir);
}
