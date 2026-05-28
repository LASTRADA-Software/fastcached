// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

#include <cstddef>
#include <expected>

namespace CowTree
{

/// Pluggable page-storage backend for a CowTree.
///
/// The page store owns the on-disk (or in-memory) representation of data
/// pages and the two alternating meta pages. CowTree calls into this
/// interface to read existing pages, allocate fresh ones, write modified
/// ones, free pages no longer referenced after a commit, and fsync.
///
/// **Lifetime contract**: BytesView returned by `Read()` remains valid
/// until the next call that mutates the page (`Write`, `Allocate`,
/// `Free`) or the page store itself is destroyed. Implementations may
/// satisfy this with internal caching or by copying into a stable
/// buffer.
///
/// **Threading**: single-writer / multi-reader. CowTree serialises
/// writers and overlaps readers; implementations need not provide
/// additional locking unless they choose to.
class IPageStore
{
  public:
    IPageStore() = default;
    IPageStore(IPageStore const&) = delete;
    IPageStore(IPageStore&&) = delete;
    IPageStore& operator=(IPageStore const&) = delete;
    IPageStore& operator=(IPageStore&&) = delete;
    virtual ~IPageStore() = default;

    /// Return a read-only view of the page at `id`.
    /// @param id Page id; must be a previously-allocated page.
    /// @return BytesView over exactly `PageSize()` bytes, or
    ///         CowTreeError::OutOfRange if `id` is unknown / out of range,
    ///         CowTreeError::IoError on backend failure.
    [[nodiscard]] virtual auto Read(PageId id) const
        -> std::expected<BytesView, CowTreeError> = 0;

    /// Allocate a fresh page id. The page contents are unspecified until
    /// the caller writes them with Write(). Implementations may extend
    /// the underlying file or pull from a free list; either way the new
    /// PageId is unique among currently-live pages.
    /// @return Newly allocated page id, never PageId::None().
    [[nodiscard]] virtual auto Allocate()
        -> std::expected<PageId, CowTreeError> = 0;

    /// Write the page contents at `id`. `data.size()` must equal
    /// `PageSize()`.
    /// @param id   Destination page.
    /// @param data Page contents.
    [[nodiscard]] virtual auto Write(PageId id, BytesView data)
        -> std::expected<void, CowTreeError> = 0;

    /// Mark a page reusable. After this call the implementation may
    /// recycle the id from a future Allocate(); CowTree calls Free()
    /// only on pages it has already replaced in a newer transaction.
    /// @param id Page id to recycle.
    [[nodiscard]] virtual auto Free(PageId id)
        -> std::expected<void, CowTreeError> = 0;

    /// Persist all data-page writes done so far. Must complete before a
    /// subsequent WriteMeta() can be considered durable — the commit
    /// protocol requires (data-sync, then meta-write, then meta-sync).
    [[nodiscard]] virtual auto SyncData()
        -> std::expected<void, CowTreeError> = 0;

    /// Read one of the two meta pages and decode it.
    /// @param slot Meta-slot to load.
    /// @return Decoded Meta on success; CowTreeError::Corrupt if the
    ///         on-disk CRC failed (the caller may still pick the other
    ///         slot if it validates); various errors on I/O failure.
    [[nodiscard]] virtual auto ReadMeta(MetaSlot slot) const
        -> std::expected<Meta, CowTreeError> = 0;

    /// Write and (durably) flush one of the two meta pages. This is the
    /// single commit point: a torn write here leaves the *other* slot
    /// as the live one on recovery, so the operation is naturally
    /// atomic at the transaction granularity.
    /// @param slot Meta-slot to overwrite.
    /// @param meta Meta record to encode and write.
    [[nodiscard]] virtual auto WriteMeta(MetaSlot slot, Meta const& meta)
        -> std::expected<void, CowTreeError> = 0;

    /// @return The configured data-page size in bytes. Constant for the
    ///         lifetime of the page store.
    [[nodiscard]] virtual auto PageSize() const noexcept -> std::size_t = 0;
};

} // namespace CowTree
