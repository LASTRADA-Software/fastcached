// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/InMemoryPageStore.hpp>

namespace
{

CowTree::BytesView B(std::string_view s) noexcept
{
    return CowTree::AsBytes(s);
}

std::string Decode(std::vector<std::byte> const& v) noexcept
{
    return std::string(CowTree::AsStringView(CowTree::BytesView { v.data(), v.size() }));
}

// Convenience: insert (key, value) and commit in one transaction.
void PutCommit(CowTree::CowTree& tree, std::string_view k, std::string_view v)
{
    auto txn = tree.BeginWrite();
    REQUIRE(txn.Put(B(k), B(v)).has_value());
    REQUIRE(txn.Commit().has_value());
}

bool EraseCommit(CowTree::CowTree& tree, std::string_view k)
{
    auto txn = tree.BeginWrite();
    auto r = txn.Erase(B(k));
    REQUIRE(r.has_value());
    REQUIRE(txn.Commit().has_value());
    return *r;
}

} // namespace

TEST_CASE("Open on a fresh page store yields an empty tree", "[cowtree]")
{
    CowTree::InMemoryPageStore store;
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    auto reader = tree.BeginRead();
    auto miss = reader.Get(B("anything"));
    REQUIRE(miss.has_value());
    REQUIRE_FALSE(miss->has_value());
}

TEST_CASE("Put then Get returns the value", "[cowtree]")
{
    CowTree::InMemoryPageStore store;
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    PutCommit(tree, "foo", "bar");

    auto reader = tree.BeginRead();
    auto got = reader.Get(B("foo"));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE(Decode(**got) == "bar");
}

TEST_CASE("Overwriting an existing key replaces the value", "[cowtree]")
{
    CowTree::InMemoryPageStore store;
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    PutCommit(tree, "k", "v1");
    PutCommit(tree, "k", "v2");

    auto reader = tree.BeginRead();
    auto got = reader.Get(B("k"));
    REQUIRE(Decode(**got) == "v2");
}

TEST_CASE("Erase removes a key and reports whether it was present", "[cowtree]")
{
    CowTree::InMemoryPageStore store;
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    PutCommit(tree, "k", "v");
    REQUIRE(EraseCommit(tree, "k") == true);
    REQUIRE(EraseCommit(tree, "k") == false);

    auto reader = tree.BeginRead();
    auto got = reader.Get(B("k"));
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->has_value());
}

TEST_CASE("Many keys span multiple pages and remain accessible", "[cowtree]")
{
    CowTree::InMemoryPageStore store { 512 }; // tiny pages -> guaranteed splits
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    constexpr int N = 200;
    for (int i = 0; i < N; ++i)
    {
        auto k = std::format("key-{:04d}", i);
        auto v = std::format("val-{:04d}", i);
        PutCommit(tree, k, v);
    }

    auto reader = tree.BeginRead();
    for (int i = 0; i < N; ++i)
    {
        auto k = std::format("key-{:04d}", i);
        auto got = reader.Get(B(k));
        REQUIRE(got.has_value());
        REQUIRE(got->has_value());
        REQUIRE(Decode(**got) == std::format("val-{:04d}", i));
    }
}

TEST_CASE("Random workload tracks std::map oracle", "[cowtree][fuzz]")
{
    CowTree::InMemoryPageStore store { 512 };
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    std::map<std::string, std::string> oracle;
    std::mt19937 rng { 0xC0DEC0DE };
    std::uniform_int_distribution<int> opDist { 0, 9 };
    std::uniform_int_distribution<int> keyDist { 0, 99 };

    for (int iter = 0; iter < 2000; ++iter)
    {
        auto const op = opDist(rng);
        auto const key = std::format("k-{:02d}", keyDist(rng));

        if (op < 7) // ~70% writes
        {
            auto const val = std::format("v-{}", iter);
            PutCommit(tree, key, val);
            oracle[key] = val;
        }
        else // ~30% erases
        {
            EraseCommit(tree, key);
            oracle.erase(key);
        }

        if (iter % 200 == 0)
        {
            auto reader = tree.BeginRead();
            for (auto const& [k, v]: oracle)
            {
                auto got = reader.Get(B(k));
                REQUIRE(got.has_value());
                REQUIRE(got->has_value());
                REQUIRE(Decode(**got) == v);
            }
        }
    }

    // Final exhaustive check.
    auto reader = tree.BeginRead();
    for (auto const& [k, v]: oracle)
    {
        auto got = reader.Get(B(k));
        REQUIRE(got.has_value());
        REQUIRE(got->has_value());
        REQUIRE(Decode(**got) == v);
    }
}

TEST_CASE("Reopening the tree finds the last committed root", "[cowtree]")
{
    CowTree::InMemoryPageStore store;
    {
        CowTree::CowTree tree { store };
        REQUIRE(tree.Open().has_value());
        PutCommit(tree, "k", "v");
    }

    CowTree::CowTree reopened { store };
    REQUIRE(reopened.Open().has_value());
    auto reader = reopened.BeginRead();
    auto got = reader.Get(B("k"));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE(Decode(**got) == "v");
}

TEST_CASE("Abort discards in-flight changes", "[cowtree]")
{
    CowTree::InMemoryPageStore store;
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    PutCommit(tree, "k", "committed");

    {
        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B("k"), B("aborted")).has_value());
        // No Commit() — destructor will Abort.
    }

    auto reader = tree.BeginRead();
    auto got = reader.Get(B("k"));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE(Decode(**got) == "committed");
}
