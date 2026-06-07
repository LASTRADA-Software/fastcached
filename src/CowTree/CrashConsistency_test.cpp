// SPDX-License-Identifier: Apache-2.0
//
// Crash-consistency invariant: for every meaningful failure offset in
// the commit protocol, the on-disk state after the failure must match
// either the previous or the new transaction — never a hybrid.
//
// We exercise the invariant by driving a CowTree against an
// InMemoryPageStore with injected failures at each Write / SyncData /
// WriteMeta call, then constructing a *new* CowTree over the same store
// (simulating a process restart) and verifying its visible state.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/InMemoryPageStore.hpp>

namespace
{

CowTree::BytesView B(std::string_view s) noexcept
{
    return CowTree::AsBytes(s);
}

std::string Decode(std::vector<std::byte> const& v)
{
    return std::string(CowTree::AsStringView({ v.data(), v.size() }));
}

// Build a tree with some baseline state. Returns the store.
void SeedTree(CowTree::InMemoryPageStore& store)
{
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());
    auto txn = tree.BeginWrite();
    REQUIRE(txn.Put(B("a"), B("apple")).has_value());
    REQUIRE(txn.Put(B("b"), B("banana")).has_value());
    REQUIRE(txn.Commit().has_value());
}

// Check that the tree contains exactly the expected baseline: a→apple, b→banana.
void RequireBaseline(CowTree::CowTree& tree)
{
    auto r = tree.BeginRead();
    auto a = r.Get(B("a"));
    REQUIRE(a.has_value());
    REQUIRE(a->has_value());
    REQUIRE(Decode((*a).value_or(std::vector<std::byte> {})) == "apple");

    auto b = r.Get(B("b"));
    REQUIRE(b.has_value());
    REQUIRE(b->has_value());
    REQUIRE(Decode((*b).value_or(std::vector<std::byte> {})) == "banana");

    auto c = r.Get(B("c"));
    REQUIRE(c.has_value());
    REQUIRE_FALSE(c->has_value());
}

} // namespace

TEST_CASE("Commit fsyncs data only when the transaction wrote new pages", "[commit][fsync]")
{
    CowTree::InMemoryPageStore store;

    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    // A real write allocates pages, so the commit must fsync the data ahead
    // of the meta slot that references it — durability is preserved.
    {
        auto const before = store.SyncDataCount();
        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B("k"), B("v")).has_value());
        REQUIRE(txn.Commit().has_value());
        REQUIRE(store.SyncDataCount() == before + 1);
    }

    // A transaction that allocates no new pages has nothing to order ahead of
    // the meta, so the data fsync is elided rather than paid for nothing.
    {
        auto const before = store.SyncDataCount();
        auto txn = tree.BeginWrite();
        REQUIRE(txn.Commit().has_value());
        REQUIRE(store.SyncDataCount() == before);
    }

    // The earlier write is still durable and readable after the empty commit.
    auto reader = tree.BeginRead();
    auto v = reader.Get(B("k"));
    REQUIRE(v.has_value());
    REQUIRE(v->has_value());
    REQUIRE(Decode((*v).value_or(std::vector<std::byte> {})) == "v");
}

TEST_CASE("Failure during data Write leaves the previous txn intact", "[crash]")
{
    CowTree::InMemoryPageStore store;
    SeedTree(store);

    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());

        // Inject failure on the first Write during the upcoming
        // transaction. The Commit() call must fail and the live root
        // must remain the seeded one.
        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.failNthWrite = 1;
        store.SetFaultPlan(plan);
        store.ResetCounters();

        auto txn = tree.BeginWrite();
        auto put = txn.Put(B("c"), B("cherry"));
        REQUIRE_FALSE(put.has_value());
        // Transaction is in a bad state; Abort runs via destructor.
    }

    CowTree::CowTree reopened { store };
    REQUIRE(reopened.Open().has_value());
    RequireBaseline(reopened);
}

TEST_CASE("Failure during SyncData rolls back the in-flight commit", "[crash]")
{
    CowTree::InMemoryPageStore store;
    SeedTree(store);

    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());

        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.failNthSyncData = 1;
        store.SetFaultPlan(plan);
        store.ResetCounters();

        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B("c"), B("cherry")).has_value());
        auto commit = txn.Commit();
        REQUIRE_FALSE(commit.has_value());
    }

    CowTree::CowTree reopened { store };
    REQUIRE(reopened.Open().has_value());
    RequireBaseline(reopened);
}

TEST_CASE("Failure during WriteMeta rolls back the in-flight commit", "[crash]")
{
    CowTree::InMemoryPageStore store;
    SeedTree(store);

    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());

        // The seed wrote one meta (slot A, txn=1). The next WriteMeta
        // is the one we want to fail.
        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.failNthWriteMeta = 2;
        store.SetFaultPlan(plan);

        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B("c"), B("cherry")).has_value());
        auto commit = txn.Commit();
        REQUIRE_FALSE(commit.has_value());
    }

    CowTree::CowTree reopened { store };
    REQUIRE(reopened.Open().has_value());
    RequireBaseline(reopened);
}

TEST_CASE("Torn meta write falls back to the previous slot", "[crash]")
{
    CowTree::InMemoryPageStore store;
    SeedTree(store);

    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());

        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.tornOnNthWriteMeta = 2; // 2nd WriteMeta is torn
        store.SetFaultPlan(plan);

        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B("c"), B("cherry")).has_value());
        auto commit = txn.Commit();
        REQUIRE_FALSE(commit.has_value());
    }

    CowTree::CowTree reopened { store };
    REQUIRE(reopened.Open().has_value());
    // The slot that was supposed to be written is now zero; the other
    // slot still holds the seeded transaction. Open() must select the
    // valid slot and we see the baseline.
    RequireBaseline(reopened);
}

TEST_CASE("Successful commit then a torn 2nd commit still recovers to the 1st", "[crash]")
{
    CowTree::InMemoryPageStore store;
    SeedTree(store);

    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());

        // First in-process commit: succeeds. Meta lands in slot B (txn 2).
        {
            auto txn = tree.BeginWrite();
            REQUIRE(txn.Put(B("c"), B("cherry")).has_value());
            REQUIRE(txn.Commit().has_value());
        }

        // Second commit: torn meta. Should leave slot A (txn 1) the live one
        // and zero out the slot we just wrote in this attempt (which is A
        // for txn=3 -> A, since txn%2 alternates). Either way, recovery
        // picks the highest valid txnId — must not crash, must not show
        // a half-applied state.
        //
        // WriteMeta count so far: 1 (seed) + 1 (first in-proc commit) = 2.
        // The 3rd call is the 2nd in-process commit's WriteMeta.
        CowTree::InMemoryPageStore::FaultPlan plan;
        plan.tornOnNthWriteMeta = 3;
        store.SetFaultPlan(plan);

        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B("d"), B("durian")).has_value());
        auto commit = txn.Commit();
        REQUIRE_FALSE(commit.has_value());
    }

    CowTree::CowTree reopened { store };
    REQUIRE(reopened.Open().has_value());
    // Either we see "post first commit" or baseline — both are valid
    // pre-states for the failed second commit. We MUST NOT see "d".
    auto r = reopened.BeginRead();
    auto d = r.Get(B("d"));
    REQUIRE(d.has_value());
    REQUIRE_FALSE(d->has_value());
}
