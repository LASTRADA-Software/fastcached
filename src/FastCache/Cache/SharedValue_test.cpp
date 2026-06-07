// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/SharedValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

[[nodiscard]] std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (auto const c: text)
        out.push_back(static_cast<std::byte>(c));
    return out;
}

[[nodiscard]] std::string Decode(std::span<std::byte const> bytes)
{
    std::string out;
    for (auto const b: bytes)
        out.push_back(static_cast<char>(b));
    return out;
}

} // namespace

TEST_CASE("SharedValue default is null and empty", "[cache][sharedvalue]")
{
    FastCache::SharedValue v;
    REQUIRE_FALSE(static_cast<bool>(v));
    REQUIRE(v.size() == 0);
    REQUIRE(v.Bytes().empty());
}

TEST_CASE("SharedValue holds and exposes its payload", "[cache][sharedvalue]")
{
    auto const v = FastCache::MakeSharedValue(Bytes("hello world"));
    REQUIRE(static_cast<bool>(v));
    REQUIRE(v.size() == 11);
    REQUIRE(Decode(v.Bytes()) == "hello world");
}

TEST_CASE("SharedValue an empty payload is non-null with zero size", "[cache][sharedvalue]")
{
    auto const v = FastCache::MakeSharedValue(Bytes(""));
    REQUIRE(static_cast<bool>(v));
    REQUIRE(v.size() == 0);
    REQUIRE(v.Bytes().empty());
}

TEST_CASE("SharedValue copies share one buffer and outlive the original", "[cache][sharedvalue]")
{
    auto copy = FastCache::SharedValue {};
    {
        auto const original = FastCache::MakeSharedValue(Bytes("shared payload"));
        copy = original;                                         // refcount bump, same buffer
        REQUIRE(copy.Bytes().data() == original.Bytes().data()); // no deep copy
    }
    // `original` is gone; `copy` still owns the buffer.
    REQUIRE(Decode(copy.Bytes()) == "shared payload");
}

TEST_CASE("SharedValue move leaves the source null", "[cache][sharedvalue]")
{
    auto src = FastCache::MakeSharedValue(Bytes("moved"));
    auto const dst = std::move(src);
    REQUIRE_FALSE(static_cast<bool>(src)); // NOLINT(bugprone-use-after-move) — intentional
    REQUIRE(Decode(dst.Bytes()) == "moved");
}

TEST_CASE("SharedValue keep-alive pins the payload independently of the handle", "[cache][sharedvalue]")
{
    std::shared_ptr<void const> keepAlive;
    std::span<std::byte const> view;
    {
        auto const v = FastCache::MakeSharedValue(Bytes("pinned bytes"));
        keepAlive = v.AsKeepAlive();
        view = v.Bytes();
    }
    // The SharedValue handle is destroyed, but the keep-alive still owns the
    // buffer — the view remains valid (verified under ASan).
    REQUIRE(keepAlive != nullptr);
    REQUIRE(Decode(view) == "pinned bytes");
    keepAlive.reset(); // releases the last reference; ASan would flag a leak/UAF
}
