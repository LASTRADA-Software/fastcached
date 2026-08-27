// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/IReclaimLog.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string_view>
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
            case CowTree::CowTreeError::InUse:
                code = StorageErrorCode::InUse;
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

        // Clamped, because this number comes off DISK and the clock counts in
        // nanoseconds: constructing the time point multiplies by a thousand, and
        // a microsecond count that does not fit overflows a signed integer --
        // undefined behaviour rather than a wrong timestamp, on a path any
        // damaged record can reach. UBSan trips on it, which is how it was
        // found: the first test to feed the parser arbitrary bytes reached it
        // immediately.
        //
        // The sentinels above are the only values that MEAN anything at the
        // extremes, so anything else out there is a damaged field, and the
        // nearest representable instant is a better answer than a trap.
        constexpr auto MaxMicros = std::chrono::duration_cast<std::chrono::microseconds>(TimePoint::duration::max()).count();
        constexpr auto MinMicros = std::chrono::duration_cast<std::chrono::microseconds>(TimePoint::duration::min()).count();
        if (v >= MaxMicros)
            return TimePoint::max();
        if (v <= MinMicros)
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

    /// The version a non-empty store carrying no marker at all must be.
    ///
    /// The marker arrived WITH v4, so its absence is itself the version stamp:
    /// a store holding records but no sentinel was written by a build that did
    /// not stamp one. That is v3 — no release ever shipped v1 or v2 of this
    /// layout — and it is why the refusal below can name a version the store
    /// never records.
    constexpr std::uint32_t PreMarkerFormatVersion = 3;

    /// Records converted per transaction.
    ///
    /// The conversion cannot be one transaction, however much atomicity would
    /// like it to be. A CoW commit replaces every page on the root-to-leaf path
    /// and the replaced ones are only freed AT the commit, so a single
    /// transaction over the whole store allocates one fresh page per record per
    /// tree level and can reuse none of them -- measured at 3 pages per record,
    /// a 20x file inflation on a 2000-record store. Worse, `CommitTxn` writes
    /// `freeRoot = PageId::None()` ("free list is in-memory only for v1"), so
    /// that garbage is not reclaimable by the next process either: it is
    /// permanent growth of the operator's cache file.
    ///
    /// Committing in slices bounds it. Each commit returns its replaced pages
    /// to the free list, and the next slice allocates out of that, so the file
    /// grows by about one slice rather than by the whole store. The cost of a
    /// small slice is more commits; the cost of a large one is the peak. A
    /// thousand records is roughly 48 MiB of headroom at the daemon's 16 KiB
    /// page size, which is a rounding error against the cache it is converting.
    constexpr std::uint64_t MigrationChunkRecords = 1000;

    /// Describe a store this build cannot read, and say what can be done about it.
    ///
    /// One helper rather than one message per call site: a marker naming another
    /// version and a pre-marker store whose ABSENT marker is its version are the
    /// same fact to an operator, and deserve the same sentence. The sentence has
    /// to carry the advice because the alternative is what an operator reaches
    /// for unprompted — deleting a store that was never damaged.
    /// @param onDisk   The version the store carries.
    /// @param readable  Whether this build still has a reader for it.
    /// @return The refusal, coded so a programmatic caller can tell it from damage.
    [[nodiscard]] StorageError UnsupportedFormat(std::uint32_t onDisk, bool readable)
    {
        // The advice is the load-bearing half. An operator reading a refusal
        // with no next step reaches for `rm -rf`, which is the outcome this
        // whole error code exists to prevent.
        //
        // Which advice, though, turns on whether a READER still exists -- not on
        // which side of the current version the store sits. A version older than
        // every reader (one whose row was retired, which the table's comment
        // describes as a supported decision) would otherwise be told to run a
        // conversion that refuses it with this very sentence.
        auto const* const remedy = [&] {
            if (readable)
                return "the store is intact and does not need to be deleted -- convert it with "
                       "`fastcached --migrate-storage` (or `fastcache-compile-node --migrate-cache`), "
                       "passing the same storage options this process was given";
            if (onDisk > CowTreeStorage::CurrentFormatVersion)
                return "upgrade to a build that writes this version; the store is intact and does not need to be "
                       "deleted";
            return "this build no longer carries a reader for that version, so it cannot be converted; the cache "
                   "has to be rebuilt";
        }();
        return MakeError(StorageErrorCode::UnsupportedFormatVersion,
                         std::format("on-disk storage format version {} (this build reads and writes {}): {}",
                                     onDisk,
                                     CowTreeStorage::CurrentFormatVersion,
                                     remedy));
    }

    /// Describe a store a conversion stopped in the middle of.
    ///
    /// A distinct sentence from `UnsupportedFormat` because the remedy is
    /// distinct: this store is neither vintage nor damaged, it is half-way, and
    /// the one thing that fixes it is running the conversion again.
    /// @param fromVersion The version the interrupted run was converting from.
    /// @return The refusal.
    /// Describe a conversion some OTHER build started and this one must not
    /// finish.
    /// @param progressFrom The version that run was converting from.
    /// @param progressTo   The version it was converting to.
    /// @return The refusal.
    [[nodiscard]] StorageError ForeignConversion(std::uint32_t progressFrom, std::uint32_t progressTo)
    {
        return MakeError(StorageErrorCode::UnsupportedFormatVersion,
                         std::format("an interrupted conversion from on-disk storage format version {} to {} is in "
                                     "progress, and this build writes version {}: finish it with the build that "
                                     "started it. The store is intact and does not need to be deleted.",
                                     progressFrom,
                                     progressTo,
                                     CowTreeStorage::CurrentFormatVersion));
    }

    [[nodiscard]] StorageError InterruptedConversion(std::uint32_t fromVersion)
    {
        return MakeError(StorageErrorCode::UnsupportedFormatVersion,
                         std::format("an interrupted conversion left this store part-way from on-disk storage "
                                     "format version {} to {}: re-run `fastcached --migrate-storage` (or "
                                     "`fastcache-compile-node --migrate-cache`) to finish it, which resumes where "
                                     "it stopped. The store is intact and does not need to be deleted.",
                                     fromVersion,
                                     CowTreeStorage::CurrentFormatVersion));
    }

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

    // Read before the store is moved from: this is the one path that knows a
    // real file is behind it, so it is the only one that can answer.
    auto const lockState = (*store)->StoreLockState();
    auto opened = OpenWithStore(std::move(options), std::move(*store));
    if (opened.has_value())
        (*opened)->_storeLockState = lockState;
    return opened;
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
    if (auto const r = EnsureFormatVersion(); !r.has_value())
        return std::unexpected(r.error());
    if (auto const r = Replay(); !r.has_value())
        return std::unexpected(r.error());
    _stats.bytesLimit = _options.maxBytes;
    return {};
}

namespace
{

    /// Is `key` one of the reserved sentinels rather than a cache entry?
    ///
    /// Both of them, in one predicate, because every caller wants the same
    /// answer: a conversion that parsed either as a record would report the
    /// store corrupt.
    /// @param key A tree key.
    /// @return True when it is reserved.
    [[nodiscard]] bool IsReservedKey(CowTree::BytesView key) noexcept
    {
        return std::ranges::equal(key, KeyView(CowTreeStorage::FormatMarkerKey))
               || std::ranges::equal(key, KeyView(CowTreeStorage::MigrationMarkerKey));
    }

    /// How far an interrupted conversion had got.
    struct MigrationProgress
    {
        /// The version that run was converting FROM. Recorded rather than
        /// re-derived, because by the time it is read the store is mixed: some
        /// records are already the new layout and the old marker still stands,
        /// so nothing on disk can be asked which reader to use.
        std::uint32_t fromVersion { 0 };

        /// The version that run was converting TO.
        ///
        /// Recorded because "resume it" is only safe when the run being resumed
        /// was heading where this build writes. A newer build interrupted
        /// part-way from 4 to 5 leaves a store whose format marker still says 4
        /// -- so without this, an older build would find a source version it can
        /// read, "resume", re-encode the v4 tail to v4, and stamp the whole
        /// store v4 over a v5 prefix. That is the silent mis-parse the version
        /// marker exists to prevent, arrived at through the machinery meant to
        /// prevent it.
        std::uint32_t toVersion { 0 };

        /// The last key that run converted. Everything at or below it is
        /// already in the new layout.
        std::vector<std::byte> lastKey;
    };

    /// Read the in-flight-conversion marker, if there is one.
    /// @param tree An opened tree.
    /// @return The progress, or nullopt when no conversion is in flight.
    [[nodiscard]] std::expected<std::optional<MigrationProgress>, StorageError> ReadMigrationProgress(CowTree::CowTree& tree)
    {
        auto reader = tree.BeginRead();
        auto const marker = reader.Get(KeyView(CowTreeStorage::MigrationMarkerKey));
        if (!marker.has_value())
            return std::unexpected(TranslateError(marker.error(), "migration-marker read"));
        if (!marker->has_value())
            return std::optional<MigrationProgress> {};

        CowTree::BytesView cursor { (*marker)->data(), (*marker)->size() };
        MigrationProgress progress;
        if (!ReadLe<std::uint32_t>(cursor, progress.fromVersion) || !ReadLe<std::uint32_t>(cursor, progress.toVersion))
            return std::unexpected(
                MakeError(StorageErrorCode::Corrupt, "migration marker is too short to hold its versions"));
        progress.lastKey.assign(cursor.begin(), cursor.end());
        return progress;
    }

    /// Record how far the conversion has got, into `txn`. The caller commits —
    /// in the SAME transaction as the records it describes, so the marker can
    /// never claim more progress than was made.
    /// @param txn         An open write transaction.
    /// @param fromVersion The version being converted from.
    /// @param lastKey     The last key converted.
    /// @return Empty on success.
    [[nodiscard]] std::expected<void, StorageError> WriteMigrationProgress(CowTree::WriteTxn& txn,
                                                                           std::uint32_t fromVersion,
                                                                           std::span<std::byte const> lastKey)
    {
        std::vector<std::byte> value;
        AppendLe<std::uint32_t>(value, fromVersion);
        AppendLe<std::uint32_t>(value, CowTreeStorage::CurrentFormatVersion);
        value.insert(value.end(), lastKey.begin(), lastKey.end());
        if (auto const put =
                txn.Put(KeyView(CowTreeStorage::MigrationMarkerKey), CowTree::BytesView { value.data(), value.size() });
            !put.has_value())
            return std::unexpected(TranslateError(put.error(), "migration-marker write"));
        return {};
    }

    /// The record-layout version a store carries.
    ///
    /// Three inputs and one place that reads them, so `EnsureFormatVersion` and
    /// `MigrateStore` cannot reach different conclusions about the same file: a
    /// marker holding a version answers directly; no marker over a store that
    /// already holds records is itself the stamp (see `PreMarkerFormatVersion`);
    /// and no marker over an empty store means nothing has been written yet,
    /// which is `nullopt` rather than a version, because the two callers do
    /// opposite things with it -- one stamps, the other has nothing to convert.
    /// @param tree An opened tree.
    /// @return The version, or nullopt for a store nothing has stamped; Corrupt
    ///         when a marker is present but too short to hold one.
    [[nodiscard]] std::expected<std::optional<std::uint32_t>, StorageError> StoredFormatVersion(CowTree::CowTree& tree)
    {
        auto reader = tree.BeginRead();
        auto const marker = reader.Get(KeyView(CowTreeStorage::FormatMarkerKey));
        if (!marker.has_value())
            return std::unexpected(TranslateError(marker.error(), "format-marker read"));

        if (marker->has_value())
        {
            CowTree::BytesView cursor { (*marker)->data(), (*marker)->size() };
            std::uint32_t onDisk = 0;
            // A marker too short to hold its own u32 is damage rather than
            // vintage: there is no version to report and nothing to convert, so
            // this one is Corrupt while a mismatch is not.
            if (!ReadLe<std::uint32_t>(cursor, onDisk))
                return std::unexpected(MakeError(StorageErrorCode::Corrupt, "format marker is too short to hold a version"));
            return onDisk;
        }

        if (tree.ItemCount() != 0)
            return PreMarkerFormatVersion;
        return std::optional<std::uint32_t> {};
    }

    /// Stamp the current version into `txn`. The caller commits.
    /// @param txn An open write transaction.
    /// @return Empty on success.
    [[nodiscard]] std::expected<void, StorageError> StampFormatVersion(CowTree::WriteTxn& txn)
    {
        std::vector<std::byte> value;
        AppendLe<std::uint32_t>(value, CowTreeStorage::CurrentFormatVersion);
        if (auto const put =
                txn.Put(KeyView(CowTreeStorage::FormatMarkerKey), CowTree::BytesView { value.data(), value.size() });
            !put.has_value())
            return std::unexpected(TranslateError(put.error(), "format-marker write"));
        return {};
    }

} // namespace

std::expected<void, StorageError> CowTreeStorage::EnsureFormatVersion()
{
    // Asked BEFORE the version, because a store with a conversion in flight has
    // no single answer to that question: some of its records are the new layout
    // and its marker still names the old one.
    auto const progress = ReadMigrationProgress(*_tree);
    if (!progress.has_value())
        return std::unexpected(progress.error());
    if (progress->has_value())
    {
        if ((*progress)->toVersion != CurrentFormatVersion)
            return std::unexpected(ForeignConversion((*progress)->fromVersion, (*progress)->toVersion));
        return std::unexpected(InterruptedConversion((*progress)->fromVersion));
    }

    auto const onDisk = StoredFormatVersion(*_tree);
    if (!onDisk.has_value())
        return std::unexpected(onDisk.error());

    if (onDisk->has_value())
    {
        if (**onDisk != CurrentFormatVersion)
            return std::unexpected(UnsupportedFormat(**onDisk, CanRead(**onDisk)));
        return {};
    }

    // Nothing stamped and nothing stored: a brand-new store, and this is the
    // first and only chance to mark it.
    auto txn = _tree->BeginWrite();
    if (auto const stamped = StampFormatVersion(txn); !stamped.has_value())
        return std::unexpected(stamped.error());
    if (auto const c = txn.Commit(); !c.has_value())
        return std::unexpected(TranslateError(c.error(), "format-marker commit"));
    return {};
}

std::expected<CowTreeStorage::MigrationReport, StorageError> CowTreeStorage::Migrate(Options const& options)
{
    // `FilePageStore::Open` creates what it cannot find, which is right for a
    // daemon starting up and wrong here: it would turn a mistyped path into a
    // brand-new empty store, report "nothing to convert" over it, and leave the
    // stray file behind -- while the store the operator meant is still refused
    // at every start with no hint that they converted something else.
    auto error = std::error_code {};
    if (!std::filesystem::exists(options.path, error) || error)
        return std::unexpected(
            MakeError(StorageErrorCode::IoError, std::format("no storage file at '{}'", options.path.string())));

    CowTree::FilePageStore::Options pageOpts;
    pageOpts.path = options.path;
    pageOpts.pageSize = options.pageSize != 0 ? options.pageSize : DefaultStoragePageSize;
    // Batched, not Fsync. `Fsync` durability fsyncs on every PAGE write, and a
    // conversion writes one page per record per tree level -- three fsyncs a
    // record, which turns a large store into hours. Batched flushes on a
    // group-commit boundary and again when the store closes, and the progress
    // marker is what makes the remaining window safe: a crash that loses the
    // last batch of slices costs those slices, which the next run redoes, not
    // the store.
    pageOpts.durability = CowTree::FilePageStore::Durability::Batched;

    auto store = CowTree::FilePageStore::Open(pageOpts);
    if (!store.has_value())
        return std::unexpected(
            TranslateError(store.error(), std::format("cannot open the store: {}", CowTree::ToStringView(store.error()))));

    // Opening had to assume a page size in order to know where the second meta
    // slot even is, and the file gets to overrule that. When it does, the slot
    // was read at the wrong offset and thrown away -- and since the two slots
    // alternate, the store may have opened on the OLDER of them. Converting on
    // top of a rolled-back root would then discard whatever the newer slot
    // recorded. Reopen with the size the file itself names, now that it is
    // known.
    if (auto const actual = (*store)->PageSize(); actual != pageOpts.pageSize)
    {
        pageOpts.pageSize = actual;
        store->reset();
        store = CowTree::FilePageStore::Open(pageOpts);
        if (!store.has_value())
            return std::unexpected(TranslateError(
                store.error(), std::format("cannot open the store: {}", CowTree::ToStringView(store.error()))));
    }
    return MigrateStore(**store);
}

std::expected<CowTreeStorage::MigrationReport, StorageError> CowTreeStorage::MigrateStore(CowTree::IPageStore& store)
{
    CowTree::CowTree tree { store };
    if (auto const r = tree.Open(); !r.has_value())
        return std::unexpected(TranslateError(r.error(), "CowTree::Open"));

    auto const progress = ReadMigrationProgress(tree);
    if (!progress.has_value())
        return std::unexpected(progress.error());

    MigrationReport report { .fromVersion = CurrentFormatVersion,
                             .toVersion = CurrentFormatVersion,
                             .recordsConverted = 0,
                             .resumed = progress->has_value() };

    // Where to read the source version from. An interrupted run recorded it,
    // and that recording is the only thing that still knows: the store's own
    // marker names the version it is converting FROM, but its records are by
    // now a mixture of both layouts.
    std::optional<std::uint32_t> sourceVersion;
    std::optional<std::vector<std::byte>> resume;
    if (progress->has_value())
    {
        // Only a conversion heading where THIS build writes may be resumed.
        // Picking up one aimed at another version would re-encode its tail into
        // the wrong layout and then stamp the store as if the whole thing were
        // in it.
        if ((*progress)->toVersion != CurrentFormatVersion)
            return std::unexpected(ForeignConversion((*progress)->fromVersion, (*progress)->toVersion));
        sourceVersion = (*progress)->fromVersion;
        resume = (*progress)->lastKey;
    }
    else
    {
        auto const onDisk = StoredFormatVersion(tree);
        if (!onDisk.has_value())
            return std::unexpected(onDisk.error());
        // An unstamped store holds nothing to convert, and `Open` will stamp
        // it. Reported as already current rather than as an error, so running
        // the conversion over a path that turns out to be empty is a no-op an
        // operator can read rather than a failure they have to interpret.
        if (!onDisk->has_value() || **onDisk == CurrentFormatVersion)
            return report;
        sourceVersion = *onDisk;
    }

    auto const formats = RecordFormats();
    auto const row = std::ranges::find(formats, *sourceVersion, &RecordFormat::version);
    if (row == formats.end())
        return std::unexpected(UnsupportedFormat(*sourceVersion, CanRead(*sourceVersion)));
    report.fromVersion = *sourceVersion;

    // Parse everything BEFORE converting anything.
    //
    // Which reader to use is an inference -- an unmarked store is assumed to be
    // v3 -- and the conversion rewrites records in place, so a wrong inference
    // is not a failed conversion but a destroyed store. One read-only pass
    // turns that into a refusal: a store whose records do not all parse under
    // the reader chosen for it is left exactly as it was found. The cost is a
    // second pass over a job that is already offline and once-per-upgrade.
    {
        auto reader = tree.BeginRead();
        std::optional<StorageError> invalid;
        auto const check = [&](CowTree::BytesView key, CowTree::BytesView value) {
            if (IsReservedKey(key))
                return true;
            auto const parsed = row->parse(value);
            if (!parsed.has_value())
            {
                invalid = MakeError(StorageErrorCode::UnsupportedFormatVersion,
                                    std::format("this store does not read as on-disk storage format version {}, so "
                                                "it cannot be converted to {}. It has NOT been modified.",
                                                *sourceVersion,
                                                CurrentFormatVersion));
                return false;
            }
            return true;
        };
        auto const scanned = resume.has_value()
                                 ? reader.ForEachAfter(CowTree::BytesView { resume->data(), resume->size() }, check)
                                 : reader.ForEach(check);
        if (!scanned.has_value())
            return std::unexpected(TranslateError(scanned.error(), "migration validation scan"));
        if (invalid.has_value())
            return std::unexpected(*invalid);
    }

    // Converted in slices rather than in one transaction; see
    // `MigrationChunkRecords` for why a single transaction is not an option.
    // Each slice pins its own read snapshot AFTER the previous slice committed,
    // so no walk ever overlaps a commit -- which is the one thing `ForEach`
    // does not tolerate.
    while (true)
    {
        auto reader = tree.BeginRead();
        auto txn = tree.BeginWrite();

        std::vector<std::byte> lastKey;
        std::uint64_t inChunk = 0;
        std::optional<StorageError> failure;

        auto const visit = [&](CowTree::BytesView key, CowTree::BytesView value) {
            if (IsReservedKey(key))
                return true;

            auto const parsed = row->parse(value);
            if (!parsed.has_value())
            {
                failure = parsed.error();
                return false;
            }
            auto const record = ReEncodeRecord(*parsed, value);
            if (auto const put = txn.Put(key, CowTree::BytesView { record.data(), record.size() }); !put.has_value())
            {
                failure = TranslateError(put.error(), "migration write");
                return false;
            }
            lastKey.assign(key.begin(), key.end());
            ++inChunk;
            return inChunk < MigrationChunkRecords;
        };

        auto const walked = resume.has_value()
                                ? reader.ForEachAfter(CowTree::BytesView { resume->data(), resume->size() }, visit)
                                : reader.ForEach(visit);
        if (!walked.has_value())
            return std::unexpected(TranslateError(walked.error(), "migration scan"));
        if (failure.has_value())
            return std::unexpected(*failure);

        if (inChunk == 0)
        {
            txn.Abort();
            break;
        }

        if (auto const recorded = WriteMigrationProgress(txn, *sourceVersion, lastKey); !recorded.has_value())
            return std::unexpected(recorded.error());
        if (auto const c = txn.Commit(); !c.has_value())
            return std::unexpected(TranslateError(c.error(), "migration commit"));

        // Committing is not enough to get the slice's replaced pages back. A
        // batching store holds them until the commit that freed them is
        // durable, which it would otherwise defer to a fixed commit interval --
        // so without this the slices allocate fresh pages the whole way and the
        // file grows per record after all, which is the one thing slicing is
        // for. Measured: without it, 4x the records cost 5.3x the growth.
        if (auto const flushed = store.Flush(); !flushed.has_value())
            return std::unexpected(TranslateError(flushed.error(), "migration flush"));

        report.recordsConverted += inChunk;
        resume = std::move(lastKey);
    }

    // The progress marker goes away in the SAME transaction that stamps the new
    // version, so there is no instant at which the store is both "converting"
    // and "converted" -- nor one in which it is neither.
    auto txn = tree.BeginWrite();
    if (auto const erased = txn.Erase(KeyView(CowTreeStorage::MigrationMarkerKey)); !erased.has_value())
        return std::unexpected(TranslateError(erased.error(), "migration-marker erase"));
    if (auto const stamped = StampFormatVersion(txn); !stamped.has_value())
        return std::unexpected(stamped.error());
    if (auto const c = txn.Commit(); !c.has_value())
        return std::unexpected(TranslateError(c.error(), "migration commit"));

    // Checked, unlike the flush the store would do in its destructor: an
    // operator told a conversion succeeded must not be told it by a path that
    // discarded the error saying otherwise.
    if (auto const flushed = store.Flush(); !flushed.has_value())
        return std::unexpected(TranslateError(flushed.error(), "migration flush"));

    return report;
}

namespace
{
    /// Append the leaf-record header every record starts with: the kind tag,
    /// the codec, and the fields `ReadCommonHeader` reads back.
    void AppendCommonHeader(std::vector<std::byte>& out, std::uint8_t kind, CompressionCodec codec, CacheEntry const& entry)
    {
        AppendLe<std::uint8_t>(out, kind);
        AppendLe<std::uint8_t>(out, static_cast<std::uint8_t>(codec));
        AppendLe<std::uint32_t>(out, entry.flags);
        AppendLe<std::uint64_t>(out, entry.cas);
        AppendLe<std::int64_t>(out, TimePointToMicros(entry.expiry));
        AppendLe<std::uint64_t>(out, entry.generation);
        AppendLe<std::int64_t>(out, TimePointToMicros(entry.lastAccess));
        AppendLe<std::uint8_t>(out, entry.stale ? std::uint8_t { 1 } : std::uint8_t { 0 });
    }
} // namespace

std::vector<std::byte> CowTreeStorage::EncodeInline(CacheEntry const& entry,
                                                    CompressionCodec codec,
                                                    std::span<std::byte const> stored,
                                                    std::uint64_t originalLen)
{
    std::vector<std::byte> out;
    out.reserve(1 + 1 + 4 + 8 + 8 + 8 + 8 + 1 + 4 + 4 + stored.size());
    AppendCommonHeader(out, RecordKindInline, codec, entry);
    AppendLe<std::uint32_t>(out, static_cast<std::uint32_t>(stored.size()));
    AppendLe<std::uint32_t>(out, static_cast<std::uint32_t>(originalLen));
    auto const offset = out.size();
    out.resize(offset + stored.size());
    if (!stored.empty())
        std::memcpy(out.data() + offset, stored.data(), stored.size());
    return out;
}

std::vector<std::byte> CowTreeStorage::EncodeOverflowDescriptor(CacheEntry const& entry,
                                                                CompressionCodec codec,
                                                                CowTree::PageId root,
                                                                std::uint64_t storedLen,
                                                                std::uint64_t originalLen)
{
    std::vector<std::byte> out;
    out.reserve(1 + 1 + 4 + 8 + 8 + 8 + 8 + 1 + 8 + 8 + 8);
    AppendCommonHeader(out, RecordKindOverflow, codec, entry);
    AppendLe<std::uint64_t>(out, storedLen);
    AppendLe<std::uint64_t>(out, originalLen);
    AppendLe<std::uint64_t>(out, root.value);
    return out;
}

namespace
{

    /// Read the fields every record layout has carried since v3, in their
    /// shared order. Split out so each version's reader is the short list of
    /// what is DIFFERENT about it, rather than a second copy of the eight
    /// fields that are not.
    /// @param cursor Advanced past the common header.
    /// @param entry  Filled in.
    /// @return False when the record runs out before the header does.
    [[nodiscard]] bool ReadCommonHeader(CowTree::BytesView& cursor, CacheEntry& entry) noexcept
    {
        if (!ReadLe<std::uint32_t>(cursor, entry.flags))
            return false;
        if (!ReadLe<std::uint64_t>(cursor, entry.cas))
            return false;
        std::int64_t expiryUs = 0;
        if (!ReadLe<std::int64_t>(cursor, expiryUs))
            return false;
        entry.expiry = MicrosToTimePoint(expiryUs);
        if (!ReadLe<std::uint64_t>(cursor, entry.generation))
            return false;
        std::int64_t lastAccessUs = 0;
        if (!ReadLe<std::int64_t>(cursor, lastAccessUs))
            return false;
        entry.lastAccess = MicrosToTimePoint(lastAccessUs);
        std::uint8_t staleByte = 0;
        if (!ReadLe<std::uint8_t>(cursor, staleByte))
            return false;
        entry.stale = staleByte != 0;
        return true;
    }

} // namespace

std::expected<CowTreeStorage::ParsedRecord, StorageError> CowTreeStorage::ParseRecord(CowTree::BytesView raw)
{
    // v4: u8 kind, u8 codec, <common header>, then
    //     inline   -> u32 stored_len, u32 original_len, [stored bytes]
    //     overflow -> u64 stored_len, u64 original_len, u64 root_page_id
    auto cursor = raw;
    ParsedRecord parsed;
    std::uint8_t kind = 0;
    if (!ReadLe<std::uint8_t>(cursor, kind))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    std::uint8_t codecByte = 0;
    if (!ReadLe<std::uint8_t>(cursor, codecByte))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    parsed.codec = static_cast<CompressionCodec>(codecByte);
    if (!ReadCommonHeader(cursor, parsed.entry))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));

    if (kind == RecordKindInline)
    {
        if (!ReadLe<std::uint32_t>(cursor, parsed.inlineLen))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        std::uint32_t originalLen = 0;
        if (!ReadLe<std::uint32_t>(cursor, originalLen))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.originalLen = originalLen;
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
        if (!ReadLe<std::uint64_t>(cursor, parsed.originalLen))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        std::uint64_t rootValue = 0;
        if (!ReadLe<std::uint64_t>(cursor, rootValue))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.root = CowTree::PageId { rootValue };
        return parsed;
    }
    return std::unexpected(MakeError(StorageErrorCode::Corrupt));
}

std::expected<CowTreeStorage::ParsedRecord, StorageError> CowTreeStorage::ParseRecordV3(CowTree::BytesView raw)
{
    // v3: u8 kind, <common header>, then
    //     inline   -> u32 len, [bytes]
    //     overflow -> u64 len, u64 root_page_id
    //
    // No codec byte and no original length: v3 stored every value verbatim, so
    // `Identity` and "original == stored" are not defaults standing in for
    // missing information -- they are what the layout meant.
    auto cursor = raw;
    ParsedRecord parsed;
    parsed.codec = CompressionCodec::Identity;
    std::uint8_t kind = 0;
    if (!ReadLe<std::uint8_t>(cursor, kind))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    if (!ReadCommonHeader(cursor, parsed.entry))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));

    // EXACT lengths, where the current reader only checks there is enough. This
    // reader is reached by inference rather than by a marker -- a store with no
    // sentinel is ASSUMED to be v3 -- and the layout before v3 had no kind byte
    // at all, so its first byte is a flags byte that reads as `RecordKindInline`
    // whenever flags are zero. Accepting a record with bytes left over is what
    // would let such a store parse, and a conversion that parses it rewrites it
    // in place: a store this build used to merely refuse would be destroyed by
    // the feature meant to rescue it. A v3 record ends exactly where its value
    // does.
    if (kind == RecordKindInline)
    {
        if (!ReadLe<std::uint32_t>(cursor, parsed.inlineLen))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.originalLen = parsed.inlineLen;
        if (cursor.size() != parsed.inlineLen)
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.inlineOffset = static_cast<std::size_t>(raw.size() - cursor.size());
        return parsed;
    }
    if (kind == RecordKindOverflow)
    {
        parsed.overflow = true;
        if (!ReadLe<std::uint64_t>(cursor, parsed.totalLen))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.originalLen = parsed.totalLen;
        std::uint64_t rootValue = 0;
        if (!ReadLe<std::uint64_t>(cursor, rootValue))
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        if (!cursor.empty())
            return std::unexpected(MakeError(StorageErrorCode::Corrupt));
        parsed.root = CowTree::PageId { rootValue };
        return parsed;
    }
    return std::unexpected(MakeError(StorageErrorCode::Corrupt));
}

std::span<CowTreeStorage::RecordFormat const> CowTreeStorage::RecordFormats() noexcept
{
    static constexpr std::array formats {
        RecordFormat { .version = PreMarkerFormatVersion, .parse = &CowTreeStorage::ParseRecordV3 },
        RecordFormat { .version = CurrentFormatVersion, .parse = &CowTreeStorage::ParseRecord },
    };

    // The table ends at the version this build writes, and covers every one
    // down to its first without a gap. Checked row by row rather than by
    // arithmetic on the first and last: {3, 5, 5} has the right first version,
    // the right last version and the right length, and silently cannot read a
    // v4 store. Anchored on the extent rather than on any row's name, so
    // adding a reader without bumping the version -- or the reverse -- fails
    // here rather than at the first store nobody can open.
    static_assert(formats.back().version == CurrentFormatVersion);
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, formats.size()),
                                      [](std::size_t i) { return formats[i].version == formats.front().version + i; }));

    return formats;
}

bool CowTreeStorage::CanRead(std::uint32_t version) noexcept
{
    auto const formats = RecordFormats();
    return std::ranges::find(formats, version, &RecordFormat::version) != formats.end();
}

std::vector<std::byte> CowTreeStorage::ReEncodeRecord(ParsedRecord const& parsed, CowTree::BytesView raw)
{
    if (parsed.overflow)
        return EncodeOverflowDescriptor(parsed.entry, parsed.codec, parsed.root, parsed.totalLen, parsed.originalLen);
    return EncodeInline(parsed.entry, parsed.codec, raw.subspan(parsed.inlineOffset, parsed.inlineLen), parsed.originalLen);
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
    // Materialise the STORED (possibly compressed) bytes, then decompress to
    // plaintext so every caller — L1 mirror, wire protocols, decorators — only
    // ever sees the original value. Decompression is on the on-disk-miss path,
    // never the L1 hit path.
    std::vector<std::byte> storedBytes;
    if (parsed->overflow)
    {
        auto value = ReadOverflowChain(parsed->root, parsed->totalLen);
        if (!value.has_value())
            return std::unexpected(value.error());
        storedBytes = std::move(*value);
    }
    else
    {
        auto const& raw = **got;
        storedBytes.assign(raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset),
                           raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset + parsed->inlineLen));
    }

    if (parsed->codec == CompressionCodec::Identity)
    {
        // Disk-backend fallback: the bytes live in a fresh heap buffer that
        // outlives any read lock, so wrapping in a SharedValue is correct (it
        // copies into its single-allocation layout, yielding no copy-elimination
        // benefit here, unlike the in-memory tier).
        entry.value = MakeSharedValue(storedBytes);
    }
    else
    {
        // Defence in depth: a corrupt descriptor must not drive a huge
        // decompress-buffer allocation. A legitimate value never exceeds
        // maxValueBytes (enforced at write time), so a larger originalLen is
        // a corrupt record. (Guard only when a cap is configured.)
        if (_options.maxValueBytes != 0 && parsed->originalLen > _options.maxValueBytes)
            return std::unexpected(MakeError(StorageErrorCode::Corrupt, "decompressed length exceeds maxValueBytes"));
        auto const plain =
            Compression::Decompress(parsed->codec, storedBytes, static_cast<std::size_t>(parsed->originalLen));
        if (!plain.has_value())
            return std::unexpected(plain.error());
        entry.value = MakeSharedValue(*plain);
    }
    return LoadedEntry { std::move(entry) };
}

std::expected<void, StorageError> CowTreeStorage::StoreEntry(std::string_view key, CacheEntry const& entry)
{
    auto const bytes = entry.ValueBytes();
    auto const originalLen = static_cast<std::uint64_t>(bytes.size());

    // Compress once, up front, then let the (possibly smaller) STORED bytes
    // drive the inline-vs-overflow decision. Threshold + shrink-check: only
    // attempt compression above the configured minimum, and only keep the
    // compressed form when it is actually smaller — otherwise store verbatim
    // under Identity so a read never pays a pointless decompress.
    auto codec = CompressionCodec::Identity;
    std::vector<std::byte> compressed;
    std::span<std::byte const> stored = bytes;
    if (_options.compression != CompressionCodec::Identity && bytes.size() >= _options.compressionMinBytes
        && Compression::IsAvailable(_options.compression))
    {
        compressed = Compression::Compress(_options.compression, bytes, _options.compressionLevel);
        if (!compressed.empty() && compressed.size() < bytes.size())
        {
            codec = _options.compression;
            stored = compressed;
        }
    }

    std::vector<std::byte> encoded;
    CowTree::PageId newChain = CowTree::PageId::None();
    if (stored.size() > InlineValueLimit())
    {
        auto chain = WriteOverflowChain(stored);
        if (!chain.has_value())
            return std::unexpected(chain.error());
        newChain = *chain;
        // Make the overflow pages durable before the meta flip references them.
        if (auto const r = _store->SyncData(); !r.has_value())
        {
            FreeChain(newChain);
            return std::unexpected(TranslateError(r.error(), "overflow SyncData"));
        }
        encoded = EncodeOverflowDescriptor(entry, codec, newChain, static_cast<std::uint64_t>(stored.size()), originalLen);
    }
    else
    {
        encoded = EncodeInline(entry, codec, stored, originalLen);
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
        // Re-stamped: touching a node is what brings it back into the live
        // generation, exactly as the stored entry's own generation is rewritten.
        it->second->generation = _liveGeneration;
        _bytesUsed += valueSize;
        _lru.splice(_lru.begin(), _lru, it->second);
        return;
    }
    // A brand-new mirror node has never been read; only an explicit Read
    // marks it fetched.
    _lru.push_front(LruNode { .key = std::string { key },
                              .bytes = valueSize,
                              .fetched = access == AccessKind::Read,
                              .generation = _liveGeneration });
    _index.emplace(_lru.front().key, _lru.begin());
    _bytesUsed += valueSize;
}

void CowTreeStorage::EraseFromLru(std::string_view key)
{
    auto it = _index.find(key);
    if (it == _index.end())
        return;
    EraseNode(it->second);
}

void CowTreeStorage::EraseNode(Iterator it)
{
    // Advanced rather than reset, for the reason spelled out on the in-memory
    // tier: restarting the sweep whenever eviction runs would leave the pass
    // permanently unfinished on a cache that is under pressure.
    if (_sweepCursor == it)
        ++_sweepCursor;
    _bytesUsed -= it->bytes;
    _index.erase(it->key);
    _lru.erase(it);
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
        // Not reported when a flush already made this invisible: FLUSHDB fired
        // its own event, and `evicted` on top of it names a second thing that
        // did not happen. Same rule as the in-memory tier's EvictToFit.
        if (victim->generation >= _liveGeneration)
            RecordReclaim(_reclaim, MutationKind::Evict, keyCopy);
        EraseNode(victim);
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

    // Re-encode ONLY the leaf record. The value bytes are never decompressed,
    // re-read, or re-compressed — the record's existing codec + original length
    // are carried through verbatim. For an overflow value we keep the existing
    // chain (same root + stored/original length) and rewrite just the
    // descriptor; for an inline value we copy the STORED (possibly compressed)
    // bytes out of the current record and re-encode them inline unchanged.
    std::size_t valueSize = 0;
    std::vector<std::byte> encoded;
    if (parsed->overflow)
    {
        valueSize = static_cast<std::size_t>(parsed->originalLen);
        mutate(entry);
        entry.cas = _nextCas++;
        encoded = EncodeOverflowDescriptor(entry, parsed->codec, parsed->root, parsed->totalLen, parsed->originalLen);
    }
    else
    {
        auto const& raw = **got;
        std::vector<std::byte> const storedBytes {
            raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset),
            raw.begin() + static_cast<std::ptrdiff_t>(parsed->inlineOffset + parsed->inlineLen)
        };
        valueSize = static_cast<std::size_t>(parsed->originalLen);
        mutate(entry);
        entry.cas = _nextCas++;
        encoded = EncodeInline(entry, parsed->codec, storedBytes, parsed->originalLen);
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
        // A lapsed TTL noticed here is a reclaim like any other: the in-memory
        // tier reports one from its own delete path, and the same client
        // sequence must not produce different events per backend. A stale
        // generation is excluded, as everywhere else — FLUSHDB already fired.
        if (entry.expiry <= now)
            RecordReclaim(_reclaim, MutationKind::Expire, key);
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

PurgeOutcome CowTreeStorage::PurgeExpired(TimePoint now, PurgeBudget budget)
{
    FC_ZONE_SCOPED_N("CowTreeStorage::PurgeExpired");

    // Walk the LRU mirror from wherever the last sweep stopped, collect keys
    // whose stored entry is expired or stale-generation, then erase them on
    // disk + drop them from the mirror. Two-phase to avoid iterator
    // invalidation by the erase. Bound to keys present in the mirror — entries
    // on disk that the LRU does not know about (post-restart, with Replay a
    // no-op) are reachable only via Get, which the reader-side fix now leaves
    // alone.
    // A victim carries WHY it is one, because only one of the three reasons is
    // an expiry: a stale generation means FLUSHDB already fired its own event,
    // and an orphan means the mirror disagreed with the disk, which is not a
    // thing that happened to the key at all.
    struct Victim
    {
        std::string key;
        bool lapsed {}; ///< The stored TTL had passed.
    };

    // See the in-memory tier for why the pass is sized up front and why the
    // cursor is what makes a bounded sweep reach the far end at all. Here each
    // step additionally costs a `LoadEntry`, so the budget is the difference
    // between a sweep and a scan of the whole store.
    auto const startSize = _lru.size();
    auto const scanLimit = budget.ScanLimitOver(startSize);
    PurgeOutcome outcome { .completedPass = scanLimit >= startSize };

    std::vector<Victim> victims;
    victims.reserve(budget.maxPurged == 0 ? scanLimit : std::min(budget.maxPurged, scanLimit));

    while (outcome.scanned < scanLimit)
    {
        if (_sweepCursor == _lru.end())
        {
            _sweepCursor = _lru.begin();
            if (_sweepCursor == _lru.end())
                break; // Nothing left in the mirror.
        }

        // Advanced before anything below can erase, so this loop never dangles
        // its own cursor; `EraseNode` covers the erases that come from
        // elsewhere, and the one below where the sweep has wrapped onto a key
        // it already collected.
        auto const node = _sweepCursor++;
        ++outcome.scanned;

        auto loaded = LoadEntry(node->key);
        if (!loaded.has_value())
            continue;
        if (!loaded->has_value())
        {
            // Disk and mirror out of sync — drop the orphan from the
            // mirror; nothing to erase on disk.
            victims.push_back(Victim { .key = node->key, .lapsed = false });
        }
        else if (auto const& entry = (*loaded)->entry; entry.expiry <= now)
        {
            if (!node->fetched)
                ++_stats.expiredUnfetched;
            victims.push_back(Victim { .key = node->key, .lapsed = true });
        }
        else if (entry.generation < _liveGeneration)
        {
            victims.push_back(Victim { .key = node->key, .lapsed = false });
        }
        else
        {
            continue;
        }

        if (budget.PurgeExhausted(victims.size()))
        {
            outcome.completedPass = false;
            break;
        }
    }

    for (auto const& victim: victims)
    {
        auto loaded = LoadEntry(victim.key);
        if (loaded.has_value() && loaded->has_value())
        {
            if (auto const r = EraseEntry(victim.key); !r.has_value())
                continue;
        }
        EraseFromLru(victim.key);
        // Reported only once the key is actually gone: a disk delete that
        // failed above took the `continue`, and an event for a key still being
        // served is worse than no event.
        if (victim.lapsed)
            RecordReclaim(_reclaim, MutationKind::Expire, victim.key);
        ++outcome.purged;
    }
    return outcome;
}

void CowTreeStorage::SetReclaimLog(IReclaimLog* log)
{
    _reclaim = log;
}

StorageStats CowTreeStorage::Snapshot() const noexcept
{
    _stats.itemCount = _index.size();
    _stats.bytesUsed = _bytesUsed;
    _stats.bytesLimit = _options.maxBytes;
    return _stats;
}

TieredStorageStats CowTreeStorage::SnapshotTiers() const noexcept
{
    TieredStorageStats tiers {};
    tiers[static_cast<std::size_t>(StorageTier::Disk)] = Snapshot();
    return tiers;
}

void CowTreeStorage::Resize(std::size_t newMaxBytes)
{
    _options.maxBytes = newMaxBytes;
    _stats.bytesLimit = newMaxBytes;
    EvictToFit();
}

std::optional<CowTree::FilePageStore::LockState> CowTreeStorage::StoreLockState() const noexcept
{
    return _storeLockState;
}

std::string DescribeMigration(std::filesystem::path const& path,
                              std::expected<CowTreeStorage::MigrationReport, StorageError> const& outcome)
{
    if (!outcome.has_value())
        // The code as well as the context: several failure paths carry only the
        // label of the step that failed, and "CowTree::Open" on its own tells an
        // operator nothing about which kind of problem to go looking for.
        return std::format("{}: {}: {}", path.string(), ToStringView(outcome.error().code), outcome.error().context);

    if (outcome->fromVersion == outcome->toVersion)
        return std::format(
            "{}: already at on-disk storage format version {}; nothing to do", path.string(), outcome->toVersion);

    return std::format("{}: converted {} record(s) from on-disk storage format version {} to {}{}",
                       path.string(),
                       outcome->recordsConverted,
                       outcome->fromVersion,
                       outcome->toVersion,
                       outcome->resumed ? " (resumed an interrupted conversion)" : "");
}

} // namespace FastCache
