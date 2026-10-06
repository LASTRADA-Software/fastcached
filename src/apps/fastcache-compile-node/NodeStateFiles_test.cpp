// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "EnrollClient.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeFormation.hpp"
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"
#include "NodeStateFiles.hpp"
#include "NodeSurfaces.hpp"
#include "RaftStoreArchiver.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
    #include <sys/stat.h>

    #include <unistd.h>
#endif

#include <core/Ranges.hpp>
#include <tests/AccessList.hpp>
#include <tests/FleetHistoryFakes.hpp>
#include <tests/NodeConditionFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/NodeKeyFakes.hpp>
#include <tests/ScopedUmask.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::ScriptedNodeKeyGuard;
using FastCache::Testing::ScriptedSecureRandom;

namespace
{

/// Write @p text to @p path, replacing whatever was there.
/// @param path The file.
/// @param text Its new contents.
void WriteText(std::filesystem::path const& path, std::string_view text)
{
    auto stream = std::ofstream { path, std::ios::binary | std::ios::trunc };
    stream << text;
}

/// The contents of a file, or empty when it cannot be read.
/// @param path The file.
/// @return Its contents.
[[nodiscard]] std::string TextOf(std::filesystem::path const& path)
{
    // The sized read rather than `std::istreambuf_iterator`, which the tree refuses (GCC at `-O3`
    // reports `-Werror=null-dereference` inside `<streambuf>` for it).
    auto stream = std::ifstream { path, std::ios::binary | std::ios::ate };
    if (!stream.is_open())
        return {};
    auto const size = stream.tellg();
    if (size <= 0)
        return {};
    stream.seekg(0, std::ios::beg);
    auto text = std::string(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), size);
    return text;
}

/// Every name directly in @p directory.
/// @param directory The directory.
/// @return The names, sorted.
[[nodiscard]] std::set<std::string> NamesIn(std::filesystem::path const& directory)
{
    auto names = std::set<std::string> {};
    for (auto const& entry: std::filesystem::directory_iterator { directory })
        names.insert(entry.path().filename().string());
    return names;
}

/// Whether any captured line contains @p needle.
/// @param logger Where it was reported.
/// @param needle Text to look for.
/// @return True when some line contains it.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view needle)
{
    auto const records = logger.Snapshot();
    return std::ranges::any_of(records, [needle](CapturingLogger::Record const& r) { return r.message.contains(needle); });
}

} // namespace

TEST_CASE("Every state file the node acts on refuses the start when another account owns it, and is left as it was",
          "[node][identity][key][secret][state]")
{
    // Through the key resolution, the seam every start and every one-shot verb crosses before it
    // acts on the state directory. One file planted at a time, beside a key that is this node's
    // own: the refusal names the planted file, what it holds and its owner, draws nothing, mints
    // nothing and changes nothing.
    auto visited = std::set<std::string_view> {};
    for (auto const& row: NodeStateFiles())
    {
        if (row.answer != ForeignStateFileAnswer::RefuseStart)
            continue;
        visited.insert(row.Name());
        CAPTURE(row.Name());
        ScratchDirectory const scratch { "node-state-foreign" };
        auto const planted = scratch.Path() / row.Name();
        WriteText(planted, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.OwnedByAnother(row.Name());
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().fault == NodeKeyFault::ForeignOwner);
        CHECK(resolved.error().message.contains(planted.string()));
        CHECK(resolved.error().message.contains(row.holds));
        CHECK(resolved.error().message.contains(row.remedy));
        CHECK(resolved.error().message.contains("scripted-owner"));
        CHECK(random.FillCount() == 0);
        CHECK(NamesIn(scratch.Path()) == std::set<std::string> { std::string { row.Name() } });
        CHECK(TextOf(planted) == "planted");
    }
    // A table that lost a row would pass the loop above by not visiting it, so the files named
    // as the ones a node acts on are asked for by their writers' own names.
    for (auto const name: { NodeKeyFileName,
                            NodeIdentityFileName,
                            Cluster::FormationRecordFileName,
                            Consensus::RaftStateFileName,
                            Consensus::RaftLogFileName,
                            Consensus::RaftSnapshotFileName,
                            Cluster::FleetEndpointsFileName })
        CHECK(visited.contains(name));
}

TEST_CASE("A history file another account owns is started without, never refused", "[node][identity][key][secret][state]")
{
    // No state of a history file may keep a node from starting: its answer is to be set aside
    // where it is read, so the key resolution passes it -- and a temporary answers as its file.
    auto visited = std::set<std::string_view> {};
    for (auto const& row: NodeStateFiles())
    {
        if (row.answer != ForeignStateFileAnswer::StartWithout)
            continue;
        visited.insert(row.Name());
        for (auto const& name: { std::string { row.Name() }, std::string { row.Name() } + ".tmp" })
        {
            CAPTURE(name);
            ScratchDirectory const scratch { "node-state-history" };
            WriteText(scratch.Path() / name, "planted");
            auto guard = ScriptedNodeKeyGuard::OwnerOnly();
            guard.OwnedByAnother(name);
            ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
            CHECK(ResolveNodeKey(scratch.Path(), random, guard).has_value());
            CHECK(TextOf(scratch.Path() / name) == "planted");
        }
    }
    for (auto const which: Enumerators<HistoryFile>())
        CHECK(visited.contains(HistoryFileNameOf(which)));
}

TEST_CASE("A foreign temporary of a state file, and a foreign entry no row names, refuse the start by name",
          "[node][identity][key][secret][state]")
{
    struct Case
    {
        std::string_view name;   // The planted entry, relative to the state directory.
        std::string_view expect; // What the refusal says about it.
    };
    for (auto const& [name, expect]: { Case { .name = "formation.tmp", .expect = "as a temporary of formation" },
                                       Case { .name = "node-id.tmp", .expect = "as a temporary of node-id" },
                                       Case { .name = "raft-log.tmp", .expect = "as a temporary of raft-log" },
                                       Case { .name = "planted", .expect = "keeps no entry by that name" } })
    {
        CAPTURE(name);
        ScratchDirectory const scratch { "node-state-unnamed" };
        auto const planted = (scratch.Path() / std::filesystem::path { name }).make_preferred();
        std::filesystem::create_directories(planted.parent_path());
        WriteText(planted, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.OwnedByAnother(planted.filename());
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().fault == NodeKeyFault::ForeignOwner);
        CHECK(resolved.error().message.contains(planted.string()));
        CHECK(resolved.error().message.contains(expect));
        CHECK(random.FillCount() == 0);
        CHECK(TextOf(planted) == "planted");
    }
}

TEST_CASE("An entry no row names is refused whoever wrote it, a directory included, and left as it is",
          "[node][identity][key][secret][state]")
{
    // The node writes nothing into its state directory without a row, so an entry no row names is
    // one it can say nothing about -- and its OWN account's copy is no less decisive than another's
    // once something reads it. The derive case below runs every writer into one directory and
    // requires the entries to equal the rows, so a new writer with no row fails there at its first
    // run; this is the start refusing what that case would have caught.
    struct Case
    {
        std::string_view planted; // What is planted, relative to the state directory.
        std::string_view refused; // The entry the refusal names.
    };
    for (auto const& [planted, refused]: { Case { .planted = "planted", .refused = "planted" },
                                           // A name the table knows, one level down, is not the node's
                                           // file: the directory holding it is refused first.
                                           Case { .planted = "nested/formation", .refused = "nested" } })
    {
        CAPTURE(planted);
        ScratchDirectory const scratch { "node-state-unknown" };
        auto const file = (scratch.Path() / std::filesystem::path { planted }).make_preferred();
        std::filesystem::create_directories(file.parent_path());
        WriteText(file, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().fault == NodeKeyFault::UnknownEntry);
        CHECK(resolved.error().message.contains((scratch.Path() / refused).string()));
        CHECK(resolved.error().message.contains("keeps no entry by that name"));
        CHECK(resolved.error().message.ends_with("nothing was changed"));
        CHECK(random.FillCount() == 0);
        CHECK(TextOf(file) == "planted");
    }

    // The control: the node's own temporaries are its files, and pass.
    ScratchDirectory const scratch { "node-state-unknown-control" };
    WriteText(scratch / "formation.tmp", "left by a crash");
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    CHECK(RefuseForeignStateFiles(scratch.Path(), guard).has_value());
}

TEST_CASE("A state file others may write is refused like another account's, named, and left as it is",
          "[node][identity][key][secret][state]")
{
    // Whoever OWNS a file, one other accounts can rewrite holds what they chose. Every row the node
    // acts on refuses the start; a history is set aside where it is read, as another account's is.
    for (auto const& row: NodeStateFiles())
    {
        CAPTURE(row.Name());
        ScratchDirectory const scratch { "node-state-writable" };
        auto const path = scratch / row.Name();
        WriteText(path, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.WritableByOthers(std::filesystem::path { row.Name() });

        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        if (row.answer == ForeignStateFileAnswer::StartWithout)
        {
            CHECK(walked.has_value());
            continue;
        }
        REQUIRE_FALSE(walked.has_value());
        CHECK(walked.error().fault == NodeKeyFault::OthersMayWrite);
        CHECK(walked.error().message.contains(path.string()));
        CHECK(walked.error().message.contains(row.holds));
        CHECK(walked.error().message.contains(OthersMayWriteRemedy(path).front()));
        CHECK(walked.error().message.contains(
            std::format("If somebody else may have, what it holds is theirs: {}.", row.remedy)));
        CHECK_FALSE(walked.error().message.contains("otherwise"));
        CHECK(walked.error().message.ends_with("Nothing was changed"));
        CHECK(TextOf(path) == "planted");
    }

    // The control: the same files, nobody else may write them.
    ScratchDirectory const scratch { "node-state-writable-control" };
    for (auto const& row: NodeStateFiles())
        WriteText(scratch / row.Name(), "mine");
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    CHECK(RefuseForeignStateFiles(scratch.Path(), guard).has_value());
}

TEST_CASE("A state file whose writers cannot be asked about refuses the start by name and is left as it is",
          "[node][identity][key][secret][state]")
{
    // "Cannot tell" is a REFUSAL, never a pass: an integrity check that cannot answer has not
    // answered no. Every row the node acts on refuses; a history is set aside where it is read.
    auto const error = std::make_error_code(std::errc::permission_denied);
    for (auto const& row: NodeStateFiles())
    {
        CAPTURE(row.Name());
        ScratchDirectory const scratch { "node-state-undetermined" };
        auto const path = scratch / row.Name();
        WriteText(path, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.WritersUnanswered(std::filesystem::path { row.Name() }, error);

        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        if (row.answer == ForeignStateFileAnswer::StartWithout)
        {
            CHECK(walked.has_value());
            continue;
        }
        REQUIRE_FALSE(walked.has_value());
        CHECK(walked.error().fault == NodeKeyFault::WritersUndetermined);
        CHECK(walked.error().message.contains(path.string()));
        CHECK(walked.error().message.contains(error.message()));
        CHECK(walked.error().message.contains(row.holds));
        CHECK(walked.error().message.ends_with("nothing was changed"));
        CHECK(TextOf(path) == "planted");
    }
}

TEST_CASE("A history file whose writers cannot be asked about is set aside, and says why", "[node][state][fleet]")
{
    // The opposite answer to the state files', for the storage rule's reason: no state of a
    // history file may keep a node from starting, so it is started without.
    ScratchDirectory const scratch { "node-state-undetermined-history" };
    auto cfg = NodeConfig {};
    cfg.clusterDir = scratch.Path();
    auto const paths = HistoryPaths::For(cfg);
    WriteText(paths.node, "planted");
    auto const error = std::make_error_code(std::errc::permission_denied);
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    guard.WritersUnanswered(paths.node.filename(), error);
    CapturingLogger logger;

    auto const kept = SetAsideForeignHistory(paths, guard, logger);
    CHECK(kept.node.empty());
    CHECK(kept.fleet == paths.fleet);
    CHECK(Logged(logger, "could not be determined"));
    CHECK(Logged(logger, error.message()));
    CHECK(TextOf(paths.node) == "planted");
}

TEST_CASE("A history file others may write is set aside where it is read, and named", "[node][state][fleet]")
{
    ScratchDirectory const scratch { "node-state-writable-history" };
    auto cfg = NodeConfig {};
    cfg.clusterDir = scratch.Path();
    auto const paths = HistoryPaths::For(cfg);
    WriteText(paths.node, "planted");
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    guard.WritableByOthers(paths.node.filename());
    CapturingLogger logger;

    auto const kept = SetAsideForeignHistory(paths, guard, logger);
    CHECK(kept.node.empty());
    CHECK(kept.fleet == paths.fleet);
    CHECK(Logged(logger, "can be written by accounts other than its owner"));
    CHECK(TextOf(paths.node) == "planted");
}

TEST_CASE("A state file this node's account, an administrator or an unnamed owner wrote is accepted",
          "[node][identity][key][secret][state]")
{
    // Only `Another` is evidence: the same standing the key has always had. Every row is written,
    // and every one is asked about -- the walk reaching nothing would accept as well.
    for (auto const owner:
         { FileOwnerStanding::ThisProcess, FileOwnerStanding::Administrative, FileOwnerStanding::Undetermined })
    {
        CAPTURE(owner);
        ScratchDirectory const scratch { "node-state-own" };
        for (auto const& row: NodeStateFiles())
            if (row.Name() != NodeKeyFileName)
                WriteText(scratch.Path() / row.Name(), "kept");
        ScriptedNodeKeyGuard guard { { .found = SecretExposure::None,
                                       .afterProtect = SecretExposure::None,
                                       .owner = owner,
                                       .writers = DirectoryWriters::OwnerOnly } };
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        REQUIRE(ResolveNodeKey(scratch.Path(), random, guard).has_value());
        for (auto const& row: NodeStateFiles())
            if (row.Name() != NodeKeyFileName)
                CHECK(std::ranges::contains(guard.Owners(), scratch.Path() / row.Name()));
    }
}

TEST_CASE("A history file another account owns is set aside where it is read, and named", "[node][state][fleet]")
{
    ScratchDirectory const scratch { "node-state-set-aside" };
    auto cfg = NodeConfig {};
    cfg.clusterDir = scratch.Path();
    auto const paths = HistoryPaths::For(cfg);
    WriteText(paths.fleet, "planted");
    WriteText(paths.node, "kept");
    // `received` is absent, and an absent file is not asked about.
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    guard.OwnedByAnother(paths.fleet.filename());
    CapturingLogger logger;

    auto const kept = SetAsideForeignHistory(paths, guard, logger);
    CHECK(kept.fleet.empty());
    CHECK(kept.node == paths.node);
    CHECK(kept.received == paths.received);
    CHECK(Logged(logger, paths.fleet.string()));
    CHECK(Logged(logger, "scripted-owner"));
    CHECK(Logged(logger, "starts without"));
    CHECK_FALSE(std::ranges::contains(guard.Owners(), paths.received));
    CHECK(TextOf(paths.fleet) == "planted");
}

TEST_CASE("Every file the node's writers leave in its state directory has a row, the key alone its owner's alone",
          "[node][state][secret]")
{
    // **The table is the complete list of what the node writes there, and two things hold it to
    // that.** At start, the walk refuses an entry no row names WHOEVER owns it, so a file a writer
    // names outside the table stops the next start. And HERE, before that: every writer is run
    // into one directory, and the entries must equal the rows -- so a new writer with no row, or a
    // row nothing writes any more, fails at this case's first run. The compiler holds only the
    // writers that name their file through a `StateFile` (`Core/StateFiles.hpp`), since an
    // enumerator without a row in both tables does not build; a path joined onto the state
    // directory by hand is caught by this case and the walk, not by the compiler.
    //
    // And the KEY is created its owner's alone by its writer, whatever the directory hands it --
    // here a list that lets every user read -- while every other file takes exactly what the
    // directory hands it: its protection is integrity, which the directory's judgement and the
    // owner walk give, and a list of its own would lock out the service account the directory
    // grants when an elevated operator wrote the file.
    //
    // And on POSIX every file's mode is its ROW's, EXACTLY, with every writer run under umask 000:
    // the create's own mode less a permissive umask would otherwise leave the files others may
    // rewrite, and a mode a writer chose for itself would show here as the wrong row's.
#if !defined(_WIN32)
    Testing::ScopedUmask const permissive { 0 };
#endif
    ScratchDirectory const scratch { "node-state-writers" };
    auto const dir = scratch.Path() / "state";
    std::filesystem::create_directories(dir);
#if defined(_WIN32)
    REQUIRE(Testing::ApplyAccessList(dir, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FR;;;BU)"));
#else
    // Readable by everyone, as systemd's `StateDirectory=` makes one, and writable by its owner
    // alone -- which the umask above would not have given it.
    std::filesystem::permissions(dir,
                                 std::filesystem::perms::owner_all | std::filesystem::perms::group_read
                                     | std::filesystem::perms::group_exec | std::filesystem::perms::others_read
                                     | std::filesystem::perms::others_exec);
#endif

    // Asked of each file where the test can see it, which is not always at the end: a snapshot
    // compacts the consensus log by REPLACING it, so a log its store created any other way would
    // read back owner-only by the time the loop below reached it.
    auto const asCreated = [](std::filesystem::path const& file) {
        CAPTURE(file.string());
#if !defined(_WIN32)
        auto const* const row = NodeStateFileRowOf(file.filename().string());
        REQUIRE(row != nullptr);
        struct ::stat info {};

        REQUIRE(::lstat(file.c_str(), &info) == 0);
        CHECK((static_cast<unsigned>(info.st_mode) & 0777U) == StateFilePosixMode(row->file));
        CHECK_FALSE(OthersMayWrite(file).value_or(true));
#endif
#if defined(_WIN32)
        CAPTURE(Testing::AccessListOf(file));
#endif
        if (file.filename() == NodeKeyFileName)
        {
            CHECK(SecretFileExposure(file) == SecretExposure::None);
#if defined(_WIN32)
            CHECK_FALSE(Testing::AccessListOf(file).contains("BU"));
            CHECK(Testing::AccessListOf(file).starts_with("D:P"));
#endif
            return;
        }
        // The archive is a directory only the node lists: a protected list of its own, as
        // `CreateOwnerOnlyDirectory` makes one -- nobody else reads the stores of clusters it left.
        if (file.filename() == ArchiveDirectoryName)
        {
            CHECK(std::filesystem::is_directory(file));
#if defined(_WIN32)
            CHECK_FALSE(Testing::AccessListOf(file).contains("BU"));
            CHECK(Testing::AccessListOf(file).starts_with("D:P"));
#endif
            return;
        }
#if defined(_WIN32)
        // Inherited, and so carrying the directory's `BU` read: nothing of its own.
        CHECK(Testing::AccessListOf(file).contains("(A;ID;FR;;;BU)"));
        CHECK_FALSE(Testing::AccessListOf(file).starts_with("D:P"));
#endif
    };

    SystemSecureRandom random;
    FileTrustNodeKeyGuard realGuard;
    REQUIRE(ResolveNodeKey(dir, random, realGuard).has_value());
    REQUIRE(ResolveNodeIdentity(dir, "", random).has_value());
    REQUIRE(Cluster::FileFormationStore { dir }
                .Save(Cluster::FormationRecord { .mode = Cluster::NodeMode::Solitary,
                                                 .own = { .clusterId = "mine", .createdAtUnixSeconds = 4 },
                                                 .joining = std::nullopt,
                                                 .fleet = std::nullopt,
                                                 .archivePending = std::nullopt,
                                                 .rejectedBy = std::nullopt,
                                                 .askedJoins = {} })
                .has_value());
    // A cluster this node left: its store, archived where the archiver puts it, before the store of
    // the cluster it runs now is written in the root below.
    {
        auto left = Consensus::FileRaftStorage::Open(dir);
        REQUIRE(left.has_value());
        REQUIRE(left->SaveState(Consensus::PersistentState { .currentTerm = Consensus::Term { .value = 1 },
                                                             .votedFor = std::string { "n-a" } })
                    .has_value());
    }
    REQUIRE(RaftStoreArchiver { dir }.Archive("left").has_value());
    REQUIRE(Cluster::FleetEndpointsFile { dir }
                .Save(Cluster::FleetEndpoints {
                    .clusterId = "mine",
                    .voters = { { .id = "n-a", .raftEndpoint = "office-a:6680", .nodeEndpoint = "office-a:6674" } } })
                .has_value());
    {
        auto store = Consensus::FileRaftStorage::Open(dir);
        REQUIRE(store.has_value());
        asCreated(dir / Consensus::RaftLogFileName);
        REQUIRE(store
                    ->SaveState(Consensus::PersistentState { .currentTerm = Consensus::Term { .value = 2 },
                                                             .votedFor = std::string { "n-a" } })
                    .has_value());
        REQUIRE(store
                    ->SaveSnapshot(Consensus::RaftSnapshot { .lastIncludedIndex = Consensus::LogIndex { .value = 1 },
                                                             .lastIncludedTerm = Consensus::Term { .value = 1 },
                                                             .configuration = { .voters = { "n-a" }, .learners = {} },
                                                             .state = {} })
                    .has_value());
    }
    auto cfg = NodeConfig {};
    cfg.clusterDir = dir;
    Testing::PlacedWallClock clock;
    // Each history under the state file it is: the name its path ends in.
    for (auto const which: Enumerators<HistoryFile>())
    {
        auto const path = HistoryPathFor(cfg, which);
        auto const* const row = NodeStateFileRowOf(path.filename().string());
        REQUIRE(row != nullptr);
        auto const saved = which == HistoryFile::Received ? Distributed::FleetNodeHistories { clock }.Save(path, row->file)
                                                          : Distributed::FleetHistory { clock }.Save(path, row->file);
        REQUIRE(saved);
    }

    auto rows = std::set<std::string> {};
    for (auto const& row: NodeStateFiles())
        rows.insert(std::string { row.Name() });
    CHECK(NamesIn(dir) == rows);

    for (auto const& name: NamesIn(dir))
        asCreated(dir / name);

    // And the walk over what the writers left, through the real guard, accepts it: every file is
    // this process's own.
    CHECK(RefuseForeignStateFiles(dir, realGuard).has_value());
}

TEST_CASE("A store that creates the state directory creates it its owner's alone", "[node][state][secret]")
{
    // The key resolution creates the directory first on every production path; a store that
    // reaches an absent one anyway creates it the same way rather than the way its parent, or the
    // process's umask, would. Both are made to hand out write to others here, or a plain create
    // would pass too.
    ScratchDirectory const scratch { "node-state-directories" };
#if defined(_WIN32)
    REQUIRE(Testing::ApplyAccessList(scratch.Path(), L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BU)"));
#else
    struct UmaskScope
    {
        ::mode_t previous = ::umask(0);
        UmaskScope() = default;
        UmaskScope(UmaskScope const&) = delete;
        UmaskScope(UmaskScope&&) = delete;
        UmaskScope& operator=(UmaskScope const&) = delete;
        UmaskScope& operator=(UmaskScope&&) = delete;
        ~UmaskScope()
        {
            ::umask(previous);
        }
    } const umaskScope;
#endif
    auto const raft = scratch.Path() / "raft";
    auto const formation = scratch.Path() / "formation";
    auto const endpoints = scratch.Path() / "endpoints";
    REQUIRE(Consensus::FileRaftStorage::Open(raft).has_value());
    REQUIRE(Cluster::FileFormationStore { formation }
                .Save(Cluster::FormationRecord { .mode = Cluster::NodeMode::Solitary,
                                                 .own = { .clusterId = "mine", .createdAtUnixSeconds = 4 },
                                                 .joining = std::nullopt,
                                                 .fleet = std::nullopt,
                                                 .archivePending = std::nullopt,
                                                 .rejectedBy = std::nullopt,
                                                 .askedJoins = {} })
                .has_value());
    REQUIRE(Cluster::FleetEndpointsFile { endpoints }
                .Save(Cluster::FleetEndpoints {
                    .clusterId = "mine",
                    .voters = { { .id = "n-a", .raftEndpoint = "office-a:6680", .nodeEndpoint = "office-a:6674" } } })
                .has_value());
    for (auto const& directory: { raft, formation, endpoints })
    {
        CAPTURE(directory.string());
        CHECK(DirectoryWritersOf(directory) == DirectoryWriters::OwnerOnly);
    }
}

TEST_CASE("A link in the state directory is refused before its owner is asked", "[node][identity][key][secret][state]")
{
    // The node writes no link, so one there was planted, pointing wherever its author chose -- and
    // its owner answers for the link, never for the file it leads to. Refused wherever it sits.
    for (auto const name: { std::string_view { "formation" }, std::string_view { "nested" } })
    {
        CAPTURE(name);
        ScratchDirectory const scratch { "node-state-link" };
        auto const planted = scratch.Path() / name;
        WriteText(planted, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.LinkedEntry(name);
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().fault == NodeKeyFault::LinkEntry);
        CHECK(resolved.error().message.contains(planted.string()));
        CHECK(random.FillCount() == 0);
        CHECK(TextOf(planted) == "planted");
    }
}

TEST_CASE("A formation record planted as a link to a file this account owns is refused on the real filesystem",
          "[node][identity][key][secret][state]")
{
    // The shape `stat` was blind to: every owner along the way is this account's, so only asking
    // whether the ENTRY is a link catches it.
    ScratchDirectory const scratch { "node-state-real-link" };
    auto const elsewhere = scratch.Path() / "elsewhere";
    WriteText(elsewhere, "a record somebody chose");
    auto const dir = scratch.Path() / "state";
    REQUIRE(CreateOwnerOnlyDirectory(dir).has_value());
    auto failure = std::error_code {};
    std::filesystem::create_symlink(elsewhere, dir / Cluster::FormationRecordFileName, failure);
    if (failure)
        SKIP("this account cannot create a symbolic link here (" << failure.message() << ")");

    FileTrustNodeKeyGuard guard;
    auto const walked = RefuseForeignStateFiles(dir, guard);
    REQUIRE_FALSE(walked.has_value());
    CHECK(walked.error().fault == NodeKeyFault::LinkEntry);
    CHECK(TextOf(elsewhere) == "a record somebody chose");
}

#if defined(_WIN32)
TEST_CASE("A state file an elevated operator writes stays readable by the service the directory grants",
          "[node][state][secret]")
{
    // `--print-identity` may run elevated before the service ever starts, and writes as
    // `Administrators`; the install then grants the service's account the directory, inheritably
    // (`GrantPathAccess`). A file with a protected list of its own takes that grant neither before
    // nor after, and the service is refused its own state with no remedy. So every state file
    // but the key takes the directory's list, and the grant reaches it whichever came first --
    // reproduced with the grant the review measured, `SERVICE` full control, inheritable.
    ScratchDirectory const scratch { "node-state-service-grant" };
    auto const dir = scratch.Path() / "state";
    SystemSecureRandom random;
    FileTrustNodeKeyGuard guard;
    REQUIRE(ResolveNodeKey(dir, random, guard).has_value());

    // Before the grant: the fleet's endpoints, as an approval saves them.
    REQUIRE(
        Cluster::FleetEndpointsFile { dir }.Save(Cluster::FleetEndpoints { .clusterId = "mine", .voters = {} }).has_value());
    REQUIRE(Testing::RunCommandLine(std::format(R"(icacls "{}" /grant *S-1-5-6:(OI)(CI)F)", dir.string()))
            == std::optional<DWORD> { 0 });
    // After it: the formation record.
    REQUIRE(Cluster::FileFormationStore { dir }
                .Save(Cluster::FormationRecord { .mode = Cluster::NodeMode::Solitary,
                                                 .own = { .clusterId = "mine", .createdAtUnixSeconds = 4 },
                                                 .joining = std::nullopt,
                                                 .fleet = std::nullopt,
                                                 .archivePending = std::nullopt,
                                                 .rejectedBy = std::nullopt,
                                                 .askedJoins = {} })
                .has_value());

    for (auto const name: { Cluster::FleetEndpointsFileName, Cluster::FormationRecordFileName })
    {
        CAPTURE(name, Testing::AccessListOf(dir / name));
        CHECK(Testing::AccessListOf(dir / name).contains("(A;ID;FA;;;SU)"));
    }
    // The control: the key keeps its own list, and the grant does not reach it -- which is why
    // only the key has the `/setowner` remedy as its ordinary answer.
    CHECK_FALSE(Testing::AccessListOf(dir / NodeKeyFileName).contains(";;;SU)"));
}

TEST_CASE("A state file the node cannot open names the command that hands it to the service", "[node][state]")
{
    auto const file = std::filesystem::path { "C:/ProgramData/fastcache-node/cluster/roster" };
    auto const hint = StateFileUnreadableHint(file);
    // Outside the CHECK: MSVC 19.44 re-reads a raw string's backslash as an escape when the macro
    // stringizes its argument (C4129 on `\<`, an error under /WX); 19.51 does not.
    auto const command = std::format(R"(icacls "{}" /setowner "NT SERVICE\<the service's name>")", file.string());
    CHECK(hint.contains(command));
    CHECK(hint.contains("--print-identity"));
}
#else
TEST_CASE("A state file the node cannot open names the chown that hands it to the service", "[node][state]")
{
    // A `sudo --print-identity` writes files root owns, the key among them mode 0600, which the
    // service's account then cannot read: the hint names the command that hands them over.
    ScratchDirectory const scratch { "node-state-chown-hint" };
    auto const file = scratch / "roster";
    WriteText(file, "roster");
    CHECK(StateFileUnreadableHint(file).contains(std::format("chown <the service's user> '{}'", file.string())));
    CHECK(StateFileUnreadableHint(scratch.Path())
              .contains(std::format("chown -R <the service's user> '{}'", scratch.Path().string())));
}
#endif

TEST_CASE("A node id that is there and cannot be read is refused, never minted over", "[node][identity][state]")
{
    // Only an ABSENT id mints: a file this account cannot open may hold the id the cluster
    // admitted, and a fresh one renamed over it would replace that member silently.
    ScratchDirectory const scratch { "node-id-unreadable" };
    auto const dir = scratch.Path() / "state";
    REQUIRE(CreateOwnerOnlyDirectory(dir).has_value());
    auto const path = dir / NodeIdentityFileName;
    WriteText(path, "n-admitted\n");
#if defined(_WIN32)
    // SYSTEM alone, the Windows counterpart of the root skip below: an elevated token HOLDS
    // Administrators, so a list granting BA is one this process reads whenever it runs elevated --
    // as every CI runner does -- and nothing here would be unreadable. The owner may still rewrite
    // the list, which is how it is restored below.
    REQUIRE(Testing::ApplyAccessList(path, L"D:P(A;;FA;;;SY)"));
#else
    if (::geteuid() == 0)
        SKIP("root reads a file whatever its mode");
    std::filesystem::permissions(path, std::filesystem::perms::none);
#endif
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    auto const resolved = ResolveNodeIdentity(dir, "", random);
#if defined(_WIN32)
    REQUIRE(Testing::ApplyAccessList(path, L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)"));
#else
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().message.contains(path.string()));
    CHECK(resolved.error().message.contains("refused rather than minted over"));
    CHECK(random.FillCount() == 0);
    CHECK(TextOf(path) == "n-admitted\n");
}

TEST_CASE("Below the top level the walk accepts the archive's layout and nothing else", "[node][state][archive]")
{
    // The archive is the one directory the node writes: one directory per archived store, named as
    // `IsArchiveDirectoryName` allows, holding that cluster's consensus store and nothing more. Each
    // store file answers as its own raft row there, so another account's copy is refused as one.
    auto const store = [](ScratchDirectory const& scratch, std::string_view clusterId) {
        auto const at = scratch.Path() / ArchiveDirectoryName / std::filesystem::path { std::string { clusterId } };
        std::filesystem::create_directories(at);
        for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
            WriteText(at / name, "archived");
        return at;
    };

    {
        // The control: an archive as the archiver lays it out is accepted.
        ScratchDirectory const scratch { "node-state-archive" };
        store(scratch, "c-laptop");
        store(scratch, "c-laptop.1");       // the same cluster left a second time
        store(scratch, "c-laptop.partial"); // an archive a crash interrupted
        store(scratch, "0123456789abcdef0123456789abcdef");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        CHECK(RefuseForeignStateFiles(scratch.Path(), guard).has_value());
    }
    {
        // A file the archiver never writes, inside a cluster's archived store.
        ScratchDirectory const scratch { "node-state-archive-stray" };
        auto const stray = store(scratch, "c-laptop") / "notes.txt";
        WriteText(stray, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        REQUIRE_FALSE(walked.has_value());
        CHECK(walked.error().fault == NodeKeyFault::UnknownEntry);
        CHECK(walked.error().message.contains(stray.string()));
    }
    {
        // A directory the archiver could never have named.
        ScratchDirectory const scratch { "node-state-archive-name" };
        auto const odd = store(scratch, "a b");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        REQUIRE_FALSE(walked.has_value());
        CHECK(walked.error().fault == NodeKeyFault::UnknownEntry);
        CHECK(walked.error().message.contains(odd.string()));
    }
    {
        // An archived store file another account owns is refused as that raft file would be.
        ScratchDirectory const scratch { "node-state-archive-foreign" };
        store(scratch, "c-laptop");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.OwnedByAnother(std::filesystem::path { Consensus::RaftLogFileName });
        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        REQUIRE_FALSE(walked.has_value());
        CHECK(walked.error().fault == NodeKeyFault::ForeignOwner);
        CHECK(walked.error().message.contains("consensus log"));
    }
}

namespace
{
/// A POSIX-semantics rename that answers what it was scripted to, and moves nothing when it refuses.
class ScriptedReplacingRename final: public Platform::IReplacingRename
{
  public:
    /// @param refusal What every rename answers; empty to rename as the platform does.
    explicit ScriptedReplacingRename(std::error_code refusal):
        _refusal { refusal }
    {
    }

    /// @copydoc Platform::IReplacingRename::RenameReplacing
    [[nodiscard]] std::error_code RenameReplacing(std::filesystem::path const& from,
                                                  std::filesystem::path const& to) const override
    {
        ++_asked;
        if (_refusal)
            return _refusal;
        return Platform::SystemReplacingRename {}.RenameReplacing(from, to);
    }

    /// @return How many renames were asked.
    [[nodiscard]] int Asked() const noexcept
    {
        return _asked;
    }

  private:
    std::error_code _refusal;
    mutable int _asked { 0 };
};

/// `ERROR_INVALID_PARAMETER`: what `SetFileInformationByHandle` answers where the rename has no POSIX
/// semantics -- and, the reason a fallback must be said, for a path form it will not take.
constexpr int InvalidParameter = 87;
} // namespace

TEST_CASE("A replace whose POSIX rename is refused as unsupported still lands, and says so", "[node][state-files]")
{
    auto const scratch = ScratchDirectory { "replace-route" };
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NodeConditions conditions;
    Consensus::SystemDurableFiles files;
    auto const refused = std::error_code { InvalidParameter, std::system_category() };
    auto const rename = ScriptedReplacingRename { refused };
    auto const route = ReportReplaceRoute(scratch.Path(), files, rename, logger, metrics, conditions);

    // Only Windows reads that answer as "no such rename here": `rename(2)` has the semantics, so a
    // POSIX build treats any refusal as the failure it is.
    if (!Platform::MeansNoPosixRename(refused))
    {
        CHECK_FALSE(route.has_value());
        CHECK(metrics.Read(IMetricsSink::Counter::StateFileReplacesFellBack) == 0);
        return;
    }
    REQUIRE(route.has_value());
    CHECK(Testing::Unwrap(route) == Platform::ReplaceRoute::Classic);
    CHECK(rename.Asked() > 0);
    CHECK(metrics.Read(IMetricsSink::Counter::StateFileReplacesFellBack) == 1);
    auto const said = logger.Snapshot();
    auto const warned = std::ranges::any_of(said, [&](auto const& record) {
        return record.level == LogLevel::Warn && record.message.contains(scratch.Path().string())
               && record.message.contains(std::format("{}", InvalidParameter)) && record.message.contains("classic rename");
    });
    CHECK(warned);
    CHECK_FALSE(std::filesystem::exists(scratch / Consensus::ReplaceProbeFileName));
}

namespace
{
/// This machine's files, with a directory sync that answers @p answer instead of syncing: a volume
/// whose filesystem cannot sync a directory, or one where a sync it could do failed.
class UnsyncedDirectoryFiles final: public Consensus::IDurableFiles
{
  public:
    /// @param answer What every directory sync's flush answers.
    explicit UnsyncedDirectoryFiles(std::error_code answer) noexcept:
        _answer { answer }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<Consensus::IDurableSink>, std::error_code> Create(
        std::filesystem::path const& path, StateFile which) override
    {
        return _system.Create(path, which);
    }

    [[nodiscard]] std::expected<void, Consensus::DirectorySyncFailure> SyncDirectory(
        std::filesystem::path const& /*directory*/) override
    {
        return std::unexpected { Consensus::DirectorySyncFailure { .step = Consensus::DirectorySyncStep::Flush,
                                                                   .code = _answer } };
    }

  private:
    std::error_code _answer;
    Consensus::SystemDurableFiles _system;
};
} // namespace

TEST_CASE("A state directory whose filesystem cannot sync a directory is said and counted once, and still serves",
          "[node][state-files][durable]")
{
    // A volume that cannot sync a directory refused EVERY state write while a failed sync failed the
    // replace (step 20 recheck, R-A). Now each of this platform's "not supported here" answers lands the
    // replace DEGRADED, and the start probe says so once and counts it -- WHICH counter, and not the
    // fallback's.
    for (auto const answer: Consensus::UnsupportedDirectorySyncAnswers())
    {
        INFO(answer.message());
        auto const scratch = ScratchDirectory { "replace-route-unsynced" };
        CapturingLogger logger;
        AtomicMetricsSink metrics;
        NodeConditions conditions;
        UnsyncedDirectoryFiles files { answer };
        auto const rename = ScriptedReplacingRename { std::error_code {} };
        auto const route = ReportReplaceRoute(scratch.Path(), files, rename, logger, metrics, conditions);
        REQUIRE(route.has_value());
        CHECK(metrics.Read(IMetricsSink::Counter::StateDirectorySyncsUnsupported) == 1);
        // And the operator's row (B3-6): latched, naming the directory, since the remedy is to move it.
        CHECK(conditions.StateOf(NodeCondition::StateDirectoryUnsynced) == CompileCacheWire::ConditionState::Raised);
        CHECK(Testing::DetailOf(conditions, NodeCondition::StateDirectoryUnsynced).contains(scratch.Path().string()));
        CHECK(metrics.Read(IMetricsSink::Counter::StateFileReplacesFellBack) == 0);
        auto const said = logger.Snapshot();
        CHECK(std::ranges::count_if(said,
                                    [&](auto const& record) {
                                        return record.level == LogLevel::Warn
                                               && record.message.contains(scratch.Path().string())
                                               && record.message.contains("cannot sync a directory");
                                    })
              == 1);
    }

    // The control: a sync that FAILED -- an I/O error, which a retry may not repeat -- is no degradation.
    // The probe cannot be written, and nothing is counted as unsupported.
    auto const scratch = ScratchDirectory { "replace-route-sync-failed" };
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NodeConditions conditions;
    UnsyncedDirectoryFiles files { std::make_error_code(std::errc::io_error) };
    auto const rename = ScriptedReplacingRename { std::error_code {} };
    CHECK_FALSE(ReportReplaceRoute(scratch.Path(), files, rename, logger, metrics, conditions).has_value());
    CHECK(metrics.Read(IMetricsSink::Counter::StateDirectorySyncsUnsupported) == 0);
    // Nor is the row raised: a probe that could not run observed nothing, and says that rather than
    // `clear`.
    CHECK(conditions.StateOf(NodeCondition::StateDirectoryUnsynced) == CompileCacheWire::ConditionState::NotEvaluated);
}

TEST_CASE("A replace whose POSIX rename works is neither said nor counted", "[node][state-files]")
{
    auto const scratch = ScratchDirectory { "replace-route-posix" };
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    NodeConditions conditions;
    Consensus::SystemDurableFiles files;
    auto const rename = ScriptedReplacingRename { std::error_code {} };
    auto const route = ReportReplaceRoute(scratch.Path(), files, rename, logger, metrics, conditions);
    REQUIRE(route.has_value());
    CHECK(Testing::Unwrap(route) == Platform::ReplaceRoute::PosixSemantics);
    CHECK(rename.Asked() > 0);
    CHECK(metrics.Read(IMetricsSink::Counter::StateFileReplacesFellBack) == 0);
    CHECK(logger.Snapshot().empty());
    CHECK_FALSE(std::filesystem::exists(scratch / Consensus::ReplaceProbeFileName));
    // This machine's directory synced, so the row is checked and benign.
    CHECK(conditions.StateOf(NodeCondition::StateDirectoryUnsynced) == CompileCacheWire::ConditionState::Clear);
}

TEST_CASE("What a crash during the replace probe leaves is no reason to refuse the next start, and the probe clears it",
          "[node][state-files]")
{
    // A SIGKILL or a power loss inside the startup probe leaves the probe, or its temporary. Both are
    // never read, so the judge passes them by NAME -- and the next probe, which every serving body
    // runs, clears them.
    {
        ScratchDirectory const scratch { "replace-probe-leftover" };
        for (auto const name: NodeStateProbeLeftovers())
            WriteText(scratch.Path() / std::filesystem::path { std::string { name } }, "a crash left this");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        INFO((walked.has_value() ? std::string {} : walked.error().message));
        CHECK(walked.has_value());

        CapturingLogger logger;
        AtomicMetricsSink metrics;
        NodeConditions conditions;
        auto const rename = ScriptedReplacingRename { std::error_code {} };
        Consensus::SystemDurableFiles files;
        auto const route = ReportReplaceRoute(scratch.Path(), files, rename, logger, metrics, conditions);
        REQUIRE(route.has_value());
        for (auto const name: NodeStateProbeLeftovers())
        {
            INFO(name);
            CHECK_FALSE(std::filesystem::exists(scratch.Path() / std::filesystem::path { std::string { name } }));
        }
    }
    {
        // By name and nothing wider: a file that merely starts like the probe is still nothing this build
        // wrote.
        ScratchDirectory const scratch { "replace-probe-lookalike" };
        auto const lookalike = scratch.Path() / (std::string { Consensus::ReplaceProbeFileName } + ".old");
        WriteText(lookalike, "planted");
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        REQUIRE_FALSE(walked.has_value());
        CHECK(walked.error().fault == NodeKeyFault::UnknownEntry);
        CHECK(walked.error().message.contains(lookalike.string()));
    }
    {
        // And only as a regular file: a DIRECTORY of that name is no probe's leftover, and the probe could
        // never replace it.
        ScratchDirectory const scratch { "replace-probe-directory" };
        std::filesystem::create_directories(scratch.Path()
                                            / std::filesystem::path { std::string { Consensus::ReplaceProbeFileName } });
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
        REQUIRE_FALSE(walked.has_value());
        CHECK(walked.error().fault == NodeKeyFault::UnknownEntry);
    }
}

TEST_CASE("A link named like the replace probe is no leftover of it, whatever it points to", "[node][state-files]")
{
    // Judged by the ENTRY, never by what a link resolves to: a symlink of that name pointing at a regular
    // file is something this build never wrote, as a directory of that name is.
    // The target lives OUTSIDE the state directory and is a live regular file, so a judge asking what
    // the link resolves to would read it as a leftover -- the reading this case exists to refuse.
    ScratchDirectory const elsewhere { "replace-probe-link-target" };
    auto const target = elsewhere.Path() / "a-regular-file";
    WriteText(target, "a regular file somewhere");
    ScratchDirectory const scratch { "replace-probe-link" };
    auto made = std::error_code {};
    std::filesystem::create_symlink(
        target, scratch.Path() / std::filesystem::path { std::string { Consensus::ReplaceProbeFileName } }, made);
    if (made)
        SKIP("this host lets this process create no symbolic link: " << made.message());
    REQUIRE(std::filesystem::is_regular_file(scratch.Path()
                                             / std::filesystem::path { std::string { Consensus::ReplaceProbeFileName } }));
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    auto const walked = RefuseForeignStateFiles(scratch.Path(), guard);
    REQUIRE_FALSE(walked.has_value());
    CHECK(walked.error().fault == NodeKeyFault::UnknownEntry);
}

namespace
{

/// A state directory as a node from before formation records left it: an identity key and a
/// consensus store -- term, vote and log -- and NO formation record.
/// @param directory The state directory, which exists.
void WriteStoreWithoutFormation(std::filesystem::path const& directory)
{
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    FileTrustNodeKeyGuard guard;
    REQUIRE(ResolveNodeKey(directory, random, guard).has_value());
    auto store = Consensus::FileRaftStorage::Open(directory);
    REQUIRE(store.has_value());
    REQUIRE(store
                ->SaveState(Consensus::PersistentState { .currentTerm = Consensus::Term { .value = 3 },
                                                         .votedFor = std::string { "b3a14d8390da2f2cbe511e91100fba43" } })
                .has_value());
    REQUIRE(std::filesystem::exists(directory / Consensus::RaftStateFileName));
    REQUIRE(std::filesystem::exists(directory / Consensus::RaftLogFileName));
    REQUIRE_FALSE(std::filesystem::exists(directory / Cluster::FormationRecordFileName));
}

/// The line the redeploy judged, verbatim but for its two paths, as `main` judges it under
/// `--print-surfaces`: parsed, its state directory read (`ReadStateDirectoryFormation`), shaped by
/// what was read (`ShapeByKeptFormation`), then rendered and judged (`ReportSurfaces`).
struct JudgedLine
{
    NodeConfig cfg;                                     ///< The configuration as `main` holds it.
    std::expected<KeptFormation, FormationUnread> kept; ///< What the state directory gave.
    SurfaceReport report;                               ///< What `--print-surfaces` prints and answers.
};

/// Judge the redeploy's line against @p stateDirectory, through @p guard as `main` does.
/// @param stateDirectory What `--cluster-dir` names.
/// @param tokenFile What `--dashboard-token-file` names.
/// @param guard Who owns and may write each entry, as the judging caller sees it.
/// @return The configuration, what was read and the verdict.
[[nodiscard]] JudgedLine JudgeRedeployLine(std::filesystem::path const& stateDirectory,
                                           std::filesystem::path const& tokenFile,
                                           INodeKeyFileGuard& guard)
{
    auto const clusterDir = std::format("--cluster-dir={}", stateDirectory.string());
    auto const token = std::format("--dashboard-token-file={}", tokenFile.string());
    auto const args = std::vector<char const*> { "--advertise=127.0.0.1:6674",
                                                 "--admin-listen=0.0.0.0:6677",
                                                 "--dashboard",
                                                 token.c_str(),
                                                 "--listen-node=127.0.0.1:6674",
                                                 "--node-id=b3a14d8390da2f2cbe511e91100fba43",
                                                 "--raft-self=127.0.0.1",
                                                 "--listen-raft=127.0.0.1:6680",
                                                 clusterDir.c_str(),
                                                 "--print-surfaces" };
    auto cfg = NodeConfig {};
    REQUIRE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg).has_value());
    auto kept = ReadStateDirectoryFormation(stateDirectory, guard);
    (void) ShapeByKeptFormation(cfg, kept);
    auto report = ReportSurfaces(cfg);
    return JudgedLine { .cfg = std::move(cfg), .kept = std::move(kept), .report = std::move(report) };
}

} // namespace

TEST_CASE("A consensus store with no formation record is a first start's and the redeploy's line is accepted on it",
          "[node][state][formation][consensus]")
{
    // The premise the refusal below was first blamed on, measured at the seam `main` runs: a store
    // written before formation records -- key, term, vote, log, and no `formation` -- is read as a
    // directory holding NO record, which the first start mints over (`ProspectiveRecord`). Nothing
    // reads the consensus files to decide the mode, so their presence alone shapes the line as solitary.
    ScratchDirectory const tokens { "raft-self-token" };
    WriteText(tokens.Path() / "dashboard-token", "a-dashboard-token");
    ScratchDirectory const scratch { "raft-self-pre-formation" };
    WriteStoreWithoutFormation(scratch.Path());

    FileTrustNodeKeyGuard guard;
    auto const judged = JudgeRedeployLine(scratch.Path(), tokens.Path() / "dashboard-token", guard);
    REQUIRE(judged.kept.has_value());
    CHECK_FALSE(judged.kept->record.has_value());
    REQUIRE(judged.cfg.formation.has_value());
    CHECK_FALSE(judged.cfg.formationUnread.has_value());
    CHECK(judged.report.text.contains("mode: solitary"));
    INFO(judged.report.refusal.value_or(std::string {}));
    CHECK_FALSE(judged.report.refusal.has_value());
}

TEST_CASE("A line whose state directory cannot be read is refused once naming the mode and the reading and never "
          "--listen-raft",
          "[node][state][formation][consensus]")
{
    // The redeploy's refusal: `--listen-raft` WAS given, and the row told the operator its absence was
    // the cause -- a sentence from before the mode moved into the state directory. When the reading of
    // that directory refuses, no record shapes the line and its mode is unknown, so the true cause is
    // the reading's own refusal, and the row names it through the sentence the mode line prints.
    //
    // The directory the redeploy met: a consensus store with no formation record, which the reading
    // refused (the case above shows the store alone is no reason). Here what it refuses is an entry no
    // row names -- a refusal every platform and account can stage, of the same kind (`JudgeStateDirectory`).
    ScratchDirectory const tokens { "raft-self-token-refused" };
    WriteText(tokens.Path() / "dashboard-token", "a-dashboard-token");
    ScratchDirectory const scratch { "raft-self-unreadable" };
    WriteStoreWithoutFormation(scratch.Path());
    WriteText(scratch.Path() / "planted", "nothing this build wrote");

    FileTrustNodeKeyGuard guard;
    auto const judged = JudgeRedeployLine(scratch.Path(), tokens.Path() / "dashboard-token", guard);
    REQUIRE_FALSE(judged.kept.has_value());
    REQUIRE(judged.kept.error().reason.contains("keeps no entry by that name"));
    CHECK_FALSE(judged.cfg.formation.has_value());
    CHECK(judged.cfg.formationUnread == std::optional<FormationUnread> { judged.kept.error() });

    REQUIRE(judged.report.refusal.has_value());
    auto const& refusal = Testing::Unwrap(judged.report.refusal);
    INFO(refusal);
    // The row that answered -- the FIRST row, ahead of every flag row (`--raft-self`'s, `--dashboard`'s)
    // this line would otherwise meet -- and what it names: the mode, the state directory where the
    // operator named it, and the reading's own refusal, which is the cause.
    CHECK(refusal == StateDirectoryUnreadRefusal(judged.cfg));
    CHECK(refusal.starts_with("this node's mode"));
    CHECK(refusal.contains(std::format("{} (named by --cluster-dir)", scratch.Path().string())));
    CHECK(refusal.contains(judged.kept.error().reason));
    // And not the flag that was given: the stale sentence named it as the cause.
    CHECK_FALSE(refusal.contains("--listen-raft"));
    CHECK_FALSE(refusal.contains(RaftSelfWithConsensusClosedRefusal));

    // One source: the mode line `--print-surfaces` prints carries the same reason, and the raft line
    // no longer promises a record a first start will mint.
    CHECK(judged.report.text.contains(std::format("mode: none ({})", FormationAbsenceOf(judged.cfg))));
    CHECK(FormationAbsenceOf(judged.cfg).contains(judged.kept.error().reason));
    CHECK(refusal.contains(FormationAbsenceOf(judged.cfg)));
    CHECK(judged.report.text.contains("not served (no formation record could be read; see the mode line)"));
    CHECK_FALSE(judged.report.text.contains("no formation record yet"));
}

namespace
{

/// The node's service account as an administrator's trusted-owner set names it by default.
constexpr std::string_view NodeServiceAccount = "NT SERVICE\\FastCacheCompileNode";

/// A SID this case gives the node's service account: a PER-SERVICE SID, five sub-authorities.
constexpr std::string_view NodeServiceSid = "S-1-5-80-1111111111-2222222222-3333333333-4444444444-5555555555";

/// Another service's virtual account.
constexpr std::string_view OtherServiceAccount = "NT SERVICE\\SomeOtherService";

/// The SID this case gives another service's virtual account.
constexpr std::string_view OtherServiceSid = "S-1-5-80-6666666666-7777777777-8888888888-9999999999-1212121212";

/// A plain user.
constexpr std::string_view PlainUser = "DARKLEON\\mallory";

/// The SID this case gives the plain user.
constexpr std::string_view PlainUserSid = "S-1-5-21-1000000000-2000000000-3000000000-1001";

/// `NT SERVICE\ALL SERVICES`, the group `--service-name="ALL SERVICES"` resolves to.
constexpr std::string_view AllServicesAccount = "NT SERVICE\\ALL SERVICES";

/// The group's SID, which is not a per-service SID.
constexpr std::string_view AllServicesSid = "S-1-5-80-0";

/// The scripted account directory: what `AccountIdOf` answers, for a fake identity on every platform.
/// @param account `DOMAIN\name`.
/// @return Its scripted SID, or nothing for an account this case does not know.
[[nodiscard]] std::optional<std::string> ScriptedAccountId(std::string const& account)
{
    struct Account
    {
        std::string_view name; ///< `DOMAIN\name`.
        std::string_view sid;  ///< Its SID.
    };
    constexpr auto Accounts = std::to_array<Account>({
        { .name = NodeServiceAccount, .sid = NodeServiceSid },
        { .name = OtherServiceAccount, .sid = OtherServiceSid },
        { .name = PlainUser, .sid = PlainUserSid },
        { .name = AllServicesAccount, .sid = AllServicesSid },
    });
    auto const* const row = core::findOrNull(Accounts, std::string_view { account }, &Account::name);
    if (row == nullptr)
        return std::nullopt;
    return std::string { row->sid };
}

/// The state files a running service writes into its directory, every one of which an
/// administrator met as "another account's" on the redeploy (MEASURED, 2026-10-06).
constexpr auto ServiceWrittenFiles =
    std::to_array<std::string_view>({ NodeKeyFileName, Consensus::RaftStateFileName, Consensus::RaftLogFileName });

/// Whether @p text tells an operator to remove something BECAUSE of who owns it: the remedy the
/// owner's second ruling forbids. A removal offered for a PLANTED entry is not this.
/// @param text A refusal.
/// @return True when it offers a removal outside the planted-only clause.
[[nodiscard]] bool OffersRemovalForOwnership(std::string_view text)
{
    return text.contains("Remove it, with what is in it") || text.contains("another account put it there")
           || text.contains("another account created it");
}

} // namespace

TEST_CASE("An administrator judges the node's own service account's state files as the service will",
          "[node][state][formation][trust]")
{
    // The owner ruling of 2026-10-06, as the controller limited it: an ADMINISTRATOR running a verb that
    // writes nothing (`StateUse::Inspects`) counts a state file owned by the node's OWN per-service
    // virtual account as the node's own, and the line is judged as the service will run it. Files of
    // any other account stay refused BY NAME; a caller that is no administrator, and a START by an
    // administrator, trust nothing more than before.
    //
    // Through the production fold (`OwnStateAccountIds`, `OwnStateAccountsGuard`) over a SCRIPTED
    // identity: which account owns each file, and what each account resolves to, are the only fakes.
    ScratchDirectory const tokens { "trust-token" };
    WriteText(tokens.Path() / "dashboard-token", "a-dashboard-token");

    auto cfg = NodeConfig {};
    REQUIRE(NodeServiceAccountOf(cfg) == std::optional<std::string> { std::string { NodeServiceAccount } });

    struct Case
    {
        std::string_view what;    ///< The case, as CAPTURE prints it.
        JudgingCaller caller;     ///< Who judges.
        StateUse use;             ///< What the invocation does with the directory.
        std::string_view owner;   ///< The account owning every file the service writes.
        std::string_view ownerId; ///< Its SID.
        bool accepted;            ///< Whether the line is judged as the service runs it.
    };
    for (auto const& [what, caller, use, owner, ownerId, accepted]: std::to_array<Case>({
             { .what = "administrator inspecting, the node's own service",
               .caller = JudgingCaller::Administrator,
               .use = StateUse::Inspects,
               .owner = NodeServiceAccount,
               .ownerId = NodeServiceSid,
               .accepted = true },
             { .what = "administrator inspecting, another service",
               .caller = JudgingCaller::Administrator,
               .use = StateUse::Inspects,
               .owner = OtherServiceAccount,
               .ownerId = OtherServiceSid,
               .accepted = false },
             { .what = "administrator inspecting, a plain user",
               .caller = JudgingCaller::Administrator,
               .use = StateUse::Inspects,
               .owner = PlainUser,
               .ownerId = PlainUserSid,
               .accepted = false },
             { .what = "no administrator inspecting, the node's own service",
               .caller = JudgingCaller::Other,
               .use = StateUse::Inspects,
               .owner = NodeServiceAccount,
               .ownerId = NodeServiceSid,
               .accepted = false },
             // The controller's ruling on I2: a FOREGROUND START by an administrator keeps the strict set.
             // So does `--print-identity`, which mints a key and an id where they are absent: the same row
             // (`StateUseOf` answers `Runs` for it, asserted beside the verb table).
             { .what = "administrator starting the node, the node's own service",
               .caller = JudgingCaller::Administrator,
               .use = StateUse::Runs,
               .owner = NodeServiceAccount,
               .ownerId = NodeServiceSid,
               .accepted = false },
         }))
    {
        CAPTURE(what);
        ScratchDirectory const scratch { "trust-state" };
        WriteStoreWithoutFormation(scratch.Path());

        auto platform = ScriptedNodeKeyGuard::OwnerOnly();
        for (auto const name: ServiceWrittenFiles)
            platform.OwnedBy(std::filesystem::path { name }, std::string { owner }, std::string { ownerId });
        OwnStateAccountsGuard guard { platform, OwnStateAccountIds(caller, use, cfg, &ScriptedAccountId) };

        auto const judged = JudgeRedeployLine(scratch.Path(), tokens.Path() / "dashboard-token", guard);
        if (accepted)
        {
            REQUIRE(judged.kept.has_value());
            CHECK(judged.report.text.contains("mode: solitary"));
            INFO(judged.report.refusal.value_or(std::string {}));
            CHECK_FALSE(judged.report.refusal.has_value());
            CHECK(judged.report.ending == CommandEnding::Completed);
            continue;
        }
        // Refused BY NAME: the file, its owner, and the seat that would judge it as the service --
        // never a removal offered merely because another account owns it. A failed check that does
        // NOT end the case, so every refused caller is judged even when one of them is accepted.
        if (judged.kept.has_value())
        {
            FAIL_CHECK("the reading accepted files " << owner << " owns");
            continue;
        }
        auto const& reason = judged.kept.error().reason;
        CHECK(reason.contains(std::format("is owned by {}", owner)));
        CHECK(std::ranges::any_of(ServiceWrittenFiles, [&reason, &scratch](std::string_view name) {
            return reason.contains((scratch.Path() / name).string());
        }));
        CHECK(reason.contains("Ownership is judged from the account asking"));
        CHECK(reason.contains("Only a file nobody but this node should have written -- a planted one"));
        CHECK_FALSE(OffersRemovalForOwnership(reason));
        // A verdict on the owner: refused, never retried, and answered ahead of every flag row.
        CHECK(judged.kept.error().stage == StartStage::IdentityKey);
        CHECK(ExitCodeFor(judged.kept.error().stage) == ExitCodeOf(ProcessExit::Refused));
        REQUIRE(judged.report.refusal.has_value());
        CHECK(Testing::Unwrap(judged.report.refusal) == StateDirectoryUnreadRefusal(judged.cfg));
        CHECK(judged.report.ending == CommandEnding::Declined);
    }
}

TEST_CASE("An administrator judges a state directory the node's own service owns as the service will",
          "[node][state][formation][trust]")
{
    // The directory's OWNER is promoted as a file's is: the service creates its own directory whenever
    // it finds none and then owns it, and an elevated `--print-surfaces` over that healthy node was
    // told to remove the directory -- the node's identity and its consensus store. Promoted, the
    // directory is still asked who BESIDES its owner may plant entries there.
    ScratchDirectory const tokens { "trust-dir-token" };
    WriteText(tokens.Path() / "dashboard-token", "a-dashboard-token");
    auto const cfg = NodeConfig {};

    struct Case
    {
        std::string_view what;    ///< The case, as CAPTURE prints it.
        std::string_view owner;   ///< Who owns the directory and what the service wrote.
        std::string_view ownerId; ///< Its SID.
        DirectoryWriters beyond;  ///< Who besides the owner may plant entries there.
        bool accepted;            ///< Whether the line is judged as the service runs it.
    };
    for (auto const& [what, owner, ownerId, beyond, accepted]: std::to_array<Case>({
             { .what = "the node's own service, owner only",
               .owner = NodeServiceAccount,
               .ownerId = NodeServiceSid,
               .beyond = DirectoryWriters::OwnerOnly,
               .accepted = true },
             { .what = "the node's own service, others may plant",
               .owner = NodeServiceAccount,
               .ownerId = NodeServiceSid,
               .beyond = DirectoryWriters::Others,
               .accepted = false },
             { .what = "another service",
               .owner = OtherServiceAccount,
               .ownerId = OtherServiceSid,
               .beyond = DirectoryWriters::OwnerOnly,
               .accepted = false },
         }))
    {
        CAPTURE(what);
        ScratchDirectory const scratch { "trust-dir-state" };
        WriteStoreWithoutFormation(scratch.Path());

        auto platform = ScriptedNodeKeyGuard { Testing::NodeKeyGuardScript { .found = SecretExposure::None,
                                                                             .afterProtect = SecretExposure::None,
                                                                             .owner = FileOwnerStanding::ThisProcess,
                                                                             .writers = DirectoryWriters::ForeignOwner } };
        platform.WritersBeyondOwnerAre(beyond);
        platform.OwnedBy(scratch.Path().filename(), std::string { owner }, std::string { ownerId });
        for (auto const name: ServiceWrittenFiles)
            platform.OwnedBy(std::filesystem::path { name }, std::string { owner }, std::string { ownerId });
        OwnStateAccountsGuard guard {
            platform, OwnStateAccountIds(JudgingCaller::Administrator, StateUse::Inspects, cfg, &ScriptedAccountId)
        };

        auto const judged = JudgeRedeployLine(scratch.Path(), tokens.Path() / "dashboard-token", guard);
        if (accepted)
        {
            REQUIRE(judged.kept.has_value());
            CHECK(judged.report.text.contains("mode: solitary"));
            CHECK_FALSE(judged.report.refusal.has_value());
            continue;
        }
        if (judged.kept.has_value())
        {
            FAIL_CHECK("the reading accepted a directory " << owner << " owns");
            continue;
        }
        auto const& reason = judged.kept.error().reason;
        CHECK_FALSE(OffersRemovalForOwnership(reason));
        if (beyond == DirectoryWriters::Others)
            CHECK(reason.contains("lets other accounts on this machine create or delete entries"));
        else
        {
            CHECK(reason.contains(std::format("is owned by {}", owner)));
            CHECK(reason.contains("Ownership is judged from the account asking"));
            CHECK(reason.contains("Only a directory nobody but this node should have created -- a planted one"));
        }
    }
}

TEST_CASE("A state directory's reading ends a start and the worksheet by its own arm", "[node][state][formation][trust]")
{
    // I1: the reading's fault keeps its STAGE, so a transient arm is retried under a supervisor (exit
    // 1, `Failed`) and a verdict is not (78, `Refused`) -- as each ended before the startup table learnt
    // to name an unread directory, which had turned every one of them into the permanent
    // `StartupRules`. `UnreadStateStage` is what `main` ends a start with, ahead of that table, and
    // `--print-surfaces` ends by the same arm (1 against 2).
    ScratchDirectory const tokens { "arm-token" };
    WriteText(tokens.Path() / "dashboard-token", "a-dashboard-token");

    struct Case
    {
        std::string_view what; ///< The case, as CAPTURE prints it.
        void (*stage)(std::filesystem::path const& directory, ScriptedNodeKeyGuard& guard); ///< What fails.
        StartStage expected;                                                                ///< The arm it ends with.
        ProcessExit exit;                                                                   ///< How a start ends.
        CommandEnding worksheet;                                                            ///< How `--print-surfaces` ends.
    };
    for (auto const& [what, stage, expected, exit, worksheet]: std::to_array<Case>({
             { .what = "whether others may write a file could not be asked",
               .stage =
                   [](std::filesystem::path const& /*directory*/, ScriptedNodeKeyGuard& guard) {
                       guard.WritersUnanswered(std::string { Consensus::RaftStateFileName },
                                               std::make_error_code(std::errc::io_error));
                   },
               .expected = StartStage::IdentityKeyIo,
               .exit = ProcessExit::Failed,
               .worksheet = CommandEnding::Failed },
             { .what = "another account owns a file",
               .stage =
                   [](std::filesystem::path const& /*directory*/, ScriptedNodeKeyGuard& guard) {
                       guard.OwnedByAnother(std::string { Consensus::RaftStateFileName });
                   },
               .expected = StartStage::IdentityKey,
               .exit = ProcessExit::Refused,
               .worksheet = CommandEnding::Declined },
             { .what = "an entry no row names",
               .stage = [](std::filesystem::path const& directory,
                           ScriptedNodeKeyGuard& /*guard*/) { WriteText(directory / "planted", "planted"); },
               .expected = StartStage::IdentityKey,
               .exit = ProcessExit::Refused,
               .worksheet = CommandEnding::Declined },
             { .what = "a formation record that is not one",
               .stage =
                   [](std::filesystem::path const& directory, ScriptedNodeKeyGuard& /*guard*/) {
                       WriteText(directory / std::string { Cluster::FormationRecordFileName }, "not a record");
                   },
               .expected = StartStage::Formation,
               .exit = ProcessExit::Failed,
               .worksheet = CommandEnding::Failed },
         }))
    {
        CAPTURE(what);
        ScratchDirectory const scratch { "arm-state" };
        WriteStoreWithoutFormation(scratch.Path());
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        stage(scratch.Path(), guard);

        auto const judged = JudgeRedeployLine(scratch.Path(), tokens.Path() / "dashboard-token", guard);
        REQUIRE_FALSE(judged.kept.has_value());
        CHECK(judged.kept.error().stage == expected);
        CHECK(UnreadStateStage(judged.cfg) == std::optional { expected });
        CHECK(ExitOf(expected) == exit);
        CHECK(judged.report.ending == worksheet);
        CHECK(Testing::Unwrap(judged.report.refusal) == StateDirectoryUnreadRefusal(judged.cfg));
        // The startup table answers only a PERMANENT one; a transient one is `main`'s, by its arm.
        auto const tableAnswers =
            StartupPolicyRejection(judged.cfg) == std::optional { StateDirectoryUnreadRefusal(judged.cfg) };
        CHECK(tableAnswers == (exit == ProcessExit::Refused));
    }
    CHECK(ExitCodeOf(ProcessExit::Failed) == 1);
    CHECK(ExitCodeFor(StartStage::StartupRules) == ExitCodeOf(ProcessExit::Refused));
}

#if !defined(_WIN32)
TEST_CASE("A state directory this account cannot list is refused with one full stop and who can list it",
          "[node][state][formation][trust]")
{
    // MEASURED on Windows: "...: Access is denied.. Nothing in it is trusted until it can be", with no
    // remedy. The system's message already ends in a full stop, and the refusal must say who may list it.
    if (::geteuid() == 0)
        SKIP("root lists any directory, so no listing can be refused here");
    ScratchDirectory const scratch { "unlistable-state" };
    WriteStoreWithoutFormation(scratch.Path());
    std::filesystem::permissions(scratch.Path(), std::filesystem::perms::none);
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    auto const kept = ReadStateDirectoryFormation(scratch.Path(), guard);
    std::filesystem::permissions(scratch.Path(), std::filesystem::perms::owner_all);
    REQUIRE_FALSE(kept.has_value());
    INFO(kept.error().reason);
    CHECK(kept.error().reason.contains("cannot list"));
    CHECK_FALSE(kept.error().reason.contains(".."));
    CHECK(kept.error().reason.contains("Judge it as an account that may list it"));
    // A listing refused is an I/O arm: retried under a supervisor.
    CHECK(kept.error().stage == StartStage::IdentityKeyIo);
}
#endif

TEST_CASE("The trusted-owner set names the service --service-name names for a verb that writes nothing",
          "[node][state][trust]")
{
    auto cfg = NodeConfig {};
    cfg.serviceName = "FastCacheCompileNodeB";
    CHECK(NodeServiceAccountOf(cfg) == std::optional<std::string> { "NT SERVICE\\FastCacheCompileNodeB" });
    // A service named otherwise is another service: its account resolves to nothing this case knows,
    // so an administrator trusts no account beyond its own and the administrators'.
    CHECK(OwnStateAccountIds(JudgingCaller::Administrator, StateUse::Inspects, cfg, &ScriptedAccountId).empty());

    cfg.serviceName = "FastCacheCompileNode";
    CHECK(OwnStateAccountIds(JudgingCaller::Administrator, StateUse::Inspects, cfg, &ScriptedAccountId)
          == std::vector<std::string> { std::string { NodeServiceSid } });
    // Every other pair trusts nothing: a start writes, and anyone else trusts no account it is not.
    CHECK(OwnStateAccountIds(JudgingCaller::Administrator, StateUse::Runs, cfg, &ScriptedAccountId).empty());
    CHECK(OwnStateAccountIds(JudgingCaller::Other, StateUse::Inspects, cfg, &ScriptedAccountId).empty());
    CHECK(OwnStateAccountIds(JudgingCaller::Other, StateUse::Runs, cfg, &ScriptedAccountId).empty());

    // A group is never one: `--service-name="ALL SERVICES"` resolves to `S-1-5-80-0`.
    cfg.serviceName = "ALL SERVICES";
    CHECK(OwnStateAccountIds(JudgingCaller::Administrator, StateUse::Inspects, cfg, &ScriptedAccountId).empty());

    // No service named: no account counts as the node's own, for anybody.
    cfg.serviceName.clear();
    CHECK_FALSE(NodeServiceAccountOf(cfg).has_value());
    CHECK(OwnStateAccountIds(JudgingCaller::Administrator, StateUse::Inspects, cfg, &ScriptedAccountId).empty());

    // Every (caller, use) pair has its row, with a reason, and only one names an account.
    REQUIRE(OwnStateAccountRows().size() == EnumeratorCount<JudgingCaller> * EnumeratorCount<StateUse>);
    cfg.serviceName = "FastCacheCompileNode";
    for (auto const& row: OwnStateAccountRows())
    {
        CAPTURE(static_cast<int>(row.caller), static_cast<int>(row.use));
        CHECK_FALSE(row.why.empty());
        CHECK(row.account(cfg).has_value() == (row.caller == JudgingCaller::Administrator && row.use == StateUse::Inspects));
    }
}

TEST_CASE("An elevated --print-identity over the service's state directory is refused as a start is", "[node][state][trust]")
{
    // `--print-identity` MINTS a key and an id into a directory that lacks them, so it is no verb that writes
    // nothing: an administrator running it over the service's own files is refused, by name, exactly as a
    // start is. Through the resolver the verb itself runs (`ResolveNodeKey`), with the guard `main` gives it.
    auto cfg = NodeConfig {};
    cfg.printIdentity = true;
    CHECK(StateUseOf(cfg) == StateUse::Runs);
    for (auto const caller: Enumerators<JudgingCaller>())
    {
        CAPTURE(static_cast<int>(caller));
        ScratchDirectory const scratch { "trust-print-identity" };
        WriteStoreWithoutFormation(scratch.Path());
        auto platform = ScriptedNodeKeyGuard::OwnerOnly();
        for (auto const name: ServiceWrittenFiles)
            platform.OwnedBy(
                std::filesystem::path { name }, std::string { NodeServiceAccount }, std::string { NodeServiceSid });
        OwnStateAccountsGuard guard { platform, OwnStateAccountIds(caller, StateUseOf(cfg), cfg, &ScriptedAccountId) };
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().fault == NodeKeyFault::ForeignOwner);
        CHECK(resolved.error().message.contains(std::format("is owned by {}", NodeServiceAccount)));
        CHECK(resolved.error().message.contains("Ownership is judged from the account asking"));
        CHECK_FALSE(OffersRemovalForOwnership(resolved.error().message));
        CHECK(random.FillCount() == 0);
    }
}

TEST_CASE("Which verbs only inspect the state directory is a table and a start runs", "[node][state][trust]")
{
    auto start = NodeConfig {};
    CHECK(StateUseOf(start) == StateUse::Runs);
    auto surfaces = NodeConfig {};
    surfaces.printSurfaces = true;
    CHECK(StateUseOf(surfaces) == StateUse::Inspects);
    auto identity = NodeConfig {};
    identity.printIdentity = true;
    CHECK(StateUseOf(identity) == StateUse::Runs);
    auto install = NodeConfig {};
    install.installService = true;
    CHECK(StateUseOf(install) == StateUse::Runs);
    for (auto const& row: StateUseRows())
    {
        CAPTURE(row.verb);
        CHECK(row.verb.starts_with("--"));
        CHECK_FALSE(row.why.empty());
    }
}

TEST_CASE("A per-service SID is S-1-5-80 and five sub-authorities", "[node][state][trust]")
{
    CHECK(IsPerServiceSid(NodeServiceSid));
    CHECK(IsPerServiceSid("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464")); // TrustedInstaller
    CHECK_FALSE(IsPerServiceSid(AllServicesSid));
    CHECK_FALSE(IsPerServiceSid("S-1-5-80-1-2-3-4"));
    CHECK_FALSE(IsPerServiceSid("S-1-5-80-1-2-3-4-5-6"));
    CHECK_FALSE(IsPerServiceSid("S-1-5-80-1-2-x-4-5"));
    CHECK_FALSE(IsPerServiceSid("S-1-5-80-1--3-4-5"));
    CHECK_FALSE(IsPerServiceSid(PlainUserSid));
    CHECK_FALSE(IsPerServiceSid(""));
}
