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
    registry.Register(handle, "foo");
    handle->Remember("foo", 7);

    REQUIRE_FALSE(handle->IsDirty());
    auto const dirtied = registry.Touched("foo");
    REQUIRE(dirtied == 1);
    REQUIRE(handle->IsDirty());
}

TEST_CASE("WatchRegistry: Touched on an unwatched key dirties nothing", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "foo");

    REQUIRE(registry.Touched("bar") == 0);
    REQUIRE_FALSE(handle->IsDirty());
}

TEST_CASE("WatchRegistry: a Touched call dirties every handle watching the key", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto a = std::make_shared<WatchHandle>();
    auto b = std::make_shared<WatchHandle>();
    registry.Register(a, "foo");
    registry.Register(b, "foo");

    REQUIRE(registry.Touched("foo") == 2);
    REQUIRE(a->IsDirty());
    REQUIRE(b->IsDirty());
}

TEST_CASE("WatchRegistry: UnregisterAll drops the index entries and clears the handle", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "foo");
    handle->Remember("foo", 1);
    registry.Register(handle, "bar");
    handle->Remember("bar", 2);
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
        registry.Register(handle, "foo");
        // handle goes out of scope and is destroyed; the registry holds
        // only a weak_ptr so Touched must not try to dereference it.
    }
    REQUIRE(registry.Touched("foo") == 0);
}

TEST_CASE("WatchRegistry: Touched between Register and Remember still dirties the handle",
          "[protocol][redis][transaction][race]")
{
    // Direct unit-test of the fix for the WATCH race. HandleWatch does
    // (1) Register, (2) PeekCas, (3) Remember. Any concurrent SET that
    // lands BETWEEN (1) and (3) must dirty the handle so EXEC aborts —
    // even if the snapshot hasn't been stored yet. The old "Remember
    // first, Register second" shape lost this dirty bit because Touched
    // walked an empty index until step 2 finished.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();

    registry.Register(handle, "k");                     // (1) index entry inserted
    REQUIRE(registry.Touched("k") == 1);                // (2) racing SET in the gap
    REQUIRE(handle->IsDirty());                         //     handle is dirtied
    handle->Remember("k", 5);                           // (3) Remember runs last
    REQUIRE(handle->IsDirty());                         //     dirty bit survives Remember
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
