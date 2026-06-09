// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/RedisTransaction.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using FastCache::WatchHandle;
using FastCache::WatchRegistry;

TEST_CASE("WatchRegistry: Touched flips dirty on a registered handle", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "foo", 7);

    REQUIRE_FALSE(handle->IsDirty());
    auto const dirtied = registry.Touched("foo");
    REQUIRE(dirtied == 1);
    REQUIRE(handle->IsDirty());
}

TEST_CASE("WatchRegistry: Touched on an unwatched key dirties nothing", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "foo", 1);

    REQUIRE(registry.Touched("bar") == 0);
    REQUIRE_FALSE(handle->IsDirty());
}

TEST_CASE("WatchRegistry: a Touched call dirties every handle watching the key", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto a = std::make_shared<WatchHandle>();
    auto b = std::make_shared<WatchHandle>();
    registry.Register(a, "foo", 1);
    registry.Register(b, "foo", 1);

    REQUIRE(registry.Touched("foo") == 2);
    REQUIRE(a->IsDirty());
    REQUIRE(b->IsDirty());
}

TEST_CASE("WatchRegistry: UnregisterAll drops the index entries and clears the handle", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "foo", 1);
    registry.Register(handle, "bar", 2);
    REQUIRE(handle->WatchedKeys().size() == 2);

    registry.UnregisterAll(handle.get());
    REQUIRE(handle->WatchedKeys().empty());
    REQUIRE_FALSE(handle->IsDirty());
    // A subsequent Touched on either key must NOT dirty the freshly-cleared
    // handle — the index entry is gone too.
    REQUIRE(registry.Touched("foo") == 0);
    REQUIRE(registry.Touched("bar") == 0);
    REQUIRE_FALSE(handle->IsDirty());
}

TEST_CASE("WatchRegistry: an expired handle is skipped without crashing", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    {
        auto handle = std::make_shared<WatchHandle>();
        registry.Register(handle, "foo", 1);
        // handle goes out of scope and is destroyed; the registry holds
        // only a weak_ptr so Touched must not try to dereference it.
    }
    REQUIRE(registry.Touched("foo") == 0);
}

TEST_CASE("WatchHandle::Clear forgets snapshots and resets dirty", "[protocol][redis][transaction]")
{
    WatchHandle handle;
    handle.Remember("foo", 1);
    handle.MarkDirty();
    REQUIRE(handle.IsDirty());

    handle.Clear();
    REQUIRE_FALSE(handle.IsDirty());
    REQUIRE(handle.WatchedKeys().empty());
}
