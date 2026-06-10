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
    REQUIRE(registry.Register(handle, "foo"));
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
    REQUIRE(registry.Register(handle, "foo"));

    REQUIRE(registry.Touched("bar") == 0);
    REQUIRE_FALSE(handle->IsDirty());
}

TEST_CASE("WatchRegistry: a Touched call dirties every handle watching the key", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto a = std::make_shared<WatchHandle>();
    auto b = std::make_shared<WatchHandle>();
    REQUIRE(registry.Register(a, "foo"));
    REQUIRE(registry.Register(b, "foo"));

    REQUIRE(registry.Touched("foo") == 2);
    REQUIRE(a->IsDirty());
    REQUIRE(b->IsDirty());
}

TEST_CASE("WatchRegistry: UnregisterAll drops the index entries and wipes snapshots, "
          "but preserves _dirty for ClaimAndClearDirty to read",
          "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    bool const inserted1 = registry.Register(handle, "foo");
    REQUIRE(inserted1);
    handle->Remember("foo", 1);
    bool const inserted2 = registry.Register(handle, "bar");
    REQUIRE(inserted2);
    handle->Remember("bar", 2);
    REQUIRE(handle->WatchedKeys().size() == 2);

    // Simulate the EXEC race: a Touched landed BEFORE we entered
    // UnregisterAll. _dirty is already true.
    handle->MarkDirty();
    REQUIRE(handle->IsDirty());

    registry.UnregisterAll(handle.get());
    // Snapshots are wiped; index entries are gone.
    REQUIRE(handle->WatchedKeys().empty());
    REQUIRE_FALSE(registry.HasAnyWatchers());
    // BUT _dirty is preserved — ClaimAndClearDirty is the sole resetter
    // (the EXEC-race fix). Were Clear to wipe _dirty here, a Touched whose
    // MarkDirty fired AFTER the index-erase but BEFORE this call would be
    // silently lost.
    REQUIRE(handle->IsDirty());

    // A subsequent Touched on either key must NOT find the index entry —
    // there are no handles to dirty for those keys anymore.
    REQUIRE(registry.Touched("foo") == 0);
    REQUIRE(registry.Touched("bar") == 0);

    // ClaimAndClearDirty atomically observes-and-clears the surviving
    // dirty bit, the way HandleExec does after UnregisterAll.
    REQUIRE(handle->ClaimAndClearDirty());
    REQUIRE_FALSE(handle->IsDirty());
}

TEST_CASE("WatchRegistry: an expired handle is skipped without crashing", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    {
        auto handle = std::make_shared<WatchHandle>();
        REQUIRE(registry.Register(handle, "foo"));
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
    REQUIRE(registry.Register(handle, "myKey"));

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

    REQUIRE(registry.Register(handle, "k")); // (1) index entry inserted
    REQUIRE(registry.Touched("k") == 1);     // (2) racing SET in the gap
    REQUIRE(handle->IsDirty());              //     handle is dirtied
    handle->Remember("k", 5);                // (3) Remember runs last
    REQUIRE(handle->IsDirty());              //     dirty bit survives Remember
}

TEST_CASE("WatchRegistry: HasAnyWatchers reflects index size lock-free", "[protocol][redis][transaction][fast-path]")
{
    // Steady-state for a daemon with no transactions in flight is
    // "_entryCount == 0", and Touched must skip its global mutex
    // acquisition entirely in that case. This is the hot-write path
    // optimisation that motivated the lock-free counter.
    WatchRegistry registry;
    REQUIRE_FALSE(registry.HasAnyWatchers());

    auto handle = std::make_shared<WatchHandle>();
    REQUIRE(registry.Register(handle, "k1"));
    REQUIRE(registry.HasAnyWatchers());

    REQUIRE(registry.Register(handle, "k2"));
    REQUIRE(registry.HasAnyWatchers());

    // Re-Register on an existing (handle, key) pair must NOT double-count
    // the entry — otherwise the counter would drift up over time. The
    // return value flags this (inserted=false on idempotent re-register).
    REQUIRE_FALSE(registry.Register(handle, "k1"));
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

TEST_CASE("WatchHandle::Clear forgets snapshots but does NOT reset dirty", "[protocol][redis][transaction][race]")
{
    // Documents the EXEC-race-fix contract: Clear() wipes the snapshot map
    // ONLY. The `_dirty` atomic is owned exclusively by
    // ClaimAndClearDirty's exchange, so a racing MarkDirty that fires
    // between UnregisterAll's index-erase and Clear() (or this direct call)
    // is preserved, not silently dropped. Were Clear() to wipe _dirty too,
    // the cross-connection mutation that should abort EXEC would be lost.
    WatchHandle handle;
    handle.Remember("foo", 1);
    handle.MarkDirty();
    REQUIRE(handle.IsDirty());

    handle.Clear();
    REQUIRE(handle.IsDirty());             // _dirty preserved
    REQUIRE(handle.WatchedKeys().empty()); // snapshot map cleared
}

TEST_CASE("WatchRegistry::Unregister removes only that key and leaves others intact", "[protocol][redis][transaction]")
{
    // Per-key Unregister is the building block for HandleWatch's
    // partial-failure rollback: a later WATCH that hits a StorageError
    // must roll back its own keys without wiping registrations from
    // earlier successful WATCH calls. UnregisterAll's blast radius is
    // wrong for that path.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    REQUIRE(registry.Register(handle, "a"));
    REQUIRE(registry.Register(handle, "b"));
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
    // Drain the dirty bit so the next assertion can observe a fresh state.
    // (Clear() wipes snapshots only; ClaimAndClearDirty is the sole _dirty
    // resetter under the EXEC-race fix.)
    REQUIRE(handle->ClaimAndClearDirty());
    // `b` is no longer in the registry index — racing SET on `b` is a no-op.
    REQUIRE(registry.Touched("b") == 0);
    REQUIRE_FALSE(handle->IsDirty());
}

TEST_CASE("WatchRegistry::Unregister is a no-op when the key is not registered", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    REQUIRE(registry.Register(handle, "a"));
    REQUIRE(registry.Unregister(handle.get(), "missing") == 0);
    REQUIRE(registry.HasAnyWatchers());
}

TEST_CASE("WatchRegistry::Unregister tolerates a null handle pointer", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    REQUIRE(registry.Unregister(nullptr, "k") == 0);
}

TEST_CASE("WatchHandle::ClaimAndClearDirty returns prior state and resets the bit", "[protocol][redis][transaction]")
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
    // Behavioural assertion: UnregisterAll's handle-side Clear runs
    // OUTSIDE the registry mutex (so a future caller taking handle->_mu
    // before registry._mu cannot deadlock), and the snapshot wipe is
    // complete by the time UnregisterAll returns. Clear no longer touches
    // _dirty (that's the EXEC-race fix — owned by ClaimAndClearDirty), so
    // we only observe the snapshot side here.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    REQUIRE(registry.Register(handle, "k"));
    handle->Remember("k", 1);

    registry.UnregisterAll(handle.get());

    // Index entry is gone; snapshots are wiped. _dirty is untouched
    // because no Touched has fired.
    REQUIRE_FALSE(handle->IsDirty());
    REQUIRE(handle->WatchedKeys().empty());
    REQUIRE_FALSE(registry.HasAnyWatchers());
}

TEST_CASE("WatchRegistry: a MarkDirty that lands BETWEEN UnregisterAll's index-erase and "
          "ClaimAndClearDirty is observed by THIS EXEC, not silently wiped",
          "[protocol][redis][transaction][race]")
{
    // The fix for the EXEC race (finding #1). Pre-fix sequence:
    //   1. Reactor B's Touched(k) takes _mu, snapshots strong_ptr of A's
    //      handle, releases _mu, calls MarkDirty (_dirty=true).
    //   2. Reactor A's UnregisterAll takes _mu, erases the index entry,
    //      releases _mu, then calls handle->Clear() which used to store
    //      _dirty=false — silently wiping B's MarkDirty.
    //   3. A's ClaimAndClearDirty exchanges false→false and returns false.
    //   4. EXEC commits over B's racing mutation.
    //
    // Post-fix: Clear() no longer touches _dirty. Step 2's Clear wipes
    // only the snapshot map; the MarkDirty from step 1 survives. Step 3's
    // exchange observes _dirty=true and watchTripped=true; EXEC replies
    // *-1 (aborted) as required.
    //
    // Model the race in deterministic order: pre-set _dirty (B's MarkDirty
    // already happened), then run UnregisterAll, then read via
    // ClaimAndClearDirty. The dirty bit must survive.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();
    REQUIRE(registry.Register(handle, "k"));
    handle->Remember("k", 1);

    // Step 1: B's MarkDirty has already fired.
    handle->MarkDirty();
    REQUIRE(handle->IsDirty());

    // Step 2: A's HandleExec runs UnregisterAll.
    registry.UnregisterAll(handle.get());

    // CRITICAL POST-FIX INVARIANT: _dirty must still be true here. If
    // Clear() were to wipe it, this assertion would fail and B's signal
    // would be lost.
    REQUIRE(handle->IsDirty());

    // Step 3: A's ClaimAndClearDirty exchanges and observes true.
    REQUIRE(handle->ClaimAndClearDirty());
    REQUIRE_FALSE(handle->IsDirty());

    // The contract is asymmetric — a Touched that lands AFTER the
    // exchange (i.e. POST-EXEC) is collected by the NEXT MULTI's drain
    // (HandleMulti's ClaimAndClearDirty), tested separately below.
}

TEST_CASE("WatchRegistry: a MarkDirty that lands AFTER ClaimAndClearDirty is drained by the next "
          "MULTI's claim, not leaked into a spurious abort",
          "[protocol][redis][transaction][race]")
{
    // Finding #9: the residual race. A Touched fires AFTER the previous
    // EXEC's ClaimAndClearDirty exchange — _dirty stays true on the
    // reused handle. The next MULTI on the same connection MUST drain
    // that stale bit (HandleMulti now calls ClaimAndClearDirty on entry),
    // or else the next EXEC would abort spuriously despite no actual
    // race on the newly WATCHed key.
    WatchHandle handle;

    // Prior cycle's EXEC exchange already ran (handle is clean).
    REQUIRE_FALSE(handle.IsDirty());

    // A late Touched lands AFTER the exchange — the comment in
    // HandleExec acknowledges this case.
    handle.MarkDirty();
    REQUIRE(handle.IsDirty());

    // Now the connection issues MULTI for a new transaction. Model
    // HandleMulti's drain.
    (void) handle.ClaimAndClearDirty();
    REQUIRE_FALSE(handle.IsDirty());

    // The fresh EXEC (no actual mutation this cycle) sees _dirty=false
    // and commits correctly.
    REQUIRE_FALSE(handle.ClaimAndClearDirty());
}

TEST_CASE("WatchRegistry::Register returns false on idempotent re-register; true on fresh insert",
          "[protocol][redis][transaction]")
{
    // Finding #5: HandleWatch's partial-rollback path must only Unregister
    // keys it FRESHLY inserted, not keys re-registered from an earlier
    // WATCH call. The return value of Register drives that filter.
    WatchRegistry registry;
    auto handle = std::make_shared<WatchHandle>();

    REQUIRE(registry.Register(handle, "a"));       // fresh insert
    REQUIRE_FALSE(registry.Register(handle, "a")); // idempotent re-register
    REQUIRE(registry.Register(handle, "b"));       // fresh insert (different key)

    // Counter must not double-count the re-register.
    REQUIRE(registry.HasAnyWatchers());
    registry.UnregisterAll(handle.get());
    REQUIRE_FALSE(registry.HasAnyWatchers());
}

TEST_CASE("WatchRegistry::Register tolerates a null shared_ptr handle", "[protocol][redis][transaction]")
{
    WatchRegistry registry;
    std::shared_ptr<WatchHandle> null;
    REQUIRE_FALSE(registry.Register(null, "k"));
    REQUIRE_FALSE(registry.HasAnyWatchers());
}

TEST_CASE("WatchRegistry::TouchedAll dirties every registered handle for FLUSHDB", "[protocol][redis][transaction][flushdb]")
{
    // Finding #6: FLUSHDB / FLUSHALL invalidate every WATCH'd key on
    // every connection. Per-key Touched does not fit (no single key
    // identifies the wipe), so a database-wide TouchedAll fan-out is
    // required. The HandleFlush handler calls this; here we exercise
    // the primitive directly.
    WatchRegistry registry;
    auto a = std::make_shared<WatchHandle>();
    auto b = std::make_shared<WatchHandle>();
    auto c = std::make_shared<WatchHandle>();
    REQUIRE(registry.Register(a, "a"));
    REQUIRE(registry.Register(b, "b"));
    // c watches multiple keys; TouchedAll must dirty it exactly once
    // (the dedup-by-handle invariant).
    REQUIRE(registry.Register(c, "c1"));
    REQUIRE(registry.Register(c, "c2"));

    REQUIRE_FALSE(a->IsDirty());
    REQUIRE_FALSE(b->IsDirty());
    REQUIRE_FALSE(c->IsDirty());

    auto const dirtied = registry.TouchedAll();
    REQUIRE(dirtied == 3);
    REQUIRE(a->IsDirty());
    REQUIRE(b->IsDirty());
    REQUIRE(c->IsDirty());
}

TEST_CASE("WatchRegistry::TouchedAll uses the lock-free fast path when nothing is watched",
          "[protocol][redis][transaction][flushdb][fast-path]")
{
    // Steady-state cost on a daemon with no transactions in flight is a
    // single atomic load and an immediate return — the empty-registry
    // FLUSHDB path must not enter the locked section.
    WatchRegistry registry;
    REQUIRE_FALSE(registry.HasAnyWatchers());
    REQUIRE(registry.TouchedAll() == 0);
}

TEST_CASE("WatchRegistry::TouchedAll skips expired weak_ptrs without crashing", "[protocol][redis][transaction][flushdb]")
{
    WatchRegistry registry;
    {
        auto handle = std::make_shared<WatchHandle>();
        REQUIRE(registry.Register(handle, "k"));
        // Handle expires; index still holds a weak_ptr that the upgrade
        // inside TouchedAll must handle gracefully.
    }
    REQUIRE(registry.TouchedAll() == 0);
}
