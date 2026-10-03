// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeStateFiles.hpp"
#include "RaftStoreArchiver.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    /// What the whole consensus store's remedy is: its three files are one store.
    constexpr std::string_view ConsensusStoreRemedy =
        "Move the consensus store -- every raft file beside it -- aside, and this node catches up from its "
        "cluster's leader";

    /// What every history file's remedy is.
    constexpr std::string_view HistoryRemedy = "Remove it to keep this node's history there again";

    /// The sentence every refusal shares, naming the owner.
    [[nodiscard]] std::string OwnedByAnother(std::filesystem::path const& path, FileOwner const& owner)
    {
        return std::format("{} is owned by {}, which is neither the account this node runs as nor an administrative "
                           "one: another account put it there",
                           path.string(),
                           owner.name);
    }

    /// Why @p path, which other accounts may WRITE, refuses the start: whoever owns it, its
    /// contents are theirs to choose.
    [[nodiscard]] std::string WritableEntryRefusal(std::filesystem::path const& path, NodeStateFileRow const& row)
    {
        auto remedy = std::string {};
        for (auto const& command: OthersMayWriteRemedy(path))
            remedy += std::format("{}{}", remedy.empty() ? "" : ", then ", command);
        return std::format("{} can be written by accounts other than its owner and the administrators. It holds {}, "
                           "and this node will not {}. If nobody else can have written it, restrict it with: {}. "
                           "If somebody else may have, what it holds is theirs: {}. Nothing was changed",
                           path.string(),
                           row.holds,
                           row.trusting,
                           remedy,
                           row.remedy);
    }

    /// Why @p path refuses the start when whether others may write it could not be asked.
    ///
    /// **Cannot tell is a refusal, never a pass**: an integrity check that cannot answer has not
    /// answered no, and a file whose permissions this account cannot read is exactly what a
    /// planted one could present.
    [[nodiscard]] std::string UndeterminedWritersRefusal(std::filesystem::path const& path,
                                                         NodeStateFileRow const& row,
                                                         std::error_code error)
    {
        return std::format("{}: whether accounts other than its owner and the administrators may write it could not "
                           "be determined ({}). It holds {}, and this node will not {} while it cannot say who may "
                           "have written it. Make its permissions readable by the account this node runs as; "
                           "nothing was changed",
                           path.string(),
                           error.message(),
                           row.holds,
                           row.trusting);
    }

    /// Why @p path, which no row names, refuses the start whoever wrote it.
    [[nodiscard]] std::string UnknownEntryRefusal(std::filesystem::path const& path)
    {
        return std::format("{} is in the directory that holds this node's identity key, and this build keeps no "
                           "entry by that name: the node writes nothing there without a row, so it cannot say what "
                           "this is or who put it there. Remove it; nothing was changed",
                           path.string());
    }

    /// Why @p path, which another account owns, refuses the start.
    [[nodiscard]] std::string ForeignEntryRefusal(std::filesystem::path const& path,
                                                  FileOwner const& owner,
                                                  NodeStateFileRow const* row,
                                                  bool temporary)
    {
        if (row == nullptr)
            return std::format("{}, into the directory that holds this node's identity key, and this build keeps no "
                               "entry by that name. Remove it; nothing in the directory is trusted while another "
                               "account's entry is in it, and nothing was changed",
                               OwnedByAnother(path, owner));
        if (temporary)
            return std::format("{}, as a temporary of {}, which holds {}: nothing in the directory is trusted while "
                               "another account's entry is in it. Remove it; nothing was changed",
                               OwnedByAnother(path, owner),
                               row->Name(),
                               row->holds);
        return std::format("{}. It holds {}, and this node will not {}. {}; nothing was changed",
                           OwnedByAnother(path, owner),
                           row->holds,
                           row->trusting,
                           row->remedy);
    }
} // namespace

namespace
{
    /// One row per `StateFile`, in enumerator order.
    constexpr EnumTable<StateFile, NodeStateFileRow> Rows { {
        NodeStateFileRow { .file = StateFile::Key,
                           .holds = "this node's identity key",
                           .trusting = "prove itself with a secret somebody else chose and holds",
                           .remedy = "Remove it to mint this node's own, which the cluster must then admit",
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::Identity,
                           .holds = "the id this node answers to",
                           .trusting = "answer to an id somebody else chose",
                           .remedy = "Remove it, and this node mints an id its cluster must then admit",
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::Formation,
                           .holds = "the formation record this node runs by",
                           .trusting = "form or join the cluster somebody else chose",
                           .remedy = "Remove it, and this node starts on a record of its own, which its cluster "
                                     "must then admit again",
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::RaftState,
                           .holds = "this node's consensus term and vote",
                           .trusting = "vote by a term somebody else wrote",
                           .remedy = ConsensusStoreRemedy,
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::RaftLog,
                           .holds = "this node's consensus log",
                           .trusting = "replay commands somebody else wrote",
                           .remedy = ConsensusStoreRemedy,
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::RaftSnapshot,
                           .holds = "this node's consensus snapshot",
                           .trusting = "restore a cluster state somebody else wrote",
                           .remedy = ConsensusStoreRemedy,
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::FleetEndpoints,
                           .holds = "the endpoints this node remembers its cluster at",
                           .trusting = "dial, and prove itself to, an endpoint somebody else chose",
                           .remedy = "Remove it, and this node finds its cluster through its configuration",
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::Roster,
                           .holds = "the roster this machine verifies grants against",
                           .trusting = "accept grants signed by keys somebody else chose",
                           .remedy = "Remove it only if this machine should trust its --voter-key anchors again",
                           .answer = ForeignStateFileAnswer::RefuseStart },
        NodeStateFileRow { .file = StateFile::NodeHistory,
                           .holds = "this node's own history",
                           .trusting = "chart readings somebody else wrote",
                           .remedy = HistoryRemedy,
                           .answer = ForeignStateFileAnswer::StartWithout },
        NodeStateFileRow { .file = StateFile::FleetHistory,
                           .holds = "the fleet's history",
                           .trusting = "chart readings somebody else wrote",
                           .remedy = HistoryRemedy,
                           .answer = ForeignStateFileAnswer::StartWithout },
        NodeStateFileRow { .file = StateFile::ReceivedHistory,
                           .holds = "the history other machines handed over",
                           .trusting = "chart readings somebody else wrote",
                           .remedy = HistoryRemedy,
                           .answer = ForeignStateFileAnswer::StartWithout },
        NodeStateFileRow { .file = StateFile::Archive,
                           .holds = "the consensus stores of clusters this node left",
                           .trusting = "keep, as the store of a cluster it left, one somebody else put there",
                           .remedy = "Remove it: it holds only the stores of clusters this node has left, and this "
                                     "node never opens them again",
                           .answer = ForeignStateFileAnswer::RefuseStart },
    } };

    static_assert(RowsInEnumeratorOrder(Rows, &NodeStateFileRow::file),
                  "NodeStateFiles must hold one row per StateFile, in enumerator order");
} // namespace

std::span<NodeStateFileRow const> NodeStateFiles()
{
    return Rows;
}

std::span<std::string_view const> NodeStateTemporarySuffixes()
{
    static constexpr auto suffixes = std::array { Consensus::ReplacementSuffix, NodeIdentityReplacementSuffix };
    return suffixes;
}

NodeStateFileRow const* NodeStateFileRowOf(std::string_view name)
{
    auto const named = [](std::string_view wanted) -> NodeStateFileRow const* {
        for (auto const& row: NodeStateFiles())
            if (row.Name() == wanted)
                return &row;
        return nullptr;
    };
    if (auto const* row = named(name))
        return row;
    for (auto const suffix: NodeStateTemporarySuffixes())
        if (name.ends_with(suffix))
            if (auto const* row = named(name.substr(0, name.size() - suffix.size())))
                return row;
    return nullptr;
}

namespace
{
    /// The row an entry BELOW the state directory's top level answers as, or nullptr.
    ///
    /// The archive is the one directory the node writes, and its layout is fixed: one directory per
    /// archived store, named as `IsArchiveDirectoryName` allows -- the rule the archiver names them
    /// by -- holding nothing but that store, each file answering as its own raft row. Anything else
    /// down there is nothing this build wrote.
    /// @param relative The entry's path relative to the state directory.
    /// @return Its row, or nullptr for an entry no row names.
    [[nodiscard]] NodeStateFileRow const* NestedEntryRowOf(std::filesystem::path const& relative)
    {
        auto parts = std::vector<std::string> {};
        for (auto const& part: relative)
            parts.push_back(part.string());
        if (parts.size() < 2 || parts.size() > 3 || parts[0] != ArchiveDirectoryName || !IsArchiveDirectoryName(parts[1]))
            return nullptr;
        if (parts.size() == 2)
            return &Rows[static_cast<std::size_t>(StateFile::Archive)];
        if (!std::ranges::contains(Consensus::FileRaftStorage::StoreFileNames(), std::string_view { parts[2] }))
            return nullptr;
        return NodeStateFileRowOf(parts[2]);
    }
} // namespace

std::expected<void, NodeKeyRefusal> RefuseForeignStateFiles(std::filesystem::path const& stateDirectory,
                                                            INodeKeyFileGuard& guard)
{
    auto failure = std::error_code {};
    auto walk = std::filesystem::recursive_directory_iterator { stateDirectory, failure };
    if (failure == std::errc::no_such_file_or_directory)
        return {};
    auto const unlisted = [&stateDirectory](std::error_code const& error) {
        return std::unexpected { NodeKeyRefusal {
            .fault = NodeKeyFault::Unreadable,
            .message = std::format("cannot list {} to ask who owns what it holds: {}. Nothing in it is trusted until "
                                   "it can be",
                                   stateDirectory.string(),
                                   error.message()) } };
    };
    if (failure)
        return unlisted(failure);

    while (walk != std::filesystem::recursive_directory_iterator {})
    {
        auto const& entry = *walk;
        // A link before its owner: the owner of a link is not the owner of what it points at,
        // and the node writes none. Never followed -- the walk does not descend through one.
        if (guard.IsLink(entry.path()))
            return std::unexpected { NodeKeyRefusal {
                .fault = NodeKeyFault::LinkEntry,
                .message = std::format("{} is a link, and this node writes none into the directory that holds its "
                                       "identity key: somebody else put it there, pointing wherever they chose, and "
                                       "nothing it leads to is this node's to trust. Remove the link; nothing was "
                                       "changed",
                                       entry.path().string()) } };
        // An entry directly in the directory is one of the node's files by its name; deeper, only
        // the archive's fixed layout is anything this build keeps.
        auto const name = entry.path().filename().string();
        auto const* row =
            walk.depth() == 0 ? NodeStateFileRowOf(name) : NestedEntryRowOf(entry.path().lexically_relative(stateDirectory));
        auto const temporary = walk.depth() == 0 && row != nullptr && name != row->Name();
        // A temporary answers as the file it was replacing: a history's is never read either, and
        // is set aside where it is read.
        auto const refusesStart = row == nullptr || row->answer == ForeignStateFileAnswer::RefuseStart;
        if (auto const owner = guard.OwnerOf(entry.path()); owner.standing == FileOwnerStanding::Another && refusesStart)
            return std::unexpected { NodeKeyRefusal {
                .fault = NodeKeyFault::ForeignOwner, .message = ForeignEntryRefusal(entry.path(), owner, row, temporary) } };
        // An entry no row names is refused WHOEVER wrote it: the node writes nothing without a
        // row, so one there is nothing this build can say anything about -- and its own account's
        // copy is no less decisive than another's once something reads it.
        if (row == nullptr)
            return std::unexpected { NodeKeyRefusal { .fault = NodeKeyFault::UnknownEntry,
                                                      .message = UnknownEntryRefusal(entry.path()) } };
        // And WHO MAY WRITE it, beside who owns it: a file others can rewrite holds what they chose.
        // A history is set aside where it is read instead, as another account's is.
        if (refusesStart)
        {
            auto const writable = guard.OthersMayWrite(entry.path());
            if (!writable.has_value())
                return std::unexpected { NodeKeyRefusal {
                    .fault = NodeKeyFault::WritersUndetermined,
                    .message = UndeterminedWritersRefusal(entry.path(), *row, writable.error()) } };
            if (*writable)
                return std::unexpected { NodeKeyRefusal { .fault = NodeKeyFault::OthersMayWrite,
                                                          .message = WritableEntryRefusal(entry.path(), *row) } };
        }
        walk.increment(failure);
        if (failure)
            return unlisted(failure);
    }
    return {};
}

std::string StateFileUnreadableHint(std::filesystem::path const& path)
{
#if defined(_WIN32)
    auto isDirectory = std::error_code {};
    std::string_view const recurse = std::filesystem::is_directory(path, isDirectory) ? " /T" : "";
    return std::format(". If an elevated command (--print-identity, --enroll-from, an install) wrote it before the "
                       "service's first start, the service's account may not be able to read it; hand it over with: "
                       "icacls \"{}\" /setowner \"NT SERVICE\\<the service's name>\"{}",
                       path.string(),
                       recurse);
#else
    auto isDirectory = std::error_code {};
    std::string_view const recurse = std::filesystem::is_directory(path, isDirectory) ? " -R" : "";
    return std::format(". If a command run as another account (sudo --print-identity, --enroll-from, an install) "
                       "wrote it before the service's first start, the service's account may not be able to read it; "
                       "hand it over with: chown{} <the service's user> '{}'",
                       recurse,
                       path.string());
#endif
}

HistoryPaths SetAsideForeignHistory(HistoryPaths paths, INodeKeyFileGuard& guard, ILogger& logger)
{
    for (auto* const path: { &paths.fleet, &paths.node, &paths.received })
    {
        auto absent = std::error_code {};
        if (path->empty() || !std::filesystem::exists(*path, absent))
            continue;
        auto const owner = guard.OwnerOf(*path);
        // "Cannot tell" sets it aside too, the opposite answer to the state files' and for the
        // storage rule's reason: no state of a history file may keep a node from starting, so
        // the safe outcome here is starting without it rather than refusing.
        auto const writable = guard.OthersMayWrite(*path);
        if (owner.standing != FileOwnerStanding::Another && writable == std::expected<bool, std::error_code> { false })
            continue;
        auto const* row = NodeStateFileRowOf(path->filename().string());
        auto const who = [&] {
            if (owner.standing == FileOwnerStanding::Another)
                return OwnedByAnother(*path, owner);
            if (writable.has_value())
                return std::format("{} can be written by accounts other than its owner and the administrators",
                                   path->string());
            return std::format("whether accounts other than its owner and the administrators may write {} could not "
                               "be determined ({})",
                               path->string(),
                               writable.error().message());
        }();
        logger.Logf(LogLevel::Warn,
                    "{}. It holds {}, which this node starts without: it is not read and not written over, and "
                    "this node keeps no history there until it is gone. {}",
                    who,
                    row != nullptr ? row->holds : std::string_view { "a history" },
                    row != nullptr ? row->remedy : HistoryRemedy);
        path->clear();
    }
    return paths;
}

std::expected<KeptFormation, std::string> ReadStateDirectoryFormation(std::filesystem::path const& stateDirectory)
{
    FileTrustNodeKeyGuard guard;
    if (auto walked = JudgeStateDirectory(stateDirectory, guard); !walked.has_value())
        return std::unexpected { std::move(walked).error().message };
    Cluster::FileFormationStore const store { stateDirectory };
    Cluster::FleetEndpointsFile endpoints { stateDirectory };
    return ReadKeptFormation(store, endpoints).transform_error([&stateDirectory](std::string const& error) {
        return error + StateFileUnreadableHint(stateDirectory / Cluster::FormationRecordFileName);
    });
}

} // namespace FastCache::Node
