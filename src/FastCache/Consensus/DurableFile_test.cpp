// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;

TEST_CASE("A missing file and an empty one are different answers", "[consensus][storage]")
{
    auto const scratch = Testing::ScratchDirectory { "durable-file" };

    auto const absent = ReadFileIfPresent(scratch / "absent");
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());

    scratch.Write("empty");
    auto const empty = ReadFileIfPresent(scratch / "empty");
    REQUIRE(empty.has_value());
    REQUIRE(Testing::Unwrap(empty).has_value());
    CHECK(Testing::Unwrap(Testing::Unwrap(empty)).empty());
}

TEST_CASE("A replaced file reads back whole, and replacing it again leaves only the new bytes", "[consensus][storage]")
{
    auto const scratch = Testing::ScratchDirectory { "durable-file-replace" };
    auto const path = scratch / "state";

    auto const first = std::vector<std::byte>(64, std::byte { 0xAA });
    REQUIRE(ReplaceFileAtomically(path, first).has_value());
    auto const second = WireFields::AsBytes("short");
    REQUIRE(ReplaceFileAtomically(path, second).has_value());

    auto const read = ReadFileIfPresent(path);
    REQUIRE(read.has_value());
    REQUIRE(Testing::Unwrap(read).has_value());
    CHECK(WireFields::AsStringView(Testing::Unwrap(Testing::Unwrap(read))) == "short");
}

TEST_CASE("A directory where a file belongs is a failure to read, never an absent file", "[consensus][storage]")
{
    auto const scratch = Testing::ScratchDirectory { "durable-file-dir" };
    scratch.Write("dir/inside");

    auto const read = ReadFileIfPresent(scratch / "dir");
    CHECK_FALSE(read.has_value());
}
