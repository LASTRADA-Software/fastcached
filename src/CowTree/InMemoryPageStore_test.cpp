// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/InMemoryPageStore.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

namespace
{

std::vector<std::byte> MakePage(std::size_t size, std::byte fill)
{
    std::vector<std::byte> page(size, fill);
    return page;
}

} // namespace

TEST_CASE("InMemoryPageStore reports configured page size", "[pagestore]")
{
    CowTree::InMemoryPageStore store { 4096 };
    REQUIRE(store.PageSize() == 4096U);
}

TEST_CASE("Allocate yields unique, non-zero PageIds", "[pagestore]")
{
    CowTree::InMemoryPageStore store;
    auto p1 = store.Allocate();
    auto p2 = store.Allocate();
    REQUIRE(p1.has_value());
    REQUIRE(p2.has_value());
    REQUIRE(p1->value != 0);
    REQUIRE(p2->value != 0);
    REQUIRE(*p1 != *p2);
}

TEST_CASE("Read returns the bytes most recently written", "[pagestore]")
{
    CowTree::InMemoryPageStore store;
    auto id = store.Allocate();
    REQUIRE(id.has_value());

    auto page = MakePage(store.PageSize(), std::byte { 0xAA });
    auto wrote = store.Write(*id, { page.data(), page.size() });
    REQUIRE(wrote.has_value());

    auto view = store.Read(*id);
    REQUIRE(view.has_value());
    REQUIRE(view->size() == store.PageSize());
    REQUIRE(view->front() == std::byte { 0xAA });
}

TEST_CASE("Free recycles the id on next Allocate", "[pagestore]")
{
    CowTree::InMemoryPageStore store;
    auto id = store.Allocate();
    REQUIRE(id.has_value());
    REQUIRE(store.Free(*id).has_value());
    auto next = store.Allocate();
    REQUIRE(next.has_value());
    REQUIRE(*next == *id);
}

TEST_CASE("Read of a freed page reports OutOfRange", "[pagestore]")
{
    CowTree::InMemoryPageStore store;
    auto id = store.Allocate();
    REQUIRE(id.has_value());
    REQUIRE(store.Free(*id).has_value());
    auto view = store.Read(*id);
    REQUIRE_FALSE(view.has_value());
    REQUIRE(view.error() == CowTree::CowTreeError::OutOfRange);
}

TEST_CASE("Write of wrong size reports InvalidArg", "[pagestore]")
{
    CowTree::InMemoryPageStore store;
    auto id = store.Allocate();
    REQUIRE(id.has_value());
    auto undersized = MakePage(store.PageSize() / 2, std::byte { 0 });
    auto wrote = store.Write(*id, { undersized.data(), undersized.size() });
    REQUIRE_FALSE(wrote.has_value());
    REQUIRE(wrote.error() == CowTree::CowTreeError::InvalidArg);
}

TEST_CASE("ReadMeta on a fresh store returns a blank, valid meta", "[pagestore][meta]")
{
    CowTree::InMemoryPageStore store;
    auto meta = store.ReadMeta(CowTree::MetaSlot::A);
    REQUIRE(meta.has_value());
    REQUIRE(meta->txnId == 0U);
    REQUIRE(meta->root.value == 0U);
    REQUIRE(meta->itemCount == 0U);
}

TEST_CASE("WriteMeta+ReadMeta round-trip", "[pagestore][meta]")
{
    CowTree::InMemoryPageStore store;
    CowTree::Meta m;
    m.pageSize = static_cast<std::uint32_t>(store.PageSize());
    m.txnId = 17;
    m.root = CowTree::PageId { 3 };
    REQUIRE(store.WriteMeta(m).has_value());

    // Read back from the slot the STORE says it wrote, rather than from one this
    // case picked: since #726 the alternation is the store's and `WriteMeta`
    // takes no slot, so a hard-coded `B` here would be asserting a choice the
    // caller no longer makes. `A` is the seeded durable slot, so this lands in B
    // -- but that is derived, not assumed.
    REQUIRE(store.LastDurableSlot() == CowTree::MetaSlot::B);
    auto loaded = store.ReadMeta(store.LastDurableSlot());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->txnId == 17U);
    REQUIRE(loaded->root.value == 3U);

    // And the alternation itself, which nothing asserted before: a second write
    // must not land on the one just made durable.
    m.txnId = 18;
    REQUIRE(store.WriteMeta(m).has_value());
    REQUIRE(store.LastDurableSlot() == CowTree::MetaSlot::A);
    auto const kept = store.ReadMeta(CowTree::MetaSlot::B);
    REQUIRE(kept.has_value());
    REQUIRE(kept->txnId == 17U);
}

TEST_CASE("FailNthWrite injects exactly once", "[pagestore][fault]")
{
    CowTree::InMemoryPageStore store;
    auto id = store.Allocate();
    REQUIRE(id.has_value());

    CowTree::InMemoryPageStore::FaultPlan plan;
    plan.failNthWrite = 2; // 2nd Write fails
    store.SetFaultPlan(plan);

    auto page = MakePage(store.PageSize(), std::byte { 0 });
    REQUIRE(store.Write(*id, { page.data(), page.size() }).has_value());

    auto second = store.Write(*id, { page.data(), page.size() });
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error() == CowTree::CowTreeError::InjectedFault);

    // Counter cleared; subsequent writes succeed.
    REQUIRE(store.Write(*id, { page.data(), page.size() }).has_value());
}

TEST_CASE("TornOnNthWriteMeta zeroes the slot, then fails", "[pagestore][fault]")
{
    CowTree::InMemoryPageStore store;

    CowTree::Meta m;
    m.pageSize = static_cast<std::uint32_t>(store.PageSize());
    m.txnId = 1;
    REQUIRE(store.WriteMeta(m).has_value());
    // Lands in B, the store having been seeded with A durable. Read back rather
    // than assumed, because everything below is about which slot the torn write
    // then takes.
    REQUIRE(store.LastDurableSlot() == CowTree::MetaSlot::B);

    // The 1st WriteMeta succeeded already; arm a torn-write on the next one.
    CowTree::InMemoryPageStore::FaultPlan plan;
    plan.tornOnNthWriteMeta = 2;
    store.SetFaultPlan(plan);

    m.txnId = 2;
    auto torn = store.WriteMeta(m);
    REQUIRE_FALSE(torn.has_value());

    // The torn slot is unreadable (CRC failure) while the other survives.
    auto a = store.ReadMeta(CowTree::MetaSlot::A);
    auto b = store.ReadMeta(CowTree::MetaSlot::B);
    REQUIRE_FALSE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(b->txnId == 1U);

    // And a failed write does not advance the alternation, so the NEXT attempt
    // retries the slot that was torn instead of stepping over the intact one --
    // which is the whole reason the previous meta is still there to fall back to.
    REQUIRE(store.LastDurableSlot() == CowTree::MetaSlot::B);
    m.txnId = 3;
    REQUIRE(store.WriteMeta(m).has_value());
    REQUIRE(store.LastDurableSlot() == CowTree::MetaSlot::A);
}

TEST_CASE("SyncData fault injection", "[pagestore][fault]")
{
    CowTree::InMemoryPageStore store;
    CowTree::InMemoryPageStore::FaultPlan plan;
    plan.failNthSyncData = 1;
    store.SetFaultPlan(plan);

    auto r = store.SyncData();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == CowTree::CowTreeError::InjectedFault);

    // Subsequent syncs succeed.
    REQUIRE(store.SyncData().has_value());
}
