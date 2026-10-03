// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialerTestUtils.hpp"
#include "EnrollClient.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CacheProtocol.hpp>
#include <tests/NodeFlagNames.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/TicketFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A reply the seed sent, as the exchange layer hands it over.
/// @param outcome What the leader decided.
/// @param roster The roster it sent with it.
/// @return The outcome a client would read.
[[nodiscard]] Cc::CacheOutcome Served(Wire::EnrollOutcome outcome, std::span<std::byte const> roster = {})
{
    return Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Hit,
                              .value = Wire::EncodeEnrollReply(outcome, roster, {}, std::nullopt),
                              .message = {},
                              .credentialIgnored = false,
                              .transportFailure = Cc::TransportFailure::None };
}

/// A refusal the seed sent.
/// @param code Why.
/// @param message Its own words.
/// @return The outcome a client would read.
[[nodiscard]] Cc::CacheOutcome Refused(Wire::ErrorCode code, std::string message = {})
{
    return Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Rejected,
                              .value = {},
                              .code = code,
                              .message = std::move(message),
                              .credentialIgnored = false,
                              .transportFailure = Cc::TransportFailure::None };
}

/// A 32-byte key whose every byte is @p fill.
/// @param fill The byte.
/// @return The key.
[[nodiscard]] Ed25519PublicKey KeyOf(std::uint8_t fill)
{
    Ed25519PublicKey key {};
    key.fill(static_cast<std::byte>(fill));
    return key;
}

/// A member record, every field named.
/// @param id Its id.
/// @param endpoint Its consensus endpoint.
/// @param key Its key.
/// @return The member.
[[nodiscard]] Cluster::ClusterMember Member(std::string_view id, std::string_view endpoint, Ed25519PublicKey const& key)
{
    return Cluster::ClusterMember { .id = std::string { id },
                                    .raftEndpoint = std::string { endpoint },
                                    .schedulerEndpoint = {},
                                    .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                    .seat = Cluster::MemberSeat::Voter,
                                    .publicKey = key };
}

/// The roster a two-member cluster hands a joiner: the leader, and @p joiner.
/// @param joiner The joiner's record as the leader holds it.
/// @return The encoded roster.
[[nodiscard]] std::vector<std::byte> RosterWith(Cluster::ClusterMember joiner)
{
    auto state = Cluster::ClusterState {};
    state.members.push_back(Member("leader", "10.0.0.1:7100", KeyOf(0x01)));
    state.members.push_back(std::move(joiner));
    return Cluster::EncodeRoster(Cluster::ProjectRoster(state));
}

} // namespace

TEST_CASE("A pending reply is a wait, and an approved one carries the roster", "[enrollment][client]")
{
    auto const waiting = ReadEnrollReply(Served(Wire::EnrollOutcome::Pending));
    CHECK(waiting.progress == EnrollProgress::Waiting);
    CHECK(waiting.roster.empty());

    auto const roster = RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x42)));
    auto const admitted = ReadEnrollReply(Served(Wire::EnrollOutcome::Approved, roster));
    CHECK(admitted.progress == EnrollProgress::Admitted);
    CHECK(admitted.roster == roster);

    // A person said no. Distinguished from every WAIT above because asking again will
    // not change it, and a client that read it as a wait would poll until its bound ran
    // out and then report a timeout for a decision that was already taken.
    auto const refused = ReadEnrollReply(Served(Wire::EnrollOutcome::Rejected));
    CHECK(refused.progress == EnrollProgress::Refused);
}

TEST_CASE("Every outcome a fleet signs is read back as the outcome its signature is checked over",
          "[enrollment][client][formation]")
{
    // `SignedOutcomeTable` decides which outcome a joiner checks a signature OVER, from the reading
    // the reply became. A swapped row verifies nothing the fleet signed -- every join stops, closed --
    // and no controller case can see it, since the controller's fake signs the same reading. So the
    // table is held to the wire here: each outcome encoded, read back, and mapped to what was sent.
    // The readings are this case's own, written out rather than read from the table.
    auto const roster = RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x42)));
    struct Row
    {
        Wire::EnrollOutcome outcome; ///< What the fleet decided and signed.
        EnrollProgress reading;      ///< What a joiner reads it as.
        bool carriesRoster;          ///< Whether the reply carries the roster.
    };
    constexpr auto rows = std::array {
        Row { .outcome = Wire::EnrollOutcome::Pending, .reading = EnrollProgress::Waiting, .carriesRoster = false },
        Row { .outcome = Wire::EnrollOutcome::Approved, .reading = EnrollProgress::Admitted, .carriesRoster = true },
        Row { .outcome = Wire::EnrollOutcome::Rejected, .reading = EnrollProgress::Refused, .carriesRoster = false },
    };
    for (auto const& row: rows)
    {
        INFO("outcome " << static_cast<int>(row.outcome));
        auto const read = ReadEnrollReply(
            Served(row.outcome, row.carriesRoster ? std::span<std::byte const> { roster } : std::span<std::byte const> {}));
        CHECK(read.progress == row.reading);
        CHECK(SignedOutcomeOf(read.progress) == std::optional { row.outcome });
    }
    CHECK(SignedOutcomeTable.size() == rows.size()); // every outcome a fleet signs, and no other

    // A wire refusal is signed by nobody, and maps to no outcome: there is nothing to check.
    for (auto const code:
         { Wire::ErrorCode::EnrollmentFull, Wire::ErrorCode::EnrollmentHostFull, Wire::ErrorCode::NotLeader })
    {
        INFO("refusal " << static_cast<int>(code));
        CHECK_FALSE(SignedOutcomeOf(ReadEnrollReply(Refused(code)).progress).has_value());
    }
    CHECK_FALSE(SignedOutcomeOf(ReadEnrollReply(Refused(Wire::ErrorCode::UnknownOpcode)).progress).has_value());
    CHECK_FALSE(SignedOutcomeOf(EnrollProgress::Redirect).has_value());
}

TEST_CASE("An approval carrying no roster is a fault rather than an admission", "[enrollment][client]")
{
    // **The client's half of the assertion the server side also makes.** A seed that
    // approved and sent nothing would have this node report an admission it can check
    // nothing about -- and the outcome byte is correct in that build, so the LENGTH is the
    // only thing that separates them.
    auto const reading = ReadEnrollReply(Served(Wire::EnrollOutcome::Approved));
    CHECK(reading.progress == EnrollProgress::Fatal);
    CHECK(reading.detail.contains("no roster"));
}

TEST_CASE("A full list or no leader is a wait and a seed too old to know the verb is not", "[enrollment][client]")
{
    // Two refusals a client answers by asking again: one says *not yet* and the other
    // says *ask in a moment, somebody is being elected*. Collapsing either into the
    // fatal arm would make a joiner give up on a fleet that was about to admit it.
    for (auto const code:
         { Wire::ErrorCode::EnrollmentFull, Wire::ErrorCode::EnrollmentHostFull, Wire::ErrorCode::NotLeader })
        CHECK(ReadEnrollReply(Refused(code)).progress == EnrollProgress::Closed);

    // And the one that says the SEED is the problem rather than this machine or this
    // moment. A build that does not implement the verb will not start implementing it
    // while this loop polls, so stopping is the only useful answer -- and the message
    // says which machine to upgrade.
    auto const old = ReadEnrollReply(Refused(Wire::ErrorCode::UnknownOpcode));
    CHECK(old.progress == EnrollProgress::Fatal);
    CHECK(old.detail.contains("older"));
}

TEST_CASE("A seed that runs no consensus is told apart from a seed that is too old", "[enrollment][client]")
{
    // **The commonest operator mistake, and it used to produce a confident wrong
    // diagnosis.** A joiner pointed at a node that runs no consensus has nothing to
    // join. It answered `UnimplementedVerb`, which arrives as
    // `UnknownOpcode`, and this client reported *the seed is running a build older than
    // this one*: somebody is sent to upgrade a node that is already current, and the
    // real remedy -- a different ADDRESS -- is never mentioned.
    auto const noCluster = ReadEnrollReply(Refused(Wire::ErrorCode::NoCluster));

    // Fatal rather than a wait: a node that runs no consensus will not start running it
    // while this loop polls, so retrying for ten minutes helps nobody.
    CHECK(noCluster.progress == EnrollProgress::Fatal);

    // **Assert what DISTINGUISHES.** Both readings are `Fatal` and both carry a
    // sentence, so a case checking only the progress passes under the defect this is
    // named for. What separates them is which machine the operator is sent to look at,
    // so the text is the assertion: this one must NOT blame the seed's build, and must
    // name the remedy.
    CHECK(!noCluster.detail.contains("older"));
    CHECK(noCluster.detail.contains("consensus"));
    CHECK(noCluster.detail.contains("--fleet-seed"));

    // And the two codes really do reach different sentences, which is the property one
    // arm alone cannot show -- a build that folded them would pass every check above
    // for whichever text it kept.
    CHECK(noCluster.detail != ReadEnrollReply(Refused(Wire::ErrorCode::UnknownOpcode)).detail);
}

TEST_CASE("A NotLeader naming an endpoint is followed, and one naming a sentence is not", "[enrollment][client]")
{
    // Judged by PARSING the message rather than by testing it for empty: an empty one is
    // replaced by the error table's default sentence, so *no leader known* and *the
    // leader is at h:p* arrive the same shape.
    auto const redirect = ReadEnrollReply(Refused(Wire::ErrorCode::NotLeader, "10.0.0.1:7000"));
    CHECK(redirect.progress == EnrollProgress::Redirect);
    CHECK(redirect.detail == "10.0.0.1:7000");

    // `SplitHostPort` takes the LAST colon, so a sentence splits into a host and a port
    // of " try again" -- and this client DIALS what it is given. A build that split
    // rather than parsed would dial that.
    CHECK(ReadEnrollReply(Refused(Wire::ErrorCode::NotLeader, "no leader: try again")).progress == EnrollProgress::Closed);
    CHECK(ReadEnrollReply(Refused(Wire::ErrorCode::NotLeader)).progress == EnrollProgress::Closed);
}

TEST_CASE("A seed that could not be reached, or answered a body this build cannot read, is fatal", "[enrollment][client]")
{
    auto const unreachable = Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Transport,
                                                .value = {},
                                                .message = {},
                                                .credentialIgnored = false,
                                                .transportFailure = Cc::TransportFailure::Unreached };
    CHECK(ReadEnrollReply(unreachable).progress == EnrollProgress::Fatal);

    auto const garbage = Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Hit,
                                            .value = std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x02 } },
                                            .message = {},
                                            .credentialIgnored = false,
                                            .transportFailure = Cc::TransportFailure::None };
    CHECK(ReadEnrollReply(garbage).progress == EnrollProgress::Fatal);
}

TEST_CASE("An admission says what the seed recorded and where this machine wrote it, and names no step it lacks",
          "[enrollment][client][learner][formation]")
{
    // What is TRUE on this build: the seed recorded the key, and this machine kept the identity it
    // asked under -- and nothing else. It names no step the binary does not have:
    // every `--flag` it prints must parse, which is how a `--raft-join` instruction outlived the
    // flag it named.
    auto const where = std::filesystem::path { "state" } / "node-a";
    auto state = Cluster::ClusterState {};
    state.members.push_back(Member("leader", "10.0.0.1:7100", KeyOf(0x01)));
    auto laptop = Member("joiner-a", "", KeyOf(0x42));
    laptop.seat = Cluster::MemberSeat::Learner;
    state.members.push_back(laptop);
    auto const roster = Cluster::EncodeRoster(Cluster::ProjectRoster(state));

    auto const member = DescribeAdmission(
        JoinerIdentity {
            .nodeId = "joiner-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Learner, .publicKey = KeyOf(0x42) },
        roster,
        where);
    REQUIRE(member.has_value());
    CHECK(member->contains("as a learner"));
    CHECK(member->contains(where.string()));
    CHECK(member->contains("nothing else"));
    CHECK(Testing::FlagsNoNodeRowAccepts(*member).empty());
}

TEST_CASE("An admission is believed only when the roster records this machine under its own key",
          "[enrollment][client][security]")
{
    auto const self = JoinerIdentity {
        .nodeId = "joiner-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Learner, .publicKey = KeyOf(0x42)
    };

    // The honest roster: the joiner is told what to compare and what was recorded.
    auto const roster = RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x42)));
    auto const admitted = DescribeAdmission(self, roster, "state");
    REQUIRE(admitted.has_value());
    CHECK(admitted->contains(Cluster::RenderRosterFingerprint(Cluster::DigestOfRoster(roster))));
    CHECK(admitted->contains("The seed recorded"));

    // **The refusal, and it is the one that makes the roster worth checking.** The same id
    // under ANOTHER key is either an operator who approved the wrong row or a reply somebody
    // rewrote -- and in both, every key the roster names is untrustworthy, so nothing it
    // says is printed as advice.
    auto const swapped = DescribeAdmission(self, RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x99))), "state");
    REQUIRE_FALSE(swapped.has_value());
    CHECK(swapped.error().contains("does not record"));
    CHECK(swapped.error().contains(FormatEd25519PublicKey(KeyOf(0x42))));
    CHECK_FALSE(swapped.error().contains("The seed recorded"));

    // And bytes that are not a roster are refused as that, never as an admission.
    auto const garbage = std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x02 } };
    CHECK_FALSE(DescribeAdmission(self, garbage, "state").has_value());
}

TEST_CASE("An admission asks the joiner to place nothing by hand", "[enrollment][client]")
{
    // #178 PR 4 left a member joiner needing `--cluster-key-file` placed by hand, and said so at
    // this moment; PR 6 retired the key, so the admission is the whole of joining. A sentence
    // still naming the flag would send an operator after a file nothing reads.
    auto const member = JoinerIdentity {
        .nodeId = "joiner-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Learner, .publicKey = KeyOf(0x42)
    };
    auto const admitted =
        DescribeAdmission(member, RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x42))), "state");
    REQUIRE(admitted.has_value());
    CHECK_FALSE(admitted->contains("--cluster-key-file"));
    // The control: the admission is really there, so the absence above is not an empty string's.
    CHECK(admitted->contains("The seed recorded"));
}

TEST_CASE("The pending list marks a row whose two addresses disagree, and leaves an honest one alone",
          "[enrollment][client][security]")
{
    // **The gate is a person reading this text, so the text is the gate.** Every other
    // case on this branch asserts what the node DECIDED; this one asserts what the
    // operator is shown before they decide, which is the only place the mismatch and
    // drift marks exist at all.
    //
    // BOTH directions, and that is the whole case rather than thoroughness: a renderer
    // that marked every row would pass a case that only checks the marked one, and it
    // would make the mark worthless at exactly forty rows -- which is the size at which
    // an operator stops reading and starts scanning for the marker.
    Wire::EnrollmentReport const report {
        .state = Wire::WireEnrollmentState::AutoApprove,
        .autoApproveSecondsLeft = 42 * 60,
        .pending = { Wire::EnrollmentPendingEntry { .nodeId = "honest",
                                                    .nodeEndpoint = "10.0.0.9:6674",
                                                    .peerId = "10.0.0.9",
                                                    .firstSeenSecondsAgo = 3,
                                                    .attempts = 2,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = KeyOf(0x11),
                                                    .rosterFingerprint = std::nullopt,
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "10.0.0.9" },
                     Wire::EnrollmentPendingEntry { .nodeId = "elsewhere",
                                                    .nodeEndpoint = "10.0.0.9:6674",
                                                    .peerId = "203.0.113.7",
                                                    .firstSeenSecondsAgo = 9,
                                                    .attempts = 5,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = KeyOf(0x22),
                                                    .rosterFingerprint = std::nullopt,
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "203.0.113.7" },
                     Wire::EnrollmentPendingEntry { .nodeId = "drifted",
                                                    .nodeEndpoint = "10.0.0.4:6674",
                                                    .peerId = "10.0.0.4",
                                                    .firstSeenSecondsAgo = 60,
                                                    .attempts = 30,
                                                    .claimsChanged = 4,
                                                    .decision = Wire::EnrollmentDecision::Approved,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = KeyOf(0x33),
                                                    .rosterFingerprint = Cluster::DigestOfRoster(
                                                        RosterWith(Member("drifted", "10.0.0.4:6680", KeyOf(0x33)))),
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "10.0.0.4" },
                     Wire::EnrollmentPendingEntry { .nodeId = "learner-w",
                                                    .nodeEndpoint = {},
                                                    .peerId = "10.0.0.5",
                                                    .firstSeenSecondsAgo = 1,
                                                    .attempts = 1,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = KeyOf(0x44),
                                                    .rosterFingerprint = std::nullopt,
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "10.0.0.5" } }
    };

    auto const text = RenderEnrollmentReport(report, "10.0.0.1:6675");

    // The window's own mode, because a list of rows with no heading is one an operator
    // can read while a deadline admits machines and act on as though a person decided each.
    CHECK(text.contains("auto-approve"));
    CHECK(text.contains("42 min left"));

    // Every row is present and carries BOTH addresses. The claimed endpoint and the
    // observed host are the comparison this list exists to make, so a renderer that
    // dropped either would leave the mark describing nothing.
    for (auto const& row: report.pending)
    {
        INFO("row " << row.nodeId);
        CHECK(text.contains(row.nodeId));
        CHECK(text.contains(row.peerId));
    }
    CHECK(text.contains("10.0.0.9:6674"));

    // The marks, per row and in both directions. `honest` and `elsewhere` claim the
    // SAME endpoint and differ only in where the request came from, so a renderer
    // keying on anything but that pair cannot separate them.
    // The WHOLE line, walked back to the preceding newline as well as forward to the
    // next one. The decision label is rendered to the LEFT of the id, so a helper that
    // only reads forward silently excludes it -- and the two assertions that need it
    // would then fail for a reason that has nothing to do with the renderer.
    auto const lineFor = [&text](std::string_view id) {
        auto const at = text.find(id);
        REQUIRE(at != std::string::npos);
        auto const begin = text.rfind('\n', at);
        auto const from = begin == std::string::npos ? 0 : begin + 1;
        auto const end = text.find('\n', at);
        return text.substr(from, end == std::string::npos ? std::string::npos : end - from);
    };

    CHECK(!lineFor("honest").contains("does not match"));
    CHECK(lineFor("elsewhere").contains("does not match"));

    // And the drift mark, which answers a different question: this row stopped tracking
    // the machine when somebody decided about it, and something has polled since
    // claiming otherwise. A row whose addresses agree can still have drifted, which is
    // why `drifted` is arranged with a matching pair.
    CHECK(!lineFor("drifted").contains("does not match"));
    CHECK(lineFor("drifted").contains("4 later poll"));
    CHECK(!lineFor("honest").contains("later poll"));

    // The decision label, because "approved" and "pending" send an operator to opposite
    // actions and the list is where they tell them apart.
    CHECK(lineFor("drifted").contains("approved"));
    CHECK(lineFor("honest").contains("pending"));

    // **The KEY, whole, for every row** (#178): it is the string an operator compares with
    // what the machine printed, and the comparison is the whole strength of an approval. A
    // prefix would be a comparison of the part an impostor can choose.
    for (auto const& row: report.pending)
    {
        INFO("row " << row.nodeId);
        CHECK(text.contains(FormatEd25519PublicKey(row.publicKey)));
    }

    // The roster fingerprint only where one was served -- absent renders nothing, never a
    // zero digest the joiner could not have printed.
    CHECK(text.contains(Cluster::RenderRosterFingerprint(Unwrap(report.pending[2].rosterFingerprint))));
    CHECK(text.find("SHA256:") == text.rfind("SHA256:"));

    // A learner states no endpoint, so it is shown as having none and carries no mismatch
    // mark: there is nothing to compare its host against.
    CHECK(lineFor("learner-w").contains("learner"));
    CHECK(lineFor("learner-w").contains("no endpoint"));
    CHECK_FALSE(lineFor("learner-w").contains("does not match"));
}

TEST_CASE("A window with nothing waiting renders as a reading and not as an empty page", "[enrollment][client]")
{
    // Absent is not zero, at the surface a person reads: a list that rendered nothing
    // for a manual window and nothing for an armed empty one would make an operator who
    // armed a deadline believe it had not taken.
    auto const manual = RenderEnrollmentReport(Wire::EnrollmentReport {}, "10.0.0.1:6675");
    CHECK(manual.contains("enrollment: manual"));
    CHECK(manual.contains("nothing waiting"));

    auto const armedAndEmpty = RenderEnrollmentReport(
        Wire::EnrollmentReport {
            .state = Wire::WireEnrollmentState::AutoApprove, .autoApproveSecondsLeft = 7 * 60, .pending = {} },
        "10.0.0.1:6675");
    CHECK(armedAndEmpty.contains("enrollment: auto-approve (7 min left)"));
    CHECK(armedAndEmpty.contains("nothing waiting"));
    CHECK(armedAndEmpty != manual);
}

namespace
{
constexpr std::string_view FirstScheduler = "10.0.0.1:7000";
constexpr std::string_view SecondScheduler = "10.0.0.2:7000";

/// A node answering `NotLeader` and naming where to go instead.
/// @param leader The endpoint the reply names.
/// @return The framed refusal.
[[nodiscard]] std::vector<std::byte> RedirectTo(std::string_view leader)
{
    return Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, std::string { leader });
}

/// A leader answering `--enroll-list` with a manual window and nothing waiting.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> QuietWindow()
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(Wire::EnrollmentReport {}));
}

/// An operator's node configured with both schedulers, in order.
/// @return The configuration.
[[nodiscard]] NodeConfig TwoSchedulers()
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.schedulers = { std::string { FirstScheduler }, std::string { SecondScheduler } };
    return cfg;
}
} // namespace

TEST_CASE("An enrollment command asks the next --scheduler when the first cannot be reached",
          "[enrollment][client][fallback]")
{
    // #1310: the operator's verbs read the same list the heartbeat does. Asserted by
    // WHICH endpoint was sent the request, because a verb that asked only the first
    // passes every case whose first entry answers.
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { {}, QuietWindow() } };
    Testing::PresentsNothing nothing;

    auto const listed = RunEnrollAdmin(cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, nothing, dialer);

    INFO("result: " << (listed.has_value() ? *listed : listed.error().reason));
    REQUIRE(listed.has_value());
    CHECK(listed->contains("manual"));
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler }, std::string { SecondScheduler } });
    CHECK(dialer.SentOn(0).empty());
    CHECK_FALSE(dialer.SentOn(1).empty());
}

TEST_CASE("An enrollment command with no --scheduler asks this machine's own node", "[enrollment][client][formation]")
{
    // A machine joins as a learner and finds its scheduler from the fleet it joined, so an
    // operator on a fleet member names nowhere: the verb asks this machine's own node, which
    // follows `NotLeader` to the leader. Asserted by WHICH endpoint was dialled.
    auto const cfg = Testing::FirstStart(NodeConfig {});
    REQUIRE(cfg.schedulers.empty());
    Testing::ScriptedDialer dialer { { QuietWindow() } };
    Testing::PresentsNothing nothing;

    auto const listed = RunEnrollAdmin(cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, nothing, dialer);

    INFO("result: " << (listed.has_value() ? *listed : listed.error().reason));
    REQUIRE(listed.has_value());
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { DefaultAdminTarget } });
}

TEST_CASE("An enrollment command that reached a scheduler is never sent to another", "[enrollment][client][fallback]")
{
    // The line `DialFirstReachable` draws: a fallback only where nothing was SENT.
    // `--enroll-approve` commits `ClusterAdmit` before it answers, so an approval that
    // may have been applied where it landed must not be proposed a second time elsewhere. The first scheduler connects and
    // then answers nothing readable; a second dial would run this script out, which the fake reports in its own voice.
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { std::vector<std::byte> { std::byte { 0xFF } } } };
    Testing::PresentsNothing nothing;

    auto const approved =
        RunEnrollAdmin(cfg,
                       EnrollCommand { .action = EnrollAction::Approve, .subject = "n9", .key = Ed25519PublicKey {} },
                       nothing,
                       dialer);

    REQUIRE_FALSE(approved.has_value());
    INFO("refusal: " << approved.error().reason);
    CHECK(approved.error().reason.contains(FirstScheduler));
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler } });
}

TEST_CASE("An enrollment command that reaches no --scheduler names every one it tried", "[enrollment][client][fallback]")
{
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { {}, {} } };
    Testing::PresentsNothing nothing;

    auto const listed = RunEnrollAdmin(cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, nothing, dialer);

    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().reason.contains(std::format("{}, {}", FirstScheduler, SecondScheduler)));
}

TEST_CASE("A NotLeader sends an enrollment command to the leader it names, not down the --scheduler list",
          "[enrollment][client][fallback]")
{
    // A redirect is an instruction. Consulting the list for it would send the request
    // to `SecondScheduler`, which the first has just said does not lead.
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { RedirectTo("10.0.0.9:7000"), QuietWindow() } };
    Testing::PresentsNothing nothing;

    auto const listed = RunEnrollAdmin(cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, nothing, dialer);

    REQUIRE(listed.has_value());
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler }, "10.0.0.9:7000" });
}

TEST_CASE("The enrollment list marks a row an armed window admitted, with when it was armed",
          "[enrollment][client][auto-approve]")
{
    auto row = Wire::EnrollmentPendingEntry { .nodeId = "laptop",
                                              .nodeEndpoint = {},
                                              .peerId = "10.1.2.3",
                                              .firstSeenSecondsAgo = 40,
                                              .attempts = 3,
                                              .claimsChanged = 0,
                                              .decision = Wire::EnrollmentDecision::Approved,
                                              .role = Wire::EnrollRole::Learner,
                                              .publicKey = KeyOf(0x42),
                                              .rosterFingerprint = std::nullopt,
                                              .autoApprovedArmedSecondsAgo = 120,
                                              .firstPeerId = "10.1.2.3" };
    auto const rendered = RenderEnrollmentReport(Wire::EnrollmentReport { .state = Wire::WireEnrollmentState::AutoApprove,
                                                                          .autoApproveSecondsLeft = 480,
                                                                          .pending = { row } },
                                                 "10.0.0.1:6675");
    CHECK(rendered.contains("enrollment: auto-approve (8 min left)"));
    CHECK(rendered.contains("auto-approved (window armed 120s ago)"));

    // The control: a row a person approved carries no such line.
    row.autoApprovedArmedSecondsAgo = std::nullopt;
    CHECK_FALSE(
        RenderEnrollmentReport(Wire::EnrollmentReport { .pending = { row } }, "10.0.0.1:6675").contains("auto-approved"));
}

TEST_CASE("The list prints the approval of each waiting row, ready to paste, naming its key", "[enrollment][client]")
{
    auto waiting = Wire::EnrollmentPendingEntry { .nodeId = "laptop",
                                                  .nodeEndpoint = {},
                                                  .peerId = "10.1.2.3",
                                                  .firstSeenSecondsAgo = 5,
                                                  .attempts = 2,
                                                  .claimsChanged = 0,
                                                  .decision = Wire::EnrollmentDecision::Pending,
                                                  .role = Wire::EnrollRole::Learner,
                                                  .publicKey = KeyOf(0x42),
                                                  .rosterFingerprint = std::nullopt,
                                                  .autoApprovedArmedSecondsAgo = std::nullopt,
                                                  .firstPeerId = "10.1.2.3" };
    auto decided = waiting;
    decided.nodeId = "desk";
    decided.decision = Wire::EnrollmentDecision::Approved;

    auto const rendered =
        RenderEnrollmentReport(Wire::EnrollmentReport { .pending = { waiting, decided } }, "10.0.0.1:6675");
    auto const line = std::format("fastcache-compile-node --scheduler=10.0.0.1:6675 --enroll-approve=laptop@{}",
                                  FormatEd25519PublicKey(KeyOf(0x42)));
    CHECK(rendered.contains(line));
    // Only a row nobody decided about: a decided one needs no approval.
    CHECK_FALSE(rendered.contains("--enroll-approve=desk@"));

    // And the line is one this binary PARSES back into the approval it names.
    auto const flag = std::format("--enroll-approve=laptop@{}", FormatEd25519PublicKey(KeyOf(0x42)));
    auto cfg = Testing::FirstStart(NodeConfig {});
    auto const argv = std::vector<char const*> { "--scheduler=10.0.0.1:6675", flag.c_str() };
    REQUIRE(ParseNodeCommandLine(std::span<char const* const> { argv }, cfg).has_value());
    CHECK(cfg.enroll.subject == "laptop");
    CHECK(cfg.enroll.key == std::optional { KeyOf(0x42) });
}

TEST_CASE("The list shows where each row first asked from and marks a row asked since from another address",
          "[enrollment][client][security]")
{
    // The per-host cap counts a row by the address it FIRST asked from, and a refusal names that
    // host. So every row shows it, and a row whose machine has since asked from somewhere else is
    // marked: otherwise a host's rows hide under another host's name, and the refusal names rows
    // nobody can find.
    auto const row = [](std::string_view id, std::string_view peer, std::string_view first) {
        return Wire::EnrollmentPendingEntry { .nodeId = std::string { id },
                                              .nodeEndpoint = {},
                                              .peerId = std::string { peer },
                                              .firstSeenSecondsAgo = 5,
                                              .attempts = 2,
                                              .claimsChanged = 0,
                                              .decision = Wire::EnrollmentDecision::Pending,
                                              .role = Wire::EnrollRole::Learner,
                                              .publicKey = KeyOf(0x42),
                                              .rosterFingerprint = std::nullopt,
                                              .autoApprovedArmedSecondsAgo = std::nullopt,
                                              .firstPeerId = std::string { first } };
    };
    auto const text = RenderEnrollmentReport(
        Wire::EnrollmentReport { .pending = { row("moved", "2001:db8::7", "198.51.100.7"),
                                              row("stayed", "198.51.100.8", "198.51.100.8"),
                                              row("mapped", "::ffff:198.51.100.9", "198.51.100.9") } },
        "10.0.0.1:6675");
    auto const lineFor = [&text](std::string_view id) {
        auto const at = text.find(std::format("  {}  ", id));
        REQUIRE(at != std::string::npos);
        auto const end = text.find('\n', at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    };

    // Every row shows its first address, beside the current one.
    CHECK(lineFor("moved").contains("from 2001:db8::7  first from 198.51.100.7"));
    CHECK(lineFor("stayed").contains("from 198.51.100.8  first from 198.51.100.8"));

    // Marked in one direction only: a renderer marking every row would pass a case that checked
    // the moved one alone. An IPv4-mapped spelling of the same address is no move, as the cap
    // folds it.
    CHECK(lineFor("moved").contains("asked since from another address"));
    CHECK_FALSE(lineFor("stayed").contains("asked since from another address"));
    CHECK_FALSE(lineFor("mapped").contains("asked since from another address"));
}

TEST_CASE("An enrollment command presents a ticket for each endpoint it asks, the leader a redirect names included",
          "[enrollment][client][ticket]")
{
    // A ticket names ONE audience, so the leader a `NotLeader` names is shown a ticket naming
    // IT: the scheduler's would be refused there as minted for somebody else.
    auto const cfg = TwoSchedulers();
    auto const authOk = Wire::EncodeReply(Wire::Status::Ok, {});
    auto behindAuth = [&](std::vector<std::byte> const& reply) {
        auto replies = authOk;
        replies.insert(replies.end(), reply.begin(), reply.end());
        return replies;
    };
    Testing::ScriptedDialer dialer { { behindAuth(RedirectTo("10.0.0.9:7000")), behindAuth(QuietWindow()) } };
    Testing::MintsForItsAudience node;
    Cc::TicketCredentials tickets {
        node, std::string { "127.0.0.1:6674" }, Cc::Credential {}, std::string {}, Cc::ExchangeBudget {}, {}
    };

    auto const listed = RunEnrollAdmin(cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, tickets, dialer);

    INFO("result: " << (listed.has_value() ? *listed : listed.error().reason));
    REQUIRE(listed.has_value());
    auto const asked = std::vector<std::string> { std::string { FirstScheduler }, "10.0.0.9:7000" };
    REQUIRE(dialer.Dialed() == asked);
    CHECK(node.audiences == asked);
    for (auto const index: { std::size_t { 0 }, std::size_t { 1 } })
    {
        INFO(asked[index]);
        auto const sent = dialer.SentOn(index);
        auto const header = Wire::DecodeRequestHeader(sent);
        REQUIRE(header.has_value());
        REQUIRE(Unwrap(header).opRaw == static_cast<std::uint8_t>(Wire::Op::Auth));
        auto const auth = Wire::DecodeAuthPayload(sent.subspan(Wire::RequestHeaderSize, Unwrap(header).payloadLength));
        REQUIRE(auth.has_value());
        CHECK(Unwrap(auth).kind == Wire::AuthKind::MachineTicket);
        CHECK(Wire::AsStringView(Unwrap(auth).secret) == Testing::TicketFor(asked[index]));
    }
}

TEST_CASE("An enrollment command refused for want of a ticket says why there was none", "[enrollment][client][ticket]")
{
    // The mint failed, so the ask went out unauthenticated and was refused `NotAMember`: what the
    // operator is told is the thing to fix -- this machine's node -- not the refusal it caused.
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, {}) } };
    Testing::MintsForItsAudience node;
    node.unreachableAfter = 0;
    Cc::TicketCredentials tickets {
        node, std::string { "127.0.0.1:6674" }, Cc::Credential {}, std::string {}, Cc::ExchangeBudget {}, {}
    };

    auto const refused = RunEnrollAdmin(cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, tickets, dialer);

    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason == Cc::ReasonFor(Cc::MintFailure::Unreachable));
}

TEST_CASE("An enrollment command that reached nobody is transient, and one the cluster refused is a decision",
          "[enrollment][client][exit]")
{
    auto const cfg = TwoSchedulers();
    auto const list = EnrollCommand { .action = EnrollAction::List, .subject = {} };
    Testing::PresentsNothing nothing;

    SECTION("no scheduler could be reached")
    {
        Testing::ScriptedDialer dialer { { {}, {} } };
        auto const refused = RunEnrollAdmin(cfg, list, nothing, dialer);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().ending == CommandEnding::Failed);
    }

    SECTION("the scheduler replied with a refusal")
    {
        Testing::ScriptedDialer dialer { { Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "not one of ours") } };
        auto const refused = RunEnrollAdmin(cfg, list, nothing, dialer);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().ending == CommandEnding::Declined);
    }

    // Two nodes each holding a stale `_knownLeader` name each other until leadership settles,
    // which it does by itself: giving up on the chain is transient, not the cluster's decision.
    SECTION("a redirect chain that did not settle: transient")
    {
        Testing::ScriptedDialer dialer { {
            RedirectTo("10.0.0.2:7000"),
            RedirectTo("10.0.0.1:7000"),
            RedirectTo("10.0.0.2:7000"),
            RedirectTo("10.0.0.1:7000"),
        } };
        auto const refused = RunEnrollAdmin(cfg, list, nothing, dialer);
        REQUIRE_FALSE(refused.has_value());
        INFO(refused.error().reason);
        CHECK(refused.error().reason.contains("gave up after"));
        CHECK(refused.error().ending == CommandEnding::Failed);
    }
}
