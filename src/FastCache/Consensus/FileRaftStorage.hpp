// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftStorage.hpp>
#include <FastCache/Core/Owner.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace FastCache::Consensus
{

/// `IRaftStorage` backed by three files in a directory.
///
/// ## Why separate files rather than one
///
/// Term and vote change once per election; the log changes on every entry; a
/// snapshot is written rarely and read only at startup. One file would mean
/// rewriting the log whenever a vote was cast, or threading the small records
/// through an append-structured file and compacting them later. Separate files
/// make each write proportional to what actually changed.
///
/// ## How each is made crash-safe, and how the two differ
///
/// The **state** file is rewritten whole, into a temporary beside it, flushed,
/// and renamed over the original. Rename is the only single filesystem operation
/// that replaces a file's contents indivisibly, so a crash leaves either the
/// previous state or the new one and never a half of each — which matters
/// because a half-written vote is indistinguishable from no vote, and a node that
/// forgets a vote it already gave can vote again in the same term.
///
/// The **log** is appended to and truncated in place, with a CRC per record.
/// It cannot use the same trick, because rewriting it whole on every entry is
/// the cost the two-file split exists to avoid. Instead a torn tail is *expected*
/// and handled on load: records are read until one fails to parse or fails its
/// CRC, and everything from there is discarded. That is correct rather than
/// lenient — a record that was still being written was never acknowledged to a
/// leader, so no one can have committed on it.
///
/// The **snapshot** is written the state file's way, whole and renamed over, for
/// the state file's reason: it is small by design and replaced rather than
/// extended. What is not interchangeable is the *order* it stands in relative to
/// the log. It is made durable **first**, and only then is the prefix it covers
/// dropped from the log — because a crash between the two leaves a snapshot plus
/// entries it already covers, which `RaftNode` reconciles on recovery, whereas
/// the opposite order leaves a log missing committed entries and a snapshot that
/// never arrived, which nothing can repair.
///
/// That ordering is also why each log record carries its own **index**. A
/// positional file cannot say where a trimmed log now begins, so the crash window
/// above would recover entry 8 as entry 1 — every index in the store off by the
/// length of the discarded prefix, silently.
///
/// The residual, recorded deliberately: `fsync` on the file does not make the
/// *directory entry* durable on POSIX, so a crash immediately after the rename
/// can in principle leave the old name pointing at the old inode. Closing that
/// means opening and syncing the parent directory, which has no Windows
/// equivalent and would be one more platform branch; the exposure is a single
/// election's vote, which the surrounding term rules already tolerate losing.
class FileRaftStorage final: public IRaftStorage
{
  public:
    /// Open (or create) the store in `directory`.
    ///
    /// A factory rather than a constructor because opening files is fallible and
    /// a half-open store is not a thing a caller should be handed.
    /// @param directory Where the two files live; created if absent.
    /// @return The store, or why it could not be opened.
    [[nodiscard]] static std::expected<FileRaftStorage, ConsensusError> Open(std::filesystem::path const& directory);

    FileRaftStorage(FileRaftStorage const&) = delete;
    FileRaftStorage& operator=(FileRaftStorage const&) = delete;
    FileRaftStorage(FileRaftStorage&& other) noexcept;
    FileRaftStorage& operator=(FileRaftStorage&& other) noexcept;
    ~FileRaftStorage() override;

    [[nodiscard]] std::expected<void, ConsensusError> SaveState(PersistentState const& state) override;
    [[nodiscard]] std::expected<void, ConsensusError> SaveLog(LogAppend const& append) override;
    [[nodiscard]] std::expected<void, ConsensusError> SaveSnapshot(RaftSnapshot const& snapshot) override;
    [[nodiscard]] std::expected<RecoveredState, ConsensusError> Load() override;

  private:
    FileRaftStorage() = default;

    /// Close the log handle if one is open.
    void CloseLog() noexcept;

    /// Byte offset at which the record for `index` begins.
    /// @param index One-based log index; one past the end yields the log's end.
    /// @return The offset, clamped into the table.
    [[nodiscard]] std::uint64_t OffsetOf(LogIndex index) const noexcept;

    /// Rebuild the offset table by walking the log file.
    ///
    /// Called from `Open`, so the table is valid before any write can use it.
    /// @param entries Receives the decoded entries when non-null.
    /// @return Nothing, or why the log could not be read.
    [[nodiscard]] std::expected<void, ConsensusError> ScanLog(std::vector<LogEntry>* entries);

    /// Read the snapshot file, if there is one.
    /// @return The snapshot, absent when none is stored, or why it is unreadable.
    [[nodiscard]] std::expected<std::optional<RaftSnapshot>, ConsensusError> ReadSnapshot();

    /// Rewrite the log without the entries at or below `through`.
    ///
    /// Written beside the log and renamed over it, so the file is only ever the
    /// whole old log or the whole new one. Called only after the snapshot covering
    /// those entries is durable.
    /// @param through The last index the snapshot covers.
    /// @return Nothing, or why the log could not be rewritten.
    [[nodiscard]] std::expected<void, ConsensusError> TrimLogThrough(LogIndex through);

    std::filesystem::path _statePath;
    std::filesystem::path _logPath;
    std::filesystem::path _snapshotPath;

    /// Open handle on the log, kept for the life of the store: reopening per
    /// append would put a path resolution and a permission check on every entry.
    gsl::owner<std::FILE*> _log { nullptr };

    /// Offset of each stored record, plus a trailing end-of-log sentinel.
    ///
    /// `_offsets[i]` is where log index `i + 1` starts, and `_offsets.back()` is
    /// the end of the last record — so the table always holds one more element
    /// than there are entries, and is never empty. The sentinel is what makes
    /// "where does the next entry go?" answerable: without it an append past the
    /// end has to guess, and guessing the last record's start overwrites it.
    std::vector<std::uint64_t> _offsets { 0 };

    /// Index of the first record in the log file; 1 until a snapshot trims it.
    ///
    /// Read from that record rather than assumed, which is what a per-record index
    /// buys: an empty log after a trim would otherwise have nothing left to say
    /// where the next entry belongs.
    LogIndex _firstIndex { .value = 1 };
};

} // namespace FastCache::Consensus
