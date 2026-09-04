// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/IPageStore.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

namespace CowTree
{

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
    /// @return Owning FilePageStore on success; CowTreeError::InUse when
    ///         another open store holds the file, CowTreeError::IoError on any
    ///         other failure to open it.
    [[nodiscard]] static auto Open(Options options) -> std::expected<std::unique_ptr<FilePageStore>, CowTreeError>;

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
    [[nodiscard]] auto TakeExclusiveLock() -> std::expected<void, CowTreeError>;
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
    std::size_t _totalDataPages { 0 };

    /// Currently-allocated data page indices (1-based). Tracked so Read
    /// of a freed page can be rejected.
    std::unordered_set<std::uint64_t> _live;

    /// In-memory free list of recyclable page ids. Populated from the
    /// on-disk free-list chain on Open and updated on Free/Allocate.
    std::vector<std::uint64_t> _freeList;

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
    MetaSlot _lastDurableSlot { MetaSlot::A };

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
