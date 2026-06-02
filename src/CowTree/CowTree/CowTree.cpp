// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <CowTree/CowTree.hpp>
#include <CowTree/PageLayout.hpp>

namespace CowTree
{

namespace
{

    /// Lexicographic byte comparison; returns <0, 0, >0 like memcmp.
    int CompareBytes(BytesView a, BytesView b) noexcept
    {
        auto const n = std::min(a.size(), b.size());
        if (n != 0)
        {
            auto const cmp = std::memcmp(a.data(), b.data(), n);
            if (cmp != 0)
                return cmp;
        }
        if (a.size() < b.size())
            return -1;
        if (a.size() > b.size())
            return 1;
        return 0;
    }

    /// Copy a BytesView into a fresh std::vector<std::byte>.
    std::vector<std::byte> CopyBytes(BytesView src)
    {
        return { src.begin(), src.end() };
    }

    /// View bytes from a std::vector.
    BytesView View(std::vector<std::byte> const& v) noexcept
    {
        return BytesView { v.data(), v.size() };
    }

} // namespace

// ============================================================================
// ReadTxn
// ============================================================================

auto ReadTxn::Get(BytesView key) const -> std::expected<std::optional<std::vector<std::byte>>, CowTreeError>
{
    if (_store == nullptr)
        return std::unexpected(CowTreeError::NotOpen);
    if (!_root)
        return std::optional<std::vector<std::byte>> {};

    auto cursor = _root;
    while (cursor)
    {
        auto page = _store->Read(cursor);
        if (!page.has_value())
            return std::unexpected(page.error());
        auto header = DecodePageHeader(*page);
        if (!header.has_value())
            return std::unexpected(header.error());

        if (header->type == PageType::Leaf)
        {
            auto entries = DecodeLeafEntries(*page, *header);
            if (!entries.has_value())
                return std::unexpected(entries.error());
            for (auto const& e: *entries)
            {
                auto const cmp = CompareBytes(e.key, key);
                if (cmp == 0)
                    return std::optional<std::vector<std::byte>> { CopyBytes(e.value) };
                if (cmp > 0)
                    return std::optional<std::vector<std::byte>> {};
            }
            return std::optional<std::vector<std::byte>> {};
        }

        // Internal: descend.
        auto entries = DecodeInternalEntries(*page, *header);
        if (!entries.has_value())
            return std::unexpected(entries.error());
        // first matching child is the entry whose key > target, else last child.
        // Recall: header.firstChild covers keys < entries[0].key
        // entries[i].child covers keys in [entries[i].key, entries[i+1].key)
        PageId next = header->firstChild;
        for (auto const& e: *entries)
        {
            if (CompareBytes(key, e.key) >= 0)
                next = e.child;
            else
                break;
        }
        cursor = next;
    }
    return std::optional<std::vector<std::byte>> {};
}

// ============================================================================
// WriteTxn
// ============================================================================

WriteTxn::WriteTxn(CowTree* tree, PageId root, TxnId baseTxn) noexcept:
    _tree { tree },
    _root { root },
    _newTxnId { baseTxn + 1 }
{
}

WriteTxn::WriteTxn(WriteTxn&& other) noexcept:
    _tree { std::exchange(other._tree, nullptr) },
    _root { other._root },
    _newPages { std::move(other._newPages) },
    _freedPages { std::move(other._freedPages) },
    _itemDelta { other._itemDelta },
    _newTxnId { other._newTxnId }
{
}

WriteTxn& WriteTxn::operator=(WriteTxn&& other) noexcept
{
    if (this != &other)
    {
        Abort();
        _tree = std::exchange(other._tree, nullptr);
        _root = other._root;
        _newPages = std::move(other._newPages);
        _freedPages = std::move(other._freedPages);
        _itemDelta = other._itemDelta;
        _newTxnId = other._newTxnId;
    }
    return *this;
}

WriteTxn::~WriteTxn()
{
    Abort();
}

auto WriteTxn::Put(BytesView key, BytesView value) -> std::expected<void, CowTreeError>
{
    if (_tree == nullptr)
        return std::unexpected(CowTreeError::NotOpen);
    auto result = _tree->PutRec(*this, _root, key, value);
    if (!result.has_value())
        return std::unexpected(result.error());

    if (result->split.has_value())
    {
        // Root split: build a new internal root holding the two halves.
        std::vector<std::pair<std::vector<std::byte>, PageId>> entries;
        entries.emplace_back(std::move(result->split->first), result->split->second);
        auto internal = _tree->WriteInternal(*this, result->left, std::span { entries.data(), entries.size() });
        if (!internal.has_value())
            return std::unexpected(internal.error());
        _root = internal->left;
    }
    else
    {
        _root = result->left;
    }
    if (result->inserted)
        ++_itemDelta;
    return {};
}

auto WriteTxn::Erase(BytesView key) -> std::expected<bool, CowTreeError>
{
    if (_tree == nullptr)
        return std::unexpected(CowTreeError::NotOpen);
    if (!_root)
        return false;
    auto result = _tree->EraseRec(*this, _root, key);
    if (!result.has_value())
        return std::unexpected(result.error());
    _root = result->left;
    if (result->erased)
        --_itemDelta;
    return result->erased;
}

auto WriteTxn::Commit() -> std::expected<TxnId, CowTreeError>
{
    if (_tree == nullptr)
        return std::unexpected(CowTreeError::NotOpen);
    return _tree->CommitTxn(*this);
}

void WriteTxn::Abort() noexcept
{
    if (_tree == nullptr)
        return;
    _tree->AbortTxn(*this);
    _tree = nullptr;
}

// ============================================================================
// CowTree
// ============================================================================

CowTree::CowTree(IPageStore& store) noexcept:
    _store { store }
{
}

auto CowTree::Open() -> std::expected<void, CowTreeError>
{
    if (_opened)
        return std::unexpected(CowTreeError::AlreadyOpen);

    auto const metaA = _store.ReadMeta(MetaSlot::A);
    auto const metaB = _store.ReadMeta(MetaSlot::B);
    if (!metaA.has_value() && !metaB.has_value())
        return std::unexpected(CowTreeError::CorruptMetas);

    Meta live;
    if (metaA.has_value() && metaB.has_value())
        live = (metaA->txnId >= metaB->txnId) ? *metaA : *metaB;
    else if (metaA.has_value())
        live = *metaA;
    else
        live = *metaB;

    _liveRoot = live.root;
    _liveTxn = live.txnId;
    _liveFreeRoot = live.freeRoot;
    _liveItemCount = live.itemCount;
    _opened = true;
    return {};
}

ReadTxn CowTree::BeginRead() const noexcept
{
    return ReadTxn { &_store, _liveRoot, _liveTxn };
}

WriteTxn CowTree::BeginWrite()
{
    return WriteTxn { this, _liveRoot, _liveTxn };
}

std::uint64_t CowTree::ItemCount() const noexcept
{
    return _liveItemCount;
}

std::size_t CowTree::PageSize() const noexcept
{
    return _store.PageSize();
}

auto CowTree::Lookup(PageId root, BytesView key) const -> std::expected<std::optional<std::vector<std::byte>>, CowTreeError>
{
    ReadTxn r { &_store, root, _liveTxn };
    return r.Get(key);
}

auto CowTree::AllocateForTxn(WriteTxn& txn) -> std::expected<PageId, CowTreeError>
{
    auto id = _store.Allocate();
    if (!id.has_value())
        return std::unexpected(id.error());
    txn._newPages.push_back(*id);
    return *id;
}

auto CowTree::WriteLeaf(WriteTxn& txn, std::span<std::pair<std::vector<std::byte>, std::vector<std::byte>> const> entries)
    -> std::expected<PutResult, CowTreeError>
{
    // Build a leaf page from `entries`. If the entries don't fit, split
    // into two leaves and return a (separator, right-page) pair.
    auto const pageSize = _store.PageSize();
    auto const capacity = PagePayloadCapacity(pageSize);

    std::size_t totalBytes = 0;
    for (auto const& [k, v]: entries)
        totalBytes += LeafEntryBytes(k.size(), v.size());

    auto buildOne = [&](std::span<std::pair<std::vector<std::byte>, std::vector<std::byte>> const> slice)
        -> std::expected<PageId, CowTreeError> {
        auto idResult = AllocateForTxn(txn);
        if (!idResult.has_value())
            return std::unexpected(idResult.error());
        // Build the page contents in a local buffer; we'll Write() it
        // into the store below. Avoids any lifetime issues with the
        // store's internal read buffer.
        std::vector<std::byte> buf(pageSize, std::byte { 0 });
        std::vector<LeafEntry> leafEntries;
        leafEntries.reserve(slice.size());
        for (auto const& [k, v]: slice)
            leafEntries.push_back({ View(k), View(v) });
        auto encoded = EncodeLeafPage(BytesSpan { buf.data(), buf.size() },
                                      std::span<LeafEntry const> { leafEntries.data(), leafEntries.size() });
        if (!encoded.has_value())
            return std::unexpected(encoded.error());
        auto wrote = _store.Write(*idResult, BytesView { buf.data(), buf.size() });
        if (!wrote.has_value())
            return std::unexpected(wrote.error());
        return *idResult;
    };

    if (totalBytes <= capacity)
    {
        auto id = buildOne(entries);
        if (!id.has_value())
            return std::unexpected(id.error());
        return PutResult { .left = *id, .split = std::nullopt, .inserted = false };
    }

    // Split point: walk forward, accumulating until adding the next entry would
    // overflow. Guarantee both halves are non-empty.
    std::size_t splitIndex = 0;
    std::size_t leftBytes = 0;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        auto const need = LeafEntryBytes(entries[i].first.size(), entries[i].second.size());
        if (leftBytes + need > capacity && i > 0)
        {
            splitIndex = i;
            break;
        }
        leftBytes += need;
        splitIndex = i + 1;
    }
    if (splitIndex == 0 || splitIndex >= entries.size())
        return std::unexpected(CowTreeError::ValueTooLarge);

    auto leftId = buildOne(entries.subspan(0, splitIndex));
    if (!leftId.has_value())
        return std::unexpected(leftId.error());
    auto rightId = buildOne(entries.subspan(splitIndex));
    if (!rightId.has_value())
        return std::unexpected(rightId.error());

    // Separator: the first key on the right half (B+tree leaf split).
    auto sepKey = entries[splitIndex].first;
    PutResult res;
    res.left = *leftId;
    res.split = std::make_pair(std::move(sepKey), *rightId);
    return res;
}

auto CowTree::WriteInternal(WriteTxn& txn,
                            PageId firstChild,
                            std::span<std::pair<std::vector<std::byte>, PageId> const> entries)
    -> std::expected<PutResult, CowTreeError>
{
    auto const pageSize = _store.PageSize();
    auto const capacity = PagePayloadCapacity(pageSize);

    std::size_t totalBytes = 0;
    for (auto const& [k, _]: entries)
        totalBytes += InternalEntryBytes(k.size());

    auto buildOne =
        [&](PageId leftmost,
            std::span<std::pair<std::vector<std::byte>, PageId> const> slice) -> std::expected<PageId, CowTreeError> {
        auto idResult = AllocateForTxn(txn);
        if (!idResult.has_value())
            return std::unexpected(idResult.error());
        std::vector<std::byte> buf(pageSize, std::byte { 0 });
        std::vector<InternalEntry> ies;
        ies.reserve(slice.size());
        for (auto const& [k, c]: slice)
            ies.push_back({ View(k), c });
        auto encoded = EncodeInternalPage(
            BytesSpan { buf.data(), buf.size() }, leftmost, std::span<InternalEntry const> { ies.data(), ies.size() });
        if (!encoded.has_value())
            return std::unexpected(encoded.error());
        auto wrote = _store.Write(*idResult, BytesView { buf.data(), buf.size() });
        if (!wrote.has_value())
            return std::unexpected(wrote.error());
        return *idResult;
    };

    if (totalBytes <= capacity)
    {
        auto id = buildOne(firstChild, entries);
        if (!id.has_value())
            return std::unexpected(id.error());
        return PutResult { .left = *id, .split = std::nullopt, .inserted = false };
    }

    // Split: walk forward; one entry "moves up" as the separator.
    std::size_t splitIndex = 0;
    std::size_t leftBytes = 0;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        auto const need = InternalEntryBytes(entries[i].first.size());
        if (leftBytes + need > capacity && i > 0)
        {
            splitIndex = i;
            break;
        }
        leftBytes += need;
        splitIndex = i + 1;
    }
    if (splitIndex == 0 || splitIndex >= entries.size())
        return std::unexpected(CowTreeError::ValueTooLarge);

    auto leftId = buildOne(firstChild, entries.subspan(0, splitIndex));
    if (!leftId.has_value())
        return std::unexpected(leftId.error());

    // The separator that goes up to the parent is entries[splitIndex].first,
    // and its child becomes the rightmost-firstChild of the new right page.
    auto sepKey = entries[splitIndex].first;
    auto rightFirstChild = entries[splitIndex].second;
    auto rightSlice = entries.subspan(splitIndex + 1);

    auto rightId = buildOne(rightFirstChild, rightSlice);
    if (!rightId.has_value())
        return std::unexpected(rightId.error());

    PutResult res;
    res.left = *leftId;
    res.split = std::make_pair(std::move(sepKey), *rightId);
    return res;
}

auto CowTree::PutRec(WriteTxn& txn, PageId node, BytesView key, BytesView value) -> std::expected<PutResult, CowTreeError>
{
    if (LeafEntryBytes(key.size(), value.size()) > PagePayloadCapacity(_store.PageSize()))
        return std::unexpected(CowTreeError::ValueTooLarge);

    if (!node)
    {
        // Empty tree: create a single-entry leaf.
        std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> entries;
        entries.emplace_back(CopyBytes(key), CopyBytes(value));
        auto leaf = WriteLeaf(txn, std::span { entries.data(), entries.size() });
        if (!leaf.has_value())
            return std::unexpected(leaf.error());
        leaf->inserted = true;
        return leaf;
    }

    auto page = _store.Read(node);
    if (!page.has_value())
        return std::unexpected(page.error());
    auto header = DecodePageHeader(*page);
    if (!header.has_value())
        return std::unexpected(header.error());

    if (header->type == PageType::Leaf)
    {
        auto existing = DecodeLeafEntries(*page, *header);
        if (!existing.has_value())
            return std::unexpected(existing.error());

        std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> entries;
        entries.reserve(existing->size() + 1);
        bool replaced = false;
        bool inserted = false;
        for (auto const& e: *existing)
        {
            auto const cmp = CompareBytes(e.key, key);
            if (!replaced && !inserted && cmp == 0)
            {
                entries.emplace_back(CopyBytes(key), CopyBytes(value));
                replaced = true;
            }
            else if (!replaced && !inserted && cmp > 0)
            {
                entries.emplace_back(CopyBytes(key), CopyBytes(value));
                inserted = true;
                entries.emplace_back(CopyBytes(e.key), CopyBytes(e.value));
            }
            else
            {
                entries.emplace_back(CopyBytes(e.key), CopyBytes(e.value));
            }
        }
        if (!replaced && !inserted)
        {
            entries.emplace_back(CopyBytes(key), CopyBytes(value));
            inserted = true;
        }

        // Retire the old page (free on commit).
        txn._freedPages.push_back(node);

        auto leaf = WriteLeaf(txn, std::span { entries.data(), entries.size() });
        if (!leaf.has_value())
            return std::unexpected(leaf.error());
        leaf->inserted = inserted;
        return leaf;
    }

    // Internal. Decode into owned bytes BEFORE recursing — the recursive
    // call may invoke Read() on other pages and invalidate the BytesView
    // backing of *page (some IPageStore implementations cache a single
    // page buffer).
    auto existingViews = DecodeInternalEntries(*page, *header);
    if (!existingViews.has_value())
        return std::unexpected(existingViews.error());
    PageId const firstChildSnapshot = header->firstChild;

    std::vector<std::pair<std::vector<std::byte>, PageId>> existingOwned;
    existingOwned.reserve(existingViews->size());
    for (auto const& e: *existingViews)
        existingOwned.emplace_back(CopyBytes(e.key), e.child);

    // Choose the child whose subtree should contain `key`.
    // Convention: firstChildSnapshot contains keys < existingOwned[0].key
    //             existingOwned[i].second contains keys in [existingOwned[i].first, existingOwned[i+1].first)
    // childIndex == std::nullopt means "use firstChildSnapshot".
    std::optional<std::size_t> childIndex;
    for (std::size_t i = 0; i < existingOwned.size(); ++i)
    {
        if (CompareBytes(View(existingOwned[i].first), key) <= 0)
            childIndex = i;
        else
            break;
    }
    PageId childPage = childIndex.has_value() ? existingOwned[*childIndex].second : firstChildSnapshot;

    auto sub = PutRec(txn, childPage, key, value);
    if (!sub.has_value())
        return std::unexpected(sub.error());

    // Retire the old internal page; build a new one with the updated child.
    txn._freedPages.push_back(node);

    std::vector<std::pair<std::vector<std::byte>, PageId>> entries;
    entries.reserve(existingOwned.size() + 1);

    PageId newFirstChild = firstChildSnapshot;
    if (!childIndex.has_value())
        newFirstChild = sub->left;

    for (std::size_t i = 0; i < existingOwned.size(); ++i)
    {
        PageId childPtr = existingOwned[i].second;
        if (childIndex.has_value() && i == *childIndex)
            childPtr = sub->left;
        entries.emplace_back(std::move(existingOwned[i].first), childPtr);
    }

    // If the child split, insert the new (separator, right) entry right
    // after the modified slot. The right page covers keys >= separator.
    if (sub->split.has_value())
    {
        std::size_t const insertAt = childIndex.has_value() ? (*childIndex + 1) : 0;
        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(insertAt),
                       std::make_pair(std::move(sub->split->first), sub->split->second));
    }

    auto built = WriteInternal(txn, newFirstChild, std::span { entries.data(), entries.size() });
    if (!built.has_value())
        return std::unexpected(built.error());
    built->inserted = sub->inserted;
    return built;
}

auto CowTree::EraseRec(WriteTxn& txn, PageId node, BytesView key) -> std::expected<EraseResult, CowTreeError>
{
    if (!node)
        return EraseResult { .left = PageId::None(), .erased = false };

    auto page = _store.Read(node);
    if (!page.has_value())
        return std::unexpected(page.error());
    auto header = DecodePageHeader(*page);
    if (!header.has_value())
        return std::unexpected(header.error());

    if (header->type == PageType::Leaf)
    {
        auto existing = DecodeLeafEntries(*page, *header);
        if (!existing.has_value())
            return std::unexpected(existing.error());

        std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> entries;
        entries.reserve(existing->size());
        bool erased = false;
        for (auto const& e: *existing)
        {
            if (!erased && CompareBytes(e.key, key) == 0)
            {
                erased = true;
                continue;
            }
            entries.emplace_back(CopyBytes(e.key), CopyBytes(e.value));
        }
        if (!erased)
            return EraseResult { .left = node, .erased = false };

        // Retire the old leaf.
        txn._freedPages.push_back(node);

        if (entries.empty())
        {
            // Leaf becomes empty. Return None — parent collapses this child.
            return EraseResult { .left = PageId::None(), .erased = true };
        }

        auto leaf = WriteLeaf(txn, std::span { entries.data(), entries.size() });
        if (!leaf.has_value())
            return std::unexpected(leaf.error());
        if (leaf->split.has_value())
        {
            // Erase made the page bigger? Shouldn't happen — but if so, treat
            // as an error since the parent path here can't absorb a split.
            return std::unexpected(CowTreeError::Corrupt);
        }
        return EraseResult { .left = leaf->left, .erased = true };
    }

    // Internal: descend. Copy entries to owned bytes BEFORE recursing,
    // since the recursive call may invalidate the page buffer underlying
    // the BytesViews.
    auto existingViews = DecodeInternalEntries(*page, *header);
    if (!existingViews.has_value())
        return std::unexpected(existingViews.error());
    PageId const firstChildSnapshot = header->firstChild;

    std::vector<std::pair<std::vector<std::byte>, PageId>> existingOwned;
    existingOwned.reserve(existingViews->size());
    for (auto const& e: *existingViews)
        existingOwned.emplace_back(CopyBytes(e.key), e.child);

    // childIndex == std::nullopt means "use firstChildSnapshot".
    std::optional<std::size_t> childIndex;
    for (std::size_t i = 0; i < existingOwned.size(); ++i)
    {
        if (CompareBytes(View(existingOwned[i].first), key) <= 0)
            childIndex = i;
        else
            break;
    }
    PageId childPage = childIndex.has_value() ? existingOwned[*childIndex].second : firstChildSnapshot;

    auto sub = EraseRec(txn, childPage, key);
    if (!sub.has_value())
        return std::unexpected(sub.error());

    if (!sub->erased)
        return EraseResult { .left = node, .erased = false };

    // Retire old internal page.
    txn._freedPages.push_back(node);

    std::vector<std::pair<std::vector<std::byte>, PageId>> entries;
    entries.reserve(existingOwned.size());
    PageId newFirstChild = firstChildSnapshot;

    if (sub->left == PageId::None())
    {
        // Child collapsed. Remove the entry that pointed to it; the
        // child to its right (if any) absorbs the range.
        if (!childIndex.has_value())
        {
            // firstChild was removed. Promote the next entry's child as
            // the new firstChild and drop that entry.
            if (existingOwned.empty())
                return EraseResult { .left = PageId::None(), .erased = true };
            newFirstChild = existingOwned[0].second;
            for (std::size_t i = 1; i < existingOwned.size(); ++i)
                entries.emplace_back(std::move(existingOwned[i].first), existingOwned[i].second);
        }
        else
        {
            // Drop existingOwned[*childIndex].
            for (std::size_t i = 0; i < existingOwned.size(); ++i)
            {
                if (i == *childIndex)
                    continue;
                entries.emplace_back(std::move(existingOwned[i].first), existingOwned[i].second);
            }
        }
    }
    else
    {
        // Rebuild with updated child pointer.
        if (!childIndex.has_value())
            newFirstChild = sub->left;
        for (std::size_t i = 0; i < existingOwned.size(); ++i)
        {
            PageId childPtr = existingOwned[i].second;
            if (childIndex.has_value() && i == *childIndex)
                childPtr = sub->left;
            entries.emplace_back(std::move(existingOwned[i].first), childPtr);
        }
    }

    // If this internal page now has no entries AND we still have a firstChild,
    // collapse the level: return the firstChild directly.
    if (entries.empty())
        return EraseResult { .left = newFirstChild, .erased = true };

    auto built = WriteInternal(txn, newFirstChild, std::span { entries.data(), entries.size() });
    if (!built.has_value())
        return std::unexpected(built.error());
    if (built->split.has_value())
        return std::unexpected(CowTreeError::Corrupt);
    return EraseResult { .left = built->left, .erased = true };
}

auto CowTree::CommitTxn(WriteTxn& txn) -> std::expected<TxnId, CowTreeError>
{
    // 1. Sync data pages.
    if (auto const r = _store.SyncData(); !r.has_value())
        return std::unexpected(r.error());

    // 2. Write the new meta page (alternating slot).
    Meta next;
    next.pageSize = static_cast<std::uint32_t>(_store.PageSize());
    next.txnId = txn._newTxnId;
    next.root = txn._root;
    next.freeRoot = PageId::None(); // free list is in-memory only for v1
    auto const newItemCount = static_cast<std::int64_t>(_liveItemCount) + txn._itemDelta;
    next.itemCount = static_cast<std::uint64_t>(std::max<std::int64_t>(0, newItemCount));

    auto const slot = (next.txnId % 2 == 0) ? MetaSlot::A : MetaSlot::B;
    if (auto const r = _store.WriteMeta(slot, next); !r.has_value())
        return std::unexpected(r.error());

    // 3. Freed pages can now be reused.
    for (auto const fp: txn._freedPages)
        std::ignore = _store.Free(fp);

    _liveRoot = next.root;
    _liveTxn = next.txnId;
    _liveFreeRoot = next.freeRoot;
    _liveItemCount = next.itemCount;

    txn._newPages.clear();
    txn._freedPages.clear();
    txn._tree = nullptr;
    return next.txnId;
}

void CowTree::AbortTxn(WriteTxn& txn) noexcept
{
    // Return new pages to the store; freed pages remain live in the tree.
    for (auto const np: txn._newPages)
        std::ignore = _store.Free(np);
    txn._newPages.clear();
    txn._freedPages.clear();
}

} // namespace CowTree
