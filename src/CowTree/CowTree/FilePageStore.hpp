// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/IPageStore.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

namespace CowTree
{

/// Why an `Open` refused, and the system error it was classified FROM.
///
/// **A refusal names what was OBSERVED, and an errno is the only observation behind a lock
/// refusal.** `InUse` on its own says *somebody holds this file* -- which was the one claim
/// that turned out to be false in
/// [#1507](https://github.com/LASTRADA-Software/fastcached/issues/1507), where 31 of 32 test
/// processes had drawn the same filename and `flock` was refusing correctly. Diagnosing that
/// meant patching the test to print the errno, because nothing on the wire out of `Open`
/// carried it; with the code present, `InUse` is falsifiable from a log.
///
/// `systemCode` is a **disengaged optional and never 0** where there was no system call.
/// `InvalidArg` is decided from the arguments and a damaged meta page is decided from bytes
/// already read, so neither has an errno to report -- and *absent* is a different fact from
/// *the call returned 0*, which is `errno`'s spelling of success. That distinction is the
/// whole defect this type exists to remove, so it cannot be represented by a sentinel.
struct OpenRefusal
{
    /// What the caller is being refused with.
    CowTreeError cause;

    /// The `errno` (POSIX) or `GetLastError()` (Windows) value `cause` was classified from,
    /// disengaged where no system call was involved.
    std::optional<int> systemCode {};

    /// A refusal with no system error behind it.
    ///
    /// **Explicit, and the verbosity at the call sites is the point.** An implicit conversion
    /// would let every existing `return std::unexpected(CowTreeError::X)` inside `Open` keep
    /// compiling untouched -- convenient, and it would also silently produce a
    /// no-system-error refusal at the three sites that DO hold an errno, which is the defect
    /// rather than a shortcut. Spelling it means each site states which of the two it is.
    ///
    /// There is deliberately **no `operator==(CowTreeError)`** either. Producing a refusal
    /// from a bare cause loses nothing; COMPARING one to a bare cause would let a consumer
    /// read the enumerator and silently discard the code, which is exactly the shape #1507
    /// was filed about. A caller that wants the cause spells `.cause`.
    /// @param from The cause, with no system error to report.
    constexpr explicit OpenRefusal(CowTreeError from) noexcept:
        cause { from }
    {
    }

    /// A refusal carrying the system error it was classified from.
    /// @param from The cause.
    /// @param code The `errno` or `GetLastError()` value observed.
    constexpr OpenRefusal(CowTreeError from, int code) noexcept:
        cause { from },
        systemCode { code }
    {
    }
};

/// File-backed `IPageStore`.
///
/// Layout: `[meta_a][meta_b][data_pages...]` where each meta page is
/// exactly `PageSize()` bytes. Data PageId `n` (1-based) lives at file
/// offset `(2 + (n - 1)) * PageSize()`.
///
/// Uses POSIX `pread` / `pwrite` (or `ReadFile` / `WriteFile` on Windows)
/// for portable, position-independent I/O without needing `lseek` state.
/// `SyncData` and `WriteMeta` issue `fsync` / `FlushFileBuffers` according
/// to the configured durability mode.
///
/// **A store file belongs to exactly one open store.** `Open` claims it
/// exclusively and holds the claim until the store is destroyed, so a second
/// opener is refused with CowTreeError::InUse rather than allowed to interleave
/// its meta-page writes with the first one's. Nothing about the claim is written
/// to the file: it is kernel state on the open file description (POSIX `flock`)
/// or on the handle (the Windows share mode), so a file written by a build that
/// takes it is byte-identical to one written by a build that does not.
class FilePageStore final: public IPageStore
{
  public:
    /// Durability policy.
    enum class Durability : std::uint8_t
    {
        Fsync,   ///< fsync after every Write/WriteMeta. Slowest, safest.
        Batched, ///< Buffer writes; fsync at SyncData() boundaries only.
        None,    ///< Never call fsync. OS page cache only. Fastest.
    };

    /// Whether this store holds its file exclusively.
    enum class LockState : std::uint8_t
    {
        Held,        ///< Claimed. A second opener is refused.
        Unavailable, ///< This filesystem cannot lock; the store opened unguarded.
    };

    /// What a failure to claim the file means.
    enum class LockFailure : std::uint8_t
    {
        Contended,   ///< Somebody else holds it. Refuse: CowTreeError::InUse.
        Unsupported, ///< The filesystem cannot lock. Open anyway, unguarded.
        Fatal,       ///< Unrelated failure. Refuse: CowTreeError::IoError.
    };

    /// Classify the system error a failed claim produced.
    ///
    /// The two platforms feed this from different calls, which is a consequence
    /// of where each one enforces exclusivity: POSIX passes `errno` from
    /// `flock`, taken after the file is already open, so anything it does not
    /// recognise means *this filesystem cannot lock* and the store opens
    /// unguarded. Windows passes `GetLastError()` from `CreateFileW` itself —
    /// the share mode is the claim, so there is no separate call to fail and no
    /// `Unsupported` case at all; anything unrecognised there is a genuine open
    /// failure.
    ///
    /// Public so the decision table can be asserted directly. The `Unsupported`
    /// arm cannot be provoked on any filesystem the tests run on, which is
    /// exactly what makes it worth pinning down.
    /// @param systemError POSIX `errno`, or Windows `GetLastError()`.
    /// @return What the caller should do about it.
    [[nodiscard]] static LockFailure ClassifyLockFailure(int systemError) noexcept;

    /// Construction options.
    struct Options
    {
        /// Filesystem path of the backing file.
        std::filesystem::path path;

        /// Page size to use when creating the file (existing files keep
        /// the page size recorded in their meta page).
        std::size_t pageSize { DefaultPageSize };

        /// Durability policy.
        Durability durability { Durability::Batched };
    };

    /// Open or create the backing file, claiming it exclusively. Existing
    /// files are inspected: the page size is taken from whichever meta page
    /// validates; new files are initialised with two blank meta pages of the
    /// requested size.
    ///
    /// The claim is taken before any of that, because writing the blank meta
    /// pages of a "new" file is itself the write that would destroy a store a
    /// second process is already using.
    /// @param options Open parameters.
    /// @return Owning FilePageStore on success; an `OpenRefusal` whose `cause` is
    ///         CowTreeError::InUse when another open store holds the file and
    ///         CowTreeError::IoError on any other failure to open it, carrying the
    ///         `systemCode` it was classified from wherever a system call decided it.
    [[nodiscard]] static auto Open(Options options) -> std::expected<std::unique_ptr<FilePageStore>, OpenRefusal>;

    FilePageStore(FilePageStore const&) = delete;
    FilePageStore(FilePageStore&&) = delete;
    FilePageStore& operator=(FilePageStore const&) = delete;
    FilePageStore& operator=(FilePageStore&&) = delete;
    ~FilePageStore() override;

    // IPageStore -----------------------------------------------------

    [[nodiscard]] auto Read(PageId id) const -> std::expected<BytesView, CowTreeError> override;

    [[nodiscard]] auto Allocate() -> std::expected<PageId, CowTreeError> override;

    [[nodiscard]] auto Write(PageId id, BytesView data) -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto Free(PageId id) -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto SyncData() -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto ReadMeta(MetaSlot slot) const -> std::expected<Meta, CowTreeError> override;

    [[nodiscard]] auto WriteMeta(Meta const& meta) -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto LastDurableSlot() const noexcept -> MetaSlot override;

    [[nodiscard]] auto PageSize() const noexcept -> std::size_t override;

    [[nodiscard]] auto Flush() -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto PageCount() const noexcept -> std::size_t override;

    /// The footprint a byte budget is enforced against; see `IPageStore::PagesInUse`.
    ///
    /// **And what holds the FILE to it.** The file's length is `2 + PageCount()` pages,
    /// so it exceeds this figure by exactly the free pages inside it. Three things keep
    /// that gap small: the free list's pages come out of the free pages rather than
    /// extending the file, `Allocate` takes the lowest free id so live pages collect at
    /// the front, and each durable flush cuts the free tail. Both directions of the
    /// relation: the file is NEVER shorter than the pages in use, and it may be LONGER by
    /// the free pages until a flush cuts them -- by a batch of copy-on-write pages at
    /// steady state, and by every page a large eviction freed in the middle of the file
    /// until churn migrates live data below them.
    [[nodiscard]] auto PagesInUse() const noexcept -> std::size_t override;

    /// @return Current durability mode.
    [[nodiscard]] Durability DurabilityMode() const noexcept;

    /// Whether this store actually holds its file exclusively.
    ///
    /// `Unavailable` is not a failure — the store is open and usable — but it
    /// says the guard against a second opener is not in force, which a caller
    /// should tell its operator rather than keep to itself.
    /// @return Held, unless the filesystem could not lock.
    [[nodiscard]] LockState StoreLockState() const noexcept;

    /// @return The total number of data pages currently allocated in
    ///         the file (live pages + free-list entries).
    [[nodiscard]] std::size_t TotalDataPages() const noexcept;

    /// Test helper: how many `fsync`/equivalent calls have been issued.
    [[nodiscard]] std::size_t FsyncCallCount() const noexcept;

    /// Test helper: simulate a hard crash. Drops the OS handle WITHOUT
    /// flushing any buffered group-commit batch and suppresses the
    /// destructor's graceful flush, so the unflushed window is discarded
    /// exactly as it would be on power loss. The object must not be used
    /// afterwards except to be destroyed.
    void SimulateCrashForTest() noexcept;

  private:
    /// Write the free list to DEDICATED pages and return the head.
    ///
    /// Private: `freeRoot` is the STORE's business, and the flush is the only moment
    /// at which a list is consistent with the meta that will name it.
    ///
    /// **The list's own pages come FROM `_freeList`, and the file is extended only for
    /// the shortfall** -- when `_freeList` holds fewer pages than the list needs to
    /// describe everything else. Taking them by extension instead grew the file by
    /// `ceil(F / idsPerPage)` pages at every flush and handed the previous list's pages
    /// back to `F`, so a store sitting at its byte bound grew geometrically and never
    /// shrank: a 64 GiB tier became a 103 GB file, 89 % of it free pages.
    ///
    /// Taking one is safe for exactly the reason a data `Allocate` taking one is: a page
    /// in `_freeList` is free in the world of the last DURABLE meta and in the world of
    /// the meta about to be written, so neither recovery reads it and overwriting it
    /// damages nothing. Two kinds of page are NOT in `_freeList`, and both are refused
    /// for the same reason -- the last durable meta still needs them until the one this
    /// flush writes is durable:
    ///
    /// - a **pending** free (`_pendingFree`): its freeing is not durable yet, and the
    ///   last durable meta's tree still references it;
    /// - a page of the **previous** list (`_freeListPages`): the last durable meta's
    ///   `freeRoot` names it.
    ///
    /// The list NAMES every page that is free in the new meta's world -- what stays in
    /// `_freeList`, the pending frees and the previous list's pages -- so a reopen
    /// rebuilds exactly the free state this process holds once the flush lands, rather
    /// than marking the last batch's frees live forever. Naming a page is not taking it.
    ///
    /// On failure every member is as it was, so a retried flush starts from the same
    /// state; a page the shortfall already extended is returned to `_freeList`.
    /// @return The head of the new chain, or `PageId::None()` when nothing is free.
    [[nodiscard]] auto WriteFreeListLocked() -> std::expected<PageId, CowTreeError>;

    /// Extend the file by one page.
    ///
    /// The only operation that lengthens the file. The page counter and the live set move
    /// only once the page's write has LANDED: moving them first left a phantom live page
    /// behind every failed extend, which on a full disk is every extend.
    /// @return The new page.
    [[nodiscard]] auto ExtendLocked() -> std::expected<PageId, CowTreeError>;

    /// Cut the file back to its highest page that is not free, after a durable flush.
    ///
    /// The truncatable tail is the run of highest ids that are in `_freeList` -- free in
    /// the meta just made durable AND in the one it superseded, which is the meta a
    /// damaged slot falls back to. So it is called BEFORE the pending frees graduate:
    /// those are still referenced by the superseded meta's tree, and the previous list's
    /// pages, which travel with them, are still that meta's `freeRoot`. Both metas may
    /// still NAME a cut page as a free-list entry; recovery skips an entry past the end of
    /// the file for that reason, while a LINK past the end stays `Corrupt`, since no list
    /// page a surviving meta names is ever cut.
    ///
    /// Not fatal when the length cannot be changed: the flush is already durable, the
    /// pages stay in `_freeList`, and the next flush tries again.
    /// @return Empty when the file is as short as it can be; the I/O error otherwise.
    [[nodiscard]] auto TruncateFreeTailLocked() -> std::expected<void, CowTreeError>;

    /// Set the backing file's length to `bytes`, position-independently.
    /// @param bytes New length in bytes.
    /// @return Empty on success; CowTreeError::IoError otherwise.
    [[nodiscard]] auto SetFileLength(std::uint64_t bytes) const -> std::expected<void, CowTreeError>;

    explicit FilePageStore(Options options) noexcept;

#if !defined(_WIN32)
    /// Claim the already-open file for this store, setting `_lockState`.
    ///
    /// POSIX only, and declared conditionally rather than left as an empty
    /// Windows body: there the share mode passed to `CreateFileW` *is* the
    /// claim, so a second function would be one nothing ever calls.
    /// @return Empty once the outcome is recorded — including the
    ///         `Unsupported` outcome, which opens unguarded rather than
    ///         failing; CowTreeError::InUse or ::IoError otherwise.
    [[nodiscard]] auto TakeExclusiveLock() -> std::expected<void, OpenRefusal>;
#endif

    /// Current length of the backing file, read from the open handle.
    /// @return Byte length, or CowTreeError::IoError if it cannot be read.
    [[nodiscard]] auto FileSizeBytes() const -> std::expected<std::uint64_t, CowTreeError>;

    /// Compute the file offset of a data page.
    [[nodiscard]] std::uint64_t DataPageOffset(PageId id) const noexcept;

    /// Compute the file offset of a meta slot.
    [[nodiscard]] std::uint64_t MetaSlotOffset(MetaSlot slot) const noexcept;

    /// Read `data.size()` bytes from `offset` into `data`.
    [[nodiscard]] auto ReadAt(std::uint64_t offset, BytesSpan data) const -> std::expected<void, CowTreeError>;

    /// Write `data.size()` bytes from `data` to `offset`.
    [[nodiscard]] auto WriteAt(std::uint64_t offset, BytesView data) const -> std::expected<void, CowTreeError>;

    /// Fsync the backing file according to durability mode.
    [[nodiscard]] auto Fsync() -> std::expected<void, CowTreeError>;

    /// Encode `meta` (forcing the on-disk page size) and write it to `slot`.
    /// Does NOT fsync. Caller must hold `_ioMutex`.
    /// @param slot Destination meta slot.
    /// @param meta Meta record to encode and persist.
    /// @return Empty on success; the underlying I/O error otherwise.
    [[nodiscard]] auto WriteSlotLocked(MetaSlot slot, Meta const& meta) -> std::expected<void, CowTreeError>;

    /// Initialise a brand-new file (write two blank meta pages, size
    /// the file to 2*pageSize).
    [[nodiscard]] auto BootstrapNewFile() -> std::expected<void, CowTreeError>;

    /// Recover state from an existing file: pick the live meta page,
    /// scan for the highest allocated data page, populate the in-memory
    /// free list by chasing `freeRoot`.
    [[nodiscard]] auto RecoverExistingFile() -> std::expected<void, CowTreeError>;

    Options _options;
#if defined(_WIN32)
    void* _handle { nullptr }; ///< Windows HANDLE.
#else
    int _fd { -1 }; ///< POSIX file descriptor.
#endif
    std::size_t _pageSize { 0 };
    /// Atomic for the same reason as `_lastDurableSlot`: `PageCount()` is a
    /// `noexcept` interface member that locked. `TotalDataPages()` read it
    /// with no lock at all, which was a plain data race -- so the atomic is
    /// what makes that reader correct rather than merely quiet.
    std::atomic<std::size_t> _totalDataPages { 0 };

    /// A set of page ids that keeps its own size readable without the lock.
    ///
    /// `PagesInUse()` is a `noexcept` interface member, so it cannot lock (see
    /// `_lastDurableSlot`), and reading an `unordered_set`'s size unlocked while a
    /// writer rehashes it is a data race. Folding the count into the only three
    /// operations that change membership is what keeps it exact: no call site can
    /// change the set and forget the counter.
    class LivePages
    {
      public:
        /// @param id A page id.
        /// @return Whether it is live.
        [[nodiscard]] bool Contains(std::uint64_t id) const
        {
            return _ids.contains(id);
        }

        /// Mark `id` live.
        void Insert(std::uint64_t id)
        {
            _ids.insert(id);
            _count = _ids.size();
        }

        /// Mark `id` not live.
        void Erase(std::uint64_t id)
        {
            _ids.erase(id);
            _count = _ids.size();
        }

        /// @return How many ids are live, readable from any thread.
        [[nodiscard]] std::size_t Count() const noexcept
        {
            return _count;
        }

      private:
        std::unordered_set<std::uint64_t> _ids;
        std::atomic<std::size_t> _count { 0 };
    };

    /// Currently-allocated data page indices (1-based). Tracked so Read
    /// of a freed page can be rejected, and counted for `PagesInUse()`.
    LivePages _live;

    /// In-memory free list of recyclable page ids. Populated from the
    /// on-disk free-list chain on Open and updated on Free/Allocate.
    ///
    /// ORDERED, and `Allocate` hands out the LOWEST id, so live data migrates to the
    /// front of the file as it is rewritten and the free pages collect at the end, where
    /// `TruncateFreeTailLocked` can cut them. A LIFO list handed back whatever was freed
    /// last, wherever it was, so a store's file never got shorter.
    std::set<std::uint64_t> _freeList;

    /// Pages holding the free list that the LAST DURABLE meta points at.
    ///
    /// Kept so the next flush can free them once its own meta supersedes that one.
    /// They must not be recycled before then: until the new meta is fsynced, the old
    /// one is still the recoverable copy and its `freeRoot` still names these.
    std::vector<std::uint64_t> _freeListPages;

    /// Group-commit (Batched durability): pages freed since the last flush.
    /// They are NOT reusable yet — reusing a page before its freeing is
    /// durable would let a crash-rollback (to the last flush) read a page that
    /// a later in-batch write overwrote. On flush they graduate to `_freeList`.
    std::vector<std::uint64_t> _pendingFree;

    /// Commits (meta writes) since the last Batched flush; drives the
    /// flush-every-N-commits boundary.
    std::size_t _commitsSinceFlush { 0 };

    /// Group-commit (Batched): the most recent committed meta, buffered in
    /// memory and written to disk only at a flush boundary. `std::nullopt`
    /// when no commit is pending since the last flush. Deferring the on-disk
    /// write is what keeps a hard crash from ever observing a torn, in-place
    /// overwrite of the last durable meta slot.
    std::optional<Meta> _pendingMeta;

    /// Slot currently holding the most recent fully-fsynced ("durable") meta.
    /// The next Batched flush always writes to the *other* slot, so a torn
    /// flush can never destroy the last durable meta. Initialised on
    /// Bootstrap/Recover and advanced by `FlushBatchLocked`.
    /// Atomic because `LastDurableSlot()` is `noexcept` on `IPageStore` and a
    /// `std::mutex::lock` can throw `std::system_error` -- which inside a
    /// `noexcept` function is `std::terminate`, not an error return. Every
    /// WRITE still happens under `_ioMutex`, so the lock goes on ordering the
    /// slot against the meta write it describes; the atomic exists so the
    /// accessor can read it without taking one.
    std::atomic<MetaSlot> _lastDurableSlot { MetaSlot::A };

    /// Whether the file is actually claimed. Both platforms can downgrade it,
    /// and neither takes the claim's word for it: POSIX reads what `flock`
    /// reported, and Windows re-opens the file it just claimed to check that
    /// the share mode was honoured rather than merely accepted.
    ///
    /// Declared against `_lastDurableSlot` rather than beside the handle it
    /// describes because both are byte-wide: a lone byte between two 8-aligned
    /// members costs seven, and `clang-analyzer-optin.performance.Padding` is
    /// enforced here.
    LockState _lockState { LockState::Held };

    /// Flush the accumulated Batched writes after this many commits.
    static constexpr std::size_t BatchedFlushInterval = 64;

    /// fsync + graduate pending frees. Caller must hold `_ioMutex`.
    [[nodiscard]] auto FlushBatchLocked() -> std::expected<void, CowTreeError>;

    /// Cached page buffer to satisfy the IPageStore lifetime contract on
    /// Read() — a returned BytesView must remain valid until the next
    /// mutating call. Mutable because Read is `const` from the consumer's
    /// point of view.
    mutable std::vector<std::byte> _readBuffer;
    mutable std::uint64_t _readBufferPageIdx { 0 };

    std::size_t _fsyncCount { 0 };

    /// Set by `SimulateCrashForTest` so the destructor skips its graceful
    /// flush (and avoids touching the already-closed handle).
    bool _crashedForTest { false };

    mutable std::mutex _ioMutex;
};

} // namespace CowTree
