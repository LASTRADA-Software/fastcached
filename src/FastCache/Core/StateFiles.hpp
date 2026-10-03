// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

/// @file StateFiles.hpp
/// Every file a compile node keeps in its state directory, and the ONE place each is named.
///
/// Header-only and in Core, because the files are written from four layers -- consensus
/// (`FileRaftStorage`), cluster (`FileFormationStore`, `FleetEndpointsFile`), distributed
/// (`FileRosterStore`, the histories) and the node itself (its key and id) -- and a name spelled
/// in any one of them would be a state file the node's table of them (`Node::NodeStateFiles()`)
/// does not know. Every writer takes its name from here, the node's table is keyed by the same
/// enum, and both tables are asserted complete: a new file is a new enumerator, and an enumerator
/// without both rows does not compile.
///
/// **And its ACCESS**, for the same reason: who may read a state file is decided here, per row, and
/// the create every writer calls (`Platform/FileTrust`'s `CreateStateFile`) takes the file rather
/// than a mode -- so on POSIX a file's mode is the row's, EXACTLY, and never whatever the umask of
/// the process that happened to write it leaves.

namespace FastCache
{

/// A file of the node's state directory.
///
/// **Private: never transmitted or persisted** -- only its NAME reaches the disk, and that is
/// the row's, not the enumerator's value.
enum class StateFile : std::uint8_t
{
    Key,             ///< The node's identity key.
    Identity,        ///< The id it answers to.
    Formation,       ///< The formation record it runs by.
    RaftState,       ///< Its consensus term and vote.
    RaftLog,         ///< Its consensus log.
    RaftSnapshot,    ///< Its consensus snapshot.
    FleetEndpoints,  ///< The endpoints it remembers its cluster at.
    Roster,          ///< The roster it verifies grants against.
    NodeHistory,     ///< Its own history.
    FleetHistory,    ///< The fleet's history, recorded while leading.
    ReceivedHistory, ///< What the other machines handed over.
    Archive,         ///< A DIRECTORY: the consensus stores of clusters it left, one subdirectory each.
    Last,            ///< Not a file: the length of a table keyed by one.
};

/// Who, besides a state file's owner and the administrators, may read it. Nobody else may ever
/// WRITE one: a state file another account can write is one the node refuses to act on.
///
/// **Private: never transmitted or persisted.**
enum class StateFileAccess : std::uint8_t
{
    /// The secret: its owner's and the administrators' alone. POSIX mode 0600; on Windows a
    /// protected list of its own, whatever the directory would hand it.
    OwnerOnly,
    /// Integrity, not secrecy: every account may read it, only its owner writes it. POSIX mode
    /// 0644 and not 0600, because a verb run as ANOTHER account -- `sudo --print-identity`, an
    /// install -- writes files the service's account then reads, and a file
    /// that account cannot read refuses its start. On Windows the directory's list, inherited:
    /// that is how the grant the directory gives the service reaches a file an elevated command
    /// wrote.
    OthersRead,
    /// A directory its owner and the administrators alone may list, enter or add to: POSIX mode
    /// 0700 and, on Windows, a protected list of its own -- what `CreateOwnerOnlyDirectory` makes.
    /// Nobody but the node reads what it holds.
    OwnerOnlyDirectory,
    Last, ///< Not an access: the length of a table keyed by one.
};

/// What one access means on POSIX.
struct StateFileAccessRow
{
    StateFileAccess access;  ///< The access.
    std::uint16_t posixMode; ///< The file's mode, exactly: never narrowed or widened by the umask.
};

/// One row per `StateFileAccess`, in enumerator order.
inline constexpr EnumTable<StateFileAccess, StateFileAccessRow> StateFileAccessModes { {
    { .access = StateFileAccess::OwnerOnly, .posixMode = 0600 },
    { .access = StateFileAccess::OthersRead, .posixMode = 0644 },
    { .access = StateFileAccess::OwnerOnlyDirectory, .posixMode = 0700 },
} };

static_assert(RowsInEnumeratorOrder(StateFileAccessModes, &StateFileAccessRow::access),
              "StateFileAccessModes must hold one row per StateFileAccess, in enumerator order");

/// How one state file is named, and who may read it.
struct StateFileNameRow
{
    StateFile file;         ///< The file.
    std::string_view name;  ///< Its name in the state directory.
    StateFileAccess access; ///< Who besides its owner may read it.
};

/// One row per `StateFile`, in enumerator order.
inline constexpr EnumTable<StateFile, StateFileNameRow> StateFileNames { {
    { .file = StateFile::Key, .name = "node-key", .access = StateFileAccess::OwnerOnly },
    { .file = StateFile::Identity, .name = "node-id", .access = StateFileAccess::OthersRead },
    { .file = StateFile::Formation, .name = "formation", .access = StateFileAccess::OthersRead },
    { .file = StateFile::RaftState, .name = "raft-state", .access = StateFileAccess::OthersRead },
    { .file = StateFile::RaftLog, .name = "raft-log", .access = StateFileAccess::OthersRead },
    { .file = StateFile::RaftSnapshot, .name = "raft-snapshot", .access = StateFileAccess::OthersRead },
    { .file = StateFile::FleetEndpoints, .name = "fleet-endpoints", .access = StateFileAccess::OthersRead },
    { .file = StateFile::Roster, .name = "roster", .access = StateFileAccess::OthersRead },
    { .file = StateFile::NodeHistory, .name = "node-history.bin", .access = StateFileAccess::OthersRead },
    { .file = StateFile::FleetHistory, .name = "fleet-history.bin", .access = StateFileAccess::OthersRead },
    { .file = StateFile::ReceivedHistory, .name = "received-history.bin", .access = StateFileAccess::OthersRead },
    { .file = StateFile::Archive, .name = "archive", .access = StateFileAccess::OwnerOnlyDirectory },
} };

static_assert(RowsInEnumeratorOrder(StateFileNames, &StateFileNameRow::file),
              "StateFileNames must hold one row per StateFile, in enumerator order");

/// The name of @p file in the state directory.
/// @param file A state file.
/// @return Its row's name.
[[nodiscard]] constexpr std::string_view StateFileName(StateFile file) noexcept
{
    return StateFileNames[static_cast<std::size_t>(file)].name;
}

/// Who besides its owner may read @p file.
/// @param file A state file.
/// @return Its row's access.
[[nodiscard]] constexpr StateFileAccess StateFileAccessOf(StateFile file) noexcept
{
    return StateFileNames[static_cast<std::size_t>(file)].access;
}

/// The POSIX mode @p file is created with, exactly.
/// @param file A state file.
/// @return Its access's mode.
[[nodiscard]] constexpr std::uint16_t StateFilePosixMode(StateFile file) noexcept
{
    return StateFileAccessModes[static_cast<std::size_t>(StateFileAccessOf(file))].posixMode;
}

} // namespace FastCache
