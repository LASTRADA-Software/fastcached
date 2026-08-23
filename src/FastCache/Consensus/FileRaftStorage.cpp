// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/Crc32c.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Owner.hpp>

#include <algorithm>
#include <array>
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
    constexpr std::uint32_t StateMagic = 0x46435253U; // "FCRS"
    constexpr std::uint32_t LogMagic = 0x4643524CU;   // "FCRL"

    /// Bumped when either layout changes. A version this build does not know is
    /// refused rather than guessed at: reading an unknown layout as if it were
    /// this one produces a log, not an error.
    constexpr std::uint16_t FormatVersion = 1;

    constexpr std::string_view StateFileName = "raft-state";
    constexpr std::string_view LogFileName = "raft-log";

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

    /// One decoded log record and how many bytes it occupied.
    struct DecodedRecord
    {
        LogEntry entry;
        std::size_t length {};
    };

    /// Decode one log record from the front of `raw`.
    /// @param raw Remaining file contents.
    /// @return The record, or nullopt when it is incomplete or corrupt.
    [[nodiscard]] std::optional<DecodedRecord> ParseLogRecord(std::span<std::byte const> raw)
    {
        constexpr auto header = sizeof(std::uint32_t) + sizeof(std::uint64_t) + 1 + sizeof(std::uint32_t);
        if (raw.size() < header + sizeof(std::uint32_t))
            return std::nullopt;

        auto cursor = std::size_t { 0 };
        auto const magic = ReadBigEndian<std::uint32_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint32_t);
        if (magic != LogMagic)
            return std::nullopt;

        auto entry = LogEntry {};
        entry.term = Term { .value = ReadBigEndian<std::uint64_t>(raw.subspan(cursor)) };
        cursor += sizeof(std::uint64_t);

        auto const kind = static_cast<std::uint8_t>(raw[cursor]);
        cursor += 1;
        if (kind > static_cast<std::uint8_t>(EntryKind::NoOp))
            return std::nullopt;

        entry.kind = static_cast<EntryKind>(kind);

        auto const payloadLength = ReadBigEndian<std::uint32_t>(raw.subspan(cursor));
        cursor += sizeof(std::uint32_t);

        auto const total = cursor + payloadLength + sizeof(std::uint32_t);
        if (total > raw.size() || !ChecksumHolds(raw.first(total)))
            return std::nullopt;

        auto const payload = raw.subspan(cursor, payloadLength);
        entry.payload.assign(payload.begin(), payload.end());

        return DecodedRecord { .entry = std::move(entry), .length = total };
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
    /// @param path What to read.
    /// @param into Destination, cleared first.
    /// @return True on success; a missing file is success with nothing read.
    [[nodiscard]] bool ReadWholeFile(std::filesystem::path const& path, std::vector<std::byte>& into)
    {
        into.clear();

        auto error = std::error_code {};
        if (!std::filesystem::exists(path, error) || error)
            return !error;

        auto const size = std::filesystem::file_size(path, error);
        if (error)
            return false;

        gsl::owner<std::FILE*> const file = OpenBinary(path, "rb");
        if (file == nullptr)
            return false;

        into.resize(static_cast<std::size_t>(size));
        auto const read = into.empty() ? std::size_t { 0 } : std::fread(into.data(), 1, into.size(), file);
        auto const ok = read == into.size();
        (void) std::fclose(file);

        if (!ok)
            into.clear();

        return ok;
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
    if (auto scanned = store.ScanLogOffsets(); !scanned.has_value())
        return std::unexpected { scanned.error() };

    return store;
}

FileRaftStorage::FileRaftStorage(FileRaftStorage&& other) noexcept:
    _statePath { std::move(other._statePath) },
    _logPath { std::move(other._logPath) },
    _log { std::exchange(other._log, nullptr) },
    _offsets { std::move(other._offsets) }
{
}

FileRaftStorage& FileRaftStorage::operator=(FileRaftStorage&& other) noexcept
{
    if (this != &other)
    {
        CloseLog();
        _statePath = std::move(other._statePath);
        _logPath = std::move(other._logPath);

        // Spelled out rather than `std::exchange`, whose return type drops the
        // `gsl::owner` annotation and so reads to clang-tidy as a raw handle being
        // assigned to an owning one.
        gsl::owner<std::FILE*> const adopted = other._log;
        other._log = nullptr;
        _log = adopted;

        _offsets = std::move(other._offsets);
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

    auto const wanted = std::max<std::uint64_t>(index.value, 1);
    auto const slot = std::min<std::size_t>(_offsets.size(), static_cast<std::size_t>(wanted));
    return _offsets[slot - 1];
}

std::expected<void, ConsensusError> FileRaftStorage::ScanLogOffsets()
{
    auto raw = std::vector<std::byte> {};
    if (!ReadWholeFile(_logPath, raw))
        return std::unexpected { FastCache::StorageFailure(std::format("cannot read {}", _logPath.string())) };

    _offsets.assign(1, 0);
    auto cursor = std::size_t { 0 };
    while (cursor < raw.size())
    {
        auto const record = ParseLogRecord(std::span { raw }.subspan(cursor));
        if (!record.has_value())
            break;

        cursor += record->length;
        _offsets.push_back(cursor);
    }

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

    // Written beside the target and renamed over it: rename is the only single
    // filesystem operation that replaces contents indivisibly, and a half-written
    // vote reads as no vote at all.
    auto const temporary = std::filesystem::path { _statePath }.concat(".tmp");
    gsl::owner<std::FILE*> const file = OpenBinary(temporary, "wb");
    if (file == nullptr)
        return std::unexpected { FastCache::StorageFailure(std::format("cannot open {}", temporary.string())) };

    auto const wrote = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    auto const flushed = wrote && FlushToDisk(file);
    (void) std::fclose(file);

    if (!flushed)
    {
        auto discard = std::error_code {};
        std::filesystem::remove(temporary, discard);
        return std::unexpected { FastCache::StorageFailure(std::format("cannot write {}", temporary.string())) };
    }

    auto error = std::error_code {};
    std::filesystem::rename(temporary, _statePath, error);
    if (error)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot replace {}: {}", _statePath.string(), error.message())) };

    return {};
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
    if (append.fromIndex.value > _offsets.size())
        return std::unexpected { FastCache::StorageFailure("a log append would leave a gap") };

    // fromIndex is a truncation point as well as a start, so anything at or after
    // it stops being part of the log.
    auto const start = OffsetOf(append.fromIndex);
    auto const wanted = std::max<std::uint64_t>(append.fromIndex.value, 1);
    auto const slot = std::min<std::size_t>(_offsets.size(), static_cast<std::size_t>(wanted));

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

    auto body = std::vector<std::byte> {};
    auto offset = start;
    for (auto const& entry: append.entries)
    {
        body.clear();
        PutBigEndian<std::uint32_t>(body, LogMagic);
        PutBigEndian<std::uint64_t>(body, entry.term.value);
        body.push_back(static_cast<std::byte>(entry.kind));
        PutBigEndian<std::uint32_t>(body, static_cast<std::uint32_t>(entry.payload.size()));
        body.insert(body.end(), entry.payload.begin(), entry.payload.end());
        PutBigEndian<std::uint32_t>(body, Crc32c::Compute(body));

        if (std::fwrite(body.data(), 1, body.size(), _log) != body.size())
            return std::unexpected { FastCache::StorageFailure("cannot write the log") };

        offset += body.size();
        _offsets.push_back(offset);
    }

    if (!FlushToDisk(_log))
        return std::unexpected { FastCache::StorageFailure("cannot flush the log") };

    return {};
}

std::expected<RecoveredState, ConsensusError> FileRaftStorage::Load()
{
    auto recovered = RecoveredState {};

    auto raw = std::vector<std::byte> {};
    if (!ReadWholeFile(_statePath, raw))
        return std::unexpected { FastCache::StorageFailure(std::format("cannot read {}", _statePath.string())) };

    if (!raw.empty())
    {
        auto const parsed = ParseState(raw);
        if (!parsed.has_value())
            return std::unexpected { parsed.error() };

        recovered.state = *parsed;
    }

    if (!ReadWholeFile(_logPath, raw))
        return std::unexpected { FastCache::StorageFailure(std::format("cannot read {}", _logPath.string())) };

    _offsets.assign(1, 0);
    auto cursor = std::size_t { 0 };
    while (cursor < raw.size())
    {
        auto const record = ParseLogRecord(std::span { raw }.subspan(cursor));
        if (!record.has_value())
            break; // A torn tail: it was never acknowledged, so nobody committed on it.

        recovered.entries.push_back(record->entry);
        cursor += record->length;
        _offsets.push_back(cursor);
    }

    return recovered;
}

} // namespace FastCache::Consensus
