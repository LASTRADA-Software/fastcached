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

TEST_CASE("WatchRegistry::Unregister removes only that key and leaves others intact",
          "[protocol][redis][transaction]")
{
    // Per-key Unregister is the building block for HandleWatch's
    // partial-failure rollback: a later WATCH that hits a StorageError
    // must roll back its own keys without wiping registrations from
    // earlier successful WATCH calls. UnregisterAll's blast radius is
    // wrong for that path.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "a");
    registry.Register(handle, "b");
    handle->Remember("a", 1);
    handle->Remember("b", 2);

    REQUIRE(registry.Unregister(handle.get(), "b") == 1);
    // Unregister must NOT touch _snapshots: both 'a' and 'b' snapshots
    // remain on the handle. (HandleWatch's partial-failure rollback drops
    // index entries only; the snapshots are inert without the index, but
    // leaving them avoids the per-key map mutation under WatchHandle::_mu.)
    {
        auto const keys = handle->WatchedKeys();
        REQUIRE(keys.size() == 2);
    }
    // `a` still in the registry index — racing SET on `a` dirties the handle.
    REQUIRE(registry.Touched("a") == 1);
    REQUIRE(handle->IsDirty());
    // `b` is no longer in the registry index — racing SET on `b` is a no-op.
    handle->Clear();
    REQUIRE(registry.Touched("b") == 0);
    REQUIRE_FALSE(handle->IsDirty());
}

TEST_CASE("WatchRegistry::Unregister is a no-op when the key is not registered",
          "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "a");
    REQUIRE(registry.Unregister(handle.get(), "missing") == 0);
    REQUIRE(registry.HasAnyWatchers());
}

TEST_CASE("WatchRegistry::Unregister tolerates a null handle pointer",
          "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    REQUIRE(registry.Unregister(nullptr, "k") == 0);
}

TEST_CASE("WatchHandle::ClaimAndClearDirty returns prior state and resets the bit",
          "[protocol][redis][transaction]")
{
    // ClaimAndClearDirty is the atomic read-and-clear primitive that closes
    // the EXEC race. The previous shape was IsDirty + Clear in two atomic
    // operations; a racing MarkDirty between them could either be observed
    // (and then wiped by Clear) or missed entirely. exchange() collapses
    // the read and the clear into one indivisible step.
    WatchHandle handle;
    handle.Remember("k", 9);
    handle.MarkDirty();

    REQUIRE(handle.ClaimAndClearDirty());
    REQUIRE_FALSE(handle.IsDirty());
    // Second claim returns false — no new dirty signal since the clear.
    REQUIRE_FALSE(handle.ClaimAndClearDirty());
    // Snapshots are wiped — a future MULTI on the same handle starts fresh.
    REQUIRE(handle.WatchedKeys().empty());
}

TEST_CASE("WatchRegistry::UnregisterAll releases _mu before calling handle->Clear",
          "[protocol][redis][transaction][lock-order]")
{
    // Behavioural assertion: while UnregisterAll runs, a concurrent Touched
    // on a DIFFERENT handle for a DIFFERENT key must NOT block (would have
    // blocked under the old shape if Clear held handle->_mu while
    // UnregisterAll held registry._mu). We cannot directly observe lock
    // release ordering from the outside, but we CAN observe that the
    // handle's Clear() runs and the dirty/snapshot wipe is complete by the
    // time UnregisterAll returns.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "k");
    handle->Remember("k", 1);
    handle->MarkDirty();
    REQUIRE(handle->IsDirty());

    registry.UnregisterAll(handle.get());

    // Index entry is gone; dirty was reset; snapshots are wiped.
    REQUIRE_FALSE(handle->IsDirty());
    REQUIRE(handle->WatchedKeys().empty());
    REQUIRE_FALSE(registry.HasAnyWatchers());
}

TEST_CASE("WatchRegistry: a MarkDirty that races past UnregisterAll is collected by the NEXT EXEC's ClaimAndClearDirty",
          "[protocol][redis][transaction][race]")
{
    // Simulates the EXEC race in deterministic order:
    //   1. Touched runs partially — snapshots a strong_ptr to the handle
    //      under _mu, releases _mu, but the MarkDirty store hasn't fired
    //      yet (modelled by snapshotting the strong_ptr by hand).
    //   2. HandleExec's UnregisterAll runs (index entry erased; Clear
    //      stores dirty=false).
    //   3. The deferred MarkDirty lands — dirty=true on the same handle.
    //   4. HandleExec calls ClaimAndClearDirty AFTER UnregisterAll.
    // Under the fix, the next EXEC will read dirty=true exactly once
    // (the racing signal is collected, not lost) and the bit will be
    // cleared so a third EXEC on a different key does not spuriously
    // abort. Under the OLD shape, the dirty bit either leaked into the
    // next EXEC (spurious *-1) or was wiped before being read.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    registry.Register(handle, "k");
    handle->Remember("k", 1);

    // Step 2: UnregisterAll runs (index entry removed; dirty cleared).
    registry.UnregisterAll(handle.get());
    REQUIRE_FALSE(handle->IsDirty());

    // Step 3: the deferred MarkDirty from the racing Touched lands.
    handle->MarkDirty();
    REQUIRE(handle->IsDirty());

    // Step 4: the connection issues a fresh WATCH; we model the NEXT
    // EXEC's ClaimAndClearDirty. It should observe the racing dirty bit
    // and reset it in one atomic step.
    REQUIRE(handle->ClaimAndClearDirty());

    // A SUBSEQUENT EXEC (no further mutations) must see dirty=false —
    // the racing signal was collected exactly once.
    REQUIRE_FALSE(handle->ClaimAndClearDirty());
}
