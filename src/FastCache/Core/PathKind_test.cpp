// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/PathKind.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <tests/ScratchPath.hpp>

using FastCache::PathNamesAFile;
using FastCache::Testing::ScratchDirectory;

TEST_CASE("PathKind: what exists answers for itself", "[core][path]")
{
    ScratchDirectory const scratch { "path-kind" };

    SECTION("an existing directory is a directory")
    {
        REQUIRE_FALSE(PathNamesAFile(scratch.Path()));
    }

    SECTION("an existing regular file is a file, whatever it is called")
    {
        // Deliberately extension-less, because the spelling rule below would say
        // otherwise: an operator who pointed a previous release at
        // `/var/db/fastcached` and got one CoW file there must keep getting one
        // file, or the next start fans out beside a cache that then reads empty.
        auto const legacy = scratch / "cache";
        std::ofstream { legacy } << "not really a CoW file";
        REQUIRE(std::filesystem::exists(legacy));

        REQUIRE(PathNamesAFile(legacy));
    }
}

TEST_CASE("PathKind: a path that does not exist is judged by its spelling", "[core][path]")
{
    ScratchDirectory const scratch { "path-kind" };

    SECTION("a file extension names a file")
    {
        REQUIRE(PathNamesAFile(scratch / "cache.cow"));
    }

    SECTION("no extension names a directory")
    {
        REQUIRE_FALSE(PathNamesAFile(scratch / "cache"));
    }

    SECTION("a missing parent does not throw and does not change the answer")
    {
        // The install-time handover asks this about a path an administrator has
        // not created yet, so an absent -- or unreadable -- parent has to come
        // back as an answer rather than as an exception out of
        // `--install-service`.
        auto const nested = scratch / "not-created-yet";
        REQUIRE(PathNamesAFile(nested / "cache.cow"));
        REQUIRE_FALSE(PathNamesAFile(nested / "cache"));
    }

    SECTION("an empty path is not a file")
    {
        // Nothing hands one over -- an empty `storage_path` is memory-only and is
        // filtered out before this -- but the answer must still be defined, and
        // "not a file" is the safe one: it can only ever lead to a `mkdir` of a
        // path that is refused, never to a file being buried under a directory.
        REQUIRE_FALSE(PathNamesAFile(std::filesystem::path {}));
    }
}
