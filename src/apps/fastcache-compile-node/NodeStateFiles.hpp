// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdminEndpoint.hpp"
#include "NodeFormation.hpp"
#include "NodeKey.hpp"

#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/StateFiles.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

/// @file NodeStateFiles.hpp
/// Every file a node keeps in its state directory, and who may have put it there.
///
/// **A file in the state directory is this node's only if nobody else could have WRITTEN it.**
/// The key is not the only file the node acts on: the id it answers to, the formation record it
/// runs by, the consensus store it replays, the endpoints it hands its credentials to and the
/// roster it verifies grants against are each as decisive, and each is one another account could
/// have planted with the contents it chose. So every entry of the state directory is asked who
/// owns it, and one another account owns is refused by name (`NodeKeyFault::ForeignOwner`),
/// naming the file, what it holds and the owner, and left as it is.
///
/// One row per file, each naming what trusting another account's copy would make this node do,
/// keyed by `StateFile`: the one enum every writer takes its file's name from
/// (`Core/StateFiles.hpp`). A new state file is a new enumerator, which does not compile without
/// a row here -- and a test runs every writer into a scratch directory besides, and requires a row
/// for each file it finds and a file for each row.

namespace FastCache::Node
{

/// Probe how the state files in @p directory are replaced, and say so when the replace FALLS BACK:
/// a Warn naming the directory and the refusal, and `StateFileReplacesFellBack`.
///
/// A fallback is the classic rename, which every replace still lands with -- but on Windows a reader
/// holding the file open then refuses it, which is the failure the POSIX-semantics rename exists to
/// close. `MeansNoPosixRename` is broad enough to take a path form the rename refuses for a
/// filesystem that has it, so the degraded property is said where it is found, once per body.
/// @param directory The node's state directory.
/// @param rename The POSIX-semantics rename every replace tries first.
/// @param logger Where a fallback, or a probe that could not run, is said.
/// @param metrics Where a fallback is counted.
/// @return The route, or nothing when the probe could not be written.
std::optional<Consensus::ReplaceRoute> ReportReplaceRoute(std::filesystem::path const& directory,
                                                          Consensus::IReplacingRename const& rename,
                                                          ILogger& logger,
                                                          IMetricsSink& metrics);

/// What a node does with a state file another account owns.
///
/// **Private: never transmitted or persisted.**
enum class ForeignStateFileAnswer : std::uint8_t
{
    /// The node acts on what the file holds, so it refuses to start while the file is there.
    RefuseStart,
    /// The file is a convenience the node may start without -- no state of a history file may
    /// keep a node from starting -- so it is set aside where it is read: not read, never written
    /// over, and named in a warning (`SetAsideForeignHistory`).
    StartWithout,
};

/// One file of the state directory.
struct NodeStateFileRow
{
    StateFile file;                ///< Which file; its NAME is `StateFileNames`', never this row's.
    std::string_view holds;        ///< What it holds, as a phrase: "this node's identity key".
    std::string_view trusting;     ///< What adopting another account's copy would make this node do.
    std::string_view remedy;       ///< What the operator does about another account's copy.
    ForeignStateFileAnswer answer; ///< Whether such a copy refuses the start or is set aside.

    /// @return The file's name in the state directory, from the one table that names it.
    [[nodiscard]] constexpr std::string_view Name() const noexcept
    {
        return StateFileName(file);
    }
};

/// Every file a node keeps in its state directory, one row per `StateFile` in enumerator order --
/// so a file named anywhere is one this table answers for, since a name exists only as a
/// `StateFile` and a `StateFile` without a row here does not compile.
/// @return The rows.
[[nodiscard]] std::span<NodeStateFileRow const> NodeStateFiles();

/// The suffixes a writer appends for the temporary it renames into place -- what a crash between
/// the write and the rename leaves behind, beside the file it was replacing.
/// @return The suffixes, each the writer's own constant.
[[nodiscard]] std::span<std::string_view const> NodeStateTemporarySuffixes();

/// The row an entry of the state directory belongs to.
/// @param name The entry's name, directly in the state directory.
/// @return The row naming it, or the row of the file it is a temporary of; null for neither.
[[nodiscard]] NodeStateFileRow const* NodeStateFileRowOf(std::string_view name);

/// Refuse the state directory when another account owns anything in it the node would act on.
///
/// Walks every entry, at any depth, and asks @p guard who owns it. Only `Another` refuses: this
/// node's own account and an administrative one may have written it, and an owner the platform
/// would not name is not evidence either way -- the same standing `ResolveNodeKey` gives the key.
/// A row whose answer is `StartWithout` is asked where it is read instead. An entry no row names,
/// temporaries aside, is refused WHOEVER owns it (`NodeKeyFault::UnknownEntry`): the node writes
/// nothing without a row, so it cannot say what such an entry is. And a file OTHER ACCOUNTS MAY
/// WRITE is refused as another account's is (`NodeKeyFault::OthersMayWrite`), naming the command
/// that restricts it: whoever owns it, its contents are whoever-may-write's to choose.
/// A LINK is refused before its owner is asked (`NodeKeyFault::LinkEntry`): the node writes none,
/// and a link's owner answers for the link rather than for what it points at. Nothing is read,
/// moved or removed.
/// @param stateDirectory Where this node keeps its state. An absent directory holds nothing.
/// @param guard Who owns each entry.
/// @return Nothing, or the refusal naming the first entry another account owns.
[[nodiscard]] std::expected<void, NodeKeyRefusal> RefuseForeignStateFiles(std::filesystem::path const& stateDirectory,
                                                                          INodeKeyFileGuard& guard);

/// What an operator is told beside a state file, or the consensus store's directory, this node
/// cannot open: the command that hands it to the service's account, since a command run as
/// another account (`--print-identity`, `--enroll-from`, an install) before the service's first
/// start writes as that account -- `icacls /setowner` on Windows, where a file with a list of its
/// own never takes the grant the directory later gives the service, and `chown` on POSIX.
/// @param path The file, or a directory (then the command recurses).
/// @return A clause to append, opening with its own separator, or empty.
[[nodiscard]] std::string StateFileUnreadableHint(std::filesystem::path const& path);

/// What @p stateDirectory holds about this node's formation, read the way a start reads it.
///
/// The directory's writers are judged first, then who owns each file (`JudgeStateDirectory`), and
/// only then the record and the remembered endpoints (`ReadKeptFormation`), so a record another
/// account could have written is never read. A refusal carries `StateFileUnreadableHint`'s remedy
/// for the record. The one reading `main` makes for every invocation, judged from the account
/// running it; `--install-service` reads the record again after its handover, without the owner
/// judgement whose answer depends on that account (`StateDirectoryFormationReader`).
/// @param stateDirectory The directory the node resolved (`ChosenStateDirectory`).
/// @return What is kept, or why it cannot be read.
[[nodiscard]] std::expected<KeptFormation, std::string> ReadStateDirectoryFormation(
    std::filesystem::path const& stateDirectory);

/// Set aside every history file another account owns, or other accounts may write, where the
/// histories are read.
///
/// A set-aside path is cleared, so the store starts empty and never writes over the file, and a
/// warning names it. The histories' answer (`ForeignStateFileAnswer::StartWithout`), applied at
/// the one place they are loaded.
/// @param paths Where the histories live.
/// @param guard Who owns each file.
/// @param logger Where each set-aside file is named.
/// @return @p paths, without the files another account owns.
[[nodiscard]] HistoryPaths SetAsideForeignHistory(HistoryPaths paths, INodeKeyFileGuard& guard, ILogger& logger);

} // namespace FastCache::Node
