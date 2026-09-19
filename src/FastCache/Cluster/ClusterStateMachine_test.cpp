// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterStateMachine.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/PreviousClusterState.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
/// A command, spelled once so a field added to `Command` lands in one place.
///
/// The alternative -- a literal per case -- is what this file had, and adding the
/// scheduler endpoint made every one of them a compile error under
/// `-Wmissing-designated-field-initializers`. That diagnostic is doing its job:
/// each of those literals WOULD have silently defaulted a field it never
/// mentioned.
/// @param kind What it does.
/// @param key The member id or setting name.
/// @param value The consensus endpoint or setting value.
/// @param scheduler Where clients reach this member while it leads.
/// @return The command.
[[nodiscard]] Command Cmd(CommandKind kind, std::string key, std::string value = {}, std::string scheduler = {})
{
    return Command { .kind = kind,
                     .key = std::move(key),
                     .value = std::move(value),
                     .schedulerEndpoint = std::move(scheduler),
                     .publicKey = std::nullopt,
                     .role = std::nullopt };
}

/// One applied entry carrying a command.
/// @param index Where it sits in the log.
/// @param command The change.
/// @return The entry a driver would hand to the machine.
[[nodiscard]] Consensus::AppliedEntry Entry(std::uint64_t index, Command const& command)
{
    return Consensus::AppliedEntry { .index = Consensus::LogIndex { .value = index }, .payload = Encode(command) };
}

/// A machine that records what it was told, so the observer can be asserted on.
struct Watched
{
    NullLogger logger;
    std::vector<ClusterState> published;
    ClusterStateMachine machine { logger, [this](ClusterState const& state) { published.push_back(state); } };
};
} // namespace

TEST_CASE("Committed entries become the cluster's state", "[cluster][statemachine]")
{
    Watched watched;

    watched.machine.Apply(Entry(1, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")));
    watched.machine.Apply(Entry(2, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675")));

    CHECK(watched.machine.State().members.size() == 2);
    CHECK(Unwrap(watched.machine.State().RaftEndpointOf("n2")) == "10.0.0.2:6675");

    // The observer fires on every change rather than on a timer, because the window
    // between "the cluster agreed" and "this node acts on it" is a window in which
    // this node refuses a peer it has already admitted.
    REQUIRE(watched.published.size() == 2);
    CHECK(watched.published.back().Endpoints() == std::vector<std::string> { "10.0.0.1:6675", "10.0.0.2:6675" });
}

TEST_CASE("Re-applying a prefix reaches the same state", "[cluster][statemachine]")
{
    // `IRaftStateMachine` documents that a recovered node re-applies from the start
    // of whatever log it holds, because the commit index is not durable. This machine
    // has to be idempotent under that, and it is by construction -- every command is
    // a set-or-replace -- but "by construction" is a claim worth an assertion.
    Watched first;
    Watched replayed;

    std::vector const log {
        Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"),
        Cmd(CommandKind::SetSetting, "lease-lifetime", "20min"),
        Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"),
        Cmd(CommandKind::Forget, "n1"),
    };

    std::uint64_t index = 0;
    for (auto const& command: log)
        first.machine.Apply(Entry(++index, command));

    // The whole log again, from the beginning, into a fresh machine.
    index = 0;
    for (auto const& command: log)
        replayed.machine.Apply(Entry(++index, command));

    CHECK(first.machine.State() == replayed.machine.State());
    CHECK(first.machine.State().members.size() == 1);
}

TEST_CASE("An entry this build cannot decode is skipped, not fatal", "[cluster][statemachine]")
{
    // An entry reaches here only after it is COMMITTED -- it is already in every
    // future leader's log and there is nobody left to refuse it to. Stopping would
    // mean this node alone stopped following a cluster the rest of which carried on,
    // which is a partition it created for itself.
    CapturingLogger logger;
    ClusterStateMachine machine { logger, {} };

    machine.Apply(Consensus::AppliedEntry { .index = Consensus::LogIndex { .value = 1 },
                                            .payload = std::vector<std::byte>(3, std::byte { 0xEE }) });
    CHECK(machine.State().members.empty());

    // Said out loud, because a state machine that silently ignored entries would
    // diverge from its peers with nothing anywhere reporting it.
    auto const records = logger.Snapshot();
    CHECK(std::ranges::any_of(records, [](auto const& record) { return record.message.contains("cannot decode"); }));

    // And the ordering is intact: the next entry applies normally.
    machine.Apply(Entry(2, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")));
    CHECK(machine.State().members.size() == 1);
}

TEST_CASE("A committed verb this build does not know is skipped by name, and the log goes on applying",
          "[cluster][statemachine][forget]")
{
    // #1309 added two verbs without moving `CommandVersion`, so a member running an older
    // build DECODES the layout and meets a verb byte it lacks. That is this case: the
    // first byte past `Last` is exactly what `AdmitClient` is to a build that predates it,
    // and the byte pins in `ClusterState_test.cpp` hold the two in step.
    //
    // What it must not do is crash, wedge, or apply the byte as whichever verb it aliases.
    // Skipping leaves such a member without the change -- no replicated client admitted,
    // and a client forget ignored -- and the rest of the log still applies around it.
    CapturingLogger logger;
    std::vector<ClusterState> published;
    ClusterStateMachine machine { logger, [&published](ClusterState const& state) { published.push_back(state); } };

    auto payload = Encode(Cmd(CommandKind::SetSetting, "fleet-open", "1"));
    // The verb sits in the second byte of the first field, after that field's u32 length
    // prefix.
    REQUIRE(payload.size() > 5);
    auto const unknownVerb = static_cast<unsigned>(CommandKind::Last);
    payload[5] = static_cast<std::byte>(unknownVerb);
    machine.Apply(Consensus::AppliedEntry { .index = Consensus::LogIndex { .value = 1 }, .payload = payload });

    CHECK(machine.State() == ClusterState {});
    CHECK(published.empty());

    // Named: the index, and the verb byte, so an operator reading the log of a member left
    // behind by an upgrade learns which entry it did not apply and why.
    auto const records = logger.Snapshot();
    CHECK(std::ranges::any_of(records, [unknownVerb](auto const& record) {
        return record.message.contains("entry 1")
               && record.message.contains(std::format("verb {} this build does not know", unknownVerb));
    }));

    machine.Apply(Entry(2, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")));
    machine.Apply(Entry(3, Cmd(CommandKind::SetSetting, "fleet-open", "1")));
    CHECK(machine.State().members.size() == 1);
    CHECK(machine.State().settings.size() == 1);
    CHECK(published.size() == 2);
}

TEST_CASE("A snapshot round-trips through the machine", "[cluster][statemachine]")
{
    Watched source;
    source.machine.Apply(Entry(1, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")));
    source.machine.Apply(Entry(2, Cmd(CommandKind::SetSetting, "fleet-open", "1")));

    Watched restored;
    REQUIRE(
        restored.machine.RestoreSnapshot(source.machine.TakeSnapshot(), Consensus::SnapshotOrigin::Installed).has_value());

    CHECK(restored.machine.State() == source.machine.State());

    // Restoring publishes too. A follower handed state it could not replay its way to
    // must tell the surfaces about it, or it would serve the membership it had before
    // the snapshot until the next unrelated change happened to arrive.
    REQUIRE(restored.published.size() == 1);
    CHECK(restored.published.back().members.size() == 1);
}

TEST_CASE("A snapshot replaces, and an unreadable one changes nothing", "[cluster][statemachine]")
{
    Watched watched;
    watched.machine.Apply(Entry(1, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")));
    watched.machine.Apply(Entry(2, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675")));

    SECTION("replace, never merge")
    {
        // A snapshot is the complete state as of its index. Folding it into what this
        // machine already holds would keep members the cluster has since removed --
        // which for a membership set means counting a node that is gone towards
        // quorum, and a quorum counted over the wrong set is two leaders.
        ClusterState smaller;
        Apply(smaller, Cmd(CommandKind::AddMember, "n3", "10.0.0.3:6675"));

        REQUIRE(watched.machine.RestoreSnapshot(Encode(smaller), Consensus::SnapshotOrigin::Installed).has_value());
        REQUIRE(watched.machine.State().members.size() == 1);
        CHECK(watched.machine.State().members.front().id == "n3");
    }

    SECTION("an unreadable snapshot leaves the state alone")
    {
        // NOT cleared. Replacing what this node holds with nothing would turn "I
        // cannot read your state" into "the cluster has no members", after which this
        // node would refuse every peer it had been serving a moment earlier.
        auto const before = watched.machine.State();
        auto const published = watched.published.size();
        auto const refused = watched.machine.RestoreSnapshot(std::vector<std::byte>(5, std::byte { 0x01 }),
                                                             Consensus::SnapshotOrigin::Installed);

        // Refused as DAMAGE: bytes no build wrote as a state -- the one refusal that is.
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::StorageFailure);
        CHECK(watched.machine.State() == before);
        CHECK(watched.published.size() == published);
    }
}

TEST_CASE("A snapshot another build encoded is refused by its version and changes nothing", "[cluster][statemachine]")
{
    // The unreadable-snapshot rule above, for the cause an upgrade produces. Kept rather
    // than cleared for the same reason, and the log line names the version: *cannot
    // decode* alone reads as damage, and sends an operator after a snapshot that is intact.
    CapturingLogger logger;
    ClusterStateMachine machine { logger, {} };
    machine.Apply(Entry(1, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")));
    auto const before = machine.State();

    ClusterState other;
    Apply(other, Cmd(CommandKind::AddMember, "n3", "10.0.0.3:6675"));
    auto snapshot = Encode(other);
    // The state's version is the first field's only byte, after its u32 length prefix.
    REQUIRE(snapshot.size() > 4);
    snapshot[4] = std::byte { 2 };
    auto const refused = machine.RestoreSnapshot(snapshot, Consensus::SnapshotOrigin::Installed);

    // Another build's, so the storage rule's code and never the damage one (#1542).
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
    CHECK(machine.State() == before);
    auto const records = logger.Snapshot();
    CHECK(std::ranges::any_of(records, [](auto const& record) {
        return record.message.contains("cannot decode") && record.message.contains("version 2");
    }));
}

TEST_CASE("A snapshot the previous build wrote, in its own layout, is refused by its version and changes nothing",
          "[cluster][statemachine][identity]")
{
    // #178. The case above flips one byte of a CURRENT encoding, which a decoder judging the
    // arity first would still call another build's; this one is the previous build's LAYOUT,
    // which such a decoder calls damage. What a follower does with it: keeps its state, and
    // names both versions -- an upgrade in progress, never a snapshot to delete.
    //
    // A follower handed the snapshot by a leader, and ONLY that. A node restarting on a
    // snapshot of its own at the previous version refuses to start; that is the case below.
    CapturingLogger logger;
    ClusterStateMachine machine { logger, {} };
    machine.Apply(Entry(1, Cmd(CommandKind::AddMember, "n9", "10.0.0.9:6675")));
    auto const before = machine.State();

    auto const refused =
        machine.RestoreSnapshot(Testing::EncodePreviousClusterState(), Consensus::SnapshotOrigin::Installed);

    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
    CHECK(machine.State() == before);
    auto const records = logger.Snapshot();
    CHECK(std::ranges::any_of(records, [](auto const& record) {
        return record.message.contains("cannot decode")
               && record.message.contains(std::format("version {}", Testing::PreviousClusterStateVersion))
               && record.message.contains("reads 7");
    }));
}

TEST_CASE("A snapshot that cannot be decoded is named for where it came from, and so is what happens next",
          "[cluster][statemachine]")
{
    // #1542. The line used to say a snapshot "arrived", which reads right for a leader's
    // and wrong for a node's own: that one did not arrive, it was recovered, and the node
    // does not keep its current state -- it has none, and it will not start. Each origin
    // is asserted to name ITSELF and not the other, so a line that said both passes neither.
    //
    // Each section's first phrase is a REQUIRE. A line naming the wrong origin fails the
    // four checks below it together, and a Catch2 binary's exit status is its failed
    // assertion count -- four is `SKIP_RETURN_CODE`, so that defect was scored SKIPPED,
    // not failed (#1152). Measured, on this case, by neutering the origin out of the line.
    CapturingLogger logger;
    ClusterStateMachine machine { logger, {} };

    auto const said = [&logger](std::string_view phrase) {
        auto const records = logger.Snapshot();
        return std::ranges::any_of(records, [phrase](auto const& record) { return record.message.contains(phrase); });
    };

    SECTION("recovered")
    {
        auto const refused =
            machine.RestoreSnapshot(Testing::EncodePreviousClusterState(), Consensus::SnapshotOrigin::Recovered);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
        REQUIRE(said("the snapshot this node recovered from its own storage"));
        CHECK(said("this node will not start on it"));
        CHECK_FALSE(said("installed"));
        CHECK_FALSE(said("keeping current state"));
    }

    SECTION("installed")
    {
        auto const refused =
            machine.RestoreSnapshot(Testing::EncodePreviousClusterState(), Consensus::SnapshotOrigin::Installed);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(said("a snapshot the leader installed"));
        CHECK(said("keeping current state"));
        CHECK_FALSE(said("recovered"));
        CHECK_FALSE(said("will not start"));
    }
}

TEST_CASE("Whether a command can be applied is asked without applying it, in the two codes held state is refused with",
          "[cluster][statemachine]")
{
    // What recovery asks of every command a node's own log holds (#1542): a question with
    // no effects, answered in the storage rule's codes rather than the peer wire's.
    Watched watched;

    SECTION("this build's command is readable, and asking changes nothing")
    {
        CHECK(watched.machine.CanRead(Encode(Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"))).has_value());
        CHECK(watched.machine.State().members.empty());
        CHECK(watched.published.empty());
    }

    SECTION("the previous build's command is another build's format, both versions named")
    {
        auto const refused = watched.machine.CanRead(Testing::EncodePreviousClusterCommand());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
        CHECK(refused.error().context.contains(std::format("version {}", Testing::PreviousClusterCommandVersion)));
        CHECK(refused.error().context.contains("reads 3"));
    }

    SECTION("bytes that are no command at all are damage")
    {
        auto const refused = watched.machine.CanRead(std::vector<std::byte>(3, std::byte { 0x01 }));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::StorageFailure);
    }
}

TEST_CASE("Whether a snapshot can be restored is asked without restoring it, in the two codes held state is refused with",
          "[cluster][statemachine]")
{
    // What the driver asks of every snapshot a leader offers, before the node takes it on
    // (#1552): a question with no effects, answered exactly as `RestoreSnapshot` would be.
    Watched watched;
    watched.machine.Apply(Entry(1, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")));
    auto const before = watched.machine.State();
    auto const published = watched.published.size();

    ClusterState other;
    Apply(other, Cmd(CommandKind::AddMember, "n3", "10.0.0.3:6675"));

    SECTION("this build's state is restorable, and asking changes nothing")
    {
        CHECK(watched.machine.CanRestore(Encode(other)).has_value());
    }

    SECTION("the previous build's state is another build's format, both versions named")
    {
        auto const refused = watched.machine.CanRestore(Testing::EncodePreviousClusterState());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
        CHECK(refused.error().context.contains(std::format("version {}", Testing::PreviousClusterStateVersion)));
        CHECK(refused.error().context.contains("reads 7"));
    }

    SECTION("bytes that are no state at all are damage")
    {
        auto const refused = watched.machine.CanRestore(std::vector<std::byte>(5, std::byte { 0x01 }));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::StorageFailure);
    }

    CHECK(watched.machine.State() == before);
    CHECK(watched.published.size() == published);
}
