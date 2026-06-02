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
