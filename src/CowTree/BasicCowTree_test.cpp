// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/InMemoryPageStore.hpp>
#include <CowTree/PageId.hpp>
#include <CowTree/PageLayout.hpp>

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
    REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == "bar");
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
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == "v2");
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
        REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == std::format("val-{:04d}", i));
    }
}

TEST_CASE("Random workload tracks std::map oracle", "[cowtree][fuzz]")
{
    CowTree::InMemoryPageStore store { 512 };
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    std::map<std::string, std::string> oracle;
    // Deterministic seed via seed_seq so the fuzz workload reproduces
    // verbatim across runs; bugprone-random-generator-seed only flags
    // direct literal seeding of the engine.
    std::seed_seq seed { 0xC0DEC0DEU, 0xDEADBEEFU, 0xFEEDFACEU, 0xCAFEBABEU };
    std::mt19937 rng { seed };
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
                REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == v);
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
        REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == v);
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
    REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == "v");
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
    REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == "committed");
}

// ============================================================================
// ForEach — the whole-keyspace scan
// ============================================================================

TEST_CASE("ForEach over an empty tree visits nothing and succeeds", "[cowtree][foreach]")
{
    CowTree::InMemoryPageStore store;
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    auto visited = 0;
    auto reader = tree.BeginRead();
    REQUIRE(reader
                .ForEach([&](CowTree::BytesView, CowTree::BytesView) {
                    ++visited;
                    return true;
                })
                .has_value());
    REQUIRE(visited == 0);
}

TEST_CASE("ForEach visits every entry of a single-leaf tree in key order", "[cowtree][foreach]")
{
    CowTree::InMemoryPageStore store;
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    // Inserted out of order on purpose: the walk must impose key order rather
    // than reproduce insertion order.
    PutCommit(tree, "delta", "4");
    PutCommit(tree, "alpha", "1");
    PutCommit(tree, "charlie", "3");
    PutCommit(tree, "bravo", "2");

    std::vector<std::pair<std::string, std::string>> seen;
    auto reader = tree.BeginRead();
    REQUIRE(reader
                .ForEach([&](CowTree::BytesView k, CowTree::BytesView v) {
                    seen.emplace_back(CowTree::AsStringView(k), CowTree::AsStringView(v));
                    return true;
                })
                .has_value());

    auto const expected = std::vector<std::pair<std::string, std::string>> {
        { "alpha", "1" }, { "bravo", "2" }, { "charlie", "3" }, { "delta", "4" }
    };
    REQUIRE(seen == expected);
}

TEST_CASE("ForEach visits every entry of a multi-level tree exactly once, in key order", "[cowtree][foreach]")
{
    // Small pages plus enough entries to force splits, so the walk has to
    // descend internal pages rather than read one leaf. A tree that never split
    // would let a walk that ignores `firstChild` pass.
    CowTree::InMemoryPageStore store { CowTree::MinPageSize };
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    std::map<std::string, std::string> written;
    for (auto const i: std::views::iota(0, 400))
    {
        auto const key = std::format("key-{:04d}", i);
        auto const value = std::format("value-{}", i);
        PutCommit(tree, key, value);
        written.emplace(key, value);
    }

    std::vector<std::pair<std::string, std::string>> seen;
    auto reader = tree.BeginRead();
    REQUIRE(reader
                .ForEach([&](CowTree::BytesView k, CowTree::BytesView v) {
                    seen.emplace_back(CowTree::AsStringView(k), CowTree::AsStringView(v));
                    return true;
                })
                .has_value());

    REQUIRE(seen.size() == written.size());
    REQUIRE(std::ranges::is_sorted(seen, {}, &std::pair<std::string, std::string>::first));
    auto const expected = std::vector<std::pair<std::string, std::string>> { written.begin(), written.end() };
    REQUIRE(seen == expected);
}

TEST_CASE("ForEach stops when the callback asks it to, and calls that success", "[cowtree][foreach]")
{
    CowTree::InMemoryPageStore store { CowTree::MinPageSize };
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());

    for (auto const i: std::views::iota(0, 200))
        PutCommit(tree, std::format("key-{:04d}", i), "v");

    // Stopping early is a caller's decision, not a failure: a scan looking for
    // one thing must not have to read the whole store to report that it found
    // it.
    std::vector<std::string> seen;
    auto reader = tree.BeginRead();
    REQUIRE(reader
                .ForEach([&](CowTree::BytesView k, CowTree::BytesView) {
                    seen.emplace_back(CowTree::AsStringView(k));
                    return seen.size() < 3;
                })
                .has_value());
    REQUIRE(seen == std::vector<std::string> { "key-0000", "key-0001", "key-0002" });
}

TEST_CASE("ForEach refuses a tree whose child pointer loops back on itself", "[cowtree][foreach]")
{
    // A cycle is the one malformed shape a per-page CRC cannot catch: every
    // page around the loop is individually valid, so only a budget stops the
    // walk. Without one it spins until the pending stack exhausts memory.
    CowTree::InMemoryPageStore store { CowTree::MinPageSize };
    CowTree::CowTree tree { store };
    REQUIRE(tree.Open().has_value());
    for (auto const i: std::views::iota(0, 400))
        PutCommit(tree, std::format("key-{:04d}", i), "v");

    auto const root = tree.BeginRead().Root();
    std::vector<std::byte> page;
    {
        auto const view = store.Read(root);
        REQUIRE(view.has_value());
        page.assign(view->begin(), view->end());
    }

    auto header = CowTree::DecodePageHeader(page);
    REQUIRE(header.has_value());
    REQUIRE(header->type == CowTree::PageType::Internal);

    // Point the leftmost child back at the page itself, then re-stamp the
    // header so the CRC agrees with the lie.
    header->firstChild = root;
    REQUIRE(CowTree::EncodePageHeader(std::span { page }, *header).has_value());
    REQUIRE(store.Write(root, CowTree::BytesView { page.data(), page.size() }).has_value());

    // The rewritten page must still pass its own CRC, or the refusal below
    // would be DecodePageHeader rejecting a botched edit rather than the budget
    // catching the cycle -- the same error for entirely the wrong reason.
    {
        auto const rewritten = store.Read(root);
        REQUIRE(rewritten.has_value());
        REQUIRE(CowTree::DecodePageHeader(*rewritten).has_value());
    }

    auto const walked = tree.BeginRead().ForEach([](CowTree::BytesView, CowTree::BytesView) { return true; });
    REQUIRE_FALSE(walked.has_value());
    REQUIRE(walked.error() == CowTree::CowTreeError::Corrupt);
}

TEST_CASE("ForEach on a default-constructed read transaction reports NotOpen", "[cowtree][foreach]")
{
    CowTree::ReadTxn const reader;
    auto const r = reader.ForEach([](CowTree::BytesView, CowTree::BytesView) { return true; });
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == CowTree::CowTreeError::NotOpen);
}
