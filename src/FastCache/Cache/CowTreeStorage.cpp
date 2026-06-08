// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/Crc32c.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/FilePageStore.hpp>
#include <CowTree/PageId.hpp>

namespace FastCache
{

namespace
{

    /// Build a StorageError with just a code (and an optional context).
    /// Spelled out as a helper because gcc -Wmissing-field-initializers
    /// rejects designated initialisers that omit fields, and writing
    /// `{ .code = X, .systemCode = 0, .context = {} }` at every call
    /// site is noisy.
    [[nodiscard]] StorageError MakeError(StorageErrorCode code, std::string context = {}) noexcept
    {
        return StorageError { .code = code, .systemCode = 0, .context = std::move(context) };
    }

    /// Map CowTreeError into FastCache::StorageError.
    [[nodiscard]] StorageError TranslateError(CowTree::CowTreeError e, std::string context = {})
    {
        StorageErrorCode code = StorageErrorCode::IoError;
        switch (e)
        {
            case CowTree::CowTreeError::ValueTooLarge:
                code = StorageErrorCode::ValueTooLarge;
                break;
            case CowTree::CowTreeError::Corrupt:
            case CowTree::CowTreeError::CorruptMetas:
                code = StorageErrorCode::Corrupt;
                break;
            case CowTree::CowTreeError::InvalidArg:
            case CowTree::CowTreeError::OutOfRange:
                code = StorageErrorCode::InvalidArgument;
                break;
            case CowTree::CowTreeError::NotFound:
                code = StorageErrorCode::KeyNotFound;
                break;
            default:
                code = StorageErrorCode::IoError;
                break;
        }
        return StorageError {
            .code = code,
            .systemCode = 0,
            .context = std::move(context),
        };
    }

    template <typename T>
    void AppendLe(std::vector<std::byte>& buf, T value)
    {
        if constexpr (std::endian::native != std::endian::little)
            value = std::byteswap(value);
        auto const offset = buf.size();
        buf.resize(offset + sizeof(T));
        std::memcpy(buf.data() + offset, &value, sizeof(T));
    }

    /// Serialize an integer in little-endian byte order directly into a
    /// caller-provided buffer (the in-place mirror of AppendLe). Lets hot-path
    /// callers fill a fixed page layout without allocating a scratch vector.
    /// Constrained to integral `T` so the byte-swap and memcpy are only ever
    /// instantiated for trivially-copyable fixed-width integers.
    /// @tparam T Integral type to serialize.
    /// @param dst Destination span; must hold at least sizeof(T) bytes.
    /// @param value Integer to store.
    template <std::integral T>
    void StoreLe(std::span<std::byte> dst, T value) noexcept
    {
        if constexpr (std::endian::native != std::endian::little)
            value = std::byteswap(value);
        std::memcpy(dst.data(), &value, sizeof(T));
    }

    template <typename T>
    bool ReadLe(CowTree::BytesView& cursor, T& out) noexcept
    {
        if (cursor.size() < sizeof(T))
            return false;
        T raw {};
        std::memcpy(&raw, cursor.data(), sizeof(T));
        cursor = cursor.subspan(sizeof(T));
        if constexpr (std::endian::native != std::endian::little)
            raw = std::byteswap(raw);
        out = raw;
        return true;
    }

    /// Convert a steady-clock TimePoint to a microsecond count for storage.
    /// `TimePoint::max()` (never expires) is stored as INT64_MAX.
    [[nodiscard]] std::int64_t TimePointToMicros(TimePoint tp)
    {
        if (tp == TimePoint::max())
            return std::numeric_limits<std::int64_t>::max();
        if (tp == TimePoint::min())
            return std::numeric_limits<std::int64_t>::min();
        return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    }

    [[nodiscard]] TimePoint MicrosToTimePoint(std::int64_t v)
    {
        if (v == std::numeric_limits<std::int64_t>::max())
            return TimePoint::max();
        if (v == std::numeric_limits<std::int64_t>::min())
            return TimePoint::min();
        return TimePoint { std::chrono::microseconds { v } };
    }

    [[nodiscard]] CowTree::BytesView KeyView(std::string_view sv) noexcept
    {
        return CowTree::BytesView { reinterpret_cast<std::byte const*>(sv.data()), sv.size() };
    }

    /// Page size for newly created stores: small and FIXED, independent of
    /// `maxValueBytes`. A value larger than the inline limit spills into an
    /// overflow-page chain, so a small write only ever touches a small page —
    /// not the multi-megabyte page a value-sized page would force.
    constexpr std::size_t DefaultStoragePageSize = 16 * 1024;

    /// Overflow page header: [u64 next_page_id][u32 chunk_len][u32 crc32c].
    constexpr std::size_t OverflowPageHeaderSize = 16;

    /// Leaf-record kind tags (first byte of the encoded record).
    constexpr std::uint8_t RecordKindInline = 0;
    constexpr std::uint8_t RecordKindOverflow = 1;

    [[nodiscard]] std::expected<std::uint64_t, StorageError> ParseUnsigned(std::span<std::byte const> bytes)
    {
        if (bytes.empty())
            return std::unexpected(MakeError(StorageErrorCode::InvalidArgument));
        std::string_view const sv { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
        std::uint64_t value = 0;
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
        if (ec != std::errc {} || ptr != sv.data() + sv.size())
            return std::unexpected(MakeError(StorageErrorCode::InvalidArgument));
        return value;
    }

} // namespace

CowTreeStorage::CowTreeStorage(Options options) noexcept:
    _options { std::move(options) }
{
}

CowTreeStorage::~CowTreeStorage() = default;

std::expected<std::unique_ptr<CowTreeStorage>, StorageError> CowTreeStorage::Open(Options options)
{
    CowTree::FilePageStore::Options pageOpts;
    pageOpts.path = options.path;
    pageOpts.pageSize = options.pageSize != 0 ? options.pageSize : DefaultStoragePageSize;
    pageOpts.durability = options.durability;

    auto store = CowTree::FilePageStore::Open(pageOpts);
    if (!store.has_value())
        return std::unexpected(TranslateError(store.error(), "FilePageStore::Open"));
    return OpenWithStore(std::move(options), std::move(*store));
}

std::expected<std::unique_ptr<CowTreeStorage>, StorageError> CowTreeStorage::OpenWithStore(
    Options options, std::unique_ptr<CowTree::IPageStore> store)
{
    auto self = std::unique_ptr<CowTreeStorage> { new CowTreeStorage { std::move(options) } };
    self->_ownedStore = std::move(store);
    self->_store = self->_ownedStore.get();
    if (auto const r = self->Initialize(); !r.has_value())
        return std::unexpected(r.error());
    return self;
}

std::expected<std::unique_ptr<CowTreeStorage>, StorageError> CowTreeStorage::OpenBorrowing(Options options,
                                                                                           CowTree::IPageStore& store)
{
    auto self = std::unique_ptr<CowTreeStorage> { new CowTreeStorage { std::move(options) } };
    self->_store = &store;
    if (auto const r = self->Initialize(); !r.has_value())
        return std::unexpected(r.error());
    return self;
}

std::expected<void, StorageError> CowTreeStorage::Initialize()
{
    _tree = std::make_unique<CowTree::CowTree>(*_store);
    if (auto const r = _tree->Open(); !r.has_value())
        return std::unexpected(TranslateError(r.error(), "CowTree::Open"));
    if (auto const r = Replay(); !r.has_value())
        return std::unexpected(r.error());
    _stats.bytesLimit = _options.maxBytes;
    return {};
}

namespace
{
    /// Append the common v3 leaf-record header (everything but the value /
    /// overflow descriptor).
    void AppendCommonHeader(std::vector<std::byte>& out, std::uint8_t kind, CacheEntry const& entry)
    {
        AppendLe<std::uint8_t>(out, kind);
        AppendLe<std::uint32_t>(out, entry.flags);
        AppendLe<std::uint64_t>(out, entry.cas);
        AppendLe<std::int64_t>(out, TimePointToMicros(entry.expiry));
        AppendLe<std::uint64_t>(out, entry.generation);
        AppendLe<std::int64_t>(out, TimePointToMicros(entry.lastAccess));
        AppendLe<std::uint8_t>(out, entry.stale ? std::uint8_t { 1 } : std::uint8_t { 0 });
    }
} // namespace

std::vector<std::byte> CowTreeStorage::EncodeInline(CacheEntry const& entry)
{
    auto const bytes = entry.ValueBytes();
    std::vector<std::byte> out;
    out.reserve(1 + 4 + 8 + 8 + 8 + 8 + 1 + 4 + bytes.size());
    AppendCommonHeader(out, RecordKindInline, entry);
    AppendLe<std::uint32_t>(out, static_cast<std::uint32_t>(bytes.size()));
    auto const offset = out.size();
    out.resize(offset + bytes.size());
    if (!bytes.empty())
        std::memcpy(out.data() + offset, bytes.data(), bytes.size());
    return out;
}

std::vector<std::byte> CowTreeStorage::EncodeOverflowDescriptor(CacheEntry const& entry,
                                                                CowTree::PageId root,
                                                                std::uint64_t totalLen)
{
    std::vector<std::byte> out;
    out.reserve(1 + 4 + 8 + 8 + 8 + 8 + 1 + 8 + 8);
    AppendCommonHeader(out, RecordKindOverflow, entry);
    AppendLe<std::uint64_t>(out, totalLen);
    AppendLe<std::uint64_t>(out, root.value);
    return out;
}

std::expected<CowTreeStorage::ParsedRecord, StorageError> CowTreeStorage::ParseRecord(CowTree::BytesView raw)
{
    auto cursor = raw;
    ParsedRecord parsed;
    auto& e = parsed.entry;
    std::uint8_t kind = 0;
    if (!ReadLe<std::uint8_t>(cursor, kind))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    if (!ReadLe<std::uint32_t>(cursor, e.flags))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    if (!ReadLe<std::uint64_t>(cursor, e.cas))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    std::int64_t expiryUs = 0;
    if (!ReadLe<std::int64_t>(cursor, expiryUs))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    e.expiry = MicrosToTimePoint(expiryUs);
    if (!ReadLe<std::uint64_t>(cursor, e.generation))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    std::int64_t lastAccessUs = 0;
    if (!ReadLe<std::int64_t>(cursor, lastAccessUs))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    e.lastAccess = MicrosToTimePoint(lastAccessUs);
    std::uint8_t staleByte = 0;
    if (!ReadLe<std::uint8_t>(cursor, staleByte))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    e.stale = staleByte != 0;

    if (kind == RecordKindInline)
    {
        if (!ReadLe<std::uint32_t>(cursor, parsed.inlineLen))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        if (cursor.size() < parsed.inlineLen)
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.inlineOffset = static_cast<std::size_t>(raw.size() - cursor.size());
        return parsed;
    }
    if (kind == RecordKindOverflow)
    {
        parsed.overflow = true;
        if (!ReadLe<std::uint64_t>(cursor, parsed.totalLen))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        std::uint64_t rootValue = 0;
        if (!ReadLe<std::uint64_t>(cursor, rootValue))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.root = CowTree::PageId { rootValue };
        return parsed;
    }
    return std::unexpected(MakeError(StorageErrorCode::Corrupt));
}

std::size_t CowTreeStorage::InlineValueLimit() const noexcept
{
    // Keep inline records small enough that several share a leaf page; larger
    // values spill to an overflow chain.
    return _store->PageSize() / 4;
}

std::expected<CowTree::PageId, StorageError> CowTreeStorage::WriteOverflowChain(std::span<std::byte const> value)
{
    auto const pageSize = _store->PageSize();
    auto const payloadPerPage = pageSize - OverflowPageHeaderSize;
    auto const pageCount = std::max<std::size_t>(1, (value.size() + payloadPerPage - 1) / payloadPerPage);

    std::vector<CowTree::PageId> ids;
    ids.reserve(pageCount);
    auto rollback = [&] {
        for (auto const id: ids)
            std::ignore = _store->Free(id);
    };
    for (std::size_t i = 0; i < pageCount; ++i)
    {
        auto allocated = _store->Allocate();
        if (!allocated.has_value())
        {
            rollback();
            return std::unexpected(TranslateError(allocated.error(), "overflow Allocate"));
        }
        ids.push_back(*allocated);
    }

    for (std::size_t i = 0; i < pageCount; ++i)
    {
        auto const offset = i * payloadPerPage;
        auto const chunkLen = std::min(payloadPerPage, value.size() - offset);
        auto const chunk = value.subspan(offset, chunkLen);
        std::uint64_t const next = (i + 1 < pageCount) ? ids[i + 1].value : 0;

        // Page layout: [u32 crc32c][u64 next][u32 chunkLen][chunk]. The CRC
        // covers everything after itself (next + chunkLen + chunk), so a torn
        // write to the chain LINK — not only the payload — is caught on read.
        // The link and CRC fields are written straight into `page` (no scratch
        // vectors) since this runs once per page on the large-value write path.
        std::vector<std::byte> page(pageSize, std::byte { 0 });
        StoreLe<std::uint64_t>(std::span { page }.subspan(sizeof(std::uint32_t)), next);
        StoreLe<std::uint32_t>(std::span { page }.subspan(sizeof(std::uint32_t) + sizeof(std::uint64_t)),
                               static_cast<std::uint32_t>(chunkLen));
        if (!chunk.empty())
            std::memcpy(page.data() + OverflowPageHeaderSize, chunk.data(), chunkLen);

        auto const crcRegion = std::span<std::byte const> { page.data() + sizeof(std::uint32_t),
                                                            (OverflowPageHeaderSize - sizeof(std::uint32_t)) + chunkLen };
        StoreLe<std::uint32_t>(std::span { page }, CowTree::Crc32c::Compute(crcRegion));

        if (auto const r = _store->Write(ids[i], CowTree::BytesView { page.data(), page.size() }); !r.has_value())
        {
            rollback();
            return std::unexpected(TranslateError(r.error(), "overflow Write"));
        }
    }
    return ids.front();
}

std::expected<CowTreeStorage::OverflowPage, StorageError> CowTreeStorage::ReadOverflowPage(CowTree::PageId id) const
{
    auto const payloadPerPage = _store->PageSize() - OverflowPageHeaderSize;
    auto view = _store->Read(id);
    if (!view.has_value())
        return std::unexpected(TranslateError(view.error(), "overflow Read"));
    // The view aliases the store's reusable read buffer; copy it out before the
    // next Read invalidates it.
    OverflowPage page;
    page.bytes.assign(view->begin(), view->end());
    if (page.bytes.size() < OverflowPageHeaderSize)
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    CowTree::BytesView headerView { page.bytes.data(), OverflowPageHeaderSize };
    std::uint32_t crc = 0;
    std::ignore = ReadLe<std::uint32_t>(headerView, crc);
    std::ignore = ReadLe<std::uint64_t>(headerView, page.next);
    std::ignore = ReadLe<std::uint32_t>(headerView, page.chunkLen);
    if (page.chunkLen > payloadPerPage || OverflowPageHeaderSize + page.chunkLen > page.bytes.size())
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    // The CRC covers next + chunkLen + chunk (everything after the CRC field),
    // so a torn write to the chain link is caught here, not just a torn payload.
    auto const crcRegion = std::span<std::byte const> { page.bytes.data() + sizeof(std::uint32_t),
                                                        (OverflowPageHeaderSize - sizeof(std::uint32_t)) + page.chunkLen };
    if (CowTree::Crc32c::Compute(crcRegion) != crc)
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    return page;
}

std::expected<std::vector<std::byte>, StorageError> CowTreeStorage::ReadOverflowChain(CowTree::PageId root,
                                                                                      std::uint64_t totalLen) const
{
    // Defence in depth: a wild `totalLen` (e.g. a CRC-consistent-but-wrong
    // descriptor from a future encoder bug or format skew) must not drive an
    // unbounded reserve that throws std::length_error/std::bad_alloc — on the
    // reactor's DetachedTask path an escaped exception terminates the daemon.
    if (totalLen > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));

    auto const payloadPerPage = _store->PageSize() - OverflowPageHeaderSize;
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(totalLen));

    auto cursor = root;
    auto const maxPages = static_cast<std::uint64_t>(totalLen / std::max<std::size_t>(1, payloadPerPage)) + 2;
    std::uint64_t visited = 0;
    while (cursor && visited < maxPages)
    {
        ++visited;
        auto page = ReadOverflowPage(cursor);
        if (!page.has_value())
            return std::unexpected(page.error());
        out.insert(out.end(),
                   page->bytes.begin() + static_cast<std::ptrdiff_t>(OverflowPageHeaderSize),
                   page->bytes.begin() + static_cast<std::ptrdiff_t>(OverflowPageHeaderSize + page->chunkLen));
        cursor = CowTree::PageId { page->next };
    }
    if (out.size() != totalLen)
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    return out;
}

void CowTreeStorage::FreeChain(CowTree::PageId root)
{
    auto cursor = root;
    std::uint64_t visited = 0;
    // A legitimate chain can't exceed the configured max value's worth of
    // pages; the bound also stops a corrupt `next` cycle from looping forever.
    auto const payloadPerPage = std::max<std::size_t>(1, _store->PageSize() - OverflowPageHeaderSize);
    auto const cap = static_cast<std::uint64_t>(_options.maxValueBytes / payloadPerPage) + 4;
    while (cursor && visited < cap)
    {
        ++visited;
        // Validate the page (CRC + bounds) BEFORE trusting its `next` link.
        // A torn/corrupt overflow page must never steer Free() into a page that
        // belongs to a different live key — that page would be handed to a later
        // Allocate and overwritten, corrupting the other value. On a validation
        // failure we stop: a damaged chain leaks its own tail at worst, which is
        // strictly safer than freeing foreign pages.
        auto page = ReadOverflowPage(cursor);
        if (!page.has_value())
            return;
        auto const next = page->next;
        std::ignore = _store->Free(cursor);
        cursor = CowTree::PageId { next };
    }
}

std::expected<void, StorageError> CowTreeStorage::Replay()
{
    // For now, we don't enumerate the tree on Open — we trust that
    // Get() will fetch entries on demand. The LRU mirror starts empty,
    // and entries enter it when they're first accessed or written.
    // This means restart loses eviction order (LRU resets) but data is
    // preserved.
    //
    // Tracking CAS continuity across restarts: scan the tree to find
    // the max CAS used and set _nextCas accordingly. For simplicity we
    // skip this; new entries get fresh CAS tokens, which is acceptable
    // since CAS only needs to be unique within a session.
    return {};
}

std::expected<std::optional<CowTreeStorage::StoredRef>, StorageError> CowTreeStorage::ReadStoredRef(
    std::string_view key) const
{
    auto reader = _tree->BeginRead();
    auto got = reader.Get(KeyView(key));
    if (!got.has_value())
        return std::unexpected(TranslateError(got.error()));
    if (!got->has_value())
        return std::optional<StoredRef> {};
    auto parsed = ParseRecord(CowTree::BytesView { (*got)->data(), (*got)->size() });
    if (!parsed.has_value())
        return std::unexpected(parsed.error());
    return StoredRef { .overflow = parsed->overflow, .root = parsed->root };
}

std::expected<std::optional<CowTreeStorage::LoadedEntry>, StorageError> CowTreeStorage::LoadEntry(std::string_view key) const
{
    if (_tree == nullptr)
        return std::unexpected(MakeError(StorageErrorCode::IoError, "not open"));
    auto reader = _tree->BeginRead();
    auto got = reader.Get(KeyView(key));
    if (!got.has_value())
        return std::unexpected(TranslateError(got.error()));
    if (!got->has_value())
        return std::optional<LoadedEntry> {};
    auto parsed = ParseRecord(CowTree::BytesView { (*got)->data(), (*got)->size() });
    if (!parsed.has_value())
        return std::unexpected(parsed.error());
    auto entry = std::move(parsed->entry);
    if (parsed->overflow)
    {
        auto value = ReadOverflowChain(parsed->root, parsed->totalLen);
        if (!value.has_value())
            return std::unexpected(value.error());
        // Disk-backend fallback: the value was just materialized from the
        // overflow chain into a fresh heap buffer, so wrapping it in a
        // SharedValue is correct (it outlives any read lock) — it simply
        // yields no copy-elimination benefit, unlike the in-memory backend.
        entry.value = MakeSharedValue(*value);
    }
    else
    {
        auto const& raw = **got;
        entry.value = MakeSharedValue(
            std::vector<std::byte> { raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset),
                                     raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset + parsed->inlineLen) });
    }
    return LoadedEntry { std::move(entry) };
}

std::expected<void, StorageError> CowTreeStorage::StoreEntry(std::string_view key, CacheEntry const& entry)
{
    auto const bytes = entry.ValueBytes();
    std::vector<std::byte> encoded;
    CowTree::PageId newChain = CowTree::PageId::None();
    if (bytes.size() > InlineValueLimit())
    {
        auto chain = WriteOverflowChain(bytes);
        if (!chain.has_value())
            return std::unexpected(chain.error());
        newChain = *chain;
        // Make the overflow pages durable before the meta flip references them.
        if (auto const r = _store->SyncData(); !r.has_value())
        {
            FreeChain(newChain);
            return std::unexpected(TranslateError(r.error(), "overflow SyncData"));
        }
        encoded = EncodeOverflowDescriptor(entry, newChain, static_cast<std::uint64_t>(bytes.size()));
    }
    else
    {
        encoded = EncodeInline(entry);
    }

    auto txn = _tree->BeginWrite();
    // Put returns the displaced record (if the key already existed), so we learn
    // the previous overflow chain to reclaim WITHOUT a separate read transaction.
    auto put = txn.Put(KeyView(key), CowTree::BytesView { encoded.data(), encoded.size() });
    if (!put.has_value())
    {
        if (newChain)
            FreeChain(newChain);
        return std::unexpected(TranslateError(put.error()));
    }
    if (auto const r = txn.Commit(); !r.has_value())
    {
        if (newChain)
            FreeChain(newChain);
        return std::unexpected(TranslateError(r.error()));
    }
    // Committed: the new value is durable, so an old overflow chain named by the
    // displaced record (if any) is now unreferenced and can be reclaimed. (CoW
    // correctness: never free the old data before the new value is durable.)
    if (put->has_value())
    {
        auto const& oldRecord = **put;
        if (auto const oldParsed = ParseRecord(CowTree::BytesView { oldRecord.data(), oldRecord.size() });
            oldParsed.has_value() && oldParsed->overflow)
            FreeChain(oldParsed->root);
    }
    return {};
}

std::expected<void, StorageError> CowTreeStorage::EraseEntry(std::string_view key)
{
    auto oldRef = ReadStoredRef(key);
    if (!oldRef.has_value())
        return std::unexpected(oldRef.error());

    auto txn = _tree->BeginWrite();
    auto r = txn.Erase(KeyView(key));
    if (!r.has_value())
        return std::unexpected(TranslateError(r.error()));
    if (auto const c = txn.Commit(); !c.has_value())
        return std::unexpected(TranslateError(c.error()));
    if (oldRef->has_value() && (*oldRef)->overflow)
        FreeChain((*oldRef)->root);
    return {};
}

void CowTreeStorage::TouchOrInsert(std::string_view key, std::size_t valueSize, AccessKind access)
{
    auto it = _index.find(key);
    if (it != _index.end())
    {
        // A read records the access, a value-rewriting Write clears the bit
        // (the new value is unread), and a TTL-only Preserve (Touch /
        // MarkStale) leaves the existing bit alone — matching
        // InMemoryLruStorage, where Touch never disturbs `fetched`.
        bool const fetched = [&] {
            if (access == AccessKind::Read)
                return true;
            if (access == AccessKind::Write)
                return false;
            return it->second->fetched;
        }();
        _bytesUsed -= it->second->bytes;
        it->second->bytes = valueSize;
        it->second->fetched = fetched;
        _bytesUsed += valueSize;
        _lru.splice(_lru.begin(), _lru, it->second);
        return;
    }
    // A brand-new mirror node has never been read; only an explicit Read
    // marks it fetched.
    _lru.push_front(LruNode { .key = std::string { key }, .bytes = valueSize, .fetched = access == AccessKind::Read });
    _index.emplace(_lru.front().key, _lru.begin());
    _bytesUsed += valueSize;
}

void CowTreeStorage::EraseFromLru(std::string_view key)
{
    auto it = _index.find(key);
    if (it == _index.end())
        return;
    _bytesUsed -= it->second->bytes;
    _lru.erase(it->second);
    _index.erase(it);
}

void CowTreeStorage::EvictToFit()
{
    FC_ZONE_SCOPED_N("CowTreeStorage::EvictToFit");
    if (_options.maxBytes == 0)
        return;
    // Track remaining attempts so a stuck disk (e.g. ENOSPC on every
    // commit) walks the LRU once and bails rather than spinning. Better
    // to leave the soft cap violated than to mutate the in-memory mirror
    // out of sync with the tree on disk.
    auto remainingAttempts = _lru.size();
    while (_bytesUsed > _options.maxBytes && !_lru.empty() && remainingAttempts != 0)
    {
        auto victim = std::prev(_lru.end());
        auto const keyCopy = victim->key;
        if (auto const r = EraseEntry(keyCopy); !r.has_value())
        {
            // Disk delete failed; rotate the stuck victim out of the
            // tail so the next iteration tries a different key. Once
            // every entry has rotated through unsuccessfully we bail.
            _lru.splice(_lru.begin(), _lru, victim);
            --remainingAttempts;
            continue;
        }
        if (!victim->fetched)
            ++_stats.evictedUnfetched;
        _bytesUsed -= victim->bytes;
        _index.erase(keyCopy);
        _lru.erase(victim);
        ++_stats.evictions;
        remainingAttempts = _lru.size();
    }
}

std::expected<GetResult, StorageError> CowTreeStorage::Get(std::string_view key, TimePoint now)
{
    FC_ZONE_SCOPED_N("CowTreeStorage::Get");
    ++_stats.cmdGet;
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value())
    {
        ++_stats.getMisses;
        return GetResult { .found = false, .entry = {} };
    }
    auto& entry = (*loaded)->entry;
    if (entry.expiry <= now || entry.generation < _liveGeneration)
    {
        // Expired or flushed. Treat as a miss; do NOT mutate the tree
        // from a read path — under ShardedStorage::Get the caller
        // holds only a shared_lock, so opening a write transaction
        // would violate CowTree's single-writer contract and race
        // concurrent expired-Gets on the same shard. Defer the on-
        // disk cleanup to PurgeExpired (writer-locked).
        ++_stats.getMisses;
        return GetResult { .found = false, .entry = {} };
    }
    entry.lastAccess = now;
    TouchOrInsert(key, entry.ValueSize(), AccessKind::Read);
    ++_stats.getHits;
    // Deliberately do NOT persist the lastAccess advance here: a read must
    // not open a write transaction (CoW page churn + log growth on every
    // hit would cripple read-heavy workloads, and under ShardedStorage the
    // single-writer contract must hold). The returned copy carries the
    // fresh lastAccess for the caller; the on-disk value only advances on
    // the next genuine write (Set / Touch / MarkStale / ...).
    GetResult result;
    result.found = true;
    result.entry = std::move(entry);
    return result;
}

std::expected<CasToken, StorageError> CowTreeStorage::UpdateRecordMetadata(std::string_view key,
                                                                           TimePoint now,
                                                                           std::function<void(CacheEntry&)> const& mutate)
{
    if (_tree == nullptr)
        return std::unexpected(MakeError(StorageErrorCode::IoError, "not open"));
    auto reader = _tree->BeginRead();
    auto got = reader.Get(KeyView(key));
    if (!got.has_value())
        return std::unexpected(TranslateError(got.error()));
    if (!got->has_value())
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto parsed = ParseRecord(CowTree::BytesView { (*got)->data(), (*got)->size() });
    if (!parsed.has_value())
        return std::unexpected(parsed.error());
    auto& entry = parsed->entry;
    if (entry.expiry <= now || entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));

    // Re-encode ONLY the leaf record. For an overflow value we keep the existing
    // chain (same root + totalLen) and rewrite just the descriptor; for an inline
    // value we copy its bytes out of the current record and re-encode inline.
    // Either way the value bytes are never re-read or re-written.
    std::size_t valueSize = 0;
    std::vector<std::byte> encoded;
    if (parsed->overflow)
    {
        valueSize = static_cast<std::size_t>(parsed->totalLen);
        mutate(entry);
        entry.cas = _nextCas++;
        encoded = EncodeOverflowDescriptor(entry, parsed->root, parsed->totalLen);
    }
    else
    {
        auto const& raw = **got;
        entry.value = MakeSharedValue(
            std::vector<std::byte> { raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset),
                                     raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset + parsed->inlineLen) });
        valueSize = entry.ValueSize();
        mutate(entry);
        entry.cas = _nextCas++;
        encoded = EncodeInline(entry);
    }

    auto txn = _tree->BeginWrite();
    // The displaced descriptor names the SAME overflow chain we are reusing, so
    // we deliberately ignore Put's returned old record — freeing that chain would
    // discard the live value. (Inline records reference no chain.)
    auto put = txn.Put(KeyView(key), CowTree::BytesView { encoded.data(), encoded.size() });
    if (!put.has_value())
        return std::unexpected(TranslateError(put.error()));
    if (auto const r = txn.Commit(); !r.has_value())
        return std::unexpected(TranslateError(r.error()));
    // Metadata-only change: preserve the LRU `fetched` bit (matching
    // InMemoryLruStorage::Touch / MarkStale).
    TouchOrInsert(key, valueSize, AccessKind::Preserve);
    return entry.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::Touch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    ++_stats.cmdTouch;
    auto const cas = UpdateRecordMetadata(key, now, [&](CacheEntry& entry) {
        entry.expiry = newExpiry;
        entry.lastAccess = now;
    });
    if (!cas.has_value())
    {
        ++_stats.touchMisses;
        return std::unexpected(cas.error());
    }
    ++_stats.touchHits;
    return *cas;
}

std::expected<GetResult, StorageError> CowTreeStorage::Peek(std::string_view key, TimePoint now)
{
    // Non-mutating: no LRU promotion, no lastAccess advance, no stats, and
    // crucially no write transaction. Expired / flushed entries read as a
    // miss but are left for the writer-locked PurgeExpired to reclaim.
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value())
        return GetResult { .found = false, .entry = {} };
    auto& entry = (*loaded)->entry;
    if (entry.expiry <= now || entry.generation < _liveGeneration)
        return GetResult { .found = false, .entry = {} };
    GetResult result;
    result.found = true;
    result.entry = std::move(entry);
    return result;
}

std::expected<CasToken, StorageError> CowTreeStorage::MarkStale(std::string_view key,
                                                                std::optional<TimePoint> newExpiry,
                                                                TimePoint now)
{
    // Marking stale rewrites no value bytes — reuse any overflow chain in place
    // and rewrite only the descriptor (preserving the LRU `fetched` bit).
    return UpdateRecordMetadata(key, now, [&](CacheEntry& entry) {
        entry.stale = true;
        if (newExpiry.has_value())
            entry.expiry = *newExpiry;
    });
}

std::expected<GetResult, StorageError> CowTreeStorage::GetAndTouch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    // Refresh the expiry, then read the refreshed entry back. The atomicity
    // boundary is the enclosing ShardedStorage's per-shard lock (this tier
    // never owns a lock); on the unwrapped single-threaded reactor there is
    // no concurrent writer to interleave between the touch and the read.
    auto const touched = Touch(key, newExpiry, now);
    if (!touched.has_value())
        return std::unexpected(touched.error());
    return Get(key, now);
}

std::expected<void, StorageError> CowTreeStorage::CompareAndDelete(std::string_view key, CasToken expected, TimePoint now)
{
    auto const peeked = Peek(key, now);
    if (!peeked.has_value())
        return std::unexpected(peeked.error());
    if (!peeked->found)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    if (peeked->entry.cas != expected)
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    return Delete(key, now);
}

std::expected<CasToken, StorageError> CowTreeStorage::Set(std::string_view key,
                                                          std::vector<std::byte> value,
                                                          std::uint32_t flags,
                                                          TimePoint expiry)
{
    FC_ZONE_SCOPED_N("CowTreeStorage::Set");
    ++_stats.cmdSet;
    if (value.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    CacheEntry e;
    e.value = MakeSharedValue(value);
    e.flags = flags;
    e.cas = _nextCas++;
    e.expiry = expiry;
    e.generation = _liveGeneration;

    if (auto const r = StoreEntry(key, e); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, e.ValueSize());
    EvictToFit();
    return e.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (loaded->has_value() && (*loaded)->entry.expiry > now && (*loaded)->entry.generation >= _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyExists));
    return Set(key, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> CowTreeStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    return Set(key, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> CowTreeStorage::Append(std::string_view key,
                                                             std::span<std::byte const> suffix,
                                                             CasToken expected,
                                                             TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto& entry = (*loaded)->entry;
    if (expected != 0 && entry.cas != expected)
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    auto const existing = entry.ValueBytes();
    if (existing.size() + suffix.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    std::vector<std::byte> combined;
    combined.reserve(existing.size() + suffix.size());
    combined.insert(combined.end(), existing.begin(), existing.end());
    combined.insert(combined.end(), suffix.begin(), suffix.end());
    entry.value = MakeSharedValue(combined);
    entry.cas = _nextCas++;
    // A value-rewriting mutation produces a fresh item nobody has read yet
    // (mirrors InMemoryLruStorage::MutateExisting and the CacheEntry contract).
    entry.stale = false;
    if (auto const r = StoreEntry(key, entry); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, entry.ValueSize());
    EvictToFit();
    return entry.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::Prepend(std::string_view key,
                                                              std::span<std::byte const> prefix,
                                                              CasToken expected,
                                                              TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto& entry = (*loaded)->entry;
    if (expected != 0 && entry.cas != expected)
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    auto const existing = entry.ValueBytes();
    if (existing.size() + prefix.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    std::vector<std::byte> merged;
    merged.reserve(prefix.size() + existing.size());
    merged.insert(merged.end(), prefix.begin(), prefix.end());
    merged.insert(merged.end(), existing.begin(), existing.end());
    entry.value = MakeSharedValue(merged);
    entry.cas = _nextCas++;
    // A value-rewriting mutation produces a fresh item nobody has read yet
    // (mirrors InMemoryLruStorage::MutateExisting and the CacheEntry contract).
    entry.stale = false;
    if (auto const r = StoreEntry(key, entry); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, entry.ValueSize());
    EvictToFit();
    return entry.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::CompareAndSwap(std::string_view key,
                                                                     CasToken expected,
                                                                     std::vector<std::byte> value,
                                                                     std::uint32_t flags,
                                                                     TimePoint expiry,
                                                                     TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
    {
        ++_stats.casMisses;
        return std::unexpected(loaded.error());
    }
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
    {
        ++_stats.casMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }
    if ((*loaded)->entry.cas != expected)
    {
        ++_stats.casBadval;
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    }
    ++_stats.casHits;
    return Set(key, std::move(value), flags, expiry);
}

std::expected<IStorage::IncrResult, StorageError> CowTreeStorage::IncrementOrInitialize(std::string_view key,
                                                                                        std::uint64_t magnitude,
                                                                                        bool decrement,
                                                                                        TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
    {
        if (decrement)
            ++_stats.decrMisses;
        else
            ++_stats.incrMisses;
        return std::unexpected(loaded.error());
    }
    // Miss = KeyNotFound, matching InMemoryLruStorage. The "initialize"
    // half of the name is the protocol layer's job: it owns the spec
    // semantics (binary `initial`/`expiration`, meta `J`/`N`) and re-issues
    // a Set on KeyNotFound. Auto-vivifying here with current=0 would ignore
    // those flags and silently bypass the binary "do not create" sentinel.
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
    {
        if (decrement)
            ++_stats.decrMisses;
        else
            ++_stats.incrMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }

    auto& existing = (*loaded)->entry;
    std::uint64_t current = 0;
    {
        auto parsed = ParseUnsigned(existing.ValueBytes());
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        current = *parsed;
    }

    // memcached: increment wraps modulo 2^64 (natural for uint64), decrement
    // saturates at 0. The full unsigned `magnitude` is honoured, so deltas in
    // [2^63, 2^64) add/subtract correctly instead of aliasing direction.
    std::uint64_t const next = [&]() -> std::uint64_t {
        if (!decrement)
            return current + magnitude;
        return magnitude >= current ? 0U : current - magnitude;
    }();
    auto const s = std::to_string(next);
    std::vector<std::byte> bytes;
    bytes.reserve(s.size());
    for (auto const c: s)
        bytes.push_back(static_cast<std::byte>(c));

    auto cas = Set(key, std::move(bytes), existing.flags, existing.expiry);
    if (!cas.has_value())
    {
        if (decrement)
            ++_stats.decrMisses;
        else
            ++_stats.incrMisses;
        return std::unexpected(cas.error());
    }
    if (decrement)
        ++_stats.decrHits;
    else
        ++_stats.incrHits;
    return IStorage::IncrResult { .value = next, .cas = *cas };
}

std::expected<void, StorageError> CowTreeStorage::Delete(std::string_view key, TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
    {
        ++_stats.deleteMisses;
        return std::unexpected(loaded.error());
    }
    if (!loaded->has_value())
    {
        ++_stats.deleteMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }
    auto const& entry = (*loaded)->entry;
    if (entry.expiry <= now || entry.generation < _liveGeneration)
    {
        // The on-disk record is stale (expired or older than the
        // current flush generation); still clean it up so a subsequent
        // restart doesn't replay the dead bytes. Caller's view is the
        // same as if the key was absent — KeyNotFound.
        std::ignore = EraseEntry(key);
        EraseFromLru(key);
        ++_stats.deleteMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }
    if (auto const r = EraseEntry(key); !r.has_value())
        return std::unexpected(r.error());
    EraseFromLru(key);
    ++_stats.deleteHits;
    return {};
}

void CowTreeStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    ++_liveGeneration;
    _flushEffectiveAt = effectiveAt;
    ++_stats.cmdFlush;
}

std::size_t CowTreeStorage::PurgeExpired(TimePoint now)
{
    // Walk the LRU mirror, collect keys whose stored entry is expired
    // or stale-generation, then erase them on disk + drop them from
    // the mirror. Two-phase to avoid iterator invalidation by
    // EraseFromLru. Bound to keys present in the mirror — entries on
    // disk that the LRU does not know about (post-restart, with Replay
    // a no-op) are reachable only via Get, which the reader-side fix
    // now leaves alone.
    std::vector<std::string> victims;
    victims.reserve(_lru.size());
    for (auto const& node: _lru)
    {
        auto loaded = LoadEntry(node.key);
        if (!loaded.has_value())
            continue;
        if (!loaded->has_value())
        {
            // Disk and mirror out of sync — drop the orphan from the
            // mirror; nothing to erase on disk.
            victims.push_back(node.key);
            continue;
        }
        auto const& entry = (*loaded)->entry;
        if (entry.expiry <= now)
        {
            if (!node.fetched)
                ++_stats.expiredUnfetched;
            victims.push_back(node.key);
        }
        else if (entry.generation < _liveGeneration)
        {
            victims.push_back(node.key);
        }
    }
    std::size_t purged = 0;
    for (auto const& key: victims)
    {
        auto loaded = LoadEntry(key);
        if (loaded.has_value() && loaded->has_value())
        {
            if (auto const r = EraseEntry(key); !r.has_value())
                continue;
        }
        EraseFromLru(key);
        ++purged;
    }
    return purged;
}

StorageStats CowTreeStorage::Snapshot() const noexcept
{
    _stats.itemCount = _index.size();
    _stats.bytesUsed = _bytesUsed;
    _stats.bytesLimit = _options.maxBytes;
    return _stats;
}

void CowTreeStorage::Resize(std::size_t newMaxBytes)
{
    _options.maxBytes = newMaxBytes;
    _stats.bytesLimit = newMaxBytes;
    EvictToFit();
}

} // namespace FastCache
