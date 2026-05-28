// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/IPageStore.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

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

    /// Open or create the backing file. Existing files are inspected:
    /// the page size is taken from whichever meta page validates; new
    /// files are initialised with two blank meta pages of the requested
    /// size.
    /// @param options Open parameters.
    /// @return Owning FilePageStore on success.
    [[nodiscard]] static auto Open(Options options)
        -> std::expected<std::unique_ptr<FilePageStore>, CowTreeError>;

    FilePageStore(FilePageStore const&) = delete;
    FilePageStore(FilePageStore&&) = delete;
    FilePageStore& operator=(FilePageStore const&) = delete;
    FilePageStore& operator=(FilePageStore&&) = delete;
    ~FilePageStore() override;

    // IPageStore -----------------------------------------------------

    [[nodiscard]] auto Read(PageId id) const
        -> std::expected<BytesView, CowTreeError> override;

    [[nodiscard]] auto Allocate()
        -> std::expected<PageId, CowTreeError> override;

    [[nodiscard]] auto Write(PageId id, BytesView data)
        -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto Free(PageId id)
        -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto SyncData()
        -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto ReadMeta(MetaSlot slot) const
        -> std::expected<Meta, CowTreeError> override;

    [[nodiscard]] auto WriteMeta(MetaSlot slot, Meta const& meta)
        -> std::expected<void, CowTreeError> override;

    [[nodiscard]] auto PageSize() const noexcept -> std::size_t override;

    /// @return Current durability mode.
    [[nodiscard]] Durability DurabilityMode() const noexcept;

    /// @return The total number of data pages currently allocated in
    ///         the file (live pages + free-list entries).
    [[nodiscard]] std::size_t TotalDataPages() const noexcept;

    /// Test helper: how many `fsync`/equivalent calls have been issued.
    [[nodiscard]] std::size_t FsyncCallCount() const noexcept;

  private:
    explicit FilePageStore(Options options) noexcept;

    /// Compute the file offset of a data page.
    [[nodiscard]] std::uint64_t DataPageOffset(PageId id) const noexcept;

    /// Compute the file offset of a meta slot.
    [[nodiscard]] std::uint64_t MetaSlotOffset(MetaSlot slot) const noexcept;

    /// Read `data.size()` bytes from `offset` into `data`.
    [[nodiscard]] auto ReadAt(std::uint64_t offset, BytesSpan data) const
        -> std::expected<void, CowTreeError>;

    /// Write `data.size()` bytes from `data` to `offset`.
    [[nodiscard]] auto WriteAt(std::uint64_t offset, BytesView data)
        -> std::expected<void, CowTreeError>;

    /// Fsync the backing file according to durability mode.
    [[nodiscard]] auto Fsync()
        -> std::expected<void, CowTreeError>;

    /// Initialise a brand-new file (write two blank meta pages, size
    /// the file to 2*pageSize).
    [[nodiscard]] auto BootstrapNewFile()
        -> std::expected<void, CowTreeError>;

    /// Recover state from an existing file: pick the live meta page,
    /// scan for the highest allocated data page, populate the in-memory
    /// free list by chasing `freeRoot`.
    [[nodiscard]] auto RecoverExistingFile()
        -> std::expected<void, CowTreeError>;

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

    /// Cached page buffer to satisfy the IPageStore lifetime contract on
    /// Read() — a returned BytesView must remain valid until the next
    /// mutating call. Mutable because Read is `const` from the consumer's
    /// point of view.
    mutable std::vector<std::byte> _readBuffer;
    mutable std::uint64_t _readBufferPageIdx { 0 };

    std::size_t _fsyncCount { 0 };
    mutable std::mutex _ioMutex;
};

} // namespace CowTree
