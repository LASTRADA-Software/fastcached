// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
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
    REQUIRE(ReplaceFileAtomically(path, first, StateFile::Formation).has_value());
    auto const second = WireFields::AsBytes("short");
    REQUIRE(ReplaceFileAtomically(path, second, StateFile::Formation).has_value());

    auto const read = ReadFileIfPresent(path);
    REQUIRE(read.has_value());
    REQUIRE(Testing::Unwrap(read).has_value());
    CHECK(WireFields::AsStringView(Testing::Unwrap(Testing::Unwrap(read))) == "short");
}

TEST_CASE("A file a reader holds open is still replaced, and the reader keeps what it opened", "[consensus][storage]")
{
    // A node's record is replaced on the formation's beat thread; a reader holding it open at that
    // moment must not make the replace fail. On Windows that needs the reader's delete sharing AND the
    // replace's POSIX-semantics rename -- measured, either alone refuses -- and this case holds both.
    auto const scratch = Testing::ScratchDirectory { "durable-file-held" };
    auto const path = scratch / "formation";
    REQUIRE(ReplaceFileAtomically(path, WireFields::AsBytes("old"), StateFile::Formation).has_value());

    auto held = OpenForReading(path);
    REQUIRE(held.has_value());
    auto const replaced = ReplaceFileAtomically(path, WireFields::AsBytes("new"), StateFile::Formation);
    INFO((replaced.has_value() ? std::string { "(replaced)" } : replaced.error().context));
    CHECK(replaced.has_value());

    auto kept = std::array<char, 8> {};
    auto const n = std::fread(kept.data(), 1, kept.size(), held->get());
    CHECK(std::string_view { kept.data(), n } == "old");
    held->reset();

    auto const read = ReadFileIfPresent(path);
    REQUIRE(read.has_value());
    REQUIRE(Testing::Unwrap(read).has_value());
    CHECK(WireFields::AsStringView(Testing::Unwrap(Testing::Unwrap(read))) == "new");
}

TEST_CASE("A directory where a file belongs is a failure to read, never an absent file", "[consensus][storage]")
{
    auto const scratch = Testing::ScratchDirectory { "durable-file-dir" };
    scratch.Write("dir/inside");

    auto const read = ReadFileIfPresent(scratch / "dir");
    CHECK_FALSE(read.has_value());
}
