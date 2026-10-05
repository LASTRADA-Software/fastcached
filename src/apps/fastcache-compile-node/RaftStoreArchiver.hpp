// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FormationController.hpp"

#include <FastCache/Core/StateFiles.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// @file RaftStoreArchiver.hpp
/// Moving a left cluster's consensus store out of the state directory's root, and keeping it.

/// Where archived stores are kept: this directory, under the state directory, one subdirectory per
/// store. The state-file table's name, never a second spelling.
inline constexpr std::string_view ArchiveDirectoryName = StateFileName(StateFile::Archive);

/// What an archive being filled is called until it is complete: `<cluster-id>.partial`. A crash
/// leaves it behind, and the next archive of that cluster finishes it.
inline constexpr std::string_view ArchiveStagingSuffix = "partial";

/// How many archives of one cluster are kept apart -- `<id>`, then `<id>.1` up to this -- before a
/// further one is refused by name. A machine leaves the same cluster rarely; a thousand times is a
/// loop, not a history.
inline constexpr unsigned MaxArchivesPerCluster = 1000;

/// Whether @p clusterId may name an archive.
///
/// **An allowlist, and deliberately narrow**: the id is about to become a path, and a fleet's id
/// reached this node in a summary another machine signed. Letters, digits, `_` and `-`, not empty,
/// within the one id bound, and no Windows device name -- a minted id is lowercase hex, so nothing
/// this software mints is refused, and a separator, a drive, a stream name, a traversal or a dot
/// cannot be spelled. No dot, because a dot is what separates an id from its archive's suffix.
/// @param clusterId The id.
/// @return True when it is safe as the stem of one path component.
[[nodiscard]] bool IsArchivableClusterId(std::string_view clusterId) noexcept;

/// Whether @p name is a directory the archiver could have made directly under the archive:
/// `<id>`, `<id>.<n>` for 1 <= n < `MaxArchivesPerCluster`, or `<id>.partial`, where `<id>` passes
/// `IsArchivableClusterId`.
///
/// The start-time walk of the state directory asks this of every directory it finds there, so the
/// walk and the archiver cannot disagree about what the archiver may have written.
/// @param name A directory's name.
/// @return True when it is one of those.
[[nodiscard]] bool IsArchiveDirectoryName(std::string_view name) noexcept;

/// Moves `Consensus::FileRaftStorage::StoreFileNames()` from a state directory into
/// `<directory>/archive/<cluster-id>/`.
///
/// Renames, never copies: the store is kept whole, as it was, and the root is left without it -- which
/// is what keeps a node that left a cluster from ever opening that cluster's log again. Nothing else
/// in the directory moves; the identity key and id above all stay where they are.
///
/// **Filled under a staging name, then renamed into place**, so a complete archive is never written
/// to again: the files go into `<id>.partial` and, once the root holds none, that directory takes
/// the first free name of `<id>`, `<id>.1`, `<id>.2`, ... An archive of the same cluster left twice
/// therefore lands beside the first rather than over it or merged into it, and a run a crash
/// interrupted is finished from its staging directory, whichever step it stopped at.
class RaftStoreArchiver final: public IStoreArchiver
{
  public:
    /// @param directory The state directory the store sits in.
    explicit RaftStoreArchiver(std::filesystem::path directory);

    /// @copydoc IStoreArchiver::Archive
    ///
    /// A store file present both in the root and in the staging directory is refused by name rather
    /// than written over, because either copy may be the only one of something.
    [[nodiscard]] std::expected<void, std::string> Archive(std::string_view clusterId) override;

  private:
    std::filesystem::path _directory;
};

} // namespace FastCache::Node
