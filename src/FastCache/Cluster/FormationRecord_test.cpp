// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
/// A solitary record holding @p clusterId and nothing optional.
/// @param clusterId The own cluster's id.
/// @param createdAtUnixSeconds When it was minted.
/// @return The record.
[[nodiscard]] FormationRecord SolitaryRecord(std::string clusterId, std::uint64_t createdAtUnixSeconds)
{
    return FormationRecord { .mode = NodeMode::Solitary,
                             .own = { .clusterId = std::move(clusterId), .createdAtUnixSeconds = createdAtUnixSeconds },
                             .joining = std::nullopt,
                             .fleet = std::nullopt,
                             .archivePending = std::nullopt,
                             .rejectedBy = std::nullopt,
                             .askedJoins = {} };
}
} // namespace

TEST_CASE("Every node mode has exactly one table row", "[cluster][formation][mode]")
{
    for (auto const mode: KnownNodeModes)
        CHECK(std::ranges::count(NodeModeTable, mode, &NodeModeRow::mode) == 1);
    CHECK(NodeModeTable.size() == KnownNodeModes.size());
}

TEST_CASE("The mode table states the spec's shape for each mode", "[cluster][formation][mode]")
{
    using enum NodeMode;
    // A learner's Raft listener is CLOSED and it dials the leader two-way.
    CHECK(NodeModeRowFor(Learner).raftListener == RaftListenerState::Closed);
    CHECK(NodeModeRowFor(Learner).dials == Consensus::RaftWire::SessionDirection::TwoWay);
    CHECK(NodeModeRowFor(Learner).scheduler == SchedulerDuty::None);
    CHECK(NodeModeRowFor(Learner).consensus == ConsensusScope::Fleet);
    // A pending node is still its solitary cluster and keeps serving.
    CHECK(NodeModeRowFor(Pending).consensus == ConsensusScope::OwnCluster);
    CHECK(NodeModeRowFor(Pending).raftListener == RaftListenerState::Open);
    CHECK(NodeModeRowFor(Pending).scheduler == SchedulerDuty::Serves);
    CHECK(NodeModeRowFor(Pending).announces == CompileCacheWire::FleetState::Pending);
    // A solitary node announces solitary, a pending one points at the fleet it asked, and a fleet
    // member announces established.
    CHECK(NodeModeRowFor(Solitary).announces == CompileCacheWire::FleetState::Solitary);
    CHECK(NodeModeRowFor(Voter).announces == CompileCacheWire::FleetState::Established);
    CHECK(NodeModeRowFor(Learner).announces == CompileCacheWire::FleetState::Established);
}

TEST_CASE("Each node mode keeps the byte every cluster dir already written holds, at the record's mode offset",
          "[cluster][formation][mode][record]")
{
    // W-14. `NodeMode` is PERSISTED -- the formation record's mode byte -- and every caller spells the
    // enumerator, so a consistent renumbering stays green everywhere else while every cluster dir
    // already written reads as another mode. The raw enumerators are the anchor, pinned as literals;
    // then the BYTE an encoded record carries at its mode offset, which is the persisted fact itself.
    CHECK(static_cast<unsigned>(NodeMode::Solitary) == 0x01U);
    CHECK(static_cast<unsigned>(NodeMode::Pending) == 0x02U);
    CHECK(static_cast<unsigned>(NodeMode::Learner) == 0x03U);
    CHECK(static_cast<unsigned>(NodeMode::Voter) == 0x04U);
    // The offset is layout too: four magic bytes, the format byte, then the mode.
    CHECK(FormationRecordModeOffset == 5U);

    auto const inFleet = FleetMembership {
        .clusterId = "fleet-c", .roster = { std::byte { 1 } }, .createdAtUnixSeconds = 1, .admittedBy = {}
    };
    auto const joining = JoinTarget { .summary = CompileCacheWire::FleetSummary { .clusterId = "fleet-c", .nodeId = "n-a" },
                                      .provenKey = {},
                                      .askedAtUnixSeconds = 9 };
    struct Row
    {
        NodeMode mode;
        std::uint8_t byte;
    };
    for (auto const& [mode, byte]: { Row { .mode = NodeMode::Solitary, .byte = 0x01 },
                                     Row { .mode = NodeMode::Pending, .byte = 0x02 },
                                     Row { .mode = NodeMode::Learner, .byte = 0x03 },
                                     Row { .mode = NodeMode::Voter, .byte = 0x04 } })
    {
        INFO("mode " << NodeModeRowFor(mode).name);
        auto record = SolitaryRecord("own-c", 10);
        record.mode = mode;
        if (mode == NodeMode::Pending)
            record.joining = joining;
        if (NodeModeRowFor(mode).consensus == ConsensusScope::Fleet)
            record.fleet = inFleet;
        auto const encoded = EncodeFormationRecord(record);
        REQUIRE(encoded.size() > FormationRecordModeOffset);
        CHECK(encoded[FormationRecordModeOffset] == std::byte { byte });
        auto const decoded = DecodeFormationRecord(encoded);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).mode == mode);
    }
}

TEST_CASE("A minted solitary record holds a fresh cluster id and the wall-clock second", "[cluster][formation][record]")
{
    Testing::ScriptedSecureRandom random { std::vector<std::byte>(ClusterIdBytes, std::byte { 0xAB }) };
    core::platform::ManualWallClock const wall { std::chrono::system_clock::time_point {
        std::chrono::seconds { 1'790'000'123 } } };
    auto const minted = MintSolitary(random, wall);
    REQUIRE(minted.has_value());
    auto const& record = Unwrap(minted);
    CHECK(record.mode == NodeMode::Solitary);
    // Sixteen 0xAB bytes, as 32 lowercase hex characters: the byte order a tie-break compares.
    CHECK(record.own.clusterId == "abababababababababababababababab");
    CHECK(record.own.createdAtUnixSeconds == 1'790'000'123);
    CHECK(FoundedHere(record));
    CHECK(CurrentClusterId(record) == record.own.clusterId);
}

TEST_CASE("A clock before the epoch mints the newest cluster, never the oldest", "[cluster][formation][record]")
{
    // The older cluster wins a tie-break, so a broken clock reading as the epoch would win every one.
    Testing::ScriptedSecureRandom random { std::vector<std::byte>(ClusterIdBytes, std::byte { 0x01 }) };
    core::platform::ManualWallClock const wall { std::chrono::system_clock::time_point { std::chrono::seconds { -5 } } };
    auto const minted = MintSolitary(random, wall);
    REQUIRE(minted.has_value());
    CHECK(Unwrap(minted).own.createdAtUnixSeconds == UnbelievableClockCreatedAt);
    CHECK(UnbelievableClockCreatedAt == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("A failed draw mints no cluster", "[cluster][formation][record]")
{
    // A weak id would let two machines imaged from one disk mint the same fleet (#1527's shape).
    Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::DeniedFailure() };
    core::platform::ManualWallClock const wall;
    CHECK_FALSE(MintSolitary(random, wall).has_value());
}

TEST_CASE("Every id a mint produces is one the cluster-id grammar accepts, and nothing near one is",
          "[cluster][formation][record][pin]")
{
    // Every nibble, so a grammar missing a digit or a letter is refused by its own mint.
    auto drawn = std::vector<std::byte> {};
    for (auto const byte: { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF })
        drawn.push_back(static_cast<std::byte>(byte));
    auto const half = drawn;
    drawn.insert(drawn.end(), half.begin(), half.end());
    REQUIRE(drawn.size() == ClusterIdBytes);
    Testing::ScriptedSecureRandom random { drawn };
    auto const minted = MintClusterId(random);
    REQUIRE(minted.has_value());
    CHECK(Unwrap(minted) == "0123456789abcdef0123456789abcdef");
    CHECK(IsMintedClusterId(Unwrap(minted)));

    auto const& id = Unwrap(minted);
    for (auto const& almost: { id.substr(1),
                               id + "0",
                               std::string { "0123456789ABCDEF0123456789ABCDEF" },
                               id.substr(1) + "g",
                               id.substr(1) + " ",
                               std::string {} })
    {
        INFO("'" << almost << "'");
        CHECK_FALSE(IsMintedClusterId(almost));
    }
}

TEST_CASE("A formation record round-trips every optional part", "[cluster][formation][record]")
{
    auto record = FormationRecord { .mode = NodeMode::Learner,
                                    .own = { .clusterId = "own-c", .createdAtUnixSeconds = 10 },
                                    .joining = std::nullopt,
                                    .fleet = FleetMembership { .clusterId = "fleet-c",
                                                               .roster = { std::byte { 1 }, std::byte { 2 } },
                                                               .createdAtUnixSeconds = 0,
                                                               .admittedBy = {} },
                                    .archivePending = std::string { "own-c" },
                                    .rejectedBy = RejectionMemo { .clusterId = "other", .atUnixSeconds = 5 },
                                    .askedJoins = {} };
    record.fleet->createdAtUnixSeconds = 1'700'000'000;
    record.fleet->admittedBy.fill(std::byte { 0x5A }); // the admitting key round-trips byte for byte
    record.askedJoins.push_back(AskedJoin { .clusterId = "fleet-c", .provenKey = {}, .askedAtUnixSeconds = 42 });
    record.askedJoins.back().provenKey.fill(std::byte { 0x3D });
    auto const first = DecodeFormationRecord(EncodeFormationRecord(record));
    REQUIRE(first.has_value());
    CHECK(Unwrap(first) == record);
    CHECK_FALSE(FoundedHere(Unwrap(first)));
    CHECK(CurrentClusterId(Unwrap(first)) == "fleet-c");
    // Named, because whole-record equality would also pass a codec that dropped both on each side.
    REQUIRE(Unwrap(first).fleet.has_value());
    CHECK(Unwrap(Unwrap(first).fleet).createdAtUnixSeconds == 1'700'000'000);
    REQUIRE(Unwrap(first).askedJoins.size() == 1);
    CHECK(Unwrap(first).askedJoins.front().askedAtUnixSeconds == 42);

    record.mode = NodeMode::Pending;
    record.fleet.reset();
    record.joining = JoinTarget { .summary = CompileCacheWire::FleetSummary { .clusterId = "fleet-c", .nodeId = "n-a" },
                                  .provenKey = {},
                                  .askedAtUnixSeconds = 99 };
    record.joining->provenKey.fill(std::byte { 0x5C });
    auto const second = DecodeFormationRecord(EncodeFormationRecord(record));
    REQUIRE(second.has_value());
    CHECK(Unwrap(second) == record);
}

TEST_CASE("A join target keeps the member list it was proven with, longer than a datagram carries",
          "[cluster][formation][record]")
{
    // A target may have been proven by a seed's ANSWER, whose list is longer than a beacon's; the
    // record keeps the summary as it was proven, so it reads at the reply's cap.
    auto record = SolitaryRecord("c", 1);
    record.mode = NodeMode::Pending;
    auto summary = CompileCacheWire::FleetSummary { .clusterId = "fleet-c", .nodeId = "n-a" };
    for (auto const index: std::views::iota(std::size_t { 0 }, CompileCacheWire::MaxFleetSummaryMembers + 9))
        summary.members.push_back(std::format("n-{}", index));
    summary.memberTotal = 40;
    summary.nodeEndpoint = "office-a:6674";
    record.joining = JoinTarget { .summary = summary, .provenKey = {}, .askedAtUnixSeconds = 7 };

    auto const decoded = DecodeFormationRecord(EncodeFormationRecord(record));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == record);
    REQUIRE(Unwrap(decoded).joining.has_value());
    CHECK(Unwrap(Unwrap(decoded).joining).summary.members.size() == CompileCacheWire::MaxFleetSummaryMembers + 9);
}

TEST_CASE("A formation record naming a cluster id past its bound is refused as damage, by name",
          "[cluster][formation][record]")
{
    // No build writes an id longer than every reader of one accepts, so such a record is damage --
    // refused by name rather than carried into a beacon every peer would drop. One row per id the
    // record holds; each at the bound decodes, which is what makes the refusal the length's.
    struct Holder
    {
        std::string_view what;
        void (*place)(FormationRecord&, std::string);
    };
    auto const holders = std::array<Holder, 5> { {
        { .what = "own cluster",
          .place = [](FormationRecord& record, std::string id) { record.own.clusterId = std::move(id); } },
        { .what = "fleet membership",
          .place =
              [](FormationRecord& record, std::string id) {
                  record.fleet = FleetMembership {
                      .clusterId = std::move(id), .roster = {}, .createdAtUnixSeconds = 0, .admittedBy = {}
                  };
              } },
        { .what = "pending archive",
          .place = [](FormationRecord& record, std::string id) { record.archivePending = std::move(id); } },
        { .what = "rejection memo",
          .place =
              [](FormationRecord& record, std::string id) {
                  record.rejectedBy = RejectionMemo { .clusterId = std::move(id), .atUnixSeconds = 5 };
              } },
        { .what = "asked fleet",
          .place =
              [](FormationRecord& record, std::string id) {
                  record.askedJoins.push_back(
                      AskedJoin { .clusterId = std::move(id), .provenKey = {}, .askedAtUnixSeconds = 5 });
              } },
    } };
    for (auto const& holder: holders)
    {
        INFO(holder.what);
        auto atBound = SolitaryRecord("c", 1);
        holder.place(atBound, std::string(CompileCacheWire::MaxIdBytes, 'c'));
        CHECK(DecodeFormationRecord(EncodeFormationRecord(atBound)).has_value());

        auto pastBound = SolitaryRecord("c", 1);
        holder.place(pastBound, std::string(CompileCacheWire::MaxIdBytes + 1, 'c'));
        auto const refused = DecodeFormationRecord(EncodeFormationRecord(pastBound));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::MalformedFrame);
        CHECK(refused.error().context.contains(
            std::format("its {} names a cluster id of {} bytes", holder.what, CompileCacheWire::MaxIdBytes + 1)));
    }
}

TEST_CASE("An empty pending archive is present and not absent", "[cluster][formation][record]")
{
    // An empty cluster id is a value the record can hold; reading it back as absent would drop
    // the reminder that a Raft store still waits to be archived.
    auto record = SolitaryRecord("c", 1);
    record.archivePending = std::string {};
    auto const decoded = DecodeFormationRecord(EncodeFormationRecord(record));
    REQUIRE(decoded.has_value());
    REQUIRE(Unwrap(decoded).archivePending.has_value());
    CHECK(Unwrap(decoded) == record);
}

TEST_CASE("A record another build wrote is refused by name and damage is not called a version",
          "[cluster][formation][record]")
{
    auto bytes = EncodeFormationRecord(SolitaryRecord("c", 1));
    auto later = bytes;
    later[FormationRecordFormatOffset] = std::byte { FormationRecordFormat + 1 };
    // A later layout may carry a mode this build has no name for; the format still decides.
    later[FormationRecordModeOffset] = std::byte { 0x7E };
    auto const refused = DecodeFormationRecord(later);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);

    auto truncated = bytes;
    truncated.resize(truncated.size() - 3);
    auto const damaged = DecodeFormationRecord(truncated);
    REQUIRE_FALSE(damaged.has_value());
    CHECK(damaged.error().code == ConsensusErrorCode::MalformedFrame);

    auto unknownMode = bytes;
    unknownMode[FormationRecordModeOffset] = std::byte { 0x7E };
    auto const unnamed = DecodeFormationRecord(unknownMode);
    REQUIRE_FALSE(unnamed.has_value());
    CHECK(unnamed.error().code == ConsensusErrorCode::MalformedFrame);

    auto notARecord = bytes;
    notARecord[0] = std::byte { 'X' };
    auto const foreign = DecodeFormationRecord(notARecord);
    REQUIRE_FALSE(foreign.has_value());
    CHECK(foreign.error().code == ConsensusErrorCode::MalformedFrame);
}

TEST_CASE("The file store says absent when nothing was ever written and saves atomically", "[cluster][formation][record]")
{
    Testing::ScratchDirectory const scratch { "formation-store" };
    FileFormationStore store { scratch.Path() };
    auto const empty = store.Load();
    REQUIRE(empty.has_value());
    CHECK(Unwrap(empty) == std::nullopt);

    auto const record = SolitaryRecord("c1", 7);
    REQUIRE(store.Save(record).has_value());
    auto const loaded = store.Load();
    REQUIRE(loaded.has_value());
    CHECK(Unwrap(loaded) == std::optional { record });
    CHECK(std::filesystem::exists(scratch.Path() / std::string { FormationRecordFileName }));
}

TEST_CASE("A formation record a later build wrote is kept and never written over", "[cluster][formation][record]")
{
    Testing::ScratchDirectory const scratch { "formation-later" };
    auto const path = scratch.Path() / std::string { FormationRecordFileName };
    auto later = EncodeFormationRecord(SolitaryRecord("later", 3));
    later[FormationRecordFormatOffset] = std::byte { FormationRecordFormat + 1 };
    REQUIRE(Consensus::ReplaceFileAtomically(path, later, StateFile::Formation).has_value());

    FileFormationStore store { scratch.Path() };
    auto const loaded = store.Load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
    CHECK(loaded.error().context.contains(path.string()));
    // Another layout is intact, so the remedy is the build that wrote it, not moving it aside.
    CHECK(loaded.error().context.contains("start the build that wrote that format"));
    CHECK_FALSE(loaded.error().context.contains("move the file aside to re-form"));

    auto const saved = store.Save(SolitaryRecord("mine", 4));
    REQUIRE_FALSE(saved.has_value());
    CHECK(saved.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
    auto const onDisk = Consensus::ReadFileIfPresent(path);
    REQUIRE(onDisk.has_value());
    CHECK(Unwrap(onDisk) == std::optional { later });
}

TEST_CASE("A damaged formation record refuses the load by name rather than reading as none", "[cluster][formation][record]")
{
    Testing::ScratchDirectory const scratch { "formation-damaged" };
    auto const path = scratch.Path() / std::string { FormationRecordFileName };
    auto damaged = EncodeFormationRecord(SolitaryRecord("c", 1));
    damaged.resize(damaged.size() - 1);
    REQUIRE(Consensus::ReplaceFileAtomically(path, damaged, StateFile::Formation).has_value());

    auto const loaded = FileFormationStore { scratch.Path() }.Load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == ConsensusErrorCode::MalformedFrame);
    CHECK(loaded.error().context.contains(path.string()));
    CHECK(loaded.error().context.contains("move the file aside to re-form"));
    CHECK_FALSE(loaded.error().context.contains("start the build that wrote that format"));
}

TEST_CASE("A damaged formation record, or a file that is not one, is kept and never written over",
          "[cluster][formation][record]")
{
    // A save over it would re-mint the node and silently leave the fleet the damaged record names.
    auto truncated = EncodeFormationRecord(SolitaryRecord("c", 1));
    truncated.resize(truncated.size() - 3);
    auto const notARecord = std::vector<std::byte> { std::byte { 'h' }, std::byte { 'e' }, std::byte { 'l' },
                                                     std::byte { 'l' }, std::byte { 'o' }, std::byte { '!' } };
    auto const kept = std::array { truncated, notARecord };
    auto const saves = Testing::ScratchDirectory { "formation-kept" };
    auto index = 0;
    for (auto const& bytes: kept)
    {
        auto const directory = saves.Path() / std::to_string(index++);
        auto const path = directory / std::string { FormationRecordFileName };
        std::error_code created;
        std::filesystem::create_directories(directory, created);
        REQUIRE_FALSE(created);
        REQUIRE(Consensus::ReplaceFileAtomically(path, bytes, StateFile::Formation).has_value());

        auto const saved = FileFormationStore { directory }.Save(SolitaryRecord("mine", 4));
        REQUIRE_FALSE(saved.has_value());
        CHECK(saved.error().code == ConsensusErrorCode::MalformedFrame);
        CHECK(saved.error().context.contains(path.string()));
        auto const onDisk = Consensus::ReadFileIfPresent(path);
        REQUIRE(onDisk.has_value());
        CHECK(Unwrap(onDisk) == std::optional { bytes });
    }
    CHECK(index == 2);
}

TEST_CASE("A record remembers each fleet and key it asked once, first ask kept, and at most the bound",
          "[cluster][formation][record]")
{
    // The memo a split of one fleet is told on, keyed by (cluster id, proven key): asking the same
    // fleet under the same key again keeps the FIRST ask; the same id under another key is another
    // claim with a memo of its own; and a machine that asked many keeps the most recent ones.
    auto record = SolitaryRecord("c", 1);
    auto const memo = [](std::string id, std::byte keyFill, std::uint64_t at) {
        auto asked = AskedJoin { .clusterId = std::move(id), .provenKey = {}, .askedAtUnixSeconds = at };
        asked.provenKey.fill(keyFill);
        return asked;
    };
    RememberAsked(record, memo("office", std::byte { 1 }, 10));
    RememberAsked(record, memo("lab", std::byte { 2 }, 20));
    RememberAsked(record, memo("office", std::byte { 1 }, 30)); // the same claim again
    REQUIRE(record.askedJoins.size() == 2);
    CHECK(record.askedJoins[0].clusterId == "office");
    CHECK(record.askedJoins[0].askedAtUnixSeconds == 10); // when it was FIRST asked
    CHECK(record.askedJoins[1].clusterId == "lab");

    RememberAsked(record, memo("office", std::byte { 3 }, 40)); // the same id under another key
    REQUIRE(record.askedJoins.size() == 3);
    CHECK(record.askedJoins[0].provenKey.front() == std::byte { 1 }); // the earlier claim is kept
    CHECK(record.askedJoins[2].clusterId == "office");
    CHECK(record.askedJoins[2].provenKey.front() == std::byte { 3 });
    CHECK(record.askedJoins[2].askedAtUnixSeconds == 40);

    for (auto const index: std::views::iota(std::uint64_t { 0 }, std::uint64_t { MaxAskedJoins }))
        RememberAsked(record, memo(std::format("fleet-{}", index), std::byte { 4 }, 100 + index));
    REQUIRE(record.askedJoins.size() == MaxAskedJoins);
    CHECK(record.askedJoins.front().clusterId == "fleet-0");
    CHECK(record.askedJoins.back().clusterId == std::format("fleet-{}", MaxAskedJoins - 1));
    CHECK(std::ranges::none_of(record.askedJoins, [](AskedJoin const& kept) { return kept.clusterId == "office"; }));

    auto const decoded = DecodeFormationRecord(EncodeFormationRecord(record));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).askedJoins == record.askedJoins);
}

TEST_CASE("Asks that went nowhere never displace the memo of a fleet that admitted this node",
          "[cluster][formation][record]")
{
    // The flood: a fleet costs nothing to mint, so a node can be led to ask many. Each ask is a memo,
    // and with oldest-first eviction the eighth would push out the one memo a split is told on. So the
    // oldest memo NOT admitted goes first, and an admitted one only when every memo is.
    auto record = SolitaryRecord("c", 1);
    auto const memo = [](std::string id, std::uint64_t at) {
        return AskedJoin { .clusterId = std::move(id), .provenKey = {}, .askedAtUnixSeconds = at };
    };
    RememberAsked(record, memo("office", 10));
    RememberAdmitted(record, "office", Ed25519PublicKey {});
    REQUIRE(record.askedJoins.front().admitted);

    for (auto const index: std::views::iota(std::uint64_t { 0 }, std::uint64_t { 3 * MaxAskedJoins }))
        RememberAsked(record, memo(std::format("minted-{}", index), 100 + index));
    REQUIRE(record.askedJoins.size() == MaxAskedJoins);
    CHECK(record.askedJoins.front().clusterId == "office"); // kept, and still the oldest
    CHECK(record.askedJoins.front().admitted);
    CHECK(record.askedJoins.back().clusterId == std::format("minted-{}", (3 * MaxAskedJoins) - 1));

    // The control, and the limit: once every memo was admitted, the oldest goes after all.
    auto allAdmitted = SolitaryRecord("c", 1);
    for (auto const index: std::views::iota(std::uint64_t { 0 }, std::uint64_t { MaxAskedJoins }))
    {
        RememberAsked(allAdmitted, memo(std::format("fleet-{}", index), index));
        RememberAdmitted(allAdmitted, std::format("fleet-{}", index), Ed25519PublicKey {});
    }
    RememberAsked(allAdmitted, memo("one-more", 99));
    REQUIRE(allAdmitted.askedJoins.size() == MaxAskedJoins);
    CHECK(allAdmitted.askedJoins.front().clusterId == "fleet-1");
    CHECK(allAdmitted.askedJoins.back().clusterId == "one-more");

    // Marked where it is, and read back as written.
    auto const decoded = DecodeFormationRecord(EncodeFormationRecord(record));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).askedJoins == record.askedJoins);
}

TEST_CASE("A record holding more asked fleets than any build keeps is damage", "[cluster][formation][record]")
{
    // No build writes more than the bound, since `RememberAsked` drops the oldest; so more is damage,
    // refused by name rather than read. The control is the bound itself, which decodes.
    auto atBound = SolitaryRecord("c", 1);
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxAskedJoins))
        atBound.askedJoins.push_back(
            AskedJoin { .clusterId = std::format("f{}", index), .provenKey = {}, .askedAtUnixSeconds = 1 });
    REQUIRE(DecodeFormationRecord(EncodeFormationRecord(atBound)).has_value());

    auto pastBound = atBound;
    pastBound.askedJoins.push_back(AskedJoin { .clusterId = "one-more", .provenKey = {}, .askedAtUnixSeconds = 1 });
    auto const refused = DecodeFormationRecord(EncodeFormationRecord(pastBound));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::MalformedFrame);
    CHECK(refused.error().context.contains("asked fleets"));
}

TEST_CASE("An earlier record format is another layout, refused by name, and never read as damage",
          "[cluster][formation][record]")
{
    // Format 2 added the fleet's age and the asked fleets; format 3 nested a summary of eleven fields
    // and memos of four; format 4 embedded a version-2 roster in a fleet membership; format 5 kept the
    // key that signed the admission; format 6 is this branch's unreleased layout -- the embedded roster
    // is version 3, its principals group gone -- FINAL only at the lane-0 flag day (see
    // `FormationRecordFormat`). An earlier record is intact and belongs to the build that wrote it:
    // `UnsupportedFormatVersion`, which is what monitoring sees, never `MalformedFrame`.
    static_assert(FormationRecordFormat == 6,
                  "this case pins format 6, unreleased and final only at the lane-0 flag day; from then on a "
                  "layout change moves the number and adds the old one below");
    for (auto const format:
         { std::uint8_t { 1 }, std::uint8_t { 2 }, std::uint8_t { 3 }, std::uint8_t { 4 }, std::uint8_t { 5 } })
    {
        INFO("format " << int { format });
        auto older = EncodeFormationRecord(SolitaryRecord("c", 1));
        older[FormationRecordFormatOffset] = std::byte { format };
        auto const refused = DecodeFormationRecord(older);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
        CHECK(refused.error().context.contains(std::format("format {}", format)));
    }
}

TEST_CASE("A learner's record holding the previous roster layout is refused by the record's own number",
          "[cluster][formation][record]")
{
    // A learner's record keeps its approval's roster (`FleetMembership::roster`), and a roster member
    // gained a fifth field -- its recorded `0xFC` endpoint -- under `RosterFormatVersion` 2. A record
    // written before holds a version-1 roster of four fields a member: built here by hand as that
    // build wrote it, and refused by NAME at the record, before anything reads the roster, so the
    // operator is told which build wrote the file and never that it is damaged.
    auto const member = WireFields::Encode({ WireFields::AsBytes(std::string_view { "n1" }),
                                             WireFields::AsBytes(std::string_view { "n1:6680" }),
                                             std::span<std::byte const> { std::array { std::byte { 0x00 } } },
                                             std::span<std::byte const> {} });
    auto const members = WireFields::Encode({ std::span<std::byte const> { member } });
    auto const empty = WireFields::Encode(WireFields::FieldList {});
    auto const oldRoster = WireFields::Encode({ std::span<std::byte const> { std::array { std::byte { 0x01 } } },
                                                std::span<std::byte const> { members },
                                                std::span<std::byte const> { empty },
                                                std::span<std::byte const> { empty } });
    auto record = SolitaryRecord("c-laptop", 1);
    record.mode = NodeMode::Learner;
    record.fleet =
        FleetMembership { .clusterId = "c-office", .roster = oldRoster, .createdAtUnixSeconds = 1, .admittedBy = {} };
    auto written = EncodeFormationRecord(record);
    written[FormationRecordFormatOffset] = std::byte { 3 }; // as the build before the roster moved wrote it

    auto const refused = DecodeFormationRecord(written);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
    CHECK(refused.error().context.contains(
        std::format("written in format 3; this build reads format {}", FormationRecordFormat)));
}
