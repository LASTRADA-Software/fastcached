// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Owner.hpp>
#include <FastCache/Core/StateFiles.hpp>

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

/// The rename a replace moves its temporary into place with: POSIX semantics first.
///
/// A seam so the answer a filesystem gives can be scripted: what makes a replace FALL BACK to the
/// classic rename is a refusal no test volume produces on demand.
class IReplacingRename
{
  public:
    IReplacingRename() = default;
    IReplacingRename(IReplacingRename const&) = delete;
    IReplacingRename(IReplacingRename&&) = delete;
    IReplacingRename& operator=(IReplacingRename const&) = delete;
    IReplacingRename& operator=(IReplacingRename&&) = delete;
    virtual ~IReplacingRename() = default;

    /// Rename @p from over @p to with POSIX semantics: the name moves at once, and a reader holding
    /// @p to open with delete sharing keeps reading the file it opened.
    /// @param from The file to move.
    /// @param to Where it goes; replaced when it exists.
    /// @return Nothing, or the platform's own refusal, untranslated.
    [[nodiscard]] virtual std::error_code RenameReplacing(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const = 0;
};

/// The platform's: `FileRenameInfoEx` with `FILE_RENAME_FLAG_POSIX_SEMANTICS` on Windows, and
/// `rename(2)` everywhere else, which has those semantics already.
class SystemReplacingRename final: public IReplacingRename
{
  public:
    /// @copydoc IReplacingRename::RenameReplacing
    [[nodiscard]] std::error_code RenameReplacing(std::filesystem::path const& from,
                                                  std::filesystem::path const& to) const override;
};

/// Whether @p refusal is a filesystem saying it has no POSIX-semantics rename -- the information
/// class or its flags not understood -- rather than anything about the files. Such a refusal is
/// answered by the classic rename; every other is a failure of the replace.
///
/// `ERROR_INVALID_PARAMETER` is among them and it is BROAD: a path form the call will not take
/// answers it on a filesystem that DOES have the rename. So a fallback is never silent -- see
/// `ProbeReplaceRoute`.
/// @param refusal What `IReplacingRename::RenameReplacing` answered.
/// @return True when the classic rename is the answer.
[[nodiscard]] bool MeansNoPosixRename(std::error_code refusal) noexcept;

/// How a replace moved its file into place. **Private**: never transmitted or persisted.
enum class ReplaceRoute : std::uint8_t
{
    PosixSemantics, ///< The rename a reader holding the file open does not refuse.
    Classic,        ///< The fallback: a reader holding the file open on Windows refuses it.
};

/// What a replace did, and why it fell back when it did.
struct ReplacedBy
{
    ReplaceRoute route { ReplaceRoute::PosixSemantics }; ///< How the file moved into place.
    std::error_code posixRefusal;                        ///< What refused the POSIX rename; empty unless `Classic`.
};

/// `ReplaceFileAtomically` through @p rename, saying which route it took.
/// @param path What to replace.
/// @param body The new contents.
/// @param which Which state file it is, and so who may read it.
/// @param rename The POSIX-semantics rename tried first.
/// @return How it was replaced, or why it could not be.
[[nodiscard]] std::expected<ReplacedBy, ConsensusError> ReplaceFileWith(std::filesystem::path const& path,
                                                                        std::span<std::byte const> body,
                                                                        StateFile which,
                                                                        IReplacingRename const& rename);

/// What `ProbeReplaceRoute` names the file it replaces in a directory.
inline constexpr std::string_view ReplaceProbeFileName = ".replace-probe";

/// Replace a probe file in @p directory the way every state file there is replaced, and say which
/// route it took -- so a node whose replaces FALL BACK says so once, at its start, rather than
/// losing the reader-proof rename in silence. The probe is removed afterwards.
///
/// The same path form and directory every state file there uses, which is what decides the
/// fallback: a filesystem without the rename, or a path form the rename will not take.
/// @param directory The state directory.
/// @param rename The POSIX-semantics rename tried first.
/// @return How a replace there moves its file, or why the probe could not be written.
[[nodiscard]] std::expected<ReplacedBy, ConsensusError> ProbeReplaceRoute(std::filesystem::path const& directory,
                                                                          IReplacingRename const& rename);

/// Replace `path` with `body`, indivisibly.
///
/// Written beside the target and renamed over it: rename is the only single
/// filesystem operation that replaces a file's contents in one step, so a
/// crash leaves either the whole previous file or the whole new one. On Windows the rename has POSIX
/// semantics, so a reader that holds the file open (`OpenForReading`) does not refuse it; a
/// filesystem that has no such rename is renamed over the classic way (`MeansNoPosixRename`), which
/// `ProbeReplaceRoute` makes visible. The new file is created with
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
