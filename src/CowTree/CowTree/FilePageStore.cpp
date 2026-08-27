// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <ranges>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include <CowTree/FilePageStore.hpp>
#include <CowTree/Meta.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/file.h>
    #include <sys/stat.h>
    #include <sys/types.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace CowTree
{

namespace
{

    /// True iff the page size is one we will accept.
    [[nodiscard]] bool IsValidPageSize(std::size_t size) noexcept
    {
        return size >= MinPageSize && size <= MaxPageSize && (size & (size - 1)) == 0;
    }

    /// One system error, and what it means for the exclusive claim.
    struct LockErrorRow
    {
        int systemError;
        FilePageStore::LockFailure failure;
    };

#if defined(_WIN32)
    /// `CreateFileW` refuses a second writer because of the share mode the
    /// first one passed; both spellings of that refusal reach us here.
    constexpr std::array LockErrorRows {
        LockErrorRow { .systemError = ERROR_SHARING_VIOLATION, .failure = FilePageStore::LockFailure::Contended },
        LockErrorRow { .systemError = ERROR_LOCK_VIOLATION, .failure = FilePageStore::LockFailure::Contended },
    };

    /// The call that failed is the open itself, so anything not named above is
    /// a genuine open failure — there is no separate claim that could have been
    /// unsupported.
    constexpr auto UnnamedLockError = FilePageStore::LockFailure::Fatal;
#else
    /// EWOULDBLOCK and EAGAIN are one value on Linux and permitted to differ by
    /// POSIX; the duplicate row costs a comparison and keeps the table honest
    /// on a platform where they split. EBADF is our own bug rather than
    /// anything about the file.
    constexpr std::array LockErrorRows {
        LockErrorRow { .systemError = EWOULDBLOCK, .failure = FilePageStore::LockFailure::Contended },
        LockErrorRow { .systemError = EAGAIN, .failure = FilePageStore::LockFailure::Contended },
        LockErrorRow { .systemError = EBADF, .failure = FilePageStore::LockFailure::Fatal },
    };

    /// The file is already open by the time `flock` runs, so an unrecognised
    /// errno means the claim could not be made rather than that the file could
    /// not be reached. ENOLCK, EOPNOTSUPP and EINVAL all arrive here and there
    /// is no closed list of them — kernels and network filesystems disagree
    /// about which they report — so this is the default rather than a fourth
    /// row somebody has to keep current.
    constexpr auto UnnamedLockError = FilePageStore::LockFailure::Unsupported;
#endif

} // namespace

auto FilePageStore::ClassifyLockFailure(int systemError) noexcept -> LockFailure
{
    // Walked rather than looked up with `std::ranges::find`, which would hand
    // back a std::array iterator -- a raw pointer on libstdc++ and libc++ and a
    // class type on MSVC. Spelling that `auto` trips `readability-qualified-auto`
    // and spelling it `auto const*` does not compile on MSVC, so there is no
    // spelling that satisfies both. A range-for over three rows needs neither.
    for (auto const& row: LockErrorRows)
        if (row.systemError == systemError)
            return row.failure;
    return UnnamedLockError;
}

FilePageStore::FilePageStore(Options options) noexcept:
    _options { std::move(options) },
    _pageSize { _options.pageSize }
{
}

FilePageStore::~FilePageStore()
{
    // Graceful shutdown: make any buffered group-commit writes durable so a
    // clean stop never loses data (only a hard crash drops the last batch).
    // A simulated crash (test seam) skips this and leaves the window unflushed.
    if (!_crashedForTest && _options.durability == Durability::Batched && _commitsSinceFlush > 0)
    {
        std::scoped_lock const lock { _ioMutex };
        std::ignore = FlushBatchLocked();
    }
    // The close below is what releases the exclusive claim, and there is
    // deliberately no explicit unlock before it: an unlock is a second thing
    // that can be ordered wrongly, and putting one ahead of the flush above
    // would let the next opener in while this store is still writing.
#if defined(_WIN32)
    if (_handle != nullptr)
    {
        ::CloseHandle(_handle);
        _handle = nullptr;
    }
#else
    if (_fd >= 0)
    {
        ::close(_fd);
        _fd = -1;
    }
#endif
}

auto FilePageStore::Open(Options options) -> std::expected<std::unique_ptr<FilePageStore>, CowTreeError>
{
    if (!IsValidPageSize(options.pageSize))
        return std::unexpected(CowTreeError::InvalidArg);

    auto store = std::unique_ptr<FilePageStore> { new FilePageStore { std::move(options) } };

    bool const existedBeforeOpen = std::filesystem::exists(store->_options.path);

#if defined(_WIN32)
    auto const path = store->_options.path.native();
    // FILE_SHARE_READ and no FILE_SHARE_WRITE IS the exclusive claim: a second
    // writer cannot obtain a handle at all, so there is no separate lock call
    // and no window in which we hold a writable handle we have not claimed.
    auto* handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        auto const failure = ClassifyLockFailure(static_cast<int>(::GetLastError()));
        return std::unexpected(failure == LockFailure::Contended ? CowTreeError::InUse : CowTreeError::IoError);
    }
    store->_handle = handle;

    // Prove the share mode is enforced rather than assuming it. A share mode is
    // honoured by the filesystem driver, and some network redirectors and
    // user-mode filesystems accept one and ignore it; on those the claim above
    // is decoration, and a second daemon would open the same store and
    // interleave its meta-page writes with no diagnostic at all.
    //
    // Asking for a second handle answers the question exactly, because it asks
    // the question: if THIS process can reopen the file it just claimed, so can
    // another one. `OPEN_EXISTING` so the probe can only ever observe, and the
    // handle is dropped immediately — nothing is ever written through it.
    auto* const probe = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(probe);
        store->_lockState = LockState::Unavailable;
    }
#else
    auto const flags = O_RDWR | O_CREAT;
    auto const fd = ::open(store->_options.path.c_str(), flags, 0644);
    if (fd < 0)
        return std::unexpected(CowTreeError::IoError);
    store->_fd = fd;

    // Before bootstrap or recovery, either of which writes meta pages: a "new"
    // file is only new because nobody has claimed it yet, and blanking its two
    // meta pages is precisely the write that would destroy a store somebody
    // else is already using.
    if (auto const claimed = store->TakeExclusiveLock(); !claimed.has_value())
        return std::unexpected(claimed.error());
#endif

    // Bootstrapping BLANKS both meta pages, so which branch runs is a
    // destructive decision — and `existedBeforeOpen` was sampled before the
    // file was open and before it was ours, which makes acting on it alone a
    // time-of-check/time-of-use bug the claim above does not close. Another
    // process can create, populate and close the store inside that window; it
    // is gone by the time we take the claim, so nothing refuses us, and we
    // would blank a store somebody had just written.
    //
    // Re-checking the length under the claim closes it: content that appeared
    // in the window means there is a store here to recover, whatever the
    // earlier observation said. An empty file that already existed is left to
    // `RecoverExistingFile` exactly as before, so the only case this changes is
    // the racing one.
    auto const sizeUnderClaim = store->FileSizeBytes();
    if (!sizeUnderClaim.has_value())
        return std::unexpected(sizeUnderClaim.error());

    if (!existedBeforeOpen && *sizeUnderClaim == 0)
    {
        if (auto const r = store->BootstrapNewFile(); !r.has_value())
            return std::unexpected(r.error());
    }
    else
    {
        if (auto const r = store->RecoverExistingFile(); !r.has_value())
            return std::unexpected(r.error());
    }

    store->_readBuffer.resize(store->_pageSize);
    return store;
}

#if !defined(_WIN32)
auto FilePageStore::TakeExclusiveLock() -> std::expected<void, CowTreeError>
{
    // flock rather than fcntl: an fcntl lock is per PROCESS, so a second store
    // in the same process would take it again and report success — which is
    // both a real way to corrupt the file and the reason the guard would be
    // untestable without spawning processes. flock is per open file
    // DESCRIPTION, so it refuses a second `open` of the same path even from
    // here, survives a fork (both halves then share the one claim, which is
    // what a daemon that forks before opening wants), and is released by
    // `close` — including the close the kernel does for a process that dies.
    while (::flock(_fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (errno == EINTR)
            continue;
        switch (ClassifyLockFailure(errno))
        {
            case LockFailure::Contended:
                return std::unexpected(CowTreeError::InUse);
            case LockFailure::Fatal:
                return std::unexpected(CowTreeError::IoError);
            case LockFailure::Unsupported:
                // Not a failure to open. This filesystem cannot lock, so the
                // store opens with no guard against a second one — refusing
                // instead would stop a deployment that works today for a
                // reason that has nothing to do with contention. The caller
                // reads `StoreLockState()` and says so.
                _lockState = LockState::Unavailable;
                return {};
        }
    }
    _lockState = LockState::Held;
    return {};
}
#endif

auto FilePageStore::FileSizeBytes() const -> std::expected<std::uint64_t, CowTreeError>
{
#if defined(_WIN32)
    LARGE_INTEGER size;
    if (::GetFileSizeEx(_handle, &size) == 0)
        return std::unexpected(CowTreeError::IoError);
    return static_cast<std::uint64_t>(size.QuadPart);
#else
    struct stat st {};
    if (::fstat(_fd, &st) != 0)
        return std::unexpected(CowTreeError::IoError);
    return static_cast<std::uint64_t>(st.st_size);
#endif
}

auto FilePageStore::BootstrapNewFile() -> std::expected<void, CowTreeError>
{
    std::vector<std::byte> blank(_pageSize, std::byte { 0 });
    // Write two blank meta pages so the file is the canonical 2*PageSize.
    Meta blankMeta;
    blankMeta.pageSize = static_cast<std::uint32_t>(_pageSize);
    blankMeta.txnId = 0;
    blankMeta.root = PageId::None();
    blankMeta.freeRoot = PageId::None();
    blankMeta.itemCount = 0;

    auto encodeA = EncodeMeta(BytesSpan { blank.data(), blank.size() }, blankMeta);
    if (!encodeA.has_value())
        return std::unexpected(encodeA.error());
    if (auto r = WriteAt(MetaSlotOffset(MetaSlot::A), BytesView { blank.data(), blank.size() }); !r.has_value())
        return std::unexpected(r.error());

    std::ranges::fill(blank, std::byte { 0 });
    auto encodeB = EncodeMeta(BytesSpan { blank.data(), blank.size() }, blankMeta);
    if (!encodeB.has_value())
        return std::unexpected(encodeB.error());
    if (auto r = WriteAt(MetaSlotOffset(MetaSlot::B), BytesView { blank.data(), blank.size() }); !r.has_value())
        return std::unexpected(r.error());

    if (auto const r = Fsync(); !r.has_value())
        return std::unexpected(r.error());
    // Both slots hold the blank txnId 0; treat A as the durable one so the
    // first Batched flush writes to B (preserving the alternating invariant).
    _lastDurableSlot = MetaSlot::A;
    return {};
}

auto FilePageStore::RecoverExistingFile() -> std::expected<void, CowTreeError>
{
    auto const metaA = ReadMeta(MetaSlot::A);
    auto const metaB = ReadMeta(MetaSlot::B);
    if (!metaA.has_value() && !metaB.has_value())
        return std::unexpected(CowTreeError::CorruptMetas);

    // Pick the live meta AND remember which slot it came from, so the next
    // Batched flush writes to the *other* slot and never overwrites the
    // currently-durable one.
    Meta live;
    if (metaA.has_value() && metaB.has_value())
    {
        if (metaA->txnId >= metaB->txnId)
        {
            live = *metaA;
            _lastDurableSlot = MetaSlot::A;
        }
        else
        {
            live = *metaB;
            _lastDurableSlot = MetaSlot::B;
        }
    }
    else if (metaA.has_value())
    {
        live = *metaA;
        _lastDurableSlot = MetaSlot::A;
    }
    else
    {
        live = *metaB;
        _lastDurableSlot = MetaSlot::B;
    }

    if (live.pageSize != _pageSize)
    {
        // File was created with a different page size. The on-disk value wins.
        if (!IsValidPageSize(live.pageSize))
            return std::unexpected(CowTreeError::Corrupt);
        _pageSize = live.pageSize;
    }

    // Compute the number of data pages currently sized into the file.
    auto const size = FileSizeBytes();
    if (!size.has_value())
        return std::unexpected(size.error());
    auto const fileSize = *size;
    auto const dataBytes = fileSize >= (2 * _pageSize) ? (fileSize - (2 * _pageSize)) : 0;
    _totalDataPages = static_cast<std::size_t>(dataBytes / _pageSize);

    // Mark every data page index live by default; consult the on-disk
    // free-list chain to subtract recycled pages.
    for (std::uint64_t i = 1; i <= _totalDataPages; ++i)
        _live.insert(i);

    // The free-list head lives in `live.freeRoot`. Each free-list page
    // (when implemented) chains via its first 8 bytes to the next free
    // page id, with the rest holding additional free ids. For the MVP
    // we treat freeRoot itself as a leaf-pointer to a singly-linked
    // chain of free pages (one id per page).
    //
    // A visited set + iteration cap guards against a corrupted or
    // adversarial file whose `next` link forms a cycle — without the
    // guard Open() loops forever and the in-memory _freeList grows
    // without bound.
    std::unordered_set<std::uint64_t> visited;
    visited.reserve(_totalDataPages);
    auto cursor = live.freeRoot;
    while (cursor)
    {
        auto const pageIndex = cursor.value;
        if (pageIndex == 0 || pageIndex > _totalDataPages)
            return std::unexpected(CowTreeError::Corrupt);
        if (!visited.insert(pageIndex).second)
            return std::unexpected(CowTreeError::Corrupt);
        _live.erase(pageIndex);
        _freeList.push_back(pageIndex);

        // Chase the chain. Use the little-endian decode path that
        // every other on-disk field uses; a raw memcpy here would
        // disagree on a big-endian host.
        std::array<std::byte, 8> buf {};
        if (auto const r = ReadAt(DataPageOffset(cursor), BytesSpan { buf.data(), buf.size() }); !r.has_value())
            return std::unexpected(r.error());
        std::uint64_t nextRaw = 0;
        std::memcpy(&nextRaw, buf.data(), sizeof(nextRaw));
        if constexpr (std::endian::native != std::endian::little)
            nextRaw = std::byteswap(nextRaw);
        cursor = PageId { nextRaw };
    }
    return {};
}

std::uint64_t FilePageStore::DataPageOffset(PageId id) const noexcept
{
    return (2 + (id.value - 1)) * static_cast<std::uint64_t>(_pageSize);
}

std::uint64_t FilePageStore::MetaSlotOffset(MetaSlot slot) const noexcept
{
    return static_cast<std::uint64_t>(slot) * static_cast<std::uint64_t>(_pageSize);
}

auto FilePageStore::ReadAt(std::uint64_t offset, BytesSpan data) const -> std::expected<void, CowTreeError>
{
#if defined(_WIN32)
    OVERLAPPED ov {};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    if (!::ReadFile(_handle, data.data(), static_cast<DWORD>(data.size()), &read, &ov))
        return std::unexpected(CowTreeError::IoError);
    if (read != data.size())
        return std::unexpected(CowTreeError::IoError);
    return {};
#else
    std::size_t total = 0;
    while (total < data.size())
    {
        auto const n = ::pread(_fd, data.data() + total, data.size() - total, static_cast<off_t>(offset + total));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(CowTreeError::IoError);
        }
        if (n == 0)
            return std::unexpected(CowTreeError::IoError);
        total += static_cast<std::size_t>(n);
    }
    return {};
#endif
}

auto FilePageStore::WriteAt(std::uint64_t offset, BytesView data) const -> std::expected<void, CowTreeError>
{
#if defined(_WIN32)
    OVERLAPPED ov {};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD wrote = 0;
    if (!::WriteFile(_handle, data.data(), static_cast<DWORD>(data.size()), &wrote, &ov))
        return std::unexpected(CowTreeError::IoError);
    if (wrote != data.size())
        return std::unexpected(CowTreeError::IoError);
    return {};
#else
    std::size_t total = 0;
    while (total < data.size())
    {
        auto const n = ::pwrite(_fd, data.data() + total, data.size() - total, static_cast<off_t>(offset + total));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(CowTreeError::IoError);
        }
        total += static_cast<std::size_t>(n);
    }
    return {};
#endif
}

auto FilePageStore::Fsync() -> std::expected<void, CowTreeError>
{
    ++_fsyncCount;
#if defined(_WIN32)
    if (!::FlushFileBuffers(_handle))
        return std::unexpected(CowTreeError::IoError);
#else
    if (::fsync(_fd) != 0)
        return std::unexpected(CowTreeError::IoError);
#endif
    return {};
}

auto FilePageStore::Read(PageId id) const -> std::expected<BytesView, CowTreeError>
{
    if (!id || id.value > _totalDataPages)
        return std::unexpected(CowTreeError::OutOfRange);
    if (!_live.contains(id.value))
        return std::unexpected(CowTreeError::OutOfRange);

    std::scoped_lock const lock { _ioMutex };
    if (_readBufferPageIdx != id.value)
    {
        if (auto const r = ReadAt(DataPageOffset(id), BytesSpan { _readBuffer.data(), _readBuffer.size() }); !r.has_value())
            return std::unexpected(r.error());
        _readBufferPageIdx = id.value;
    }
    return BytesView { _readBuffer.data(), _readBuffer.size() };
}

auto FilePageStore::Allocate() -> std::expected<PageId, CowTreeError>
{
    std::scoped_lock const lock { _ioMutex };
    if (!_freeList.empty())
    {
        auto const idx = _freeList.back();
        _freeList.pop_back();
        _live.insert(idx);
        // Invalidate the read cache for this slot — it may hold the
        // freed page's stale bytes.
        if (_readBufferPageIdx == idx)
            _readBufferPageIdx = 0;
        return PageId { idx };
    }
    auto const newIdx = static_cast<std::uint64_t>(_totalDataPages) + 1;
    _totalDataPages = static_cast<std::size_t>(newIdx);
    _live.insert(newIdx);

    // Extend the file with zeros so a subsequent Read before Write does
    // not race against a sparse-file allocation pattern.
    std::vector<std::byte> blank(_pageSize, std::byte { 0 });
    if (auto const r = WriteAt(DataPageOffset(PageId { newIdx }), BytesView { blank.data(), blank.size() }); !r.has_value())
        return std::unexpected(r.error());
    return PageId { newIdx };
}

auto FilePageStore::Write(PageId id, BytesView data) -> std::expected<void, CowTreeError>
{
    if (!id || id.value > _totalDataPages)
        return std::unexpected(CowTreeError::OutOfRange);
    if (data.size() != _pageSize)
        return std::unexpected(CowTreeError::InvalidArg);
    std::scoped_lock const lock { _ioMutex };
    if (auto const r = WriteAt(DataPageOffset(id), data); !r.has_value())
        return std::unexpected(r.error());
    _live.insert(id.value);
    if (_readBufferPageIdx == id.value)
        _readBufferPageIdx = 0;
    if (_options.durability == Durability::Fsync)
    {
        if (auto const r = Fsync(); !r.has_value())
            return std::unexpected(r.error());
    }
    return {};
}

auto FilePageStore::Free(PageId id) -> std::expected<void, CowTreeError>
{
    if (!id || id.value > _totalDataPages)
        return std::unexpected(CowTreeError::OutOfRange);
    std::scoped_lock const lock { _ioMutex };
    if (!_live.contains(id.value))
        return std::unexpected(CowTreeError::OutOfRange);
    _live.erase(id.value);
    // Batched group-commit defers reuse until the freeing is durable (see
    // _pendingFree); other modes recycle immediately.
    if (_options.durability == Durability::Batched)
        _pendingFree.push_back(id.value);
    else
        _freeList.push_back(id.value);
    if (_readBufferPageIdx == id.value)
        _readBufferPageIdx = 0;
    return {};
}

auto FilePageStore::FlushBatchLocked() -> std::expected<void, CowTreeError>
{
    // Nothing committed since the last flush: nothing to make durable.
    if (!_pendingMeta.has_value())
    {
        _commitsSinceFlush = 0;
        return {};
    }
    // Copy the buffered meta out under the has_value() guard, before the Fsync
    // and the reset() below (so the access is provably checked and never dangles).
    auto const pending = *_pendingMeta;

    // Crash-safe group commit, in strict order:
    //   1. fsync the buffered DATA pages so they are durable before any meta
    //      references them (data pages carry no read-time checksum);
    //   2. write the meta to the slot that does NOT hold the last durable meta,
    //      so a torn write here can never destroy the recoverable copy;
    //   3. fsync the META.
    // A crash between (1) and (3) simply leaves the previous durable slot intact
    // and loses only this unflushed window — never a corrupt/unopenable store.
    if (auto const r = Fsync(); !r.has_value())
        return std::unexpected(r.error());
    auto const target = (_lastDurableSlot == MetaSlot::A) ? MetaSlot::B : MetaSlot::A;
    if (auto const r = WriteSlotLocked(target, pending); !r.has_value())
        return std::unexpected(r.error());
    if (auto const r = Fsync(); !r.has_value())
        return std::unexpected(r.error());

    _lastDurableSlot = target;
    _pendingMeta.reset();
    // The freeing is now durable, so freed pages may be recycled.
    _freeList.insert(_freeList.end(), _pendingFree.begin(), _pendingFree.end());
    _pendingFree.clear();
    _commitsSinceFlush = 0;
    return {};
}

auto FilePageStore::SyncData() -> std::expected<void, CowTreeError>
{
    // Only strict Fsync mode flushes data per commit. Batched defers the data
    // fsync to the group-commit boundary in FlushBatchLocked, which fsyncs data
    // *before* writing the meta that references it; None never flushes.
    if (_options.durability != Durability::Fsync)
        return {};
    std::scoped_lock const lock { _ioMutex };
    return Fsync();
}

auto FilePageStore::ReadMeta(MetaSlot slot) const -> std::expected<Meta, CowTreeError>
{
    std::vector<std::byte> buf(_pageSize, std::byte { 0 });
    if (auto const r = ReadAt(MetaSlotOffset(slot), BytesSpan { buf.data(), buf.size() }); !r.has_value())
        return std::unexpected(r.error());
    return DecodeMeta(BytesView { buf.data(), buf.size() });
}

auto FilePageStore::WriteSlotLocked(MetaSlot slot, Meta const& meta) -> std::expected<void, CowTreeError>
{
    std::vector<std::byte> buf(_pageSize, std::byte { 0 });
    auto effective = meta;
    effective.pageSize = static_cast<std::uint32_t>(_pageSize);
    if (auto const r = EncodeMeta(BytesSpan { buf.data(), buf.size() }, effective); !r.has_value())
        return std::unexpected(r.error());
    return WriteAt(MetaSlotOffset(slot), BytesView { buf.data(), buf.size() });
}

auto FilePageStore::WriteMeta(MetaSlot slot, Meta const& meta) -> std::expected<void, CowTreeError>
{
    std::scoped_lock const lock { _ioMutex };

    if (_options.durability == Durability::Batched)
    {
        // Group commit: buffer the latest meta in memory and make it durable
        // only at a flush boundary. We deliberately do NOT write the slot on
        // every commit. Overwriting a slot in place without an fsync (the old
        // behaviour) let the OS write back a partial page at any time, so a hard
        // crash inside the unflushed window could leave BOTH alternating slots
        // torn and the store unopenable. Deferring the write — and flushing it
        // to the slot that does not hold the last durable meta (see
        // FlushBatchLocked) — guarantees one durable, self-consistent meta is
        // always on disk. The caller-chosen `slot` is recomputed at flush time.
        _pendingMeta = meta;
        if (++_commitsSinceFlush >= BatchedFlushInterval)
            return FlushBatchLocked();
        return {};
    }

    // Fsync / None: write the caller's alternating slot immediately. In Fsync
    // mode every commit is fsynced and the other slot still holds the prior
    // durable meta, so a torn write never loses both. None never fsyncs.
    if (auto const r = WriteSlotLocked(slot, meta); !r.has_value())
        return std::unexpected(r.error());
    if (_options.durability == Durability::Fsync)
        return Fsync(); // strict: every commit is durable
    return {};
}

std::size_t FilePageStore::PageSize() const noexcept
{
    return _pageSize;
}

auto FilePageStore::Flush() -> std::expected<void, CowTreeError>
{
    // Exactly the group-commit boundary the interval would have reached on its
    // own, taken on demand: fsync the data, write the buffered meta to the
    // non-durable slot, fsync the meta, and release the pages whose freeing
    // that meta has now made durable.
    std::scoped_lock const lock { _ioMutex };
    return FlushBatchLocked();
}

std::size_t FilePageStore::PageCount() const noexcept
{
    std::scoped_lock const lock { _ioMutex };
    return _totalDataPages;
}

FilePageStore::Durability FilePageStore::DurabilityMode() const noexcept
{
    return _options.durability;
}

FilePageStore::LockState FilePageStore::StoreLockState() const noexcept
{
    return _lockState;
}

std::size_t FilePageStore::TotalDataPages() const noexcept
{
    return _totalDataPages;
}

std::size_t FilePageStore::FsyncCallCount() const noexcept
{
    return _fsyncCount;
}

void FilePageStore::SimulateCrashForTest() noexcept
{
    // Drop the OS handle WITHOUT flushing: any buffered (unflushed)
    // group-commit window is discarded, exactly as on power loss. The flag
    // stops the destructor from flushing or re-closing the handle.
    _crashedForTest = true;
#if defined(_WIN32)
    if (_handle != nullptr)
    {
        ::CloseHandle(_handle);
        _handle = nullptr;
    }
#else
    if (_fd >= 0)
    {
        ::close(_fd);
        _fd = -1;
    }
#endif
}

} // namespace CowTree
