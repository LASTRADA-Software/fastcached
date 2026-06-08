// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/IPageStore.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

namespace CowTree
{

class WriteTxn;
class ReadTxn;
class CowTree;

/// Read transaction over a CowTree.
///
/// Captures the root + transaction id of the tree at the moment of
/// `BeginRead()`. Look-ups consult exactly that snapshot — even if a
/// writer commits a newer transaction in the meantime. Construction is
/// O(1) (just an atomic load) which is what makes Snapshot() cheap.
class ReadTxn
{
  public:
    /// Construct an empty (invalid) read transaction.
    ReadTxn() = default;

    /// Look up a key under this snapshot.
    /// @param key Lookup key.
    /// @return The value bytes (owned copy) when present, std::nullopt on
    ///         miss; CowTreeError on I/O or corruption.
    [[nodiscard]] auto Get(BytesView key) const -> std::expected<std::optional<std::vector<std::byte>>, CowTreeError>;

    /// @return The txnId of the snapshot this transaction observes.
    [[nodiscard]] TxnId Snapshot() const noexcept
    {
        return _txnId;
    }

    /// @return The root page id of the snapshot (PageId::None() if the
    ///         tree was empty when this transaction began).
    [[nodiscard]] PageId Root() const noexcept
    {
        return _root;
    }

    /// @return True if the transaction holds a valid snapshot.
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return _store != nullptr;
    }

  private:
    friend class CowTree;

    ReadTxn(IPageStore* store, PageId root, TxnId txnId) noexcept:
        _store { store },
        _root { root },
        _txnId { txnId }
    {
    }

    IPageStore* _store { nullptr };
    PageId _root { PageId::None() };
    TxnId _txnId { 0 };
};

/// Write transaction over a CowTree.
///
/// Holds the pending root for the in-flight transaction. `Put`/`Erase`
/// allocate fresh pages along the modified path (copy-on-write). The
/// transaction is observable only after `Commit()` writes the new meta
/// page; `Abort()` frees the new-page chain.
class WriteTxn
{
  public:
    WriteTxn() = default;
    WriteTxn(WriteTxn const&) = delete;
    WriteTxn& operator=(WriteTxn const&) = delete;
    WriteTxn(WriteTxn&& other) noexcept;
    WriteTxn& operator=(WriteTxn&& other) noexcept;

    /// Discards the new-page chain if `Commit()` was never called.
    ~WriteTxn();

    /// Insert or replace `key` with `value`.
    /// @param key   Insertion key.
    /// @param value New value bytes.
    /// @return The previous value bytes when `key` already existed (letting the
    ///         caller reclaim any out-of-line backing the old record named
    ///         without a separate read transaction), or `std::nullopt` when the
    ///         key was newly inserted; CowTreeError::ValueTooLarge if a single
    ///         key+value pair exceeds the per-page payload limit.
    [[nodiscard]] auto Put(BytesView key, BytesView value)
        -> std::expected<std::optional<std::vector<std::byte>>, CowTreeError>;

    /// Remove `key`.
    /// @return true if a key was removed, false if it was already absent.
    [[nodiscard]] auto Erase(BytesView key) -> std::expected<bool, CowTreeError>;

    /// Atomically commit this transaction: data-sync new pages, write
    /// the meta page that names the new root.
    /// @return The committed txnId on success.
    [[nodiscard]] auto Commit() -> std::expected<TxnId, CowTreeError>;

    /// Discard the in-flight transaction. The live root is unchanged
    /// and the freshly allocated pages are returned to the free list.
    void Abort() noexcept;

    /// @return True iff the transaction is still pending (no commit/abort yet).
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return _tree != nullptr;
    }

  private:
    friend class CowTree;

    WriteTxn(CowTree* tree, PageId root, TxnId baseTxn) noexcept;

    CowTree* _tree { nullptr };

    /// Working root for the transaction. Replaced on every Put/Erase
    /// that touches the root-to-leaf path.
    PageId _root { PageId::None() };

    /// Pages allocated by this transaction so far. Used to roll back on
    /// Abort.
    std::vector<PageId> _newPages;

    /// Pages from the previous root chain that this transaction is
    /// retiring. Freed on Commit.
    std::vector<PageId> _freedPages;

    /// Logical entry count delta applied by this transaction.
    std::int64_t _itemDelta { 0 };

    /// The transaction id this build is targeting (one past the snapshot).
    TxnId _newTxnId { 0 };
};

/// Persistent copy-on-write B+tree backed by an `IPageStore`.
///
/// Single-writer / multi-reader: at most one WriteTxn exists at any
/// time, while any number of ReadTxn handles may coexist with the writer
/// and with each other. Snapshots are O(1) (capture root + txnId only).
///
/// The on-disk commit protocol is:
///   1. Write all new data pages (via IPageStore::Write).
///   2. IPageStore::SyncData() — flush data pages.
///   3. Encode new Meta naming the new root + txnId + freeRoot.
///   4. IPageStore::WriteMeta(slot = txnId mod 2, ...) — single-page
///      durable write. This is the atomic commit point.
/// Recovery picks the meta slot with the higher txnId and valid CRC; a
/// torn meta write leaves the previous slot intact and rolls back the
/// in-flight transaction.
///
/// Pages freed by a committed transaction are returned to the free list
/// immediately. This means held ReadTxn snapshots can be invalidated by
/// a subsequent commit + page-reuse cycle; the cache use case never
/// holds a snapshot across a writer call so this is acceptable. The
/// limitation is documented in `README.md`.
class CowTree
{
  public:
    /// Construct over a page store. The store must outlive this tree.
    /// @param store Backing page storage.
    explicit CowTree(IPageStore& store) noexcept;

    CowTree(CowTree const&) = delete;
    CowTree(CowTree&&) = delete;
    CowTree& operator=(CowTree const&) = delete;
    CowTree& operator=(CowTree&&) = delete;
    ~CowTree() = default;

    /// Open the tree. Picks the meta slot with the higher (valid-CRC)
    /// txnId; initialises empty if both metas are blank (txnId 0).
    /// @return Empty on success; CowTreeError::CorruptMetas if neither
    ///         meta validates.
    [[nodiscard]] auto Open() -> std::expected<void, CowTreeError>;

    /// Begin a read transaction over the currently-committed root.
    [[nodiscard]] ReadTxn BeginRead() const noexcept;

    /// Alias for BeginRead — emphasises that the captured root is a
    /// snapshot pinned at this moment.
    [[nodiscard]] ReadTxn Snapshot() const noexcept
    {
        return BeginRead();
    }

    /// Begin a write transaction. There must be no other live write
    /// transaction (single-writer contract).
    [[nodiscard]] WriteTxn BeginWrite();

    /// @return Approximate item count from the last committed meta.
    [[nodiscard]] std::uint64_t ItemCount() const noexcept;

    /// @return Page size of the backing store.
    [[nodiscard]] std::size_t PageSize() const noexcept;

  private:
    friend class WriteTxn;

    IPageStore& _store;
    PageId _liveRoot { PageId::None() };
    TxnId _liveTxn { 0 };
    PageId _liveFreeRoot { PageId::None() };
    std::uint64_t _liveItemCount { 0 };
    bool _opened { false };

    /// Look up `key` starting from `root`.
    [[nodiscard]] auto Lookup(PageId root, BytesView key) const
        -> std::expected<std::optional<std::vector<std::byte>>, CowTreeError>;

    /// Apply Put recursively. Returns the new (root of subtree) page id,
    /// and optionally a (separator, right-sibling) pair if the subtree
    /// split during the insert.
    struct PutResult
    {
        PageId left;
        std::optional<std::pair<std::vector<std::byte>, PageId>> split;
        bool inserted { false };                           ///< True when the key was new (vs. replaced).
        std::optional<std::vector<std::byte>> replaced {}; ///< Previous value bytes when the key was replaced.
    };

    [[nodiscard]] auto PutRec(WriteTxn& txn, PageId node, BytesView key, BytesView value)
        -> std::expected<PutResult, CowTreeError>;

    /// Apply Erase recursively. Returns the (possibly new) subtree root
    /// and a flag indicating whether the key was found.
    struct EraseResult
    {
        PageId left;
        bool erased { false };
    };

    [[nodiscard]] auto EraseRec(WriteTxn& txn, PageId node, BytesView key) -> std::expected<EraseResult, CowTreeError>;

    /// Allocate a new page and remember it for rollback on abort.
    [[nodiscard]] auto AllocateForTxn(WriteTxn& txn) -> std::expected<PageId, CowTreeError>;

    /// Commit the pending transaction (called from WriteTxn::Commit).
    [[nodiscard]] auto CommitTxn(WriteTxn& txn) -> std::expected<TxnId, CowTreeError>;

    /// Abort the pending transaction (called from WriteTxn::Abort /
    /// destructor).
    void AbortTxn(WriteTxn& txn) noexcept;

    /// Build a fresh leaf page from the given entries; returns the new
    /// page id (or a split if the entries didn't fit).
    [[nodiscard]] auto WriteLeaf(WriteTxn& txn,
                                 std::span<std::pair<std::vector<std::byte>, std::vector<std::byte>> const> entries)
        -> std::expected<PutResult, CowTreeError>;

    /// Build a fresh internal page from the given children + separators.
    [[nodiscard]] auto WriteInternal(WriteTxn& txn,
                                     PageId firstChild,
                                     std::span<std::pair<std::vector<std::byte>, PageId> const> entries)
        -> std::expected<PutResult, CowTreeError>;
};

} // namespace CowTree
