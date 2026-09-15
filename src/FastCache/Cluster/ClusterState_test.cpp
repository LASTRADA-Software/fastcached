// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
/// A command, spelled once so the cases below vary only what they are about.
/// @param kind What it does.
/// @param key The member id or setting name.
/// @param value The consensus endpoint or setting value.
/// @param scheduler Where clients reach this member while it leads.
/// @return The command.
[[nodiscard]] Command Cmd(CommandKind kind, std::string key, std::string value = {}, std::string scheduler = {})
{
    return Command {
        .kind = kind, .key = std::move(key), .value = std::move(value), .schedulerEndpoint = std::move(scheduler)
    };
}
/// Why `command` may not be proposed, having required that it may not.
/// @param command The change.
/// @return The refusal's context, which is what an operator reads.
[[nodiscard]] std::string Refused(Command const& command)
{
    auto const answer = Validate(command);
    REQUIRE_FALSE(answer.has_value());
    return answer.error().context;
}
} // namespace

TEST_CASE("A member is admitted with an endpoint, never without one", "[cluster][state]")
{
    // The residual `RaftMembership` recorded, closed here. Consensus carries ids
    // alone -- correct, since an id is all a quorum count needs -- which left a node
    // the cluster had agreed to admit unreachable until something else supplied its
    // address. Admitting and addressing are one decision now, replicated together.
    CHECK(Validate(Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")).has_value());

    // Refused at the PROPOSER, which is the only place a change can be refused: an
    // entry is applied after it is committed, so by then there is nobody left to
    // report to and no way to un-commit it.
    CHECK(Refused(Cmd(CommandKind::AddMember, "n1")).contains("endpoint"));

    CHECK_FALSE(Validate(Cmd(CommandKind::AddMember, "", "10.0.0.1:6675")).has_value());
}

TEST_CASE("A member the cluster records has to be one it can name", "[cluster][state]")
{
    // #159. Everything `AddMember` records becomes a `ClusterMember` and is read back
    // out as TEXT -- by `/fleet.json`, which RFC 8259 requires to be UTF-8; by the
    // fleet page, which embeds an SVG, which is XML; by `--cluster-status`; and by
    // the logs. The encoders repair what reaches them, deliberately and as a last
    // resort, but a leader whose state holds bytes nobody can name has a member
    // nobody can name.
    // All three, and the field is named because they send an operator to three
    // different places: an id is typed into `--cluster-admit`, a consensus endpoint
    // into `--raft-peer`, and a scheduler endpoint is announced by the member itself.
    CHECK(Refused(Cmd(CommandKind::AddMember, "n\x80", "10.0.0.1:6675")).contains("a member id"));
    CHECK(Refused(Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675\xE2\x82")).contains("consensus endpoint"));
    CHECK(Refused(Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675", "10.0.0.1:7000\xFF")).contains("scheduler endpoint"));

    // And a setting's value, which is free-form and therefore the likeliest of the
    // lot to carry something surprising. Its NAME needs no rule of its own: a key
    // this build does not know is already refused whatever its bytes are.
    CHECK(Refused(Cmd(CommandKind::SetSetting, "lease-lifetime", "600000\x80")).contains("setting's value"));

    // Encoding, not ASCII -- an id is opaque to consensus, which matches it byte for
    // byte.
    CHECK(Validate(Cmd(CommandKind::AddMember, "arbeiter-\xC3\xA9\xE2\x82\xAC", "b\xC3\xBCro.example:6675")).has_value());
}

TEST_CASE("A member that cannot be named can still be forgotten", "[cluster][state]")
{
    // The trap #159 exists to record, pinned open. `Validate` governs every verb, so
    // a rule applied to all of them alike would also govern `RemoveMember` -- whose
    // key IS the offending id. A member that reached replicated state through a peer
    // built before any of this existed would then count towards quorum forever,
    // refused by the very check meant to keep it out, with `--cluster-forget` the
    // one thing that could have removed it.
    //
    // So the answer is a property of the VERB, and this is the case that says so.
    CHECK(Validate(Cmd(CommandKind::RemoveMember, "n\x80")).has_value());

    // Which is only useful if it does what it says: the state has to lose it.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n\x80", "10.0.0.1:6675"));
    REQUIRE(state.members.size() == 1);
    Apply(state, Cmd(CommandKind::RemoveMember, "n\x80"));
    CHECK(state.members.empty());
}

TEST_CASE("A setting this build does not know is refused, not stored", "[cluster][state]")
{
    // A key nobody knows would otherwise be replicated to every node, snapshotted and
    // carried across restarts while doing nothing -- and the only symptom would be
    // that the thing the operator configured did not happen.
    CHECK(Validate(Cmd(CommandKind::SetSetting, "lease-lifetime", "20min")).has_value());
    CHECK(Validate(Cmd(CommandKind::SetSetting, "fleet-open", "1")).has_value());

    CHECK(Refused(Cmd(CommandKind::SetSetting, "upsteam", "typo")).contains("upsteam"));

    // The table is the only source of truth for what is a setting, so a local flag
    // that describes ONE machine is not one -- replicating `--slots` would impose one
    // host's size on all of them.
    CHECK(FindSetting("slots") == nullptr);
}

namespace ConsumerGuard
{
// Synthetic tables, so the guard can be watched REFUSING and ACCEPTING. They are at
// namespace scope because `RowsCarryAConsumer` is `consteval`: a span over a block
// local is not a constant expression, and a runtime call is not available to fall
// back on -- deliberately, since a runtime `CHECK` of a `consteval` predicate cannot
// fail in a translation unit that compiled and would read as a guarantee while
// asserting nothing.

/// The omission the guard exists for: a row that says nothing about who reads it.
constexpr std::array<SettingSpec, 1> SaysNothing { SettingSpec { .name = "x", .summary = "y" } };

/// A row claiming BOTH that something reads it and that nothing does.
constexpr std::array<SettingSpec, 1> SaysBoth { SettingSpec {
    .name = "x", .summary = "y", .readBy = "Thing::Reader", .unreadBecause = "nothing yet, #1" } };

/// An opt-out that names no issue -- `forgot` in the vocabulary of `decided`.
constexpr std::array<SettingSpec, 1> OptOutWithNoIssue { SettingSpec {
    .name = "x", .summary = "y", .unreadBecause = "we will get to it" } };

/// A row that names a reader.
constexpr std::array<SettingSpec, 1> NamesAReader { SettingSpec { .name = "x", .summary = "y", .readBy = "Thing::Reader" } };

/// An opt-out spelled properly.
constexpr std::array<SettingSpec, 1> OptOutWithAnIssue { SettingSpec {
    .name = "x", .summary = "y", .unreadBecause = "nothing reads it until #4242 wires the scheduler" } };
} // namespace ConsumerGuard

TEST_CASE("Every replicated setting says what reads it", "[cluster][state]")
{
    // #1124. `SettingTable`'s header names the failure it exists to prevent -- a
    // setting accepted, replicated, snapshotted and carried across restarts while
    // doing nothing -- and `FindSetting` closes it only for a MISSPELLED key. Two of
    // the three rows were write-only at once under a correctly spelled name.
    //
    // Both directions, because one alone establishes nothing: a guard nobody has
    // watched refuse is not a guard, and one nobody has watched accept is not known
    // to work (#1031). These are `static_assert`s rather than `CHECK`s, so a broken
    // guard fails the BUILD of this file; the case then exists to name the property
    // and to carry the runtime half below.
    static_assert(!RowsCarryAConsumer(ConsumerGuard::SaysNothing), "a row saying nothing must be refused");
    static_assert(!RowsCarryAConsumer(ConsumerGuard::SaysBoth), "a row saying both must be refused");
    static_assert(!RowsCarryAConsumer(ConsumerGuard::OptOutWithNoIssue), "an opt-out must name its issue");
    static_assert(RowsCarryAConsumer(ConsumerGuard::NamesAReader), "a row naming a reader must be accepted");
    static_assert(RowsCarryAConsumer(ConsumerGuard::OptOutWithAnIssue), "a stated opt-out must be accepted");
    static_assert(RowsCarryAConsumer(SettingTable), "and the table this build ships");

    // The runtime half, and it is not a restatement of the line above: the guard
    // accepts an opt-out, so a table where every row had opted out would satisfy it
    // completely. This is the tally -- `RefuseUntriaged`'s argument in the metrics
    // rules -- and it NAMES the rows rather than counting them, because a count
    // cannot be acted on. Adding a legitimate opt-out is expected to fail this and to
    // be acknowledged here; that is the visibility, not an obstacle.
    std::string optedOut;
    for (auto const& row: SettingTable)
        if (row.readBy.empty())
            optedOut += std::string { row.name } + " (" + std::string { row.unreadBecause } + ") ";
    CHECK(optedOut.empty());

    // And every reader named is a real one. A claim is only as good as somebody
    // checking it, so the two live rows are spelled out here: the guard cannot tell a
    // true `Class::Function` from a plausible one, and this is where a rename that
    // left the column behind shows up.
    REQUIRE(SettingTable.size() == 2);
    CHECK(FindSetting(FleetOpenSetting)->readBy == "NodeMembership::AgreedOpenness");
    CHECK(FindSetting(LeaseLifetimeSetting)->readBy == "SchedulerService::AgreedLeaseLifetime");
}

TEST_CASE("A key this cluster refuses to replicate is refused BY NAME", "[cluster][state]")
{
    // The whole subject is the DIFFERENCE between two refusals, so asserting that the
    // key appears in the message would pass under both -- `no such cluster setting:
    // upstream` names it too. What has to be asserted is the answer only the row can
    // give: why this cluster will not agree on it, and which flag does the job.
    auto const refused = Refused(Cmd(CommandKind::SetSetting, "upstream", "cache.internal:6674"));
    CHECK(refused.contains("upstream is not a replicated setting"));
    CHECK(refused.contains("--requirepass"));
    CHECK(refused.contains("--upstream"));

    // The control, and the reason this case exists: a genuine typo must go on getting
    // the typo answer. Transposed rather than doubled, so `upstream` is not a
    // substring of it and a contains-assertion cannot pass for the wrong reason.
    auto const typo = Refused(Cmd(CommandKind::SetSetting, "upstrean", "cache.internal:6674"));
    CHECK(typo.contains("no such cluster setting"));
    CHECK_FALSE(typo.contains("--requirepass"));

    // And it is refused rather than shadowed by a row. Re-adding the row fails the
    // BUILD on the `static_assert` beside `RefusedSettingTable`, which is the real
    // guard; this is what still fails if somebody drops that assert in the same edit.
    CHECK(FindSetting("upstream") == nullptr);
    CHECK(FindRefusedSetting("upstream") != nullptr);
    CHECK(FindRefusedSetting("lease-lifetime") == nullptr);
}

TEST_CASE("Applying is total, and admitting a known member moves it", "[cluster][state]")
{
    // `Apply` cannot fail, which is a property consensus needs rather than a
    // convenience: it runs after commitment, when refusing is no longer an option.
    ClusterState state;

    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    REQUIRE(state.members.size() == 2);

    // Sorted by id, so two nodes that applied the same entries hold byte-identical
    // state whatever order the entries arrived in.
    CHECK(state.members[0].id == "n1");
    CHECK(state.members[1].id == "n2");

    // One verb for "join" and "moved": a node that moved has the same identity and a
    // new address, and making an operator remove it first would leave a window in
    // which the cluster has agreed it does not exist.
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.9:6675"));
    CHECK(state.members.size() == 2);
    CHECK(Unwrap(state.RaftEndpointOf("n1")) == "10.0.0.9:6675");

    Apply(state, Cmd(CommandKind::RemoveMember, "n1"));
    CHECK(state.members.size() == 1);
    CHECK_FALSE(state.RaftEndpointOf("n1").has_value());

    // Removing something that is not there is a no-op rather than a fault, for the
    // same reason: by the time this runs the cluster has already agreed to it.
    Apply(state, Cmd(CommandKind::RemoveMember, "n1"));
    CHECK(state.members.size() == 1);
}

TEST_CASE("A command round-trips, and an unknown verb is refused", "[cluster][state][wire]")
{
    // Every field a different value, so a transposition cannot survive.
    auto const original = Cmd(CommandKind::SetSetting, "lease-lifetime", "20min");
    auto const decoded = DecodeCommand(Encode(original));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == original);

    // A verb byte this build does not know is refused rather than applied as
    // whichever enumerator it happens to alias -- which would change the cluster's
    // state in a way nobody wrote down.
    auto bytes = Encode(original);
    // The verb sits in the second byte of the first field, after that field's u32
    // length prefix.
    bytes[5] = static_cast<std::byte>(0xEE);
    auto const unknown = DecodeCommand(bytes);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ConsensusErrorCode::UnknownMessageType);

    // And a payload that is not a command at all, said as damage rather than as another build.
    auto const empty = DecodeCommand({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == ConsensusErrorCode::MalformedFrame);
}

TEST_CASE("A whole state round-trips, members and settings apart", "[cluster][state][wire]")
{
    // The snapshot format, and the case that matters is the boundary: members and
    // settings are both `(string, string)` pairs, so a count read wrongly would
    // silently turn a member into a setting or the reverse -- a cluster that had
    // agreed on three members recovering with two.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::SetSetting, "lease-lifetime", "20min"));

    auto const restored = DecodeState(Encode(state));
    REQUIRE(restored.has_value());
    CHECK(*restored == state);
    CHECK(restored->members.size() == 2);
    CHECK(restored->settings.size() == 1);

    // An empty state is a legitimate one -- a cluster that has agreed nothing yet --
    // and must survive the round trip rather than being read as malformed.
    ClusterState const empty;
    auto const emptyBack = DecodeState(Encode(empty));
    REQUIRE(emptyBack.has_value());
    CHECK(*emptyBack == empty);
}

TEST_CASE("A truncated snapshot is refused rather than half-read", "[cluster][state][wire]")
{
    // Read as far as it goes, a truncated snapshot yields a member with an empty
    // endpoint -- which this node would then replicate onward as an address nobody
    // can dial, and which the fleet would lease out.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));

    auto bytes = Encode(state);
    REQUIRE(bytes.size() > 8);
    bytes.resize(bytes.size() - 4);
    auto const truncated = DecodeState(bytes);
    REQUIRE_FALSE(truncated.has_value());
    // Damage, and said as damage: the version byte is intact, so *another build wrote
    // this* would send an operator to upgrade a machine whose snapshot is broken.
    CHECK(truncated.error().code == ConsensusErrorCode::MalformedFrame);

    // A member count larger than the pairs present is the same fault reached by a
    // different route, and must fail the same way.
    auto overcounted = Encode(state);
    overcounted[8] = static_cast<std::byte>(0xFF);
    auto const miscounted = DecodeState(overcounted);
    REQUIRE_FALSE(miscounted.has_value());
    CHECK(miscounted.error().code == ConsensusErrorCode::MalformedFrame);
}

TEST_CASE("A state another build encoded is refused by its version while a command keeps its own", "[cluster][state][wire]")
{
    // #1309 added the admitted clients and the forgotten hosts, so the state's version
    // moved -- and the byte is pinned as well as the refusal, because a symbol both ends
    // spell can only test the NAME of a wire constant. The version is the first field's
    // only byte, after that field's u32 length prefix.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675", "10.0.0.1:7000"));
    auto bytes = Encode(state);
    REQUIRE(bytes.size() > 4);
    CHECK(bytes[4] == std::byte { 4 });

    // What the previous build wrote. Refused BY NAME, both versions stated, and never
    // as `MalformedFrame`: those bytes are intact, and *damaged* is what gets a healthy
    // snapshot deleted.
    bytes[4] = std::byte { 3 };
    auto const older = DecodeState(bytes);
    REQUIRE_FALSE(older.has_value());
    CHECK(older.error().code == ConsensusErrorCode::UnsupportedVersion);
    CHECK(older.error().context.contains("version 3"));
    CHECK(older.error().context.contains("reads 4"));

    // The COMMAND layout did not change -- #1309 added verbs, not fields -- so its version
    // must not have either. A committed entry this build cannot decode is skipped, so
    // moving this byte with the state's would make a node restarting onto its own log skip
    // every entry in it.
    auto command = Encode(Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675", "10.0.0.1:7000"));
    REQUIRE(command.size() > 4);
    CHECK(command[4] == std::byte { 2 });

    // And a command another build encoded is refused by ITS version, by name, never as damage.
    command[4] = std::byte { 3 };
    auto const newer = DecodeCommand(command);
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().code == ConsensusErrorCode::UnsupportedVersion);
    CHECK(newer.error().context.contains("command encoding version 3"));
    CHECK(newer.error().context.contains("reads 2"));
}

TEST_CASE("A member's scheduler endpoint says whether it was never announced or cleared by a re-admit", "[cluster][state]")
{
    // #1340. Both states carry an empty endpoint, and that is right: a re-admit
    // applies wholesale because a node that moved moved both ports. What tells them
    // apart is what the member had BEFORE -- asserted on both sides of each pair,
    // because a case rendering only one of them passes under the defect.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "quiet", "10.0.0.1:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "led", "10.0.0.2:6675", "10.0.0.2:7000"));

    auto const stateOf = [&state](std::string_view id) {
        auto const it = std::ranges::find(state.members, id, &ClusterMember::id);
        REQUIRE(it != state.members.end());
        return SchedulerEndpointStateOf(*it);
    };

    CHECK(stateOf("quiet") == SchedulerEndpointState::NeverAnnounced);
    CHECK(stateOf("led") == SchedulerEndpointState::Announced);

    // The recovery path's re-approval, through the verb `ClusterAdmit` sends: the
    // endpoint goes, and the member says it went.
    Apply(state, Cmd(CommandKind::AddMember, "led", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "quiet", "10.0.0.1:6675"));
    CHECK_FALSE(state.SchedulerEndpointOf("led").has_value());
    CHECK_FALSE(state.SchedulerEndpointOf("quiet").has_value());
    CHECK(stateOf("led") == SchedulerEndpointState::Cleared);
    CHECK(stateOf("quiet") == SchedulerEndpointState::NeverAnnounced);

    // A second re-admit -- the move after the recovery -- is still a cleared endpoint,
    // not a member that has never said.
    Apply(state, Cmd(CommandKind::AddMember, "led", "10.0.0.9:6675"));
    CHECK(stateOf("led") == SchedulerEndpointState::Cleared);

    // Announcing again is announced, and the next re-admit clears it again.
    Apply(state, Cmd(CommandKind::AddMember, "led", "10.0.0.9:6675", "10.0.0.9:7000"));
    CHECK(stateOf("led") == SchedulerEndpointState::Announced);

    // A forget is a positive act, so an id admitted from absence has announced nothing
    // -- whatever an earlier member of that name once did.
    Apply(state, Cmd(CommandKind::RemoveMember, "led"));
    Apply(state, Cmd(CommandKind::AddMember, "led", "10.0.0.9:6675"));
    CHECK(stateOf("led") == SchedulerEndpointState::NeverAnnounced);

    // And the three spellings are three, so no renderer reading the table can print
    // two of them alike.
    CHECK(SchedulerEndpointStateTable[static_cast<std::size_t>(SchedulerEndpointState::NeverAnnounced)].name
          != SchedulerEndpointStateTable[static_cast<std::size_t>(SchedulerEndpointState::Cleared)].name);
    CHECK(SchedulerEndpointStateTable[static_cast<std::size_t>(SchedulerEndpointState::Announced)].name
          != SchedulerEndpointStateTable[static_cast<std::size_t>(SchedulerEndpointState::Cleared)].name);
}

TEST_CASE("A cleared scheduler endpoint survives a snapshot as cleared", "[cluster][state][wire]")
{
    // A snapshot is how a follower that fell behind learns the state, and a history
    // the encoder dropped would turn every cleared member back into one that never
    // announced -- the reporting defect, reintroduced one hop from where it was fixed.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "cleared", "10.0.0.1:6675", "10.0.0.1:7000"));
    Apply(state, Cmd(CommandKind::AddMember, "cleared", "10.0.0.1:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "quiet", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::SetSetting, "fleet-open", "1"));

    auto const restored = DecodeState(Encode(state));
    REQUIRE(restored.has_value());
    CHECK(*restored == state);
    REQUIRE(restored->members.size() == 2);
    CHECK(SchedulerEndpointStateOf(restored->members[0]) == SchedulerEndpointState::Cleared);
    CHECK(SchedulerEndpointStateOf(restored->members[1]) == SchedulerEndpointState::NeverAnnounced);
    CHECK(restored->settings.size() == 1);
}

TEST_CASE("A snapshot whose scheduler endpoint history cannot be true is refused", "[cluster][state][wire]")
{
    // An endpoint with no announcement behind it is the one combination `Apply` never
    // produces, and reading it would leave every renderer choosing which half to
    // believe.
    ClusterState const contradictory { .members = { ClusterMember { .id = "n1",
                                                                    .raftEndpoint = "10.0.0.1:6675",
                                                                    .schedulerEndpoint = "10.0.0.1:7000",
                                                                    .schedulerEndpointHistory =
                                                                        SchedulerEndpointHistory::NeverAnnounced } },
                                       .settings = {},
                                       .clients = {},
                                       .forgotten = {} };
    auto const refused = DecodeState(Encode(contradictory));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::MalformedFrame);

    // A history byte naming no history. With one member and no settings the byte is
    // the encoding's last.
    ClusterState quiet;
    Apply(quiet, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    auto bytes = Encode(quiet);
    REQUIRE(DecodeState(bytes).has_value());
    bytes.back() = static_cast<std::byte>(EnumeratorCount<SchedulerEndpointHistory>);
    auto const unknown = DecodeState(bytes);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ConsensusErrorCode::MalformedFrame);
}

TEST_CASE("Endpoints come back in the order a membership oracle wants", "[cluster][state]")
{
    // `Distributed::ClusterMembership` takes endpoints and keys on hosts, so this is
    // the shape the two layers meet in.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));

    CHECK(state.Endpoints() == std::vector<std::string> { "10.0.0.1:6675", "10.0.0.2:6675" });
}

TEST_CASE("A member carries the port a client speaks to, not only the one peers do", "[cluster][state]")
{
    // The defect this pairing closes. `NotLeader` carries a redirect, and while one
    // address was recorded it was the CONSENSUS port -- so a follower answered "ask
    // the leader, at its Raft peer port" and a client that took the advice spoke the
    // scheduler protocol at a socket that has never heard of it. Two ports, two
    // facts.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675", "10.0.0.1:7000"));

    CHECK(Unwrap(state.RaftEndpointOf("n1")) == "10.0.0.1:6675");
    CHECK(Unwrap(state.SchedulerEndpointOf("n1")) == "10.0.0.1:7000");

    // A member that has never announced one is absent rather than empty, because a
    // caller has nothing different to do about "not a member" and "nowhere to send
    // you" -- both mean the client compiles locally.
    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    CHECK(Unwrap(state.RaftEndpointOf("n2")) == "10.0.0.2:6675");
    CHECK_FALSE(state.SchedulerEndpointOf("n2").has_value());
    CHECK_FALSE(state.SchedulerEndpointOf("nobody").has_value());

    // `Endpoints()` stays the consensus ones: it feeds the fleet's membership oracle,
    // which keys on the HOST, and only this endpoint is guaranteed to be there.
    CHECK(state.Endpoints() == std::vector<std::string> { "10.0.0.1:6675", "10.0.0.2:6675" });
}

TEST_CASE("Re-admitting a member replaces its whole record", "[cluster][state]")
{
    // Wholesale, and that is the right way round: a record is re-proposed when it has
    // changed, and a node that moved moved both of its ports -- so keeping a
    // scheduler endpoint the command did not repeat would redirect clients at an
    // address that member no longer answers, which is worse than redirecting them
    // nowhere at all.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675", "10.0.0.1:7000"));
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.9:6675"));

    REQUIRE(state.members.size() == 1);
    CHECK(Unwrap(state.RaftEndpointOf("n1")) == "10.0.0.9:6675");
    CHECK_FALSE(state.SchedulerEndpointOf("n1").has_value());
}

TEST_CASE("A verb that has no scheduler endpoint may not carry one", "[cluster][state]")
{
    // A field a verb ignores is a field somebody misunderstood, and the refusal is at
    // the proposer because that is the only place anything can be refused.
    CHECK(Validate(Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675", "10.0.0.1:7000")).has_value());
    CHECK_FALSE(Validate(Cmd(CommandKind::RemoveMember, "n1", {}, "10.0.0.1:7000")).has_value());
    CHECK_FALSE(Validate(Cmd(CommandKind::SetSetting, "lease-lifetime", "20min", "10.0.0.1:7000")).has_value());
}

TEST_CASE("A member's two endpoints survive a snapshot apart", "[cluster][state][wire]")
{
    // Members are triples and settings are pairs, so the boundary between them is
    // arithmetic rather than a delimiter -- and getting it wrong reads every member's
    // scheduler endpoint as the next member's id, which decodes and produces a state
    // nothing would report as wrong.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675", "10.0.0.1:7000"));
    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::SetSetting, "lease-lifetime", "20min"));
    Apply(state, Cmd(CommandKind::SetSetting, "fleet-open", "1"));

    auto const restored = DecodeState(Encode(state));
    REQUIRE(restored.has_value());
    CHECK(*restored == state);
    CHECK(restored->members.size() == 2);
    CHECK(restored->settings.size() == 2);
}

TEST_CASE("An AddMember round-trips both of its endpoints", "[cluster][state][wire]")
{
    // Every field a different value, so a transposition cannot survive -- the arm
    // this verb gained is the one a copied encoder gets wrong.
    auto const original = Cmd(CommandKind::AddMember, "n7", "10.0.0.7:6675", "10.0.0.7:7000");
    auto const decoded = DecodeCommand(Encode(original));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == original);
}

TEST_CASE("A peer is an identity and an address, in one token", "[cluster][state]")
{
    // Both halves together because they are one fact. A member id with no address is
    // a node the cluster counts towards quorum and cannot reach -- the residual
    // `RaftMembership` recorded, and the reason a member carries its endpoint at all.
    auto const peer = ParseMemberSpec("n1=10.0.0.1:6680");
    REQUIRE(peer.has_value());
    CHECK(Unwrap(peer).id == "n1");
    CHECK(Unwrap(peer).raftEndpoint == "10.0.0.1:6680");
}

TEST_CASE("A peer specification splits at the first separator", "[cluster][state]")
{
    // The endpoint may contain an `=` and the identity may not. Splitting at the LAST
    // one instead would read `n1=host=1:6675` as an id of `n1=host` -- an id no
    // operator wrote, which would then silently never match a vote and leave the
    // cluster one member short of a quorum it thinks it has.
    auto const odd = ParseMemberSpec("n1=weird=host:6680");
    REQUIRE(odd.has_value());
    CHECK(Unwrap(odd).id == "n1");
    CHECK(Unwrap(odd).raftEndpoint == "weird=host:6680");
}

TEST_CASE("A peer with no dialable address is refused", "[cluster][state]")
{
    // Refused at startup, where an operator is watching. A member recorded with an
    // address nobody can dial registers, is counted towards every quorum, and is
    // never reached -- the fleet is then one node short of forming one and nothing
    // says why.
    CHECK_FALSE(ParseMemberSpec("n1").has_value());
    CHECK_FALSE(ParseMemberSpec("n1=").has_value());
    CHECK_FALSE(ParseMemberSpec("=10.0.0.1:6680").has_value());
    CHECK_FALSE(ParseMemberSpec("n1=10.0.0.1").has_value());
    CHECK_FALSE(ParseMemberSpec("").has_value());

    // A port is not a port because it parsed as a number. `0` means "pick one" to
    // a bind and names nothing to a dial, so a member recorded with it is counted
    // towards every quorum and never reached -- which is the same failure as an
    // address with no port at all, arrived at by looking like one.
    CHECK_FALSE(ParseMemberSpec("n1=10.0.0.1:0").has_value());
    CHECK_FALSE(ParseMemberSpec("n1=10.0.0.1:99999").has_value());
    CHECK_FALSE(ParseMemberSpec("n1=10.0.0.1:http").has_value());
}

TEST_CASE("An IPv6 peer keeps its address rather than its last colon group", "[cluster][state]")
{
    // Split through `Core/HostPort`, which is the whole reason that header exists: a
    // naive `rfind(':')` takes `[::1]:6680` apart at the wrong colon and yields a
    // host of `[::` and a port of `1]`.
    auto const peer = ParseMemberSpec("n1=[2001:db8::1]:6680");
    REQUIRE(peer.has_value());
    CHECK(Unwrap(peer).id == "n1");
    CHECK(Unwrap(peer).raftEndpoint == "[2001:db8::1]:6680");
}

TEST_CASE("A lease lifetime the cluster may not agree on is refused, and one it may is accepted", "[cluster][state]")
{
    namespace Wire = FastCache::CompileCacheWire;

    // **Both directions, and the accepting one is not decoration.** A guard nobody has
    // watched ACCEPT is not known to work (#1031): a validator that refused every value
    // would pass every refusal assertion below and make the setting unsettable, which is
    // this ticket delivering nothing while looking delivered.
    SECTION("a value the cluster may agree on")
    {
        CHECK(Validate(Cmd(CommandKind::SetSetting, std::string { LeaseLifetimeSetting }, "30min")).has_value());
        CHECK(ParseLeaseLifetime("30min") == std::chrono::milliseconds { std::chrono::minutes { 30 } });

        // The two ends of the legal range, exactly, in the text `FormatDuration` writes --
        // which is the text the refusals below print for an operator to type back.
        // Inside-the-range values alone would pass against a validator whose bounds are
        // off by any amount.
        CHECK(Validate(Cmd(CommandKind::SetSetting,
                           std::string { LeaseLifetimeSetting },
                           FormatDuration(Wire::MaxCompileLeaseLifetime)))
                  .has_value());
        CHECK(Validate(Cmd(CommandKind::SetSetting,
                           std::string { LeaseLifetimeSetting },
                           FormatDuration(Wire::DefaultCompileIdleTimeout + std::chrono::milliseconds { 1 })))
                  .has_value());

        // The shipped default must itself be settable, or an operator cannot type the
        // value their fleet is already running.
        CHECK(Validate(Cmd(CommandKind::SetSetting,
                           std::string { LeaseLifetimeSetting },
                           FormatDuration(Wire::DefaultCompileLeaseTimeout)))
                  .has_value());
    }

    SECTION("one millisecond past the ceiling is refused, and the refusal names the ceiling")
    {
        // Just past, rather than absurdly past: a bound tested only against a wild value
        // passes against an off-by-a-lot bound, which is the one that would let a site
        // set a lease lifetime that widens the post-restart replay window well beyond
        // what `MaxCompileLeaseLifetime` argues is acceptable.
        auto const refusal = Refused(Cmd(CommandKind::SetSetting,
                                         std::string { LeaseLifetimeSetting },
                                         FormatDuration(Wire::MaxCompileLeaseLifetime + std::chrono::milliseconds { 1 })));
        CHECK(refusal.contains(std::format("at most {}", FormatDuration(Wire::MaxCompileLeaseLifetime))));
        // The REASON travels, because an operator meeting a bare "too large" has no way
        // to know this is a replay bound rather than a number somebody rounded.
        CHECK(refusal.contains("replayable"));
    }

    SECTION("a value at or below the idle bound is refused, which the static_assert can no longer cover")
    {
        // `CompileCacheWire` asserts `DefaultCompileIdleTimeout < DefaultCompileLeaseTimeout`
        // at build time, and that now covers the DEFAULT alone: a cluster can agree on a
        // lifetime the constant never had. Below the idle bound a healthy worker's own
        // reactor jitter outlives the job, so the fleet reads as stopped -- the exact
        // failure the build-time assertion exists to prevent, reachable at run time.
        CHECK(Refused(Cmd(CommandKind::SetSetting,
                          std::string { LeaseLifetimeSetting },
                          FormatDuration(Wire::DefaultCompileIdleTimeout)))
                  .contains("silence"));
        CHECK(Refused(Cmd(CommandKind::SetSetting, std::string { LeaseLifetimeSetting }, "1ms")).contains("silence"));
    }

    SECTION("what is not a duration")
    {
        // A bare number is refused BY NAME, and it is the case with history: this setting
        // read `600000` as milliseconds, so an operator typing what the old documentation
        // said is told the grammar rather than handed a lifetime of a different length.
        CHECK(
            Refused(Cmd(CommandKind::SetSetting, std::string { LeaseLifetimeSetting }, "600000")).contains("names no unit"));
        // Parsed WHOLE: trailing or inner text is a refusal, never a prefix adopted.
        CHECK_FALSE(Refused(Cmd(CommandKind::SetSetting, std::string { LeaseLifetimeSetting }, "10min later")).empty());
        CHECK_FALSE(Refused(Cmd(CommandKind::SetSetting, std::string { LeaseLifetimeSetting }, "600 000ms")).empty());
        CHECK(Refused(Cmd(CommandKind::SetSetting, std::string { LeaseLifetimeSetting }, "-1s")).contains("negative"));
        CHECK_FALSE(Refused(Cmd(CommandKind::SetSetting, std::string { LeaseLifetimeSetting }, "")).empty());
    }

    SECTION("the settings this build constrains nothing about are unaffected")
    {
        // The control for the column itself: a `refuse` wired to the wrong row, or run
        // for every row, would refuse this -- and every assertion above would still
        // pass. One unconstrained row rather than two since #1123, so the control is
        // thinner than it was; it is still the only thing here that fails when the
        // column is run unconditionally.
        CHECK(Validate(Cmd(CommandKind::SetSetting, "fleet-open", "not-a-number")).has_value());
    }
}

TEST_CASE("Forgetting a client records that it was forgotten, and admitting it again clears that",
          "[cluster][state][forget]")
{
    // #1309. A local `--fleet-member` list may still name a host the cluster has
    // forgotten, and a node can only refuse it if the forget left something behind:
    // absence from `clients` is also the state of every host a list names and the cluster
    // never admitted, so an erase alone decides nothing.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AdmitClient, "10.0.0.7"));
    CHECK(state.AdmitsClient("10.0.0.7"));
    CHECK_FALSE(state.HasForgotten("10.0.0.7"));

    Apply(state, Cmd(CommandKind::ForgetClient, "10.0.0.7"));
    CHECK_FALSE(state.AdmitsClient("10.0.0.7"));
    CHECK(state.HasForgotten("10.0.0.7"));
    CHECK(state.forgotten == std::vector<std::string> { "10.0.0.7" });

    // The control: a host nobody forgot is not forgotten, including one the cluster never
    // admitted -- which is every host a local list names.
    CHECK_FALSE(state.HasForgotten("10.0.0.8"));

    // A re-admit is the route back, and it clears the tombstone rather than leaving a
    // host both admitted and forgotten for a reader to pick between.
    Apply(state, Cmd(CommandKind::AdmitClient, "10.0.0.7"));
    CHECK(state.AdmitsClient("10.0.0.7"));
    CHECK_FALSE(state.HasForgotten("10.0.0.7"));
    CHECK(state.forgotten.empty());

    // By host: a port is not something a caller is matched on, and the dual-stack spelling
    // of the same machine is the same entry rather than a second one.
    Apply(state, Cmd(CommandKind::ForgetClient, "10.0.0.7:6674"));
    CHECK(state.HasForgotten("::ffff:10.0.0.7"));
    Apply(state, Cmd(CommandKind::ForgetClient, "::ffff:10.0.0.7"));
    CHECK(state.forgotten.size() == 1);
}

TEST_CASE("Forgetting a member records its host as forgotten, and admitting that host again clears it",
          "[cluster][state][forget]")
{
    // The decommission this ticket is for: a member is forgotten, and every node whose
    // own list still names its machine must be able to refuse it. The command carries an
    // id only, so the host comes from the record being removed.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));

    Apply(state, Cmd(CommandKind::RemoveMember, "n1"));
    CHECK(state.HasForgotten("10.0.0.1"));
    CHECK_FALSE(state.HasForgotten("10.0.0.2"));

    // Forgetting an id that is not a member leaves nothing: there is no host to derive,
    // and a tombstone for a guess would refuse a machine nobody forgot.
    Apply(state, Cmd(CommandKind::RemoveMember, "nobody"));
    CHECK(state.forgotten.size() == 1);

    // Re-admitting a member at that host clears it; so would `AdmitClient`, the route a
    // demoted member takes back as a plain worker.
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    CHECK_FALSE(state.HasForgotten("10.0.0.1"));

    Apply(state, Cmd(CommandKind::RemoveMember, "n2"));
    Apply(state, Cmd(CommandKind::AdmitClient, "10.0.0.2"));
    CHECK_FALSE(state.HasForgotten("10.0.0.2"));

    // Never loopback: a caller on a node's own machine is admitted whatever a list says,
    // so a tombstone for it could narrow nothing -- and every member of a one-machine
    // cluster answers on loopback.
    Apply(state, Cmd(CommandKind::AddMember, "local", "127.0.0.1:6675"));
    Apply(state, Cmd(CommandKind::RemoveMember, "local"));
    CHECK_FALSE(state.HasForgotten("127.0.0.1"));
}

TEST_CASE("Each cluster verb keeps the byte a log entry already carries", "[cluster][state][wire][forget]")
{
    // The BYTE, not the symbol: every caller in this tree spells the enumerator, so a
    // consistent renumbering stays green here while every log a fleet has already written
    // decodes as a different verb. And the two client verbs sit above the byte every
    // build before them treats as unknown, which is what makes such a build SKIP them
    // rather than apply one as something else.
    auto const byteOf = [](CommandKind kind) {
        auto const bytes = Encode(Cmd(kind, "10.0.0.7"));
        // The verb sits in the second byte of the first field, after that field's u32
        // length prefix.
        REQUIRE(bytes.size() > 5);
        return static_cast<unsigned>(bytes[5]);
    };
    CHECK(byteOf(CommandKind::AddMember) == 0U);
    CHECK(byteOf(CommandKind::RemoveMember) == 1U);
    CHECK(byteOf(CommandKind::SetSetting) == 2U);
    CHECK(byteOf(CommandKind::AdmitClient) == 3U);
    CHECK(byteOf(CommandKind::ForgetClient) == 4U);
}

TEST_CASE("A client command names a machine that is not this one, and nothing else", "[cluster][state][forget]")
{
    CHECK(Validate(Cmd(CommandKind::AdmitClient, "10.0.0.7")).has_value());
    CHECK(Validate(Cmd(CommandKind::ForgetClient, "ci-runner-3.example:6674")).has_value());

    CHECK(Refused(Cmd(CommandKind::AdmitClient, ":6674")).contains("must name a host"));
    CHECK(Refused(Cmd(CommandKind::ForgetClient, "127.0.0.1")).contains("loopback"));
    CHECK(Refused(Cmd(CommandKind::AdmitClient, "::1")).contains("loopback"));
    CHECK(Refused(Cmd(CommandKind::ForgetClient, "10.0.0.7", "extra")).contains("nothing else"));
    CHECK(Refused(Cmd(CommandKind::AdmitClient, "10.0.0.7", {}, "10.0.0.7:7000")).contains("nothing else"));

    // Recorded, so it has to be text: the host is printed by every renderer of the state.
    CHECK(Refused(Cmd(CommandKind::AdmitClient, "10.0.0.\x80")).contains("a client host"));
}

TEST_CASE("Clients and forgotten hosts survive a snapshot apart from each other", "[cluster][state][wire][forget]")
{
    // Both groups are lists of hosts, so a count read wrongly would silently turn an
    // admitted client into a forgotten one -- a machine refused everywhere after a
    // follower restores a snapshot.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    Apply(state, Cmd(CommandKind::SetSetting, "fleet-open", "0"));
    Apply(state, Cmd(CommandKind::AdmitClient, "10.0.0.5"));
    Apply(state, Cmd(CommandKind::AdmitClient, "10.0.0.6"));
    Apply(state, Cmd(CommandKind::ForgetClient, "10.0.0.9"));

    auto const restored = DecodeState(Encode(state));
    REQUIRE(restored.has_value());
    CHECK(*restored == state);
    CHECK(restored->clients == std::vector<std::string> { "10.0.0.5", "10.0.0.6" });
    CHECK(restored->forgotten == std::vector<std::string> { "10.0.0.9" });

    // A count that claims more hosts than the bytes carry is damage, said as damage.
    auto bytes = Encode(state);
    auto shortened = bytes;
    shortened.resize(shortened.size() - 1);
    auto const truncated = DecodeState(shortened);
    REQUIRE_FALSE(truncated.has_value());
    CHECK(truncated.error().code == ConsensusErrorCode::MalformedFrame);
}
