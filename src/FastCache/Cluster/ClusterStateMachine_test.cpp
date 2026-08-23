// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterStateMachine.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
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

    watched.machine.Apply(Entry(1, Command { .kind = CommandKind::AddMember, .key = "n1", .value = "10.0.0.1:6675" }));
    watched.machine.Apply(Entry(2, Command { .kind = CommandKind::AddMember, .key = "n2", .value = "10.0.0.2:6675" }));

    CHECK(watched.machine.State().members.size() == 2);
    CHECK(Unwrap(watched.machine.State().EndpointOf("n2")) == "10.0.0.2:6675");

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
        Command { .kind = CommandKind::AddMember, .key = "n1", .value = "10.0.0.1:6675" },
        Command { .kind = CommandKind::SetSetting, .key = "upstream", .value = "cache.internal:6674" },
        Command { .kind = CommandKind::AddMember, .key = "n2", .value = "10.0.0.2:6675" },
        Command { .kind = CommandKind::RemoveMember, .key = "n1", .value = {} },
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
    machine.Apply(Entry(2, Command { .kind = CommandKind::AddMember, .key = "n1", .value = "10.0.0.1:6675" }));
    CHECK(machine.State().members.size() == 1);
}

TEST_CASE("A snapshot round-trips through the machine", "[cluster][statemachine]")
{
    Watched source;
    source.machine.Apply(Entry(1, Command { .kind = CommandKind::AddMember, .key = "n1", .value = "10.0.0.1:6675" }));
    source.machine.Apply(Entry(2, Command { .kind = CommandKind::SetSetting, .key = "fleet-open", .value = "1" }));

    Watched restored;
    restored.machine.RestoreSnapshot(source.machine.TakeSnapshot());

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
    watched.machine.Apply(Entry(1, Command { .kind = CommandKind::AddMember, .key = "n1", .value = "10.0.0.1:6675" }));
    watched.machine.Apply(Entry(2, Command { .kind = CommandKind::AddMember, .key = "n2", .value = "10.0.0.2:6675" }));

    SECTION("replace, never merge")
    {
        // A snapshot is the complete state as of its index. Folding it into what this
        // machine already holds would keep members the cluster has since removed --
        // which for a membership set means counting a node that is gone towards
        // quorum, and a quorum counted over the wrong set is two leaders.
        ClusterState smaller;
        Apply(smaller, Command { .kind = CommandKind::AddMember, .key = "n3", .value = "10.0.0.3:6675" });

        watched.machine.RestoreSnapshot(Encode(smaller));
        REQUIRE(watched.machine.State().members.size() == 1);
        CHECK(watched.machine.State().members.front().id == "n3");
    }

    SECTION("an unreadable snapshot leaves the state alone")
    {
        // NOT cleared. Replacing what this node holds with nothing would turn "I
        // cannot read your state" into "the cluster has no members", after which this
        // node would refuse every peer it had been serving a moment earlier.
        auto const before = watched.machine.State();
        watched.machine.RestoreSnapshot(std::vector<std::byte>(5, std::byte { 0x01 }));

        CHECK(watched.machine.State() == before);
    }
}
