// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

/// Open -- or reopen -- the free-list fixture's store at `path`.
///
/// One options block for both directions, because a seed and a reopen that
/// disagreed about `pageSize` would not fail: `RecoverExistingFile` takes the
/// on-disk value and carries on.
///
/// `Fsync` rather than the default `Batched`, which buffers `WriteMeta` until
/// a flush boundary this fixture never reaches -- and the seeded meta has to
/// be on disk, or the reopen finds no chain to walk.
/// @param path Filesystem path of the store file.
/// @return Whatever `FilePageStore::Open` answered.
auto OpenFreeListStore(std::filesystem::path const& path)
{
    CowTree::FilePageStore::Options opts;
    opts.path = path;
    opts.pageSize = FreeListPageSize;
    opts.durability = CowTree::FilePageStore::Durability::Fsync;
    return CowTree::FilePageStore::Open(opts);
}

/// Build a store at `path` whose durable meta names a free-list chain rooted
/// at page 1, and close it. Page 3 is left live.
///
/// The chain's shape is the caller's, so a case that wants a damaged one says
/// so in data rather than patching bytes into the closed file afterwards --
/// which would mean restating `FilePageStore`'s private data-page offset here,
/// the very thing the neighbouring sweep in `CowTreeStorage_test.cpp` argues a
/// fixture must not do.
///
/// That rule is about the DATA-page offset, which is private. It does not reach
/// the meta slots: `Meta.hpp` documents those offsets as the on-disk contract,
/// and `WriteMeta` recomputes the CRC so damaged meta bytes cannot be expressed
/// through the API at all. The `[meta][corrupt]` case below therefore does patch
/// the closed file, deliberately -- said here as well as there, because a reader
/// who lands on this comment must not carry away the opposite instruction.
///
/// Both pages are written BEFORE the meta that names `freeRoot`, so this store
/// walks nothing while it is open -- the injection route is invisible in the
/// file, which is the same one a patch-the-closed-file route would leave.
///
/// It is NOT a file any writer in this tree produces, and that is deliberate
/// rather than an oversight: `CowTree::CommitTxn` writes
/// `freeRoot = PageId::None()` unconditionally, so no real store has a chain
/// at all. The block comment above the cases carries the whole argument.
/// @param path Filesystem path to create the store at.
/// @param nextOfOne What page 1 chains to.
/// @param nextOfTwo What page 2 chains to.
void SeedFreeListChain(std::filesystem::path const& path, CowTree::PageId nextOfOne, CowTree::PageId nextOfTwo)
{
    auto const store = OpenFreeListStore(path);
    REQUIRE(store.has_value());

    for (auto const expected: std::views::iota(std::uint64_t { 1 }, FreeListFixturePages + 1))
    {
        auto const id = (*store)->Allocate();
        REQUIRE(id.has_value());
        REQUIRE(id->value == expected);
    }

    std::vector<std::byte> page(FreeListPageSize, std::byte { 0 });
    PutNextLink(page, nextOfOne.value);
    REQUIRE((*store)->Write(CowTree::PageId { 1 }, CowTree::BytesView { page.data(), page.size() }).has_value());
    PutNextLink(page, nextOfTwo.value);
    REQUIRE((*store)->Write(CowTree::PageId { 2 }, CowTree::BytesView { page.data(), page.size() }).has_value());

    CowTree::Meta meta;
    meta.pageSize = static_cast<std::uint32_t>(FreeListPageSize);
    meta.txnId = 1;
    meta.root = CowTree::PageId::None();
    meta.freeRoot = CowTree::PageId { 1 };
    meta.itemCount = 0;

    // Derived, not chosen. `Meta`'s own contract is that the slot matching
    // `txnId mod 2` holds the most recent commit attempt, and `CommitTxn`
    // implements exactly this expression -- so a hard-coded slot would seed a
    // parity no writer produces, and `RecoverExistingFile` would not notice
    // because it tie-breaks on `txnId` alone. The chain is the only thing
    // about this file that is meant to be unusual.
    auto const slot = (meta.txnId % 2 == 0) ? CowTree::MetaSlot::A : CowTree::MetaSlot::B;
    REQUIRE((*store)->WriteMeta(slot, meta).has_value());
}

// ---------------------------------------------------------------------------
// One damaged meta slot, in a real file (#632).
//
// The two-slot alternation is what makes an interrupted meta write survivable,
// and it is the CowTree library's own promise rather than FastCache's --
// `src/CowTree/` is a standalone sibling any project can pick up, so a
// guarantee asserted only from the FastCache suite would not travel with it.
//
// `CowTreeStorage_test.cpp` asks the OPERATOR's version of this question -- a
// daemon's store opens, serves the surviving commit and says nothing. This case
// asks the LIBRARY's: `FilePageStore` recovers, `CowTree::Open` reads the
// surviving slot's root, and the next commit does not spend the slot that
// survived -- which lives in `FilePageStore` and nowhere else, so it is pinned
// here rather than only from the FastCache suite.
//
// The fixtures are separate because a TEST helper cannot be shared: `src/tests/`
// and `src/FastCache/` are off `CowTreeTests`' include path, which is the
// boundary that keeps this library standalone. That is narrower than "nothing
// can be shared" -- `src/CowTree/CowTree/` is on BOTH suites' paths under the
// same `<CowTree/...>` spelling, so a fact about the LAYOUT belongs there if it
// ever earns a public helper. What is duplicated below is test I/O, which does
// not.
//
// Damaged meta bytes have no API route -- `WriteMeta` encodes and CRCs whatever
// it is handed -- so the file is patched directly, at `Meta.hpp`'s own
// documented layout ("slot A at offset 0, slot B at offset PageSize"). The
// injection is asserted rather than assumed, so a layout that moved fails here
// instead of quietly damaging nothing.
//
// Watched refusing, one break at a time, measured on `gcc-debug`: reading the
// `CorruptMetas` condition as "EITHER slot failed" turns this case red in
// `FilePageStore::RecoverExistingFile` (at `Open`) and again in `CowTree::Open`
// (at `tree.Open()`). Inverting the both-valid `txnId` tie-break does NOT --
// correctly, since only one slot is valid here; eight existing cases cover that
// comparison already.
// ---------------------------------------------------------------------------

/// Page size the meta-damage case builds its store with.
///
/// The named minimum rather than the literal 512 it happens to equal: a bare
/// literal becomes an INVALID page size the day `MinPageSize` rises, and `Open`
/// would then refuse the store in a case whose whole point is that it opens.
constexpr std::size_t MetaDamagePageSize = CowTree::MinPageSize;

/// Byte the damage flips, as an offset inside an encoded meta page.
///
/// It has to land in the CRC'd payload -- the leading
/// `MetaEncodedSize - sizeof(u32)` bytes. Half way in is inside the payload for
/// any layout the encoder can grow into.
///
/// The region a decoder genuinely cannot see is NOT the trailing CRC word, which
/// is the tempting guess: `DecodeMeta` recomputes the CRC over the payload and
/// compares it against the stored word, so flipping the STORED word leaves the
/// recomputed one unchanged and the comparison always mismatches -- measured
/// `Corrupt` at both ends of it. What nothing reads is the zero PADDING past
/// `MetaEncodedSize`, where a flip decodes clean (measured at offset 60 and at
/// `pageSize - 1`). `DamageMetaSlot` asserts detection rather than trusting
/// either claim.
constexpr std::size_t MetaDamageOffset = CowTree::MetaEncodedSize / 2;

/// Read a whole file into memory.
/// @param path The file to read.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> ReadWholeFile(std::filesystem::path const& path)
{
    std::ifstream f { path, std::ios::binary | std::ios::ate };
    REQUIRE(f.good());
    auto const size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> bytes(size);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    REQUIRE(f.good());
    return bytes;
}

/// The bytes of one meta slot, out of a store file read whole.
/// @param file The whole store file.
/// @param slot Which slot.
/// @return A view over that slot's page.
[[nodiscard]] std::span<std::byte const> SlotBytes(std::span<std::byte const> file, CowTree::MetaSlot slot)
{
    REQUIRE(file.size() >= 2 * MetaDamagePageSize);
    return file.subspan(static_cast<std::size_t>(slot) * MetaDamagePageSize, MetaDamagePageSize);
}

/// Decode one meta slot out of a store file read whole.
/// @param file The whole store file.
/// @param slot Which slot.
/// @return Whatever `DecodeMeta` answered for it.
[[nodiscard]] std::expected<CowTree::Meta, CowTree::CowTreeError> DecodeSlot(std::span<std::byte const> file,
                                                                             CowTree::MetaSlot slot)
{
    auto const page = SlotBytes(file, slot);
    return CowTree::DecodeMeta(CowTree::BytesView { page.data(), page.size() });
}

/// Replace a file's contents wholesale.
/// @param path  The file to write.
/// @param bytes What it should hold afterwards.
void WriteWholeFile(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    std::ofstream f { path, std::ios::binary | std::ios::trunc };
    REQUIRE(f.good());
    f.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(f.good());
}

/// The other meta slot.
/// @param slot One slot.
/// @return The one it alternates with.
[[nodiscard]] constexpr CowTree::MetaSlot OtherSlot(CowTree::MetaSlot slot) noexcept
{
    return slot == CowTree::MetaSlot::A ? CowTree::MetaSlot::B : CowTree::MetaSlot::A;
}

/// Flip one byte inside meta slot `slot` of the closed store file at `path`.
///
/// Both directions of the injection are asserted: the named slot must stop
/// decoding -- as `Corrupt`, since `DecodeMeta` checks the CRC before magic and
/// version, so damage never presents as a store of another vintage -- and the
/// other slot must still decode.
/// @param path Store file to damage in place.
/// @param slot Which meta slot to damage.
void DamageMetaSlot(std::filesystem::path const& path, CowTree::MetaSlot slot)
{
    auto bytes = ReadWholeFile(path);
    REQUIRE(bytes.size() >= 2 * MetaDamagePageSize);
    bytes[(static_cast<std::size_t>(slot) * MetaDamagePageSize) + MetaDamageOffset] ^= std::byte { 0xFF };
    WriteWholeFile(path, bytes);

    auto const after = ReadWholeFile(path);
    auto const damaged = DecodeSlot(after, slot);
    REQUIRE_FALSE(damaged.has_value());
    REQUIRE(damaged.error() == CowTree::CowTreeError::Corrupt);
    REQUIRE(DecodeSlot(after, OtherSlot(slot)).has_value());
}

/// Open -- or reopen -- the meta-damage case's store at `path`.
/// @param path Filesystem path of the store file.
/// @return Whatever `FilePageStore::Open` answered.
auto OpenMetaDamageStore(std::filesystem::path const& path)
{
    CowTree::FilePageStore::Options opts;
    opts.path = path;
    opts.pageSize = MetaDamagePageSize;
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
// seeded through `FilePageStore`'s own API because nothing else in this tree
// writes one. `CowTree::CommitTxn` sets `Meta::freeRoot = PageId::None()`
// unconditionally ("free list is in-memory only for v1"), and
// `BootstrapNewFile` writes the same, so a store produced by `CowTreeStorage`
// -- which is every store `fastcached` and `fastcache-compile-node` own --
// carries an empty chain and the walk terminates on its first test. What
// these cases pin is therefore the guard itself, on the layout
// `RecoverExistingFile` is written to read, and not a refusal an operator can
// reach today.
// ---------------------------------------------------------------------------

TEST_CASE("An intact free-list chain is walked at Open and its pages are recycled", "[filestore][open][freelist]")
{
    TempFile tmp;
    SeedFreeListChain(tmp.path, CowTree::PageId { 2 }, CowTree::PageId::None());

    auto const store = OpenFreeListStore(tmp.path);
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
    // Page 1 chains to a page the file does not have. This is the shape a
    // truncated or partially-rewritten store takes, and it is the one an
    // unguarded walk would follow into a `ReadAt` past the end.
    SeedFreeListChain(tmp.path, CowTree::PageId { 1'000'000 }, CowTree::PageId::None());

    auto const store = OpenFreeListStore(tmp.path);
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
    // Page 2 links back to page 1. Every id in the loop is in range, so the
    // bounds check above cannot see this one: without the visited set `Open`
    // never returns and `_freeList` grows without bound, which is an unbounded
    // answer rather than a wrong one and is why it is a separate case.
    SeedFreeListChain(tmp.path, CowTree::PageId { 2 }, CowTree::PageId { 1 });

    auto const store = OpenFreeListStore(tmp.path);
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

TEST_CASE("One damaged meta slot opens on the surviving slot's commit", "[filestore][open][meta][corrupt]")
{
    // Two durable commits, so BOTH slots carry a real one. With a single commit
    // the other slot still holds the blank `txnId == 0` meta `BootstrapNewFile`
    // wrote, and "recovered onto the previous commit" would be
    // indistinguishable from "recovered onto nothing".
    TempFile tmp;
    {
        auto store = OpenMetaDamageStore(tmp.path);
        REQUIRE(store.has_value());
        CowTree::CowTree tree { **store };
        REQUIRE(tree.Open().has_value());
        for (auto const* const key: { "first", "second" })
        {
            auto txn = tree.BeginWrite();
            REQUIRE(txn.Put(B(key), B("value")).has_value());
            REQUIRE(txn.Commit().has_value());
            // The default `Batched` durability buffers the meta until a flush
            // boundary, so without this both commits would collapse into the
            // one write the destructor makes and only one slot would move.
            REQUIRE((*store)->Flush().has_value());
        }
    }

    auto const seeded = ReadWholeFile(tmp.path);
    auto const seededA = DecodeSlot(seeded, CowTree::MetaSlot::A);
    auto const seededB = DecodeSlot(seeded, CowTree::MetaSlot::B);
    REQUIRE(seededA.has_value());
    REQUIRE(seededB.has_value());
    REQUIRE(seededA->txnId != seededB->txnId);
    // Which slot the alternation left the newest commit in is read off the file
    // rather than assumed: it follows from how many flushes happened, and a
    // hard-coded slot would silently stop damaging the live one the day that
    // changed. `>=` is what `RecoverExistingFile` and `CowTree::Open` both
    // spell, so this mirrors them rather than paraphrasing them -- the tie is
    // unreachable here, the two ids being asserted different above, but a
    // fixture that reads differently from the code it models has to be
    // re-proved by hand every time somebody compares the two.
    auto const live = seededA->txnId >= seededB->txnId ? CowTree::MetaSlot::A : CowTree::MetaSlot::B;

    /// One row of the table below.
    struct Row
    {
        CowTree::MetaSlot damaged; ///< Which slot to damage; the survivor is `OtherSlot` of it.
        bool secondKeyReadable;    ///< Whether the newest commit's key survives.
        std::string_view what;     ///< What the row is about.
    };

    // Both directions, because only the pair says the SURVIVING slot was
    // chosen: damage the live slot alone and a build that always took the older
    // valid slot would pass, damage the stale one alone and one that always
    // took the newer would.
    auto const rows = std::vector<Row> {
        { .damaged = OtherSlot(live),
          .secondKeyReadable = true,
          .what = "the stale slot is damaged: the newest commit still serves" },
        { .damaged = live,
          .secondKeyReadable = false,
          .what = "the live slot is damaged: the store falls back to the previous commit" },
    };

    for (auto const& row: rows)
    {
        INFO(row.what);
        // Every row starts from the seeded bytes, so one row's damage is not
        // still in the file when the next one runs.
        WriteWholeFile(tmp.path, seeded);
        DamageMetaSlot(tmp.path, row.damaged);

        auto const survivorSlot = OtherSlot(row.damaged);
        // The survivor's meta and page bytes are already in hand from the seed:
        // the file has just been restored from those very bytes and
        // `DamageMetaSlot` touched only the other slot, so there is nothing to
        // re-read here.
        auto const& survivor = survivorSlot == CowTree::MetaSlot::A ? seededA : seededB;
        auto const survivorPage = SlotBytes(seeded, survivorSlot);

        // `Corrupt` at open means BOTH slots failed, and one is not both. This
        // is the whole claim, and `CorruptMetas` is what a build reading that
        // condition as "either" would answer here instead.
        auto const store = OpenMetaDamageStore(tmp.path);
        REQUIRE(store.has_value());

        {
            CowTree::CowTree tree { **store };
            REQUIRE(tree.Open().has_value());
            // Which meta the tree came up on, asked directly. The key
            // assertions below would also pass over a default-constructed
            // `Meta`, whose empty root reads as a tree that simply has no keys
            // in it.
            REQUIRE(tree.ItemCount() == survivor->itemCount);

            auto const reader = tree.BeginRead();
            auto const first = reader.Get(B("first"));
            REQUIRE(first.has_value());
            REQUIRE(first->has_value());
            auto const second = reader.Get(B("second"));
            // A key from a commit the surviving slot never saw is a MISS and
            // not a failure: the store is consistent, just with an earlier
            // moment.
            REQUIRE(second.has_value());
            REQUIRE(second->has_value() == row.secondKeyReadable);

            // And the store must not spend the one slot it has left. Recovery
            // records the SURVIVING slot as `_lastDurableSlot`, so
            // `FlushBatchLocked` writes the OTHER one -- the damaged one -- and
            // ordinary use repairs the file instead of leaving it one torn write
            // from unopenable. That member lives in `FilePageStore` and nowhere
            // else, which is why the assertion belongs in THIS suite: a build
            // that recovered correctly and then wrote over the good slot passes
            // every assertion above it.
            //
            // Scoped to `Batched`, asserted rather than assumed, because the
            // claim is FALSE under `Fsync`: there the slot is `CommitTxn`'s
            // `txnId % 2`, which consults nothing recovery recorded, and a store
            // whose parity an earlier Batched run broke overwrites the survivor
            // instead. Measured; see the storage rulebook's Open work.
            REQUIRE((*store)->DurabilityMode() == CowTree::FilePageStore::Durability::Batched);
            auto txn = tree.BeginWrite();
            REQUIRE(txn.Put(B("after-recovery"), B("value")).has_value());
            REQUIRE(txn.Commit().has_value());
            REQUIRE((*store)->Flush().has_value());
        }

        auto const after = ReadWholeFile(tmp.path);
        // Byte-for-byte, not "still decodes": a slot rewritten with an equally
        // valid meta would decode fine and would still have been the one spent.
        REQUIRE(std::ranges::equal(survivorPage, SlotBytes(after, survivorSlot)));
        // The other half, which is what makes the first mean something: the
        // commit did land somewhere, in the slot that was already damaged, and
        // it is newer than what the store recovered from. Without this a build
        // that wrote no meta at all would pass.
        auto const repaired = DecodeSlot(after, row.damaged);
        REQUIRE(repaired.has_value());
        REQUIRE(repaired->txnId > survivor->txnId);
    }
}
