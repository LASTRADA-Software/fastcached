// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "EnrollClient.hpp"
#include "NodeCredential.hpp"
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"
#include "NodeStateFiles.hpp"
#include "RaftStoreArchiver.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
    #include <sys/stat.h>

    #include <unistd.h>
#endif

#include <tests/AccessList.hpp>
#include <tests/FleetHistoryFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/NodeKeyFakes.hpp>
#include <tests/ScopedUmask.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>

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
                            Cluster::FleetEndpointsFileName,
                            Distributed::RosterFileName })
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
                                       Case { .name = "node-id.new", .expect = "as a temporary of node-id" },
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

TEST_CASE("Enrollment refuses a consensus store another account wrote for that, never as this node's history",
          "[enrollment][client][state]")
{
    // Enrollment reads the consensus history BEFORE it resolves the key, so the walk runs ahead of
    // that read: a planted store holding a term would otherwise be refused as "this node took part
    // in a cluster before" -- the wrong problem, with a remedy that deletes the evidence.
    ScratchDirectory const scratch { "enroll-foreign-store" };
    {
        auto store = Consensus::FileRaftStorage::Open(scratch.Path());
        REQUIRE(store.has_value());
        REQUIRE(store
                    ->SaveState(Consensus::PersistentState { .currentTerm = Consensus::Term { .value = 3 },
                                                             .votedFor = std::string { "n-9" } })
                    .has_value());
    }
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.clusterDir = scratch.Path();
    cfg.enrollFrom = "10.0.0.1:6680";
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    guard.OwnedByAnother(Consensus::RaftStateFileName);
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

    auto const refused = RunEnrollClient(cfg, random, guard);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("consensus term and vote"));
    CHECK(refused.error().contains("scripted-owner"));
    CHECK_FALSE(refused.error().contains("already holds consensus state"));
    CHECK(random.FillCount() == 0);
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
    REQUIRE(Distributed::FileRosterStore { dir / Distributed::RosterFileName }
                .Save(Cluster::PersistedRoster {
                    .certificate = Cluster::CertifiedRoster { .clusterId = "mine",
                                                              .version = 1,
                                                              .roster = Cluster::EncodeRoster(Cluster::Roster {}),
                                                              .endorsements = {} },
                    .certifiedUntil = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } } })
                .has_value());
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
    // `--enroll-from` is run elevated before the service ever starts, and writes as
    // `Administrators`; the install then grants the service's account the directory, inheritably
    // (`GrantPathAccess`). A file with a protected list of its own takes that grant neither before
    // nor after, and the service is refused its own roster with no remedy. So every state file
    // but the key takes the directory's list, and the grant reaches it whichever came first --
    // reproduced with the grant the review measured, `SERVICE` full control, inheritable.
    ScratchDirectory const scratch { "node-state-service-grant" };
    auto const dir = scratch.Path() / "state";
    SystemSecureRandom random;
    FileTrustNodeKeyGuard guard;
    REQUIRE(ResolveNodeKey(dir, random, guard).has_value());

    // Before the grant: the roster, as enrollment saves it.
    REQUIRE(Distributed::FileRosterStore { dir / Distributed::RosterFileName }
                .Save(Cluster::PersistedRoster {
                    .certificate = Cluster::CertifiedRoster { .clusterId = "mine",
                                                              .version = 1,
                                                              .roster = Cluster::EncodeRoster(Cluster::Roster {}),
                                                              .endorsements = {} },
                    .certifiedUntil = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } } })
                .has_value());
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

    for (auto const name: { Distributed::RosterFileName, Cluster::FormationRecordFileName })
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
    CHECK(hint.contains(std::format(R"(icacls "{}" /setowner "NT SERVICE\<the service's name>")", file.string())));
    CHECK(hint.contains("--enroll-from"));
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
    REQUIRE(Testing::ApplyAccessList(path, L"D:P(A;;FA;;;SY)(A;;FA;;;BA)"));
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
    CHECK(resolved.error().contains(path.string()));
    CHECK(resolved.error().contains("never minted over"));
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
