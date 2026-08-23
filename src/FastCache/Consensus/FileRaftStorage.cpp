// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Core/Crc32c.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Owner.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace FastCache::Consensus
{

namespace
{
    /// Identifies these files and rejects anything else pointed at this
    /// directory, so a mistyped path fails loudly rather than being parsed as a
    /// log of garbage.
    constexpr std::uint32_t StateMagic = 0x46435253U;    // "FCRS"
    constexpr std::uint32_t LogMagic = 0x4643524CU;      // "FCRL"
    constexpr std::uint32_t SnapshotMagic = 0x4643524EU; // "FCRN"

    /// Bumped when either layout changes. A version this build does not know is
    /// refused rather than guessed at: reading an unknown layout as if it were
    /// this one produces a log, not an error.
    constexpr std::uint16_t FormatVersion = 1;

    constexpr std::string_view StateFileName = "raft-state";
    constexpr std::string_view LogFileName = "raft-log";
    constexpr std::string_view SnapshotFileName = "raft-snapshot";

    /// Append a big-endian integer to `out`.
    template <typename T>
    void PutBigEndian(std::vector<std::byte>& out, T value)
    {
        auto scratch = std::array<std::byte, sizeof(T)> {};
        WriteBigEndian<T>(scratch, value);
        out.insert(out.end(), scratch.begin(), scratch.end());
    }

    /// Open a file in binary mode, spelling the path the way the platform wants.
    ///
    /// `std::fopen` takes a narrow path, which on Windows is converted through
    /// the active code page — so a directory containing a character that page
    /// cannot represent would fail to open for a reason having nothing to do with
    /// the storage. `_wfopen` takes the `wstring` the path already holds there.
    /// @param path File to open.
    /// @param mode An `fopen` mode string.
    /// @return The stream, or nullptr.
    [[nodiscard]] gsl::owner<std::FILE*> OpenBinary(std::filesystem::path const& path, char const* mode)
    {
#if defined(_WIN32)
        auto wide = std::wstring {};
        for (auto const* cursor = mode; *cursor != '\0'; ++cursor)
            wide.push_back(static_cast<wchar_t>(*cursor));

        return ::_wfopen(path.wstring().c_str(), wide.c_str());
#else
        return std::fopen(path.c_str(), mode);
#endif
    }

    /// Verify a trailing CRC32C over everything before it.
    /// @param raw The whole record, CRC included.
    /// @return True when the record is intact.
    [[nodiscard]] bool ChecksumHolds(std::span<std::byte const> raw) noexcept
    {
        if (raw.size() < sizeof(std::uint32_t))
            return false;

        auto const body = raw.first(raw.size() - sizeof(std::uint32_t));
        auto const stored = ReadBigEndian<std::uint32_t>(raw.subspan(body.size()));
        return Crc32c::Compute(body) == stored;
    }

    /// Decode the state file.
    /// @param raw Its whole contents.
    /// @return The state, or why it could not be read.
    [[nodiscard]] std::expected<PersistentState, ConsensusError> ParseState(std::span<std::byte const> raw)
    {
        constexpr auto minimum = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t)
                                 + sizeof(std::uint32_t) + 1 + sizeof(std::uint32_t);
        if (raw.size() < minimum || !ChecksumHolds(raw))
            return std::unexpected { FastCache::StorageFailure("the state record is corrupt") };

        auto cursor = std::size_t { 0 };
        auto const magic = ReadBigEndian<std::uint32_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint32_t);
        if (magic != StateMagic)
            return std::unexpected { FastCache::StorageFailure("the state file is not one of ours") };

        auto const version = ReadBigEndian<std::uint16_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint16_t);
        if (version != FormatVersion)
            return std::unexpected { FastCache::StorageFailure(
                std::format("state format version {} is not supported (this build writes {})", version, FormatVersion)) };

        auto state = PersistentState {};
        state.currentTerm = Term { .value = ReadBigEndian<std::uint64_t>(raw.subspan(cursor)) };
        cursor += sizeof(std::uint64_t);

        auto const length = ReadBigEndian<std::uint32_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint32_t);
        auto const present = raw[cursor] != std::byte { 0 };
        cursor += 1;

        if (cursor + length + sizeof(std::uint32_t) != raw.size())
            return std::unexpected { FastCache::StorageFailure("the state record's length does not fit") };

        if (present)
        {
            auto voted = std::string {};
            voted.reserve(length);
            for (auto const byte: raw.subspan(cursor, length))
                voted.push_back(static_cast<char>(byte));

            state.votedFor = std::move(voted);
        }

        return state;
    }

    /// One decoded log record, the index it claims, and how many bytes it took.
    struct DecodedRecord
    {
        LogEntry entry;
        LogIndex index {};
        std::size_t length {};
    };

    /// Encode one log record, index included.
    /// @param index The index this entry occupies.
    /// @param entry What to write.
    /// @return The record's bytes, CRC last.
    [[nodiscard]] std::vector<std::byte> EncodeLogRecord(LogIndex index, LogEntry const& entry)
    {
        auto body = std::vector<std::byte> {};
        PutBigEndian<std::uint32_t>(body, LogMagic);
        PutBigEndian<std::uint64_t>(body, index.value);
        PutBigEndian<std::uint64_t>(body, entry.term.value);
        body.push_back(static_cast<std::byte>(entry.kind));
        PutBigEndian<std::uint32_t>(body, static_cast<std::uint32_t>(entry.payload.size()));
        body.insert(body.end(), entry.payload.begin(), entry.payload.end());
        PutBigEndian<std::uint32_t>(body, Crc32c::Compute(body));
        return body;
    }

    /// Decode one log record from the front of `raw`.
    /// @param raw Remaining file contents.
    /// @return The record, or nullopt when it is incomplete or corrupt.
    [[nodiscard]] std::optional<DecodedRecord> ParseLogRecord(std::span<std::byte const> raw)
    {
        constexpr auto header =
            sizeof(std::uint32_t) + sizeof(std::uint64_t) + sizeof(std::uint64_t) + 1 + sizeof(std::uint32_t);
        if (raw.size() < header + sizeof(std::uint32_t))
            return std::nullopt;

        auto cursor = std::size_t { 0 };
        auto const magic = ReadBigEndian<std::uint32_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint32_t);
        if (magic != LogMagic)
            return std::nullopt;

        auto index = LogIndex { .value = ReadBigEndian<std::uint64_t>(raw.subspan(cursor)) };
        cursor += sizeof(std::uint64_t);

        auto entry = LogEntry {};
        entry.term = Term { .value = ReadBigEndian<std::uint64_t>(raw.subspan(cursor)) };
        cursor += sizeof(std::uint64_t);

        // The range check is `DecodeWireEnum`'s, whose bound lives beside the
        // enum, so this reader and `RaftWire`'s cannot disagree about the highest
        // kind -- a disagreement that would not fail to compile but would reject
        // every record carrying a newly added one.
        auto const kind = DecodeWireEnum<EntryKind>(static_cast<std::uint8_t>(raw[cursor]));
        cursor += 1;
        if (!kind.has_value())
            return std::nullopt;

        entry.kind = *kind;

        auto const payloadLength = ReadBigEndian<std::uint32_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint32_t);

        auto const total = cursor + payloadLength + sizeof(std::uint32_t);
        if (total > raw.size() || !ChecksumHolds(raw.first(total)))
            return std::nullopt;

        auto const payload = raw.subspan(cursor, payloadLength);
        entry.payload.assign(payload.begin(), payload.end());

        return DecodedRecord { .entry = std::move(entry), .index = index, .length = total };
    }

    /// Encode a snapshot record.
    /// @param snapshot What to write.
    /// @return Its bytes, CRC last.
    [[nodiscard]] std::vector<std::byte> EncodeSnapshot(RaftSnapshot const& snapshot)
    {
        auto body = std::vector<std::byte> {};
        PutBigEndian<std::uint32_t>(body, SnapshotMagic);
        PutBigEndian<std::uint16_t>(body, FormatVersion);
        PutBigEndian<std::uint64_t>(body, snapshot.lastIncludedIndex.value);
        PutBigEndian<std::uint64_t>(body, snapshot.lastIncludedTerm.value);

        // The member set through `Membership::Encode`, so this reader and the log
        // entry's cannot come to disagree about how a configuration is spelled.
        auto const members = Membership::Encode(snapshot.members);
        auto const tail =
            WireFields::Encode({ std::span<std::byte const> { members }, std::span<std::byte const> { snapshot.state } });
        body.insert(body.end(), tail.begin(), tail.end());

        PutBigEndian<std::uint32_t>(body, Crc32c::Compute(body));
        return body;
    }

    /// Decode a snapshot record.
    /// @param raw Its whole contents.
    /// @return The snapshot, or why it could not be read.
    [[nodiscard]] std::expected<RaftSnapshot, ConsensusError> ParseSnapshot(std::span<std::byte const> raw)
    {
        constexpr auto minimum = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t)
                                 + sizeof(std::uint64_t) + sizeof(std::uint32_t);
        if (raw.size() < minimum || !ChecksumHolds(raw))
            return std::unexpected { FastCache::StorageFailure("the snapshot record is corrupt") };

        auto cursor = std::size_t { 0 };
        auto const magic = ReadBigEndian<std::uint32_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint32_t);
        if (magic != SnapshotMagic)
            return std::unexpected { FastCache::StorageFailure("the snapshot file is not one of ours") };

        auto const version = ReadBigEndian<std::uint16_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint16_t);
        if (version != FormatVersion)
            return std::unexpected { FastCache::StorageFailure(
                std::format("snapshot format version {} is not supported (this build writes {})", version, FormatVersion)) };

        auto snapshot = RaftSnapshot {};
        snapshot.lastIncludedIndex = LogIndex { .value = ReadBigEndian<std::uint64_t>(raw.subspan(cursor)) };
        cursor += sizeof(std::uint64_t);
        snapshot.lastIncludedTerm = Term { .value = ReadBigEndian<std::uint64_t>(raw.subspan(cursor)) };
        cursor += sizeof(std::uint64_t);

        auto const tail = raw.subspan(cursor, raw.size() - cursor - sizeof(std::uint32_t));
        auto const fields = WireFields::SplitExactly(tail, 2);
        if (!fields.has_value())
            return std::unexpected { FastCache::StorageFailure("the snapshot record's fields do not fit") };

        auto members = Membership::Decode((*fields)[0]);
        if (!members.has_value())
            return std::unexpected { FastCache::StorageFailure("the snapshot's configuration is malformed") };

        snapshot.members = *std::move(members);
        snapshot.state.assign((*fields)[1].begin(), (*fields)[1].end());
        return snapshot;
    }

    /// Seek to an absolute offset that may exceed 2 GiB.
    ///
    /// `std::fseek` takes a `long`, which is 32-bit under LLP64 -- every Windows
    /// preset here -- and on 32-bit POSIX. A log of a few million entries passes
    /// that, after which the cast lands the next write at an arbitrary offset and
    /// silently corrupts the file. Same platform seam as `FlushToDisk`.
    /// @param file The open stream.
    /// @param offset Absolute byte offset.
    /// @return True on success.
    [[nodiscard]] bool SeekTo(std::FILE* file, std::uint64_t offset) noexcept
    {
#if defined(_WIN32)
        return ::_fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
        return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    }

    /// Flush a stream all the way to the platter.
    ///
    /// `fflush` alone only pushes the C library's buffer into the kernel, which a
    /// power loss still discards -- so it is the pair that makes a write durable,
    /// and the reason this is one helper rather than two calls at each site.
    /// @param file The open stream.
    /// @return True when both stages succeeded.
    [[nodiscard]] bool FlushToDisk(std::FILE* file) noexcept
    {
        if (std::fflush(file) != 0)
            return false;

#if defined(_WIN32)
        return ::_commit(::_fileno(file)) == 0;
#else
        return ::fsync(::fileno(file)) == 0;
#endif
    }

    /// Read a whole file into memory.
    ///
    /// Returns the reason rather than a bare failure flag. Every step here can
    /// fail for a different and actionable cause -- the path is a directory, the
    /// permissions are wrong, the disk gave up mid-read -- and a caller handed
    /// only "false" can say no more than "cannot read <path>", which is the one
    /// thing the operator already knew. `std::filesystem` reports through
    /// `error_code` and `fopen` through `errno`, so both are translated here where
    /// they are still in scope; a caller cannot recover them afterwards.
    /// @param path What to read.
    /// @param into Destination, cleared first.
    /// @return Nothing, or why it could not be read; a missing file reads as empty.
    [[nodiscard]] std::expected<void, ConsensusError> ReadWholeFile(std::filesystem::path const& path,
                                                                    std::vector<std::byte>& into)
    {
        into.clear();

        auto error = std::error_code {};
        auto const present = std::filesystem::exists(path, error);
        if (error)
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot stat {}: {}", path.string(), error.message())) };

        // A file that is not there is not a failure: a store starting for the
        // first time has neither a log nor a snapshot, and that is the ordinary
        // case rather than an error.
        if (!present)
            return {};

        auto const size = std::filesystem::file_size(path, error);
        if (error)
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot size {}: {}", path.string(), error.message())) };

        errno = 0;
        gsl::owner<std::FILE*> const file = OpenBinary(path, "rb");
        if (file == nullptr)
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot open {}: {}", path.string(), std::generic_category().message(errno))) };

        into.resize(static_cast<std::size_t>(size));

        errno = 0;
        auto const read = into.empty() ? std::size_t { 0 } : std::fread(into.data(), 1, into.size(), file);
        auto const failure = errno;
        auto const truncated = std::ferror(file) != 0 || read != into.size();
        (void) std::fclose(file);

        if (truncated)
        {
            into.clear();

            // A short read with no errno is a file that shrank between the size
            // call and the read, which is a different fault from an I/O error and
            // is worth saying so rather than reporting errno 0 as a cause.
            return std::unexpected { FastCache::StorageFailure(
                failure != 0 ? std::format("cannot read {}: {}", path.string(), std::generic_category().message(failure))
                             : std::format("{} is shorter than its reported size of {} bytes", path.string(), size)) };
        }

        return {};
    }

    /// Replace `path` with `body`, indivisibly.
    ///
    /// Written beside the target and renamed over it: rename is the only single
    /// filesystem operation that replaces a file's contents in one step, so a
    /// crash leaves either the whole previous file or the whole new one.
    /// @param path What to replace.
    /// @param body The new contents.
    /// @return Nothing, or why it could not be replaced.
    [[nodiscard]] std::expected<void, ConsensusError> ReplaceFileAtomically(std::filesystem::path const& path,
                                                                            std::span<std::byte const> body)
    {
        auto const temporary = std::filesystem::path { path }.concat(".tmp");

        errno = 0;
        gsl::owner<std::FILE*> const file = OpenBinary(temporary, "wb");
        if (file == nullptr)
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot open {}: {}", temporary.string(), std::generic_category().message(errno))) };

        errno = 0;
        auto const wrote = body.empty() || std::fwrite(body.data(), 1, body.size(), file) == body.size();
        auto const flushed = wrote && FlushToDisk(file);
        auto const failure = errno;
        (void) std::fclose(file);

        if (!flushed)
        {
            auto discard = std::error_code {};
            std::filesystem::remove(temporary, discard);
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot write {}: {}", temporary.string(), std::generic_category().message(failure))) };
        }

        auto error = std::error_code {};
        std::filesystem::rename(temporary, path, error);
        if (error)
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot replace {}: {}", path.string(), error.message())) };

        return {};
    }
} // namespace

std::expected<FileRaftStorage, ConsensusError> FileRaftStorage::Open(std::filesystem::path const& directory)
{
    auto error = std::error_code {};
    std::filesystem::create_directories(directory, error);
    if (error)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot create {}: {}", directory.string(), error.message())) };

    auto store = FileRaftStorage {};
    store._statePath = directory / StateFileName;
    store._logPath = directory / LogFileName;
    store._snapshotPath = directory / SnapshotFileName;

    // Opened for update rather than append: a truncation has to seek backwards,
    // and "a" would silently move every write to the end regardless.
    store._log = OpenBinary(store._logPath, "r+b");
    if (store._log == nullptr)
    {
        // Only when there is nothing there. "w+b" TRUNCATES, so retrying with it
        // unconditionally would destroy the durable log on any other failure -- a
        // sharing violation, a permission quirk -- which is a total loss of state
        // on the one path whose entire purpose is not losing state.
        auto probe = std::error_code {};
        if (std::filesystem::exists(store._logPath, probe) && !probe)
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot open the existing {}", store._logPath.string())) };

        store._log = OpenBinary(store._logPath, "w+b");
    }

    if (store._log == nullptr)
        return std::unexpected { FastCache::StorageFailure(std::format("cannot open {}", store._logPath.string())) };

    // Scanned here rather than in Load, so the offset table is valid from the
    // moment the store exists. Deriving it in Load left an ordering requirement
    // nothing enforced -- and Load returns early when the *state* file is corrupt,
    // without ever reaching the log -- so a SaveLog after that would see an empty
    // table, compute a start of zero, and erase every stored record.
    if (auto scanned = store.ScanLog(nullptr); !scanned.has_value())
        return std::unexpected { scanned.error() };

    // And the snapshot with it, by the same argument, **unconditionally** rather
    // than only when the scan found nothing. Two things ride on that. A log a
    // snapshot has trimmed to nothing has no record left to state where it
    // resumes, so the scan above leaves `_firstIndex` at 1 and a `SaveLog` before
    // `Load` would be refused as a gap. And reading it here is what makes a
    // damaged snapshot refuse to *open* the store -- a store that opens and then
    // fails on first read is a store whose caller has to remember the ordering
    // again, which is the requirement this whole step exists to remove.
    auto snapshot = store.ReadSnapshot();
    if (!snapshot.has_value())
        return std::unexpected { snapshot.error() };

    if (snapshot->has_value() && store._offsets.size() == 1)
        store._firstIndex = (*snapshot)->lastIncludedIndex.Advanced(1);

    return store;
}

FileRaftStorage::FileRaftStorage(FileRaftStorage&& other) noexcept:
    _statePath { std::move(other._statePath) },
    _logPath { std::move(other._logPath) },
    _snapshotPath { std::move(other._snapshotPath) },
    _log { std::exchange(other._log, nullptr) },
    _offsets { std::move(other._offsets) },
    _firstIndex { other._firstIndex }
{
}

FileRaftStorage& FileRaftStorage::operator=(FileRaftStorage&& other) noexcept
{
    if (this != &other)
    {
        CloseLog();
        _statePath = std::move(other._statePath);
        _logPath = std::move(other._logPath);
        _snapshotPath = std::move(other._snapshotPath);

        // Spelled out rather than `std::exchange`, whose return type drops the
        // `gsl::owner` annotation and so reads to clang-tidy as a raw handle being
        // assigned to an owning one.
        gsl::owner<std::FILE*> const adopted = other._log;
        other._log = nullptr;
        _log = adopted;

        _offsets = std::move(other._offsets);
        _firstIndex = other._firstIndex;
    }

    return *this;
}

FileRaftStorage::~FileRaftStorage()
{
    CloseLog();
}

void FileRaftStorage::CloseLog() noexcept
{
    if (_log != nullptr)
    {
        (void) std::fclose(_log);
        _log = nullptr;
    }
}

std::uint64_t FileRaftStorage::OffsetOf(LogIndex index) const noexcept
{
    // The table carries one more element than there are entries, and the last is
    // the end of the log. That sentinel is the whole point: without it, "where
    // does index N+1 begin?" -- the ordinary append -- has no answer, and the
    // earlier version returned the last record's *start* instead, so every append
    // landed on top of the entry before it. No test caught that, because each one
    // either started at index 1 or truncated inside the existing range.
    if (_offsets.empty())
        return 0;

    // Relative to the log's own first index, not to 1. After a snapshot has
    // trimmed the prefix the file no longer starts at index 1, and an offset
    // computed from the absolute index would land the next append that many
    // records into the file.
    auto const wanted = std::max<std::uint64_t>(index.value, _firstIndex.value);
    auto const slot = std::min<std::size_t>(_offsets.size(), static_cast<std::size_t>(wanted - _firstIndex.value) + 1);
    return _offsets[slot - 1];
}

std::expected<void, ConsensusError> FileRaftStorage::ScanLog(std::vector<LogEntry>* entries)
{
    auto raw = std::vector<std::byte> {};
    if (auto read = ReadWholeFile(_logPath, raw); !read.has_value())
        return std::unexpected { read.error() };

    _offsets.assign(1, 0);
    _firstIndex = LogIndex { .value = 1 };

    auto cursor = std::size_t { 0 };
    auto expected = std::optional<LogIndex> {};
    while (cursor < raw.size())
    {
        auto record = ParseLogRecord(std::span { raw }.subspan(cursor));
        if (!record.has_value())
            break; // A torn tail: it was never acknowledged, so nobody committed on it.

        // The first record states where this log begins; every later one must
        // continue it. A gap means the file was rewritten by something that is not
        // this store, and reading past it would place every subsequent entry under
        // an index it does not hold -- so the scan stops there and treats the rest
        // as a tail, which is the one disposal this format already knows is safe.
        if (!expected.has_value())
            _firstIndex = record->index;
        else if (record->index != *expected)
            break;

        expected = record->index.Advanced(1);
        cursor += record->length;
        _offsets.push_back(cursor);

        if (entries != nullptr)
            entries->push_back(std::move(record->entry));
    }

    return {};
}

std::expected<void, ConsensusError> FileRaftStorage::TrimLogThrough(LogIndex through)
{
    auto raw = std::vector<std::byte> {};
    if (auto read = ReadWholeFile(_logPath, raw); !read.has_value())
        return std::unexpected { read.error() };

    // Only the records above the boundary survive, re-written in place with the
    // indices they already carry -- so a trim renumbers nothing and a crash
    // mid-way leaves the whole old file, which still reads correctly.
    auto kept = std::vector<std::byte> {};
    auto cursor = std::size_t { 0 };
    while (cursor < raw.size())
    {
        auto const record = ParseLogRecord(std::span { raw }.subspan(cursor));
        if (!record.has_value())
            break;

        if (record->index > through)
        {
            auto const bytes = std::span { raw }.subspan(cursor, record->length);
            kept.insert(kept.end(), bytes.begin(), bytes.end());
        }

        cursor += record->length;
    }

    // The handle is closed across the rename: on Windows a file cannot be
    // replaced while it is open, and reopening afterwards is also what makes the
    // stream's position and buffer agree with the file it now refers to.
    CloseLog();

    if (auto replaced = ReplaceFileAtomically(_logPath, kept); !replaced.has_value())
    {
        _log = OpenBinary(_logPath, "r+b");
        return std::unexpected { replaced.error() };
    }

    _log = OpenBinary(_logPath, "r+b");
    if (_log == nullptr)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot reopen {} after trimming", _logPath.string())) };

    if (auto scanned = ScanLog(nullptr); !scanned.has_value())
        return std::unexpected { scanned.error() };

    // An empty log has no record left to state where it begins, so the boundary
    // is taken from the snapshot instead. Without this the next append is refused
    // as a gap, permanently: `SaveLog` would be comparing against 1.
    if (_offsets.size() == 1)
        _firstIndex = through.Advanced(1);

    return {};
}

std::expected<void, ConsensusError> FileRaftStorage::SaveState(PersistentState const& state)
{
    auto body = std::vector<std::byte> {};
    PutBigEndian<std::uint32_t>(body, StateMagic);
    PutBigEndian<std::uint16_t>(body, FormatVersion);
    PutBigEndian<std::uint64_t>(body, state.currentTerm.value);

    auto const voted = state.votedFor.value_or(std::string {});
    PutBigEndian<std::uint32_t>(body, static_cast<std::uint32_t>(voted.size()));
    // A vote for nobody and a vote for the empty string would otherwise read the
    // same, so presence is its own byte rather than inferred from the length.
    body.push_back(static_cast<std::byte>(state.votedFor.has_value() ? 1 : 0));
    for (auto const character: voted)
        body.push_back(static_cast<std::byte>(character));

    PutBigEndian<std::uint32_t>(body, Crc32c::Compute(body));

    // A half-written vote reads as no vote at all, so this file is replaced rather
    // than rewritten in place.
    return ReplaceFileAtomically(_statePath, body);
}

std::expected<void, ConsensusError> FileRaftStorage::SaveSnapshot(RaftSnapshot const& snapshot)
{
    auto const body = EncodeSnapshot(snapshot);
    if (auto written = ReplaceFileAtomically(_snapshotPath, body); !written.has_value())
        return std::unexpected { written.error() };

    // Only now. A crash between these two leaves a durable snapshot beside a log
    // that still holds the entries it covers, which `RaftNode` reconciles on
    // recovery; the opposite order leaves a log missing committed entries and no
    // snapshot to replace them, which nothing can repair.
    return TrimLogThrough(snapshot.lastIncludedIndex);
}

std::expected<void, ConsensusError> FileRaftStorage::SaveLog(LogAppend const& append)
{
    if (_log == nullptr)
        return std::unexpected { FastCache::StorageFailure("log is not open") };

    // A gap is refused rather than quietly closed up: `OffsetOf` clamps to the end
    // sentinel, so an out-of-range `fromIndex` would append at the end under a
    // different index than it claims and leave the store disagreeing with the node
    // about where entries live. No correct driver produces one, which is why it is
    // worth making an error rather than leaving it to luck.
    // Both ends, because a trimmed log no longer begins at 1: an append below its
    // first index names entries the snapshot has deliberately replaced.
    if (append.fromIndex < _firstIndex || append.fromIndex.value > _firstIndex.value + _offsets.size() - 1)
        return std::unexpected { FastCache::StorageFailure("a log append would leave a gap") };

    // fromIndex is a truncation point as well as a start, so anything at or after
    // it stops being part of the log.
    auto const start = OffsetOf(append.fromIndex);
    auto const slot = static_cast<std::size_t>(append.fromIndex.value - _firstIndex.value) + 1;

    // Truncated BEFORE the replacement is written, and made durable -- the
    // opposite of the obvious order. Writing first and shortening afterwards looks
    // safer, since the file is then never briefly short, but it is not: when a
    // replacement record happens to be the same encoded length as the one it
    // replaces (routine for a no-op, common for same-size commands) the bytes
    // after the new tail are an *intact* old record with valid magic and a valid
    // CRC, so a crash in between recovers entries the cluster had deleted. That is
    // the divergent log this contract exists to prevent. Cutting first can only
    // lose entries that were already being replaced, and a leader re-sends those.
    // Compared against the FILE's size, not against the last good record's end.
    // After a torn tail was discarded on load those two differ, and gating on the
    // offset table would leave the tail's bytes in the file for the next append to
    // write over the front of -- surviving as a suffix that is discarded on load
    // only because it happens to fail to parse. That is the same "an intact old
    // record with a valid CRC" hazard argued about below, left to luck.
    auto sizeError = std::error_code {};
    auto const onDisk = std::filesystem::file_size(_logPath, sizeError);
    if (sizeError)
        return std::unexpected { FastCache::StorageFailure(std::format("cannot size the log: {}", sizeError.message())) };

    if (start < onDisk)
    {
        auto error = std::error_code {};
        std::filesystem::resize_file(_logPath, start, error);
        if (error)
            return std::unexpected { FastCache::StorageFailure(
                std::format("cannot truncate the log: {}", error.message())) };

        if (!FlushToDisk(_log))
            return std::unexpected { FastCache::StorageFailure("cannot flush the truncated log") };
    }

    _offsets.resize(slot);

    if (!SeekTo(_log, start))
        return std::unexpected { FastCache::StorageFailure("cannot seek the log") };

    auto offset = start;
    auto index = append.fromIndex;
    for (auto const& entry: append.entries)
    {
        auto const body = EncodeLogRecord(index, entry);
        if (std::fwrite(body.data(), 1, body.size(), _log) != body.size())
            return std::unexpected { FastCache::StorageFailure("cannot write the log") };

        offset += body.size();
        index = index.Advanced(1);
        _offsets.push_back(offset);
    }

    if (!FlushToDisk(_log))
        return std::unexpected { FastCache::StorageFailure("cannot flush the log") };

    return {};
}

std::expected<std::optional<RaftSnapshot>, ConsensusError> FileRaftStorage::ReadSnapshot()
{
    auto raw = std::vector<std::byte> {};
    if (auto read = ReadWholeFile(_snapshotPath, raw); !read.has_value())
        return std::unexpected { read.error() };

    if (raw.empty())
        return std::optional<RaftSnapshot> {};

    auto parsed = ParseSnapshot(raw);
    if (!parsed.has_value())
        return std::unexpected { parsed.error() };

    return std::optional<RaftSnapshot> { *std::move(parsed) };
}

std::expected<RecoveredState, ConsensusError> FileRaftStorage::Load()
{
    auto recovered = RecoveredState {};

    auto raw = std::vector<std::byte> {};
    if (auto read = ReadWholeFile(_statePath, raw); !read.has_value())
        return std::unexpected { read.error() };

    if (!raw.empty())
    {
        auto const parsed = ParseState(raw);
        if (!parsed.has_value())
            return std::unexpected { parsed.error() };

        recovered.state = *parsed;
    }

    auto snapshot = ReadSnapshot();
    if (!snapshot.has_value())
        return std::unexpected { snapshot.error() };

    recovered.snapshot = *std::move(snapshot);

    // The same walk `Open` performs, rather than a second copy of it: the offset
    // table and the entries come from one pass over one grammar, so a rule that
    // changes cannot change for only one of them.
    if (auto scanned = ScanLog(&recovered.entries); !scanned.has_value())
        return std::unexpected { scanned.error() };

    // A log trimmed to nothing carries no record to state where it begins, so the
    // snapshot answers instead -- the same repair `Open` makes, and needed in both
    // because either can be the first to touch the store after a restart.
    if (recovered.entries.empty() && recovered.snapshot.has_value())
        _firstIndex = recovered.snapshot->lastIncludedIndex.Advanced(1);

    recovered.firstIndex = _firstIndex;
    return recovered;
}

} // namespace FastCache::Consensus
