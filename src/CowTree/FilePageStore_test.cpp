// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/FilePageStore.hpp>

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

/// Allocate a fresh tmp path that gets cleaned up by the destructor.
struct TempFile
{
    std::filesystem::path path;

    TempFile()
    {
        std::mt19937_64 rng { std::random_device {}() };
        path = std::filesystem::temp_directory_path() / ("cowtree-test-" + std::to_string(rng()) + ".cow");
        std::filesystem::remove(path);
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempFile(TempFile const&) = delete;
    TempFile& operator=(TempFile const&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
};

} // namespace

TEST_CASE("FilePageStore opens a fresh file and reports the page size", "[filestore]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.pageSize = 4096;

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());
    REQUIRE((*store)->PageSize() == 4096U);
}

TEST_CASE("Allocated page survives close + reopen", "[filestore][persist]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.pageSize = 4096;

    {
        auto store = CowTree::FilePageStore::Open(opts);
        REQUIRE(store.has_value());
        CowTree::CowTree tree { **store };
        REQUIRE(tree.Open().has_value());

        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B("persist"), B("yes")).has_value());
        REQUIRE(txn.Commit().has_value());
    }

    {
        auto store = CowTree::FilePageStore::Open(opts);
        REQUIRE(store.has_value());
        CowTree::CowTree tree { **store };
        REQUIRE(tree.Open().has_value());

        auto r = tree.BeginRead();
        auto got = r.Get(B("persist"));
        REQUIRE(got.has_value());
        REQUIRE(got->has_value());
        REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == "yes");
    }
}

TEST_CASE("Durability=Fsync issues fsync calls per write", "[filestore][durability]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.durability = CowTree::FilePageStore::Durability::Fsync;

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());

    auto const before = (*store)->FsyncCallCount();
    CowTree::CowTree tree { **store };
    REQUIRE(tree.Open().has_value());
    auto txn = tree.BeginWrite();
    REQUIRE(txn.Put(B("k"), B("v")).has_value());
    REQUIRE(txn.Commit().has_value());

    REQUIRE((*store)->FsyncCallCount() > before);
}

TEST_CASE("Durability=None makes no fsync calls during writes", "[filestore][durability]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.durability = CowTree::FilePageStore::Durability::None;

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());

    // The fresh-file bootstrap issues one fsync; reset the comparison from there.
    auto const baseline = (*store)->FsyncCallCount();

    CowTree::CowTree tree { **store };
    REQUIRE(tree.Open().has_value());
    auto txn = tree.BeginWrite();
    REQUIRE(txn.Put(B("k"), B("v")).has_value());
    REQUIRE(txn.Commit().has_value());

    REQUIRE((*store)->FsyncCallCount() == baseline);
}

TEST_CASE("Durability=Batched coalesces fsyncs across commits (group commit)", "[filestore][durability][batched]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.durability = CowTree::FilePageStore::Durability::Batched;

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());
    auto const baseline = (*store)->FsyncCallCount();

    CowTree::CowTree tree { **store };
    REQUIRE(tree.Open().has_value());
    for (int i = 0; i < 10; ++i) // fewer than the group-commit flush interval
    {
        auto const key = "k" + std::to_string(i);
        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B(key), B("v")).has_value());
        REQUIRE(txn.Commit().has_value());
    }
    // No fsync yet: Batched defers it to the flush boundary.
    REQUIRE((*store)->FsyncCallCount() == baseline);
}

TEST_CASE("Durability=Batched defers freed-page reuse until the flush boundary", "[filestore][durability][batched]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.pageSize = 4096;
    opts.durability = CowTree::FilePageStore::Durability::Batched;

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());
    auto& pages = **store;
    std::vector<std::byte> const page(4096, std::byte { 0 });

    auto p1 = pages.Allocate();
    REQUIRE(p1.has_value());
    REQUIRE(pages.Write(*p1, CowTree::BytesView { page.data(), page.size() }).has_value());
    REQUIRE(pages.Free(*p1).has_value());

    // p1 is pending (its freeing isn't durable yet), so the next Allocate must
    // NOT hand it back — it would let a crash-rollback read an overwritten page.
    auto p2 = pages.Allocate();
    REQUIRE(p2.has_value());
    REQUIRE(p2->value != p1->value);

    // Cross the flush interval (64 meta writes) so the freeing becomes durable
    // and the pending page graduates to the reusable free list.
    CowTree::Meta meta;
    for (std::uint64_t i = 1; i <= 64; ++i)
    {
        meta.txnId = i;
        REQUIRE(pages.WriteMeta((i % 2 == 0) ? CowTree::MetaSlot::A : CowTree::MetaSlot::B, meta).has_value());
    }

    // Now the previously-freed page is reusable.
    auto p3 = pages.Allocate();
    REQUIRE(p3.has_value());
    REQUIRE(p3->value == p1->value);
}

TEST_CASE("Durability=Batched flushes buffered writes on graceful close", "[filestore][durability][batched]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.durability = CowTree::FilePageStore::Durability::Batched;

    {
        auto store = CowTree::FilePageStore::Open(opts);
        REQUIRE(store.has_value());
        CowTree::CowTree tree { **store };
        REQUIRE(tree.Open().has_value());
        for (int i = 0; i < 5; ++i) // fewer than the flush interval -> only the dtor flush persists these
        {
            auto const key = "k" + std::to_string(i);
            auto txn = tree.BeginWrite();
            REQUIRE(txn.Put(B(key), B("value")).has_value());
            REQUIRE(txn.Commit().has_value());
        }
    } // FilePageStore destructor flushes the buffered batch

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());
    CowTree::CowTree tree { **store };
    REQUIRE(tree.Open().has_value());
    auto reader = tree.BeginRead();
    auto got = reader.Get(B("k3"));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == "value");
}
