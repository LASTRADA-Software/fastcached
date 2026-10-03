// SPDX-License-Identifier: Apache-2.0
#include "FormationLoop.hpp"
#include "LiveNodeConfig.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeDefaults.hpp"
#include "NodeMembership.hpp"
#include "NodeReload.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/ProcessExit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/NodeFormationControllerFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{
/// A body that served until it was asked to stop, as `WorkerBody` ends one.
constexpr auto Served = FastCache::ExitCodeOf(FastCache::ProcessExit::Served);
/// A formation that could not be adopted: the start's `Formation` stage.
constexpr auto FormationRefused = FastCache::ExitCodeFor(FastCache::StartStage::Formation);
} // namespace

namespace
{
/// A store whose record this build cannot read.
class UnreadableFormationStore final: public Cluster::IFormationStore
{
  public:
    /// @return The refusal a record another build wrote is read with.
    [[nodiscard]] std::expected<std::optional<Cluster::FormationRecord>, ConsensusError> Load() const override
    {
        return std::unexpected(UnsupportedFormatVersion("the formation record was written in format 9"));
    }

    /// @param record What would be kept.
    /// @return Nothing; every save is recorded.
    [[nodiscard]] std::expected<void, ConsensusError> Save(Cluster::FormationRecord const& record) override
    {
        saves.push_back(record);
        return {};
    }

    std::vector<Cluster::FormationRecord> saves; ///< Every record saved: none, if nothing minted over it.
};

/// What every mint here draws: one byte value, so the id it produces is known.
/// @return The random source.
[[nodiscard]] Testing::ScriptedSecureRandom MintBytes()
{
    return Testing::ScriptedSecureRandom { std::vector<std::byte>(Cluster::ClusterIdBytes, std::byte { 0xab }) };
}

/// The loop's controls on paper: a stop flag a case sets, every pause recorded rather than waited, and
/// a clock a case advances to say how long a body served.
struct Pacing
{
    /// @return The controls, reading this object; valid for as long as it.
    [[nodiscard]] LoopControls Controls()
    {
        return LoopControls { .stopRequested = [this] { return stop; },
                              .pause = [this](std::chrono::milliseconds wait) { pauses.push_back(wait); },
                              .clock = clock };
    }

    bool stop = false;                             ///< What `stopRequested` answers.
    std::vector<std::chrono::milliseconds> pauses; ///< Every wait asked for, in order.
    core::platform::ManualClock clock;             ///< What a body's serving time is read from.
};

/// Copies and closes descriptors on paper: each copy is a new number, and every close is recorded.
struct RecordingDescriptors final: IInheritedDescriptors
{
    /// @param refusing Whether every copy is refused.
    explicit RecordingDescriptors(bool refusing = false):
        refuse { refusing }
    {
    }

    /// @copydoc IInheritedDescriptors::Duplicate
    [[nodiscard]] std::expected<int, std::string> Duplicate(int descriptor) const override
    {
        copiedFrom.push_back(descriptor);
        if (refuse)
            return std::unexpected { std::string { "out of descriptors" } };
        return 100 + static_cast<int>(copiedFrom.size());
    }

    /// @copydoc IInheritedDescriptors::Close
    void Close(int descriptor) const noexcept override
    {
        closed.push_back(descriptor);
    }

    bool refuse;                         ///< Whether every copy is refused.
    mutable std::vector<int> copiedFrom; ///< Every descriptor a copy was asked of, in order.
    mutable std::vector<int> closed;     ///< Every descriptor closed, in order.
};
} // namespace

TEST_CASE("A reform adopts the record again before every body and runs until one stops", "[node][formation][loop]")
{
    // A restart and a transition are one code path: every body is built from the record as it is
    // NOW, adopted into a fresh copy of what the start had.
    CapturingLogger logger;
    Pacing pacing;
    auto const modes = std::array { Cluster::NodeMode::Solitary, Cluster::NodeMode::Learner, Cluster::NodeMode::Voter };
    auto const ends = std::array { BodyEnd::Reform, BodyEnd::Reform, BodyEnd::Stopped };
    auto seen = std::vector<Cluster::NodeMode> {};
    auto adoptions = std::size_t { 0 };
    // As production's: the start's own adoption is already in the base, so a fresh copy is one that
    // holds THAT formation, never the one the previous adoption wrote.
    auto base = NodeConfig {};
    base.nodeId = "n-laptop";
    base.formation = NodeFormationView { .mode = Cluster::NodeMode::Solitary,
                                         .clusterId = "c-start",
                                         .createdAtUnixSeconds = 0,
                                         .foundedHere = true,
                                         .fleetMembers = {},
                                         .fleetSchedulers = {} };

    auto const exit = RunFormationLoop(
        LiveNodeConfig { base, nullptr },
        [&seen, &ends](NodeConfig const& cfg) {
            REQUIRE(cfg.formation.has_value());
            seen.push_back(cfg.formation->mode);
            return BodyOutcome { .end = ends.at(seen.size() - 1), .exitCode = Served };
        },
        [&modes, &adoptions](NodeConfig cfg) -> std::expected<NodeConfig, std::string> {
            // A fresh copy every time: nothing the previous adoption wrote is here.
            REQUIRE(cfg.formation.has_value());
            CHECK(cfg.formation->clusterId == "c-start");
            CHECK(cfg.nodeId == "n-laptop");
            cfg.formation = NodeFormationView { .mode = modes.at(adoptions),
                                                .clusterId = std::format("c-adopted-{}", adoptions),
                                                .createdAtUnixSeconds = 0,
                                                .foundedHere = true,
                                                .fleetMembers = {},
                                                .fleetSchedulers = {} };
            ++adoptions;
            return cfg;
        },
        pacing.Controls(),
        logger);

    CHECK(exit == Served);
    CHECK(seen == std::vector<Cluster::NodeMode>(modes.begin(), modes.end()));
    CHECK(adoptions == 3);
    CHECK(pacing.pauses == std::vector { ReformBackoff.at(1) }); // the second reform in a row waits
}

TEST_CASE("A reform under an inherited listener serves a copy of it and never binds --listen-node",
          "[node][formation][loop][socket-activation]")
{
    // The supervisor hands its listening socket over once, and a body's listener closes what it
    // adopts. A reformed body handed nothing would bind --listen-node itself, on the port the
    // supervisor holds; one handed the original would serve the descriptor the last body closed. So
    // every body is handed a COPY, and the original stays the process's until the hold goes.
    constexpr auto Inherited = 3;
    auto const descriptors = RecordingDescriptors {};
    auto served = std::vector<std::optional<int>> {};
    {
        ActivationHold const activation { Inherited, descriptors };
        CapturingLogger logger;
        Pacing pacing;
        auto const exit = RunFormationLoop(
            LiveNodeConfig { NodeConfig {}, nullptr },
            [&](NodeConfig const&) {
                auto copy = activation.ForBody();
                REQUIRE(copy.has_value());
                served.push_back(*copy);
                return BodyOutcome { .end = served.size() < 3 ? BodyEnd::Reform : BodyEnd::Stopped, .exitCode = Served };
            },
            [](NodeConfig const& cfg) -> std::expected<NodeConfig, std::string> { return cfg; },
            pacing.Controls(),
            logger);
        CHECK(exit == Served);

        // Every body served the supervisor's socket -- an engaged descriptor, which a body adopts
        // rather than binding -- and each its own copy of it, never the original.
        REQUIRE(served.size() == 3);
        CHECK(served == std::vector<std::optional<int>> { 101, 102, 103 });
        CHECK(descriptors.copiedFrom == std::vector<int> { Inherited, Inherited, Inherited });
        CHECK(descriptors.closed.empty()); // the original outlives every body
    }
    CHECK(descriptors.closed == std::vector<int> { Inherited }); // and goes with the hold
}

TEST_CASE("A hold with nothing handed over copies nothing and a copy that fails is said",
          "[node][formation][loop][socket-activation]")
{
    {
        auto const descriptors = RecordingDescriptors {};
        {
            ActivationHold const activation { std::nullopt, descriptors };
            auto const nothing = activation.ForBody();
            REQUIRE(nothing.has_value());
            CHECK_FALSE(nothing->has_value()); // the body binds --listen-node, as without a supervisor
        }
        CHECK(descriptors.copiedFrom.empty());
        CHECK(descriptors.closed.empty());
    }
    {
        auto const descriptors = RecordingDescriptors { true };
        ActivationHold const activation { 3, descriptors };
        auto const refused = activation.ForBody();
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "out of descriptors");
    }
}

TEST_CASE("A refused adoption ends the loop with the usage exit and says why", "[node][formation][loop]")
{
    CapturingLogger logger;
    Pacing pacing;
    auto runs = 0;
    auto const exit = RunFormationLoop(
        LiveNodeConfig { NodeConfig {}, nullptr },
        [&runs](NodeConfig const&) {
            ++runs;
            return BodyOutcome {};
        },
        [](NodeConfig const&) -> std::expected<NodeConfig, std::string> {
            return std::unexpected { std::string { "formation record: written by a later build" } };
        },
        pacing.Controls(),
        logger);
    CHECK(exit == FormationRefused);
    CHECK(runs == 0);
    CHECK(std::ranges::any_of(logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains("written by a later build");
    }));
}

TEST_CASE("A body ends for a reform only when it served cleanly and nobody asked it to stop", "[node][formation][loop]")
{
    // Every combination of the three inputs -- all eight -- because the wrong answer either way is
    // silent: a refusal read as a reform restarts a node that should have stopped, and a stop read as
    // a reform ignores the operator.
    struct Row
    {
        int exitCode;
        bool reformAsked;
        bool stopAsked;
        BodyEnd end;
    };
    constexpr auto rows = std::array {
        Row { .exitCode = Served, .reformAsked = true, .stopAsked = false, .end = BodyEnd::Reform },
        Row { .exitCode = Served, .reformAsked = false, .stopAsked = false, .end = BodyEnd::Stopped },
        Row { .exitCode = Served, .reformAsked = true, .stopAsked = true, .end = BodyEnd::Stopped },
        Row { .exitCode = Served, .reformAsked = false, .stopAsked = true, .end = BodyEnd::Stopped },
        Row { .exitCode = FormationRefused, .reformAsked = true, .stopAsked = false, .end = BodyEnd::Refused },
        Row { .exitCode = FormationRefused, .reformAsked = false, .stopAsked = false, .end = BodyEnd::Refused },
        Row { .exitCode = FormationRefused, .reformAsked = true, .stopAsked = true, .end = BodyEnd::Refused },
        Row { .exitCode = FormationRefused, .reformAsked = false, .stopAsked = true, .end = BodyEnd::Refused },
    };
    // Eight rows are all eight combinations only if no two name the same one and every exit code is one
    // of the two: a duplicated row passes a count and leaves a combination untested.
    constexpr auto everyCombinationOnce = [](auto const& table) {
        for (auto const first: std::views::iota(std::size_t { 0 }, table.size()))
        {
            if (table[first].exitCode != Served && table[first].exitCode != FormationRefused)
                return false;
            for (auto const second: std::views::iota(first + 1, table.size()))
                if (table[first].exitCode == table[second].exitCode && table[first].reformAsked == table[second].reformAsked
                    && table[first].stopAsked == table[second].stopAsked)
                    return false;
        }
        return true;
    };
    static_assert(rows.size() == 8, "two exit codes, a reform asked or not, a stop asked or not");
    static_assert(everyCombinationOnce(rows), "the eight rows must be eight DISTINCT combinations of the two exit codes");
    for (auto const& row: rows)
    {
        ReformRequest reform;
        if (row.reformAsked)
            reform.RequestReform();
        INFO("exit " << row.exitCode << ", reform asked " << row.reformAsked << ", stop asked " << row.stopAsked);
        auto const outcome = ClassifyBodyEnd(row.exitCode, reform, row.stopAsked);
        CHECK(outcome.end == row.end);
        CHECK(outcome.exitCode == row.exitCode);
        CHECK_FALSE(reform.Pending()); // taken, whatever it decided: the next body starts with none
    }
}

TEST_CASE("A body draining for a stop says what the fixtures wait on, and one draining for a reform says so",
          "[node][formation][loop]")
{
    // The stop's sentence is a marker: `dist-compile-e2e.sh` waits on these bytes.
    CHECK(DrainSentence(false) == "stop requested; no longer accepting compiles");
    CHECK(DrainSentence(true).contains("starting again from its record"));
    CHECK_FALSE(DrainSentence(true).contains("stop requested"));
}

TEST_CASE("Adopting an absent record mints one and saves it before anything reads it", "[node][formation][loop]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver;
    Testing::ScratchDirectory const scratch { "formation-adopt" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    auto random = MintBytes();
    core::platform::ManualWallClock const wall;
    auto cfg = NodeConfig {};

    auto const adopted = AdoptFormation(cfg, store, archiver, endpoints, random, wall);
    INFO((adopted.has_value() ? std::string { "(adopted)" } : adopted.error()));
    REQUIRE(adopted.has_value());
    REQUIRE(cfg.formation.has_value());
    auto const formation = Testing::Unwrap(cfg.formation);
    CHECK(formation.mode == Cluster::NodeMode::Solitary);
    CHECK(formation.clusterId == "abababababababababababababababab");
    REQUIRE(store.Saves().size() == 1);
    CHECK(store.Saves().front().own.clusterId == formation.clusterId);
    CHECK(archiver.Archived().empty());
}

TEST_CASE("Adopting a record a move left half done archives the left store before the body opens it",
          "[node][formation][loop]")
{
    // The other half of the rule a dissolve keeps: a move RECORDS where the node is going and moves no
    // store, and the adoption -- at the start after a crash, and at every reform -- moves it, before
    // any consensus tier opens the directory. A learner never starts over the solitary log it left.
    Testing::InMemoryFormationStore store;
    auto pending = Testing::LearnerIn("c-solo", "c-office");
    pending.archivePending = "c-solo";
    REQUIRE(store.Save(pending).has_value());
    Testing::RecordingArchiver archiver;
    Testing::ScratchDirectory const scratch { "formation-adopt-resume" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    auto random = MintBytes();
    core::platform::ManualWallClock const wall;
    auto cfg = NodeConfig {};
    cfg.nodeId = "n-laptop";

    auto const adopted = AdoptFormation(cfg, store, archiver, endpoints, random, wall);
    INFO((adopted.has_value() ? std::string { "(adopted)" } : adopted.error()));
    REQUIRE(adopted.has_value());
    CHECK(archiver.Archived() == std::vector<std::string> { "c-solo" });
    CHECK_FALSE(Testing::Unwrap(adopted).record.archivePending.has_value());
    REQUIRE(cfg.formation.has_value());
    CHECK(Testing::Unwrap(cfg.formation).mode == Cluster::NodeMode::Learner);
}

TEST_CASE("Adopting a record a move left half done refuses when its store cannot be put away", "[node][formation][loop]")
{
    Testing::InMemoryFormationStore store;
    auto pending = Testing::LearnerIn("c-solo", "c-office");
    pending.archivePending = "c-solo";
    REQUIRE(store.Save(pending).has_value());
    Testing::RecordingArchiver archiver;
    archiver.FailArchives("the staging directory is not writable");
    Testing::ScratchDirectory const scratch { "formation-adopt-resume-refused" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    auto random = MintBytes();
    core::platform::ManualWallClock const wall;
    auto cfg = NodeConfig {};

    auto const adopted = AdoptFormation(cfg, store, archiver, endpoints, random, wall);
    REQUIRE_FALSE(adopted.has_value());
    CHECK(adopted.error().contains("c-solo"));
    CHECK(adopted.error().contains("the staging directory is not writable"));
    CHECK_FALSE(cfg.formation.has_value()); // nothing shaped from a record it will not start on
}

TEST_CASE("Adopting a record this build cannot read refuses and never mints over it", "[node][formation][loop]")
{
    UnreadableFormationStore store;
    Testing::RecordingArchiver archiver;
    Testing::ScratchDirectory const scratch { "formation-adopt-refused" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    auto random = MintBytes();
    core::platform::ManualWallClock const wall;
    auto cfg = NodeConfig {};

    auto const adopted = AdoptFormation(cfg, store, archiver, endpoints, random, wall);
    REQUIRE_FALSE(adopted.has_value());
    CHECK(adopted.error().contains("format 9"));
    CHECK(store.saves.empty());
    CHECK_FALSE(cfg.formation.has_value());
}

TEST_CASE("A stop that arrives between two bodies starts no further one", "[node][formation][loop]")
{
    // The body ended for a reform, and the stop arrived after it was classified -- while it drained, or
    // during the wait. Starting the next body would bind every port and start consensus only to drain.
    CapturingLogger logger;
    Pacing pacing;
    auto runs = 0;
    auto const exit = RunFormationLoop(
        LiveNodeConfig { NodeConfig {}, nullptr },
        [&](NodeConfig const&) {
            // Only the first body reforms, so a loop that ignores the stop runs a second body -- red --
            // rather than reforming forever.
            ++runs;
            pacing.stop = true;
            return BodyOutcome { .end = runs == 1 ? BodyEnd::Reform : BodyEnd::Stopped, .exitCode = Served };
        },
        [](NodeConfig const& cfg) -> std::expected<NodeConfig, std::string> { return cfg; },
        pacing.Controls(),
        logger);
    CHECK(exit == Served);
    CHECK(runs == 1);
    CHECK(std::ranges::any_of(logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.message.contains("a stop arrived between two bodies");
    }));
}

TEST_CASE("Reforms in a row wait their backoff row, and a body that served long starts the run again",
          "[node][formation][loop]")
{
    // A record that flaps between two shapes is paced as a supervisor's start limit paces restarts.
    // Five reforms: the fourth body served `ReformRunEnds`, so the run is 1, 2, 3, then 1, 2 again.
    CapturingLogger logger;
    Pacing pacing;
    auto const served =
        std::array { std::chrono::milliseconds { 0 }, std::chrono::milliseconds { 0 },
                     std::chrono::milliseconds { 0 }, std::chrono::duration_cast<std::chrono::milliseconds>(ReformRunEnds),
                     std::chrono::milliseconds { 0 }, std::chrono::milliseconds { 0 } };
    auto runs = std::size_t { 0 };
    auto const exit = RunFormationLoop(
        LiveNodeConfig { NodeConfig {}, nullptr },
        [&](NodeConfig const&) {
            pacing.clock.advance(served.at(runs));
            ++runs;
            return BodyOutcome { .end = runs < served.size() ? BodyEnd::Reform : BodyEnd::Stopped, .exitCode = Served };
        },
        [](NodeConfig const& cfg) -> std::expected<NodeConfig, std::string> { return cfg; },
        pacing.Controls(),
        logger);
    CHECK(exit == Served);
    CHECK(runs == served.size());
    // Runs 1, 2, 3, 1, 2: the first of a run waits nothing.
    CHECK(pacing.pauses == std::vector { ReformBackoff.at(1), ReformBackoff.at(2), ReformBackoff.at(1) });
    CHECK(std::ranges::count_if(logger.Snapshot(),
                                [](CapturingLogger::Record const& record) {
                                    return record.level == LogLevel::Warn && record.message.contains("reforms in a row");
                                })
          == 3);
}

TEST_CASE("A run of reforms longer than the table waits its last row every time", "[node][formation][loop]")
{
    CapturingLogger logger;
    Pacing pacing;
    auto const reforms = ReformBackoff.size() + 2;
    auto runs = std::size_t { 0 };
    auto const exit = RunFormationLoop(
        LiveNodeConfig { NodeConfig {}, nullptr },
        [&](NodeConfig const&) {
            ++runs;
            return BodyOutcome { .end = runs <= reforms ? BodyEnd::Reform : BodyEnd::Stopped, .exitCode = Served };
        },
        [](NodeConfig const& cfg) -> std::expected<NodeConfig, std::string> { return cfg; },
        pacing.Controls(),
        logger);
    CHECK(exit == Served);
    REQUIRE(pacing.pauses.size() == reforms - 1);
    CHECK(pacing.pauses.back() == ReformBackoff.back());
    CHECK(pacing.pauses.at(pacing.pauses.size() - 2) == ReformBackoff.back());
}

TEST_CASE("Adopting again at a reform refuses a record that is gone and never mints one", "[node][formation][loop]")
{
    // Minting belongs to the first start: the body that just ended was adopted from a record, so one
    // that is gone is lost state. A mint would make the node solitary in a new cluster, over the
    // fleet's store still in the root, as the reload refuses the same state by name.
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver;
    Testing::ScratchDirectory const scratch { "formation-readopt-gone" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    auto cfg = NodeConfig {};

    auto const adopted = ReadoptFormation(cfg, store, archiver, endpoints);
    REQUIRE_FALSE(adopted.has_value());
    CHECK(adopted.error() == FormationRecordGone);
    CHECK(store.Saves().empty());
    CHECK_FALSE(cfg.formation.has_value());
}

namespace
{
/// Where this laptop runs in every serving case: a name its peers can dial, and no worker -- so the
/// startup rules a reform is judged by refuse nothing but what a case sets up.
/// @param record The record the start adopted.
/// @param names This machine's names.
/// @return The configuration the start shaped.
[[nodiscard]] NodeConfig LaptopShapedBy(Cluster::FormationRecord const& record,
                                        NodeHostNames const& names = NodeHostNames {
                                            .fqdn = "laptop.corp.example", .dnsSuffix = "corp.example", .withheld = {} })
{
    auto cfg = NodeConfig {};
    cfg.nodeId = "n-laptop";
    cfg.slots = 0;
    ApplyHostNames(cfg, names);
    REQUIRE(ApplyFormation(cfg, record, {}).has_value());
    return cfg;
}

/// What `main` serves with, on paper: every collaborator a fake a case reads back.
struct ServingRig
{
    /// @return The parts, reading this object; valid for as long as it.
    [[nodiscard]] NodeBodies Parts()
    {
        return NodeBodies { .adoption =
                                ReformAdoption {
                                    .store = store, .archiver = archiver, .endpoints = endpoints, .publisher = &publisher },
                            .reform = reform,
                            .activation = activation,
                            .controls = pacing.Controls() };
    }

    Testing::InMemoryFormationStore store;                    ///< Where the record is kept.
    Testing::RecordingArchiver archiver;                      ///< What every archive was.
    Testing::ScratchDirectory scratch { "node-bodies" };      ///< Where the endpoints file lives.
    Cluster::FleetEndpointsFile endpoints { scratch.Path() }; ///< The remembered endpoints.
    ReformRequest reform;                                     ///< What a body raises to reform.
    RecordingDescriptors descriptors;                         ///< Every copy and close.
    ActivationHold activation { 3, descriptors };             ///< What a supervisor handed over.
    Testing::RecordingPublisher publisher;                    ///< Every configuration published.
    Pacing pacing;                                            ///< The stop flag and every pause.
    CapturingLogger logger;                                   ///< What the loop said.
};

/// What one body was run with.
struct BodyRun
{
    Cluster::NodeMode shapedMode; ///< The mode its configuration was shaped in.
    Cluster::NodeMode recordMode; ///< The mode of the record it was handed.
    std::string recordCluster;    ///< The cluster that record names as its own.
    std::optional<int> served;    ///< The socket it served.
    std::size_t archivedBefore;   ///< How many stores had been archived when it started.
    std::size_t publishedBefore;  ///< How many configurations had been published when it started.
};
} // namespace

TEST_CASE("The first body runs by the start's adoption, and every reform adopts, publishes and serves its own socket",
          "[node][formation][loop][serving]")
{
    // The production path, which `main` only hands a body to: the start adopted the solitary record and
    // judged it; the body made a move -- a learner record naming the solitary store still in the root,
    // saved as the beat thread saves one -- and asked for a reform. The reform adopts it AGAIN, which
    // archives that store before the learner body runs, publishes what the learner runs by, and hands
    // the learner its own copy of the supervisor's socket.
    ServingRig rig;
    auto running = AdoptedFormation { .record = Testing::Minted("c-solo", 500), .remembered = {} };
    auto const base = LaptopShapedBy(running.record);
    auto runs = std::vector<BodyRun> {};

    auto const exit = RunNodeBodies(
        LiveNodeConfig { base, nullptr },
        running,
        rig.Parts(),
        [&](NodeConfig const& cfg, Cluster::FormationRecord const& record, std::optional<int> served) {
            REQUIRE(cfg.formation.has_value());
            runs.push_back(BodyRun { .shapedMode = cfg.formation->mode,
                                     .recordMode = record.mode,
                                     .recordCluster = record.own.clusterId,
                                     .served = served,
                                     .archivedBefore = rig.archiver.Archived().size(),
                                     .publishedBefore = rig.publisher.published.size() });
            if (runs.size() == 1)
            {
                auto moved = Testing::LearnerIn("c-solo", "c-office");
                moved.archivePending = "c-solo";
                REQUIRE(rig.store.Save(moved).has_value());
                rig.reform.RequestReform();
            }
            return Served;
        },
        rig.logger);

    CHECK(exit == Served);
    REQUIRE(runs.size() == 2);

    // The first body: the start's adoption, untouched -- nothing archived, nothing published.
    CHECK(runs[0].shapedMode == Cluster::NodeMode::Solitary);
    CHECK(runs[0].recordMode == Cluster::NodeMode::Solitary);
    CHECK(runs[0].archivedBefore == 0);
    CHECK(runs[0].publishedBefore == 0);

    // The reformed body: adopted again, its left store archived BEFORE it ran, and published.
    CHECK(runs[1].shapedMode == Cluster::NodeMode::Learner);
    CHECK(runs[1].recordMode == Cluster::NodeMode::Learner);
    CHECK(runs[1].archivedBefore == 1);
    CHECK(rig.archiver.Archived() == std::vector<std::string> { "c-solo" });
    REQUIRE(rig.publisher.published.size() == 1);
    CHECK(Testing::Unwrap(rig.publisher.published[0].formation).mode == Cluster::NodeMode::Learner);
    CHECK(running.record.mode == Cluster::NodeMode::Learner); // what a reload now shapes by

    // Each body its OWN copy of the supervisor's socket: the first body's listener closed its own.
    CHECK(runs[0].served == std::optional { 101 });
    CHECK(runs[1].served == std::optional { 102 });
    CHECK(rig.descriptors.copiedFrom == std::vector<int> { 3, 3 });
}

TEST_CASE("A body that refused while a reform was pending ends the node, and no further body starts",
          "[node][formation][loop][serving]")
{
    ServingRig rig;
    auto running = AdoptedFormation { .record = Testing::Minted("c-solo", 500), .remembered = {} };
    auto runs = 0;
    auto const exit = RunNodeBodies(
        LiveNodeConfig { LaptopShapedBy(running.record), nullptr },
        running,
        rig.Parts(),
        [&](NodeConfig const&, Cluster::FormationRecord const&, std::optional<int>) {
            ++runs;
            if (runs == 1) // only the first, so a refusal read as a reform ends red rather than looping
                rig.reform.RequestReform();
            return FormationRefused;
        },
        rig.logger);
    CHECK(exit == FormationRefused);
    CHECK(runs == 1);
    CHECK_FALSE(rig.reform.Pending());
    CHECK(rig.publisher.published.empty());
}

TEST_CASE("A reform into a shape the startup rules refuse is refused by name before the body runs",
          "[node][formation][loop][serving]")
{
    // The reform's OWN judge, for a record no move of this build writes: every move is judged before
    // its record is written (`StartupShapeJudge`), so a VOTER record for a founder whose name reaches
    // only this machine arrives here only as a crash or another build left it -- planted below. A
    // restart refuses that shape by name; so must the reform, or it serves an address no peer can
    // reach and every reload until the next restart is declined.
    ServingRig rig;
    auto running = AdoptedFormation { .record = Testing::Minted("c-solo", 500), .remembered = {} };
    auto const withheld = NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = "localhost" };
    auto const base = LaptopShapedBy(running.record, withheld);
    REQUIRE_FALSE(StartupPolicyRejection(base).has_value()); // the start passed it

    auto runs = 0;
    auto const exit = RunNodeBodies(
        LiveNodeConfig { base, nullptr },
        running,
        rig.Parts(),
        [&](NodeConfig const&, Cluster::FormationRecord const&, std::optional<int>) {
            ++runs;
            if (runs > 1) // a body the rules should have refused: red, and the loop ends
                return Served;
            auto voter = Testing::Minted("c-solo", 500);
            voter.mode = Cluster::NodeMode::Voter;
            REQUIRE(rig.store.Save(voter).has_value());
            rig.reform.RequestReform();
            return Served;
        },
        rig.logger);

    CHECK(exit == FormationRefused);
    CHECK(runs == 1);
    CHECK(rig.publisher.published.empty());
    CHECK(std::ranges::any_of(rig.logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains(ConsensusNameReachesOnlyThisMachineRefusal);
    }));
}

TEST_CASE("A reform that finds the record gone refuses by name and mints no cluster", "[node][formation][loop][serving]")
{
    ServingRig rig;
    auto running = AdoptedFormation { .record = Testing::Minted("c-solo", 500), .remembered = {} };
    auto runs = 0;
    auto const exit = RunNodeBodies(
        LiveNodeConfig { LaptopShapedBy(running.record), nullptr },
        running,
        rig.Parts(),
        [&](NodeConfig const&, Cluster::FormationRecord const&, std::optional<int>) {
            ++runs;        // the store holds nothing: the record this body ran by has vanished
            if (runs == 1) // a second body is the defect: red, and the loop ends
                rig.reform.RequestReform();
            return Served;
        },
        rig.logger);
    CHECK(exit == FormationRefused);
    CHECK(runs == 1);
    CHECK(rig.store.Saves().empty()); // nothing minted
    CHECK(running.record.own.clusterId == "c-solo");
    CHECK(std::ranges::any_of(rig.logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains(FormationRecordGone);
    }));
}

TEST_CASE("A reload shapes by the record the running body was adopted from, never one saved after it",
          "[node][formation][loop][serving][reload]")
{
    // A move saves its record on the beat thread before the body it ends has drained, so the file can
    // be AHEAD of the body. A reload during that window must still describe the body that runs.
    ServingRig rig;
    auto running = AdoptedFormation { .record = Testing::Minted("c-solo", 500), .remembered = {} };
    auto const basis = RunningFormationOf(running);
    auto seen = std::vector<Cluster::NodeMode> {};

    auto const exit = RunNodeBodies(
        LiveNodeConfig { LaptopShapedBy(running.record), nullptr },
        running,
        rig.Parts(),
        [&](NodeConfig const&, Cluster::FormationRecord const&, std::optional<int>) {
            if (seen.empty())
            {
                REQUIRE(rig.store.Save(Testing::LearnerIn("c-solo", "c-office")).has_value());
                rig.reform.RequestReform();
            }
            else
            {
                // Saved after this body was adopted, as a later move would be.
                auto ahead = Testing::Minted("c-new", 900);
                REQUIRE(rig.store.Save(ahead).has_value());
            }
            auto const kept = basis();
            REQUIRE(kept.has_value());
            REQUIRE(kept->record.has_value());
            seen.push_back(kept->record->mode);
            return Served;
        },
        rig.logger);
    CHECK(exit == Served);
    CHECK(seen == std::vector { Cluster::NodeMode::Solitary, Cluster::NodeMode::Learner });
}

TEST_CASE("The reloader publishes what a reformed body runs by", "[node][formation][loop][reload]")
{
    auto initial = NodeConfig {};
    initial.nodeId = "n-started";
    NodeReloader reloader { initial,
                            std::filesystem::path { "unread.yaml" },
                            [](std::filesystem::path const&) -> std::expected<NodeConfig, ConfigError> {
                                return NodeConfig {};
                            },
                            &ValidateNodeReloadable };
    ReloaderPublisher publisher { reloader };
    auto reformed = initial;
    reformed.nodeId = "n-reformed";
    publisher.Publish(reformed);
    CHECK(reloader.Current()->nodeId == "n-reformed");
}

namespace
{
/// A node with a configuration file, as `main` builds one: the reloader holding what the start shaped,
/// its "file" a candidate a case writes, the publisher a reform tells, and the source every body is
/// adopted from.
struct ReloadingRig
{
    /// @param started What the start shaped.
    explicit ReloadingRig(NodeConfig const& started):
        start { started },
        file { started },
        reloader { started,
                   std::filesystem::path { "fastcache-compile-node.yaml" },
                   [this](std::filesystem::path const&) -> std::expected<NodeConfig, ConfigError> { return file; },
                   &ValidateNodeReloadable },
        publisher { reloader },
        inForce { start, &reloader }
    {
    }

    /// @return The parts, reading this rig and @p serving; valid for as long as both.
    /// @param serving Everything else a body is run with.
    [[nodiscard]] NodeBodies Parts(ServingRig& serving)
    {
        auto parts = serving.Parts();
        parts.adoption.publisher = &publisher;
        return parts;
    }

    NodeConfig start;            ///< What the start shaped, which no reload changes.
    NodeConfig file;             ///< What the next reload reads.
    NodeReloader reloader;       ///< The live configuration.
    ReloaderPublisher publisher; ///< What a reform publishes into.
    LiveNodeConfig inForce;      ///< What every body is adopted from.
};
} // namespace

TEST_CASE("A reform keeps a --fleet-member entry an accepted reload removed", "[node][formation][loop][reload]")
{
    // REMOVAL is the direction an admission path must get right: an operator turns `--fleet-open` off
    // and reloads, and the next move that reforms -- here a yield into a learner -- must not re-admit
    // a stranger by shaping the new body from the configuration the process STARTED with.
    ServingRig rig;
    auto running = AdoptedFormation { .record = Testing::Minted("c-solo", 500), .remembered = {} };
    auto started = LaptopShapedBy(running.record);
    started.fleetOpen = true;
    ReloadingRig node { started };
    auto admitted = std::vector<Distributed::Membership> {};

    auto const exit = RunNodeBodies(
        node.inForce,
        running,
        node.Parts(rig),
        [&](NodeConfig const& shaped, Cluster::FormationRecord const&, std::optional<int>) {
            // How a body answers "is this peer one of ours": a membership built from what it runs by.
            NodeMembership const membership { shaped, rig.logger };
            admitted.push_back(membership.Classify("10.9.0.8"));
            if (admitted.size() == 1)
            {
                node.file.fleetOpen = false;
                REQUIRE(node.reloader.Reload().has_value());
                REQUIRE(rig.store.Save(Testing::LearnerIn("c-solo", "c-office")).has_value());
                rig.reform.RequestReform();
            }
            return Served;
        },
        rig.logger);

    CHECK(exit == Served);
    REQUIRE(admitted.size() == 2);
    CHECK(admitted[0] == Distributed::Membership::Member); // open when the first body started
    CHECK(admitted[1] == Distributed::Membership::Outsider);
    auto const live = node.reloader.Current();
    CHECK_FALSE(live->fleetOpen);
    CHECK(Testing::Unwrap(live->formation).mode == Cluster::NodeMode::Learner); // and it IS the reform's
}

TEST_CASE("A reform keeps a --requirepass an accepted reload rotated", "[node][formation][loop][reload]")
{
    // The credential this node presents is read from the reloader (`ConfiguredCredential`); a reform
    // that published the start's configuration put the rotated-out secret back in force.
    ServingRig rig;
    auto running = AdoptedFormation { .record = Testing::Minted("c-solo", 500), .remembered = {} };
    auto started = LaptopShapedBy(running.record);
    started.requirePass = "old-secret";
    ReloadingRig node { started };
    auto presented = std::vector<std::string> {};

    auto const exit = RunNodeBodies(
        node.inForce,
        running,
        node.Parts(rig),
        [&](NodeConfig const& shaped, Cluster::FormationRecord const&, std::optional<int>) {
            ConfiguredCredential const credential { shaped, &node.reloader };
            if (presented.empty())
            {
                node.file.requirePass = "new-secret";
                REQUIRE(node.reloader.Reload().has_value());
                REQUIRE(rig.store.Save(Testing::LearnerIn("c-solo", "c-office")).has_value());
                rig.reform.RequestReform();
            }
            presented.emplace_back(credential.Current().secret.View());
            return Served;
        },
        rig.logger);

    CHECK(exit == Served);
    CHECK(presented == std::vector<std::string> { "new-secret", "new-secret" });
    CHECK(node.reloader.Current()->requirePass.View() == "new-secret");
}
