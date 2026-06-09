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

TEST_CASE("WatchRegistry: Touched accepts a non-NUL-terminated string_view (transparent hashing)",
          "[protocol][redis][transaction][transparent-hash]")
{
    // Pre-fix `_index` defaulted to `hash<std::string>` so every Touched
    // call allocated a `std::string` to find the bucket. With
    // TransparentStringHash + std::equal_to<>, a string_view over a
    // non-NUL-terminated buffer must locate the entry verbatim. If the
    // map silently fell back to a string conversion, this test would still
    // pass — but the test_fixture lookup pattern (a string_view aliasing
    // a larger buffer) is the realistic hot path from RedisResp where
    // ParsedCommand args are owned string slices.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "myKey");

    // A view that aliases a longer buffer (no embedded NUL) — must match
    // the registered key "myKey" by content, not by C-string identity.
    char const buffer[] = "myKeyExtraBytes"; // longer than "myKey"
    auto const view = std::string_view { buffer, 5 };
    REQUIRE(view == "myKey");
    REQUIRE(registry.Touched(view) == 1);
    REQUIRE(handle->IsDirty());
}

TEST_CASE("WatchHandle: Remember accepts a non-NUL-terminated string_view",
          "[protocol][redis][transaction][transparent-hash]")
{
    // Symmetric guard for WatchHandle::_snapshots — same transparent
    // hashing contract applies.
    WatchHandle handle;
    char const buffer[] = "snapKeyExtra";
    auto const view = std::string_view { buffer, 7 };
    REQUIRE(view == "snapKey");

    handle.Remember(view, 42);
    auto const keys = handle.WatchedKeys();
    REQUIRE(keys.size() == 1);
    REQUIRE(keys[0] == "snapKey");

    // Re-Remember by view must update in place, not push a duplicate.
    handle.Remember(view, 99);
    REQUIRE(handle.WatchedKeys().size() == 1);
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

TEST_CASE("WatchRegistry: HasAnyWatchers reflects index size lock-free",
          "[protocol][redis][transaction][fast-path]")
{
    // Steady-state for a daemon with no transactions in flight is
    // "_entryCount == 0", and Touched must skip its global mutex
    // acquisition entirely in that case. This is the hot-write path
    // optimisation that motivated the lock-free counter.
    WatchRegistry registry;
    REQUIRE_FALSE(registry.HasAnyWatchers());

    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "k1");
    REQUIRE(registry.HasAnyWatchers());

    registry.Register(handle, "k2");
    REQUIRE(registry.HasAnyWatchers());

    // Re-Register on an existing (handle, key) pair must NOT double-count
    // the entry — otherwise the counter would drift up over time.
    registry.Register(handle, "k1");
    REQUIRE(registry.HasAnyWatchers());

    registry.UnregisterAll(handle.get());
    REQUIRE_FALSE(registry.HasAnyWatchers());
}

TEST_CASE("WatchRegistry: Touched on empty registry returns 0 without entering the locked section",
          "[protocol][redis][transaction][fast-path]")
{
    // The contract: when nothing is watching, Touched is a single
    // acquire-load on the atomic counter and a return. We exercise this by
    // verifying it returns 0 — the lock-free shortcut is unobservable from
    // the outside beyond the absence of side effects, but the behavioural
    // contract (zero handles dirtied) holds.
    WatchRegistry registry;
    for (auto i = 0; i < 1000; ++i)
        REQUIRE(registry.Touched("nope") == 0);
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
