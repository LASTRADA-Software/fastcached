// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Owner.hpp>
#include <FastCache/Core/StateFiles.hpp>
#include <FastCache/Platform/ReplacingRename.hpp>

#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

/// @file DurableFile.hpp
/// Reading a file whole and replacing one indivisibly and durably, for every file a node keeps
/// in its state directory and reads back as a unit: the Raft state and snapshot, and the roster
/// a worker adopted (#178).
///
/// One copy, because every step here fails for its own reason -- a path that is a directory,
/// a permission, a disk that gave up mid-read, a power loss between the write and the platter
/// -- and a second author of the same four steps gets one of them wrong.
namespace FastCache::Consensus
{

/// Open a file in binary mode, spelling the path the way the platform wants.
///
/// `std::fopen` takes a narrow path, which on Windows is converted through
/// the active code page — so a directory containing a character that page
/// cannot represent would fail to open for a reason having nothing to do with
/// the storage. `_wfsopen` takes the `wstring` the path already holds there.
/// @param path File to open.
/// @param mode An `fopen` mode string.
/// @return The stream, or nullptr.
[[nodiscard]] gsl::owner<std::FILE*> OpenBinary(std::filesystem::path const& path, char const* mode);

/// A stream that closes itself.
using ReadStream = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

/// Open @p path for reading, in binary mode, without keeping it from being REPLACED.
///
/// A reader holding the file open while another thread replaces it (`ReplaceFileAtomically`) must
/// not make that replace fail, and on Windows it takes BOTH halves -- measured on NTFS and ReFS: the
/// reader opened with delete sharing, which `_wfopen` never asks for, AND the replace renaming with
/// POSIX semantics. Either alone and the rename is refused. A reader keeps reading the file it
/// opened. On POSIX an open handle never keeps a rename from replacing a file.
/// @param path File to open.
/// @return The stream, or why it could not be opened.
[[nodiscard]] std::expected<ReadStream, std::error_code> OpenForReading(std::filesystem::path const& path);

/// Make the directory entries a rename just wrote in @p directory survive a power loss.
///
/// A file flushed to the platter and renamed into place is NOT yet durable: the rename changed the
/// DIRECTORY, and until the directory is flushed too, a power loss can bring back the entry that
/// named the old file -- for the Raft term and vote, a vote forgotten, and a node that votes twice
/// in one term. So every replace syncs the parent directory after its rename.
///
/// POSIX: the directory opened read-only and `fsync`ed. Windows: `FlushFileBuffers` on a handle
/// to the directory opened with `FILE_FLAG_BACKUP_SEMANTICS` and `FILE_WRITE_DATA` (for a
/// directory, the add-file right a replace already needs). MEASURED (Windows 11 26200, an
/// unelevated process, NTFS and ReFS): that call succeeds; the same call on a handle opened with
/// read access only is refused with `ERROR_ACCESS_DENIED`. `MOVEFILE_WRITE_THROUGH` is not the
/// answer here: it belongs to `MoveFileEx`, and the replace renames through
/// `SetFileInformationByHandle` (`Platform::RenameIntoPlace`), which has no write-through flag.
/// That the flush makes the entry durable is Microsoft's documented behaviour, not something a
/// test here measured -- only a power cut could.
/// @param directory The directory the rename wrote into.
/// @return Nothing, or why the directory could not be opened or flushed.
[[nodiscard]] std::error_code SyncDirectoryToDisk(std::filesystem::path const& directory);

/// Flush a stream all the way to the platter.
///
/// `fflush` alone only pushes the C library's buffer into the kernel, which a
/// power loss still discards -- so it is the pair that makes a write durable,
/// and the reason this is one helper rather than two calls at each site.
/// @param file The open stream.
/// @return True when both stages succeeded.
[[nodiscard]] bool FlushToDisk(std::FILE* file) noexcept;

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
/// @return The bytes; NOTHING when there is no file at all, which is not the same answer as
///         an empty one; or why it could not be read.
[[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, ConsensusError> ReadFileIfPresent(
    std::filesystem::path const& path);

/// What `ReplaceFileAtomically` appends to a file's name for the temporary it writes first, and
/// renames into place. A crash between the two leaves one behind, so a directory holding such a
/// file names its temporaries through this rather than through a literal of its own.
inline constexpr std::string_view ReplacementSuffix = ".tmp";

/// One temporary a replace writes: its bytes, then flushed to the platter, then a CHECKED close.
///
/// Three steps rather than one write, because each fails for its own reason and each is a property
/// of the replace: `Sync` is what survives a power loss (`FlushToDisk`), and `Close` is where a
/// buffered write can first meet a full volume -- a write whose close was not asked is one nobody
/// knows landed. A seam so each can be failed, and seen to be asked, without a failing disk.
class IDurableSink
{
  public:
    IDurableSink() = default;
    IDurableSink(IDurableSink const&) = delete;
    IDurableSink(IDurableSink&&) = delete;
    IDurableSink& operator=(IDurableSink const&) = delete;
    IDurableSink& operator=(IDurableSink&&) = delete;
    virtual ~IDurableSink() = default;

    /// Append @p bytes.
    /// @param bytes What to write.
    /// @return Nothing, or why they could not all be written.
    [[nodiscard]] virtual std::error_code Write(std::span<std::byte const> bytes) = 0;

    /// Flush what was written all the way to the platter.
    /// @return Nothing, or why it could not be.
    [[nodiscard]] virtual std::error_code Sync() = 0;

    /// Close the file. Called once, before the file is renamed or removed.
    /// @return Nothing, or why the close failed: what was written is not known to be stored.
    [[nodiscard]] virtual std::error_code Close() = 0;
};

/// How a replace creates its temporary -- with the access its state file's row gives it, which is the
/// caller's choice (`StateFile`) rather than the writer's -- and makes the rename that follows durable.
class IDurableFiles
{
  public:
    IDurableFiles() = default;
    IDurableFiles(IDurableFiles const&) = delete;
    IDurableFiles(IDurableFiles&&) = delete;
    IDurableFiles& operator=(IDurableFiles const&) = delete;
    IDurableFiles& operator=(IDurableFiles&&) = delete;
    virtual ~IDurableFiles() = default;

    /// Create @p path exclusively, with the access @p which's row gives it.
    /// @param path The temporary.
    /// @param which Which state file it will become.
    /// @return A sink over it, or why it could not be created.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IDurableSink>, std::error_code> Create(
        std::filesystem::path const& path, StateFile which) = 0;

    /// Flush @p directory, after a rename into it, so the new entry survives a power loss
    /// (`SyncDirectoryToDisk`).
    /// @param directory The directory the rename wrote into.
    /// @return Nothing, or why it could not be flushed.
    [[nodiscard]] virtual std::error_code SyncDirectory(std::filesystem::path const& directory) = 0;
};

/// This machine's: `CreateStateFile`, `fwrite`, `FlushToDisk`, a checked `fclose` and
/// `SyncDirectoryToDisk`.
class SystemDurableFiles final: public IDurableFiles
{
  public:
    /// @copydoc IDurableFiles::Create
    [[nodiscard]] std::expected<std::unique_ptr<IDurableSink>, std::error_code> Create(std::filesystem::path const& path,
                                                                                       StateFile which) override;

    /// @copydoc IDurableFiles::SyncDirectory
    [[nodiscard]] std::error_code SyncDirectory(std::filesystem::path const& directory) override;
};

/// `ReplaceFileAtomically` through @p files and @p rename, saying which route it took.
/// @param path What to replace.
/// @param body The new contents.
/// @param which Which state file it is, and so who may read it.
/// @param files How the temporary is created, written, synced and closed.
/// @param rename The POSIX-semantics rename tried first.
/// @return How it was replaced, or why it could not be.
[[nodiscard]] std::expected<Platform::ReplacedBy, ConsensusError> ReplaceFileWith(std::filesystem::path const& path,
                                                                                  std::span<std::byte const> body,
                                                                                  StateFile which,
                                                                                  IDurableFiles& files,
                                                                                  Platform::IReplacingRename const& rename);

/// What `ProbeReplaceRoute` names the file it replaces in a directory.
inline constexpr std::string_view ReplaceProbeFileName = ".replace-probe";

/// The temporary a replace of `ReplaceProbeFileName` writes first (`ReplacementSuffix`).
inline constexpr std::string_view ReplaceProbeTemporaryName = ".replace-probe.tmp";
static_assert(ReplaceProbeTemporaryName.size() == ReplaceProbeFileName.size() + ReplacementSuffix.size()
                  && ReplaceProbeTemporaryName.starts_with(ReplaceProbeFileName)
                  && ReplaceProbeTemporaryName.ends_with(ReplacementSuffix),
              "the probe's temporary is the probe's name with the writer's suffix");

/// Replace a probe file in @p directory the way every state file there is replaced, and say which
/// route it took -- so a node whose replaces FALL BACK says so once, at its start, rather than
/// losing the reader-proof rename in silence. The probe is removed afterwards.
///
/// The same path form and directory every state file there uses, which is what decides the
/// fallback: a filesystem without the rename, or a path form the rename will not take.
///
/// A crash inside the probe leaves `ReplaceProbeFileName` or `ReplaceProbeTemporaryName` behind.
/// Neither is ever READ: the next probe clears a stale temporary before it writes (every replace
/// does) and replaces, then removes, a stale probe -- so the state directory's judge names both
/// rather than refusing the next start over a file nothing trusts.
/// @param directory The state directory.
/// @param rename The POSIX-semantics rename tried first.
/// @return How a replace there moves its file, or why the probe could not be written.
[[nodiscard]] std::expected<Platform::ReplacedBy, ConsensusError> ProbeReplaceRoute(
    std::filesystem::path const& directory, Platform::IReplacingRename const& rename);

/// Replace `path` with `body`, indivisibly.
///
/// Written beside the target, flushed to the platter, closed with the close CHECKED, renamed over
/// it, and the directory flushed: rename is the only single filesystem operation that replaces a
/// file's contents in one step, so a crash leaves either the whole previous file or the whole new
/// one, and the directory sync is what keeps a power loss from bringing back the previous one after
/// the replace was reported (`SyncDirectoryToDisk`). A failed directory sync is a failed replace. On Windows the
/// rename has POSIX semantics (`Platform::RenameIntoPlace`), so a reader that holds the file open
/// (`OpenForReading`) does not refuse it; a filesystem that has no such rename is renamed over the
/// classic way, which `ProbeReplaceRoute` makes visible.
///
/// **The node's ONE durable writer**: every file it keeps in its state directory and reads back as
/// a unit is replaced through this -- the Raft state, log and snapshot, the roster, the formation
/// record and the remembered endpoints, the node's id and the fleet histories. The new file is created with
/// the access @p which's row of the state-file table gives it (`CreateStateFile`), so its mode is
/// that row's whatever the umask says.
/// @param path What to replace.
/// @param body The new contents.
/// @param which Which state file it is, and so who may read it.
/// @return Nothing, or why it could not be replaced.
[[nodiscard]] std::expected<void, ConsensusError> ReplaceFileAtomically(std::filesystem::path const& path,
                                                                        std::span<std::byte const> body,
                                                                        StateFile which);

} // namespace FastCache::Consensus
