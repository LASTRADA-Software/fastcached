// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

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
    [[nodiscard]] virtual auto Read(PageId id) const -> std::expected<BytesView, CowTreeError> = 0;

    /// Allocate a fresh page id. The page contents are unspecified until
    /// the caller writes them with Write(). Implementations may extend
    /// the underlying file or pull from a free list; either way the new
    /// PageId is unique among currently-live pages.
    /// @return Newly allocated page id, never PageId::None().
    [[nodiscard]] virtual auto Allocate() -> std::expected<PageId, CowTreeError> = 0;

    /// Write the page contents at `id`. `data.size()` must equal
    /// `PageSize()`.
    /// @param id   Destination page.
    /// @param data Page contents.
    [[nodiscard]] virtual auto Write(PageId id, BytesView data) -> std::expected<void, CowTreeError> = 0;

    /// Mark a page reusable. After this call the implementation may
    /// recycle the id from a future Allocate(); CowTree calls Free()
    /// only on pages it has already replaced in a newer transaction.
    /// @param id Page id to recycle.
    [[nodiscard]] virtual auto Free(PageId id) -> std::expected<void, CowTreeError> = 0;

    /// Persist all data-page writes done so far. Must complete before a
    /// subsequent WriteMeta() can be considered durable — the commit
    /// protocol requires (data-sync, then meta-write, then meta-sync).
    [[nodiscard]] virtual auto SyncData() -> std::expected<void, CowTreeError> = 0;

    /// Read one of the two meta pages and decode it.
    /// @param slot Meta-slot to load.
    /// @return Decoded Meta on success; CowTreeError::Corrupt if the
    ///         on-disk CRC failed (the caller may still pick the other
    ///         slot if it validates); various errors on I/O failure.
    [[nodiscard]] virtual auto ReadMeta(MetaSlot slot) const -> std::expected<Meta, CowTreeError> = 0;

    /// Write and (durably) flush the meta page. This is the single commit
    /// point: a torn write here leaves the *other* slot as the live one on
    /// recovery, so the operation is naturally atomic at the transaction
    /// granularity.
    ///
    /// **Which of the two slots is written is the store's decision, not the
    /// caller's, and there is deliberately no argument for it** (#726). That
    /// atomicity holds only if the slot written is the one NOT holding the last
    /// durable meta, and which slot that is is state only the store has --
    /// recovery records it at `Open` from the two slots it read. A caller
    /// choosing for itself has to model the alternation, and a caller that
    /// models it wrongly does not get a wrong slot, it gets a store with one
    /// good meta page overwritten by the commit that was supposed to be
    /// survivable.
    ///
    /// This used to take the slot, and `CowTree::CommitTxn` derived it from
    /// `txnId mod 2`. That is right only while the parity holds, and a batched
    /// flush breaks the parity routinely by writing the last commit's id into
    /// the alternating slot -- so on an ordinary batched-written store the first
    /// strict-durability commit after a one-slot recovery spent the survivor.
    /// @param meta Meta record to encode and write.
    [[nodiscard]] virtual auto WriteMeta(Meta const& meta) -> std::expected<void, CowTreeError> = 0;

    /// @return The slot the most recent `WriteMeta` made durable -- so the next
    ///         one targets the other. Exposed so a test can assert the
    ///         alternation itself rather than infer it from page contents.
    [[nodiscard]] virtual auto LastDurableSlot() const noexcept -> MetaSlot = 0;

    /// @return The configured data-page size in bytes. Constant for the
    ///         lifetime of the page store.
    [[nodiscard]] virtual auto PageSize() const noexcept -> std::size_t = 0;

    /// Make everything committed so far durable, now.
    ///
    /// A store that batches commits into groups holds a window of writes that
    /// are not on disk and, with them, a set of freed pages it dare not recycle
    /// yet — reusing a page whose freeing is not durable would let a crash
    /// resurrect a tree pointing at overwritten data. That window is normally
    /// closed on a fixed commit interval, which is right for a server taking
    /// small writes and wrong for a job that commits in a few large slices and
    /// wants each one to become both durable and reclaimable before it starts
    /// the next.
    ///
    /// A no-op for a store with nothing deferred.
    /// @return Empty on success; CowTreeError when the flush itself fails.
    [[nodiscard]] virtual auto Flush() -> std::expected<void, CowTreeError> = 0;

    /// Total data pages this store currently holds, live and free alike.
    ///
    /// Grows as pages are allocated and never shrinks, so it is an upper bound
    /// on how many DISTINCT pages any structure inside the store can span. That
    /// is what it exists for: a tree walk reaches each of its pages once, so a
    /// walk that has read more pages than this is following a cycle — and a
    /// cycle is precisely what a per-page CRC cannot catch, since every page
    /// around the loop is individually valid.
    /// @return The page count; zero for a store nothing has allocated in yet.
    [[nodiscard]] virtual auto PageCount() const noexcept -> std::size_t = 0;
};

} // namespace CowTree
