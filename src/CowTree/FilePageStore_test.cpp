// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/FilePageStore.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

// The lock-classifier case below asserts over the platform's own error
// constants: `errno` values on POSIX, `GetLastError()` values on Windows.
#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>

    #include <cerrno>

    #include <fcntl.h>
#endif

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

/// Page size the free-list cases below build their store with.
constexpr std::size_t FreeListPageSize = 4096;

/// Data pages the free-list fixture allocates: 1 and 2 become the chain, 3
/// stays live so a walk that erased too much is visible as well as one that
/// erased too little.
constexpr std::uint64_t FreeListFixturePages = 3;

/// Store `value` little-endian into the first 8 bytes of `page`.
///
/// Written out rather than memcpy'd because that is what the walk in
/// `RecoverExistingFile` decodes: a raw copy would agree with it on a
/// little-endian host and disagree on every other one.
/// @param page Destination page buffer; must hold at least 8 bytes.
/// @param value Next-page id to encode.
void PutNextLink(std::span<std::byte> page, std::uint64_t value) noexcept
{
    for (auto const i: std::views::iota(std::size_t { 0 }, sizeof(value)))
        page[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
}

/// Build a store at `path` whose durable meta names a two-page free-list
/// chain (page 1 -> page 2 -> end), leaving page 3 live.
///
/// The chain is written through `FilePageStore`'s own public API because
/// nothing else in the tree can produce one: `CowTree::CommitTxn` pins
/// `Meta::freeRoot` to `PageId::None()` ("free list is in-memory only for
/// v1"), so a store written by `CowTreeStorage` never has a chain for
/// `RecoverExistingFile` to walk. See the case comments below.
/// @param path Filesystem path to create the store at.
void SeedFreeListChain(std::filesystem::path const& path)
{
    CowTree::FilePageStore::Options opts;
    opts.path = path;
    opts.pageSize = FreeListPageSize;
    // Fsync rather than Batched: `WriteMeta` must reach the slot this call
    // names instead of being buffered until a flush boundary that never comes.
    opts.durability = CowTree::FilePageStore::Durability::Fsync;

    auto const store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());

    for (auto const expected: std::views::iota(std::uint64_t { 1 }, FreeListFixturePages + 1))
    {
        auto const id = (*store)->Allocate();
        REQUIRE(id.has_value());
        REQUIRE(id->value == expected);
    }

    std::vector<std::byte> page(FreeListPageSize, std::byte { 0 });
    PutNextLink(page, 2);
    REQUIRE((*store)->Write(CowTree::PageId { 1 }, CowTree::BytesView { page.data(), page.size() }).has_value());
    PutNextLink(page, CowTree::PageId::None().value);
    REQUIRE((*store)->Write(CowTree::PageId { 2 }, CowTree::BytesView { page.data(), page.size() }).has_value());

    CowTree::Meta meta;
    meta.pageSize = static_cast<std::uint32_t>(FreeListPageSize);
    meta.txnId = 1;
    meta.root = CowTree::PageId::None();
    meta.freeRoot = CowTree::PageId { 1 };
    meta.itemCount = 0;
    REQUIRE((*store)->WriteMeta(CowTree::MetaSlot::A, meta).has_value());
}

/// Overwrite the 8-byte next-link of data page `id` in a CLOSED store file.
///
/// Damage goes in behind the store's back rather than through `Write`,
/// because the point is a file that a previous process left damaged -- and
/// because opening the store to damage it would run the very walk under test.
/// @param path Store file to damage.
/// @param id 1-based data page whose next-link to overwrite.
/// @param next Raw value to write in its place.
void DamageNextLink(std::filesystem::path const& path, std::uint64_t id, std::uint64_t next)
{
    std::fstream file { path, std::ios::binary | std::ios::in | std::ios::out };
    REQUIRE(file.good());

    std::array<std::byte, 8> encoded {};
    PutNextLink(encoded, next);
    std::array<char, 8> raw {};
    std::ranges::transform(encoded, raw.begin(), [](std::byte b) { return static_cast<char>(b); });

    auto const offset = static_cast<std::streamoff>((2 + (id - 1)) * FreeListPageSize);
    file.seekp(offset);
    file.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    file.flush();
    REQUIRE(file.good());
}

/// Reopen the store at `path` with the fixture's options.
/// @param path Store file to open.
/// @return Whatever `FilePageStore::Open` answered.
auto ReopenSeeded(std::filesystem::path const& path)
{
    CowTree::FilePageStore::Options opts;
    opts.path = path;
    opts.pageSize = FreeListPageSize;
    opts.durability = CowTree::FilePageStore::Durability::Fsync;
    return CowTree::FilePageStore::Open(opts);
}

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

TEST_CASE("Durability=Batched defers meta-slot writes until the flush boundary", "[filestore][durability][batched]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.pageSize = 4096;
    opts.durability = CowTree::FilePageStore::Durability::Batched;

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());
    CowTree::CowTree tree { **store };
    REQUIRE(tree.Open().has_value());

    for (int i = 0; i < 10; ++i) // fewer than the flush interval
    {
        auto const key = "k" + std::to_string(i);
        auto txn = tree.BeginWrite();
        REQUIRE(txn.Put(B(key), B("v")).has_value());
        REQUIRE(txn.Commit().has_value());
    }

    // Crash-safety invariant: intermediate commits never overwrite a meta slot
    // in place, so both slots still hold the bootstrap txnId 0. (The old code
    // pwrote a fresh, un-fsynced txnId to an alternating slot on every commit,
    // so a torn write inside the window could leave NO recoverable slot.)
    auto const a = (*store)->ReadMeta(CowTree::MetaSlot::A);
    auto const b = (*store)->ReadMeta(CowTree::MetaSlot::B);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->txnId == 0U);
    REQUIRE(b->txnId == 0U);
}

TEST_CASE("Durability=Batched: a crash inside the unflushed window reopens consistent",
          "[filestore][durability][batched][crash]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.pageSize = 4096;
    opts.durability = CowTree::FilePageStore::Durability::Batched;

    {
        auto store = CowTree::FilePageStore::Open(opts);
        REQUIRE(store.has_value());
        CowTree::CowTree tree { **store };
        REQUIRE(tree.Open().has_value());
        for (int i = 0; i < 10; ++i) // < flush interval -> nothing durable yet
        {
            auto const key = "k" + std::to_string(i);
            auto txn = tree.BeginWrite();
            REQUIRE(txn.Put(B(key), B("v")).has_value());
            REQUIRE(txn.Commit().has_value());
        }
        (*store)->SimulateCrashForTest(); // hard crash: no flush
    }

    // Reopen MUST succeed (never CorruptMetas) and show the last durable state
    // -- here the empty bootstrap. The window is cleanly lost, not half-applied.
    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());
    CowTree::CowTree tree { **store };
    REQUIRE(tree.Open().has_value());
    auto r = tree.BeginRead();
    auto got = r.Get(B("k0"));
    REQUIRE(got.has_value()); // a clean miss, not an error / corruption
    REQUIRE_FALSE(got->has_value());
}

TEST_CASE("Durability=Batched: commits flushed before a crash survive it", "[filestore][durability][batched][crash]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;
    opts.pageSize = 4096;
    opts.durability = CowTree::FilePageStore::Durability::Batched;

    {
        auto store = CowTree::FilePageStore::Open(opts);
        REQUIRE(store.has_value());
        CowTree::CowTree tree { **store };
        REQUIRE(tree.Open().has_value());
        for (int i = 0; i < 80; ++i) // > flush interval (64): the first window is made durable
        {
            auto const key = "k" + std::to_string(i);
            auto txn = tree.BeginWrite();
            REQUIRE(txn.Put(B(key), B("v")).has_value());
            REQUIRE(txn.Commit().has_value());
        }
        (*store)->SimulateCrashForTest();
    }

    auto store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());
    CowTree::CowTree tree { **store };
    REQUIRE(tree.Open().has_value());
    auto r = tree.BeginRead();
    // A key from the flushed first window survives the crash...
    auto early = r.Get(B("k0"));
    REQUIRE(early.has_value());
    REQUIRE(early->has_value());
    REQUIRE(Decode((*early).value_or(std::vector<std::byte> {})) == "v");
    // ...while a key written only in the unflushed tail is cleanly lost.
    auto tail = r.Get(B("k79"));
    REQUIRE(tail.has_value());
    REQUIRE_FALSE(tail->has_value());
}

// ============================================================================
// Exclusive claim on the backing file
//
// A second store on one path used to be silent data loss on POSIX: both opened,
// both wrote meta pages, and whichever committed last decided what the other
// one's transactions had never happened. Windows refused it, but as a generic
// open failure that named no cause.
//
// These run in ONE process on purpose. `flock` is per open file description
// and a Windows share mode is per handle, so a second `Open` from here meets
// exactly the kernel enforcement a second process would -- and unlike a second
// process, it runs on every platform in CI.
// ============================================================================

TEST_CASE("A second store on one path is refused as InUse", "[filestore][lock]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;

    auto first = CowTree::FilePageStore::Open(opts);
    REQUIRE(first.has_value());
    REQUIRE((*first)->StoreLockState() == CowTree::FilePageStore::LockState::Held);

    auto second = CowTree::FilePageStore::Open(opts);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error() == CowTree::CowTreeError::InUse);
}

TEST_CASE("A store refused as InUse leaves the first one writable", "[filestore][lock]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;

    auto first = CowTree::FilePageStore::Open(opts);
    REQUIRE(first.has_value());
    CowTree::CowTree tree { **first };
    REQUIRE(tree.Open().has_value());

    // The refusal must touch nothing: a guard that costs the incumbent its
    // store would be worse than the corruption it prevents.
    REQUIRE_FALSE(CowTree::FilePageStore::Open(opts).has_value());

    auto txn = tree.BeginWrite();
    REQUIRE(txn.Put(B("survivor"), B("yes")).has_value());
    REQUIRE(txn.Commit().has_value());

    auto reader = tree.BeginRead();
    auto got = reader.Get(B("survivor"));
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE(Decode((*got).value_or(std::vector<std::byte> {})) == "yes");
}

TEST_CASE("Closing a store releases the file for the next one", "[filestore][lock]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;

    {
        auto first = CowTree::FilePageStore::Open(opts);
        REQUIRE(first.has_value());
    }

    // Every restart depends on this, so it is asserted rather than assumed:
    // the claim is released by the close in the destructor, not held to the
    // end of the process.
    auto second = CowTree::FilePageStore::Open(opts);
    REQUIRE(second.has_value());
}

TEST_CASE("A crashed store releases the file", "[filestore][lock][crash]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;

    auto crashed = CowTree::FilePageStore::Open(opts);
    REQUIRE(crashed.has_value());
    (*crashed)->SimulateCrashForTest();

    // What the kernel does for a process that dies: the descriptor goes, and
    // the claim goes with it. A recovering daemon must not have to clear
    // anything by hand -- that is the failure mode a lock FILE would have.
    auto next = CowTree::FilePageStore::Open(opts);
    REQUIRE(next.has_value());
}

TEST_CASE("ClassifyLockFailure maps this platform's lock errors", "[filestore][lock]")
{
    using LockFailure = CowTree::FilePageStore::LockFailure;

    struct Row
    {
        int systemError;
        LockFailure expected;
        char const* what;
    };

    // Asserted as a table because the arms differ per platform and the one
    // that matters most cannot be provoked: no filesystem CI runs on reports
    // "cannot lock", so `Unsupported` would otherwise be reasoning nobody ever
    // checks.
#if defined(_WIN32)
    auto const rows = std::vector<Row> {
        { .systemError = ERROR_SHARING_VIOLATION,
          .expected = LockFailure::Contended,
          .what = "another handle holds the file" },
        { .systemError = ERROR_LOCK_VIOLATION, .expected = LockFailure::Contended, .what = "a byte-range lock refuses us" },
        { .systemError = ERROR_FILE_NOT_FOUND, .expected = LockFailure::Fatal, .what = "the open itself failed" },
        { .systemError = ERROR_ACCESS_DENIED, .expected = LockFailure::Fatal, .what = "no rights to the file" },
    };
#else
    auto const rows = std::vector<Row> {
        { .systemError = EWOULDBLOCK, .expected = LockFailure::Contended, .what = "another descriptor holds the file" },
        { .systemError = EAGAIN, .expected = LockFailure::Contended, .what = "the same condition, other spelling" },
        { .systemError = EBADF, .expected = LockFailure::Fatal, .what = "our own bug, not the filesystem's limit" },
        { .systemError = ENOLCK, .expected = LockFailure::Fatal, .what = "transient, not a capability statement" },
        { .systemError = EINVAL, .expected = LockFailure::Unsupported, .what = "this descriptor cannot be flocked" },
        { .systemError = EOPNOTSUPP, .expected = LockFailure::Unsupported, .what = "the filesystem does not implement it" },
    };
#endif

    for (auto const& row: rows)
    {
        INFO(row.what);
        CHECK(CowTree::FilePageStore::ClassifyLockFailure(row.systemError) == row.expected);
    }
}

TEST_CASE("An empty file that already existed is not blanked into a fresh store", "[filestore][open]")
{
    TempFile tmp;
    {
        std::ofstream created { tmp.path, std::ios::binary };
        REQUIRE(created.good());
    }
    REQUIRE(std::filesystem::file_size(tmp.path) == 0U);

    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;

    // Whether to bootstrap is decided from the pre-open `exists()` AND the
    // length read back under the claim, so this pins the conservative half of
    // that pair: a file the operator put there is never silently initialised
    // just because it happens to be empty. It is refused, exactly as before.
    auto const store = CowTree::FilePageStore::Open(opts);
    REQUIRE_FALSE(store.has_value());
    REQUIRE(store.error() == CowTree::CowTreeError::CorruptMetas);
}

// ---------------------------------------------------------------------------
// The free-list walk at Open.
//
// `docs/operations/corrupt-store.md` tells an operator that opening a store
// "reads the two meta slots, walks the free list, and looks up two reserved
// keys", and that damage anywhere in that reach refuses the process to start
// with `Corrupt`. Two of those three are pinned by `CowTreeStorage_test`
// against an `InMemoryPageStore`; the free-list walk is not reachable from
// there at all, because it lives in `FilePageStore::RecoverExistingFile` and
// has no in-memory equivalent (#580). These three cases are that third one,
// and they need a real file for it.
//
// What they are honest about, so nobody reads more into them: the chain is
// built through `FilePageStore`'s own API because nothing else in this tree
// writes one. `CowTree::CommitTxn` sets `Meta::freeRoot = PageId::None()`
// unconditionally, so a store produced by `CowTreeStorage` -- which is every
// store `fastcached` and `fastcache-compile-node` own -- carries an empty
// chain and the walk terminates on its first test. What these cases pin is
// therefore the guard itself, on the layout `RecoverExistingFile` is written
// to read, and not a refusal an operator can reach today.
// ---------------------------------------------------------------------------

TEST_CASE("An intact free-list chain is walked at Open and its pages are recycled", "[filestore][open][freelist]")
{
    TempFile tmp;
    SeedFreeListChain(tmp.path);

    auto const store = ReopenSeeded(tmp.path);
    REQUIRE(store.has_value());

    // The control direction, and the one that makes the two damage cases below
    // mean anything: an Open that never walked the chain would leave pages 1
    // and 2 live and would also, trivially, never refuse. `Read` rejects a page
    // that is not live, so this says the walk ran and consumed exactly the two
    // pages the chain named.
    REQUIRE((*store)->TotalDataPages() == FreeListFixturePages);
    REQUIRE_FALSE((*store)->Read(CowTree::PageId { 1 }).has_value());
    REQUIRE_FALSE((*store)->Read(CowTree::PageId { 2 }).has_value());
    REQUIRE((*store)->Read(CowTree::PageId { 3 }).has_value());
}

TEST_CASE("A free-list link pointing past the end of the file refuses the store at Open",
          "[filestore][open][freelist][corrupt]")
{
    TempFile tmp;
    SeedFreeListChain(tmp.path);

    // Page 1's next-link, rewritten to a page the file does not have. This is
    // the shape a truncated or partially-rewritten store takes, and it is the
    // one an unguarded walk would follow into a `ReadAt` past the end.
    DamageNextLink(tmp.path, 1, 1'000'000);

    auto const store = ReopenSeeded(tmp.path);
    REQUIRE_FALSE(store.has_value());
    // `Corrupt`, which `CowTreeStorage::TranslateError` carries out to the
    // operator as `StorageErrorCode::Corrupt` with `context=FilePageStore::Open`
    // -- the exact line the operator page quotes. Not `CorruptMetas`: both meta
    // slots read back fine, and it is the structure beneath them that did not
    // hold. Not `OutOfRange` or `IoError` either, both of which say the caller
    // asked for something silly rather than that the store is damaged.
    REQUIRE(store.error() == CowTree::CowTreeError::Corrupt);
}

TEST_CASE("A free-list chain that loops refuses the store at Open", "[filestore][open][freelist][corrupt]")
{
    TempFile tmp;
    SeedFreeListChain(tmp.path);

    // Page 2 links back to page 1. Every id in the loop is in range, so the
    // bounds check above cannot see this one: without the visited set `Open`
    // never returns and `_freeList` grows without bound, which is an unbounded
    // answer rather than a wrong one and is why it is a separate case.
    DamageNextLink(tmp.path, 2, 1);

    auto const store = ReopenSeeded(tmp.path);
    REQUIRE_FALSE(store.has_value());
    REQUIRE(store.error() == CowTree::CowTreeError::Corrupt);
}

#if !defined(_WIN32)
TEST_CASE("The store descriptor does not survive an exec", "[filestore][lock]")
{
    TempFile tmp;
    CowTree::FilePageStore::Options opts;
    opts.path = tmp.path;

    auto const store = CowTree::FilePageStore::Open(opts);
    REQUIRE(store.has_value());

    struct stat target {};
    REQUIRE(::stat(tmp.path.c_str(), &target) == 0);

    // A `flock` lives on the open file description and is released only when
    // the LAST descriptor on it closes, so a copy inherited by a compiler child
    // would keep the store claimed after this process exits -- and the next
    // start would blame a second node that does not exist. Found by scanning
    // rather than by asking the store, because the point is what any inherited
    // copy would look like, not what the member happens to hold.
    bool found = false;
    for (int fd = 0; fd < 256; ++fd)
    {
        struct stat here {};
        if (::fstat(fd, &here) != 0)
            continue;
        if (here.st_dev != target.st_dev || here.st_ino != target.st_ino)
            continue;
        auto const descriptorFlags = ::fcntl(fd, F_GETFD);
        REQUIRE(descriptorFlags != -1);
        CHECK((descriptorFlags & FD_CLOEXEC) != 0);
        found = true;
    }
    // Without this the case passes by finding nothing at all.
    REQUIRE(found);
}
#endif
