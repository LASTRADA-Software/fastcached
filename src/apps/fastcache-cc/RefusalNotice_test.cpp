// SPDX-License-Identifier: Apache-2.0
#include "../../tests/ScratchPath.hpp"
#include "RefusalNotice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

using namespace FastCache::Cc;
namespace Wire = FastCache::CompileCacheWire;
using namespace std::chrono_literals;

namespace
{
constexpr auto Epoch = std::chrono::system_clock::time_point {};
}

TEST_CASE("RefusalNotice: a refusal about the daemon is persistent, one about the request is not", "[launcher]")
{
    // The distinction the whole design rests on. `UnsupportedVersion` will be just as
    // true for the next translation unit; `MalformedValue` is about one stored object
    // and says nothing about the next.
    CHECK(PersistentRefusalFor(Wire::ErrorCode::UnsupportedVersion) != nullptr);
    CHECK(PersistentRefusalFor(Wire::ErrorCode::Unauthenticated) != nullptr);

    CHECK(PersistentRefusalFor(Wire::ErrorCode::MalformedValue) == nullptr);
    CHECK(PersistentRefusalFor(Wire::ErrorCode::StorageWriteFailed) == nullptr);
    CHECK(PersistentRefusalFor(Wire::ErrorCode::PayloadTooLarge) == nullptr);
}

TEST_CASE("RefusalNotice: a thousand units produce one line, not a thousand", "[launcher]")
{
    auto const dir = FastCache::Testing::UniqueScratchPath("refusal-throttle");
    std::filesystem::create_directories(dir);

    // The defect this exists to prevent is silence; the defect it must not introduce
    // is a line per translation unit. Both directions asserted, because a throttle
    // that never announces passes a test that only checks for absence of spam.
    CHECK(ShouldAnnounceRefusal(dir, "127.0.0.1:6674", Wire::ErrorCode::UnsupportedVersion, Epoch));

    auto announced = 0;
    for (auto i = 0; i < 1000; ++i)
        if (ShouldAnnounceRefusal(dir, "127.0.0.1:6674", Wire::ErrorCode::UnsupportedVersion, Epoch + 1s))
            ++announced;
    CHECK(announced == 0);

    // And it must start speaking again once the interval passes, or somebody who
    // fixes the daemon never learns the message stopped for the right reason.
    CHECK(ShouldAnnounceRefusal(dir, "127.0.0.1:6674", Wire::ErrorCode::UnsupportedVersion, Epoch + 301s));
}

TEST_CASE("RefusalNotice: two daemons and two causes throttle separately", "[launcher]")
{
    auto const dir = FastCache::Testing::UniqueScratchPath("refusal-keys");
    std::filesystem::create_directories(dir);

    CHECK(ShouldAnnounceRefusal(dir, "a:1", Wire::ErrorCode::UnsupportedVersion, Epoch));

    // A different daemon is a different fact and must not be suppressed by the first.
    CHECK(ShouldAnnounceRefusal(dir, "b:2", Wire::ErrorCode::UnsupportedVersion, Epoch));
    // So is a different cause on the same daemon: "too old" and "no credential" are
    // fixed in different places.
    CHECK(ShouldAnnounceRefusal(dir, "a:1", Wire::ErrorCode::Unauthenticated, Epoch));

    // ...and each is then throttled on its own.
    CHECK(!ShouldAnnounceRefusal(dir, "a:1", Wire::ErrorCode::UnsupportedVersion, Epoch + 1s));
    CHECK(!ShouldAnnounceRefusal(dir, "b:2", Wire::ErrorCode::UnsupportedVersion, Epoch + 1s));
}

TEST_CASE("RefusalNotice: no state directory answers YES, not silence", "[launcher]")
{
    // A machine that cannot persist the stamp is exactly the one where suppressing
    // the line would make it permanent, and this function exists to break a silence.
    // Failing closed here would restore the bug on the hosts least able to notice.
    CHECK(ShouldAnnounceRefusal({}, "127.0.0.1:6674", Wire::ErrorCode::UnsupportedVersion, Epoch));
    CHECK(ShouldAnnounceRefusal({}, "127.0.0.1:6674", Wire::ErrorCode::UnsupportedVersion, Epoch + 1s));
}

TEST_CASE("RefusalNotice: the line says what it means for the build, not only what happened", "[launcher]")
{
    auto const* const row = PersistentRefusalFor(Wire::ErrorCode::UnsupportedVersion);
    REQUIRE(row != nullptr);

    auto const line = RefusalNoticeLine("10.0.0.4:6674", *row, "unsupported wire version 3, this build speaks 5");

    // The endpoint, so an operator knows WHICH daemon on a multi-cache setup.
    CHECK(line.contains("10.0.0.4:6674"));
    // The daemon's own words, which carry the version range the category cannot.
    CHECK(line.contains("this build speaks 5"));
    // The remedy, which is the half a developer watching a slow build actually needs.
    CHECK(line.contains("version pair"));
    // And the consequence, stated plainly rather than left to be inferred.
    CHECK(line.contains("without a cache"));

    // A daemon that sent no message still produces a usable line, with no empty
    // parenthesis left behind.
    auto const bare = RefusalNoticeLine("10.0.0.4:6674", *row, "");
    CHECK(!bare.contains("()"));
    CHECK(bare.contains("refusing every request;"));
}
