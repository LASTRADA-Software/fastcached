// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <system_error>

/// @file ReplacingRename.hpp
/// The one rename that moves a written file over the one it replaces: POSIX semantics first, the
/// classic rename only where a filesystem has no POSIX-semantics one -- and which of the two it took
/// is SAID, never decided in silence.
///
/// **Dependency-free, and that is load-bearing**: `fastcache-cc` does not link `FastCache`, so it
/// compiles this TU in (`_fc_cc_core`), as it does `Environment.cpp`. The node's durable writer
/// (`Consensus/DurableFile`) and the launcher's atomic write (`Cc::WriteFileAtomically`) are two
/// POLICIES over this one rename -- one with fsync and a state file's access, one without -- and
/// were two copies of it until a fix landing in one could miss the other.
namespace FastCache::Platform
{

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
/// answers it on a filesystem that DOES have the rename. So a fallback is never silent: the route
/// travels back in `ReplacedBy`, and the node probes it at its start (`ProbeReplaceRoute`).
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

    /// What refused the directory sync after the rename, on a filesystem that cannot sync a directory
    /// at all (`Consensus::MeansDirectorySyncUnsupported`): the replace landed and is reported, and its
    /// survival of a power loss is as good as that filesystem makes it. Empty when the directory was
    /// synced -- every other refusal of the sync fails the replace instead.
    std::error_code directoryUnsynced;
};

/// Move @p from over @p to: through @p rename, and through the classic rename when the refusal says
/// the filesystem has no POSIX-semantics one (`MeansNoPosixRename`). Every other refusal is the
/// answer, and leaves @p to as it was; @p from is the caller's to remove.
/// @param from The written file.
/// @param to The file it replaces.
/// @param rename The POSIX-semantics rename tried first.
/// @return Which route moved it, or why neither did.
[[nodiscard]] std::expected<ReplacedBy, std::error_code> RenameIntoPlace(std::filesystem::path const& from,
                                                                         std::filesystem::path const& to,
                                                                         IReplacingRename const& rename);

} // namespace FastCache::Platform
