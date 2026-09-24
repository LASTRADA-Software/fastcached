// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// An identity key whose every byte is @p fill, so two cases' keys differ visibly.
/// @param fill The byte.
/// @return The key.
[[nodiscard]] std::array<std::byte, Wire::IdentityPublicKeyBytes> KeyOf(std::uint8_t fill)
{
    std::array<std::byte, Wire::IdentityPublicKeyBytes> key {};
    key.fill(static_cast<std::byte>(fill));
    return key;
}

/// The key most cases ask under.
/// @return The key.
[[nodiscard]] std::array<std::byte, Wire::IdentityPublicKeyBytes> TheKey()
{
    return KeyOf(0x11);
}

/// A member's claim under @p key.
/// @param id The identity it claims.
/// @param endpoint The consensus address it claims.
/// @param key The key it asks under.
/// @return The claim.
[[nodiscard]] JoinerClaim Claim(std::string_view id,
                                std::string_view endpoint,
                                std::array<std::byte, Wire::IdentityPublicKeyBytes> const& key)
{
    return JoinerClaim { .nodeId = id, .raftEndpoint = endpoint, .role = Wire::EnrollRole::Member, .publicKey = key };
}

/// One offer from a machine that is not already on the list.
/// @param window The window to ask.
/// @param id The identity the joiner claims.
/// @return What the window decided.
[[nodiscard]] EnrollDecision Offer(EnrollmentWindow& window, std::string_view id)
{
    return window.Offer(Claim(id, "10.0.0.9:7100", TheKey()), "10.0.0.9");
}
} // namespace

TEST_CASE("A window is closed until somebody opens it, and a restart is what closes it again", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // The DEFAULT is the case worth pinning: the window is a runtime decision and a
    // process that has just started has taken none.
    CHECK(window.Summary().first == Wire::WireEnrollmentState::Closed);
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Closed);

    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    CHECK(window.Summary().first == Wire::WireEnrollmentState::Open);
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Pending);
    CHECK(window.Summary().second == 1);

    // A RESTART, modelled the one way a restart can be modelled here: the window lives
    // in this object and nowhere else, so a fresh one IS a restarted process. That the
    // new one is closed and empty is the whole of "a restart closes it", and it is
    // asserted rather than argued because the alternative -- persisting it -- is the
    // design this feature deliberately did not take.
    EnrollmentWindow restarted { clock };
    CHECK(restarted.Summary().first == Wire::WireEnrollmentState::Closed);
    CHECK(restarted.Summary().second == 0);
    CHECK(Offer(restarted, "joiner-a") == EnrollDecision::Closed);
}

TEST_CASE("Closing forgets what was waiting, so a later window cannot approve an older request", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    REQUIRE(window.Summary().second == 1);

    REQUIRE(window.Close() == EnrollControlOutcome::Done);
    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    // The list, and not merely the state: a request made while nobody was watching
    // must not be approvable after they stopped.
    CHECK(window.Summary().second == 0);
    CHECK(!window.Find("joiner-a").has_value());
}

TEST_CASE("Opening an open window changes nothing and says so, rather than restarting its age", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    clock.advance(std::chrono::seconds { 90 });

    // `AlreadyInForce` rather than `Done`, and the AGE is what makes it matter: an
    // operator running the verb twice has not asked for the warning clock to be reset,
    // and a reset would let a window be held open indefinitely with its age never
    // growing past the interval.
    CHECK(window.Open() == EnrollControlOutcome::AlreadyInForce);
    CHECK(window.Report().openForSeconds == 90);

    REQUIRE(window.Close() == EnrollControlOutcome::Done);
    CHECK(window.Close() == EnrollControlOutcome::AlreadyInForce);
}

TEST_CASE("The pending list refuses past its bound and keeps the machine that arrived first", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollments))
        REQUIRE(Offer(window, std::format("joiner-{}", index)) == EnrollDecision::Pending);
    REQUIRE(window.Summary().second == MaxPendingEnrollments);

    // The refusal, and then the half a suite skips: that the bound REFUSED rather than
    // EVICTED. Asserting only the refusal passes under an implementation that evicts
    // and then refuses, which is the silent failure the bound exists to prevent -- a
    // flooder pushing the real joiner off the list the operator is reading.
    CHECK(Offer(window, "joiner-late") == EnrollDecision::Full);
    CHECK(window.Find("joiner-0").has_value());
    CHECK(window.Summary().second == MaxPendingEnrollments);

    // And a machine ALREADY on the list goes on polling while the list is full, which
    // is what keeps a flood from silencing the joiners that arrived before it.
    CHECK(Offer(window, "joiner-0") == EnrollDecision::Pending);
}

TEST_CASE("A flooder fills the list and the genuine joiner is refused and RECORDED NOWHERE",
          "[enrollment][window][security]")
{
    // **The same bound arranged from the other side, and it is not the same case.** The
    // one above fills the list with legitimate joiners and asks what happens to the
    // sixty-fifth; this fills it with junk from one stranger and asks what happens to
    // the machine an operator is actually waiting for. Only this arrangement can show
    // the cost, and after a decided row stopped refreshing its claim the two stopped
    // being interchangeable at all.
    //
    // It needs no id knowledge and no operator error: anybody who can reach the port
    // while a window is open can do it. That is the accepted half of refusing rather
    // than evicting -- evicting would let the flooder push the real joiner OFF the list
    // the operator is reading, which is silent from both ends, where this is visible as
    // sixty-four rows nobody recognises and a counter that moved.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollments))
        REQUIRE(window.Offer(Claim(std::format("junk-{}", index), "10.0.0.9:7100", KeyOf(0x66)), "203.0.113.7")
                == EnrollDecision::Pending);

    // The real machine arrives second and is not merely refused -- it leaves NO ROW, so
    // it cannot be approved, and an operator reading `--enroll-list` sees sixty-four
    // strangers and no sign of the joiner they are waiting for.
    CHECK(window.Offer(Claim("joiner-real", "10.0.0.4:6680", TheKey()), "10.0.0.4") == EnrollDecision::Full);
    CHECK_FALSE(window.Find("joiner-real").has_value());

    // The flood IS visible, which is what makes the trade defensible: every row carries
    // the host it came from, and sixty-four sharing one is the shape to look for.
    auto const report = window.Report();
    REQUIRE(report.pending.size() == MaxPendingEnrollments);
    CHECK(std::ranges::all_of(report.pending,
                              [](Wire::EnrollmentPendingEntry const& row) { return row.peerId == "203.0.113.7"; }));

    // And closing the window is the remedy: it forgets the flood, so the operator opens
    // it again and the genuine joiner's next poll is recorded.
    REQUIRE(window.Close() == EnrollControlOutcome::Done);
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    CHECK(window.Offer(Claim("joiner-real", "10.0.0.4:6680", TheKey()), "10.0.0.4") == EnrollDecision::Pending);
}

TEST_CASE("A joiner polls, so repeat offers count attempts and refresh what it claims", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    REQUIRE(window.Offer(Claim("joiner-a", "10.0.0.9:7100", TheKey()), "10.0.0.9") == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("joiner-a", "10.0.0.9:7100", TheKey()), "10.0.0.9") == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("joiner-a", "node-a.example:7100", TheKey()), "198.51.100.4") == EnrollDecision::Pending);

    // One row for one id, however many times it asked.
    REQUIRE(window.Summary().second == 1);
    auto const entry = window.Find("joiner-a");
    REQUIRE(entry.has_value());
    CHECK(Unwrap(entry).attempts == 3);

    // The LATEST claim, WHILE THE ROW IS STILL PENDING. A machine that renumbers itself
    // before anybody has looked at it should be read as it is now, not as it first
    // announced itself.
    CHECK(Unwrap(entry).raftEndpoint == "node-a.example:7100");
    CHECK(Unwrap(entry).peerId == "198.51.100.4");
    CHECK(Unwrap(entry).claimsChanged == 0);
}

TEST_CASE("A decided row stops tracking the machine, so what was approved is what travels", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(window.Offer(Claim("joiner-a", "10.0.0.9:7100", TheKey()), "10.0.0.9") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);

    // **The assertion that discriminates.** `--enroll-list` and `--enroll-approve` are
    // two one-shot processes minutes apart, and the joiner polls every couple of seconds
    // in between -- so while the claim refreshed unconditionally, the endpoint handed to
    // `ClusterAdmit` was not necessarily the endpoint the eye that approved it had read.
    // A test asserting only that the row still exists passes under that; this one
    // asserts the VALUE, which is the thing that travels.
    REQUIRE(window.Offer(Claim("joiner-a", "evil.example:7100", TheKey()), "198.51.100.4") == EnrollDecision::Approved);

    auto const entry = window.Find("joiner-a");
    REQUIRE(entry.has_value());
    CHECK(Unwrap(entry).raftEndpoint == "10.0.0.9:7100");
    CHECK(Unwrap(entry).peerId == "10.0.0.9");

    // Marked, and NEVER gated -- #242 settled that comparing hosts refuses the
    // documented setup and stops only a third host. The poll above was still answered;
    // what it changed is what an operator sees in the row.
    CHECK(Unwrap(entry).claimsChanged == 1);
}

TEST_CASE("An approval is answered on every poll, because what it leads to is no secret", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);

    // **Every poll, and the row does not move** (#178). While an approval led to the cluster
    // key it was spendable once, and a joiner whose one reply was lost was stranded until an
    // operator re-approved it. What an approval leads to now is the ROSTER, which is no secret,
    // so answering it again costs nothing and a lost reply is recovered by the joiner simply
    // asking again. Asserting "the first poll is answered" passes under the old spend; the
    // SECOND and THIRD answers are the ones that tell the two apart.
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Approved);
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Approved);
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Approved);
    CHECK(Unwrap(window.Find("joiner-a")).decision == Wire::EnrollmentDecision::Approved);
}

TEST_CASE("A poll under another KEY is another machine: counted, held, and never recorded", "[enrollment][window][security]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(window.Offer(Claim("joiner-a", "10.0.0.9:7100", TheKey()), "10.0.0.9") == EnrollDecision::Pending);

    // **While the row is still PENDING**, which is the arrangement that matters: an operator
    // reads the key on `--enroll-list`, compares it with what the machine printed, and
    // approves minutes later. A row that took whichever key polled LAST would let a second
    // machine swap in a key nobody compared, between the reading and the approval.
    auto const impostor = KeyOf(0x99);
    CHECK(window.Offer(Claim("joiner-a", "10.0.0.9:7100", impostor), "10.0.0.9") == EnrollDecision::Pending);
    CHECK(Unwrap(window.Find("joiner-a")).publicKey == TheKey());
    CHECK(Unwrap(window.Find("joiner-a")).claimsChanged == 1);

    // And after the approval the impostor is STILL answered `Pending` -- never `Approved`,
    // which is what would send it to fetch a roster it is not on and tell it it was admitted.
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(window.Offer(Claim("joiner-a", "10.0.0.9:7100", impostor), "10.0.0.9") == EnrollDecision::Pending);
    CHECK(Unwrap(window.Find("joiner-a")).publicKey == TheKey());
    CHECK(Unwrap(window.Find("joiner-a")).claimsChanged == 2);

    // The control: the machine that asked first, under the key that was compared, is the one
    // the approval answers. Without it the two checks above pass under a window that answers
    // `Pending` to everybody.
    CHECK(window.Offer(Claim("joiner-a", "10.0.0.9:7100", TheKey()), "10.0.0.9") == EnrollDecision::Approved);

    // A ROLE is held the same way: a worker's request under a member's id is another claim.
    auto worker = Claim("joiner-a", "", TheKey());
    worker.role = Wire::EnrollRole::Worker;
    CHECK(window.Offer(worker, "10.0.0.9") == EnrollDecision::Pending);
    CHECK(Unwrap(window.Find("joiner-a")).role == Wire::EnrollRole::Member);
}

TEST_CASE("The roster fingerprint a joiner was handed is recorded on its row", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);

    // Absent until something was served -- a disengaged optional, never a zero digest, which
    // would render as a fingerprint the joiner could never have printed.
    CHECK_FALSE(Unwrap(window.Find("joiner-a")).rosterFingerprint.has_value());

    std::array<std::byte, Wire::RosterFingerprintBytes> first {};
    first.fill(std::byte { 0x01 });
    std::array<std::byte, Wire::RosterFingerprintBytes> second {};
    second.fill(std::byte { 0x02 });

    // The LATEST wins, because the latest is the one the joiner holds: it stops polling once
    // it has been told `Approved`, and a roster that moved between two answers is the one it
    // printed last.
    window.NoteServed("joiner-a", first);
    window.NoteServed("joiner-a", second);
    CHECK(Unwrap(window.Find("joiner-a")).rosterFingerprint == second);
    auto const report = window.Report();
    REQUIRE(report.pending.size() == 1);
    CHECK(report.pending.front().rosterFingerprint == second);

    // An id with no row records nothing and creates nothing.
    window.NoteServed("nobody", first);
    CHECK(window.Summary().second == 1);
}

TEST_CASE("A roster records a joiner only under the key and the role it asked for", "[enrollment][window]")
{
    // The one question both ends ask -- the leader before it answers `Approved`, the joiner before
    // it believes it -- so it is pinned once, here, in every direction it can be wrong.
    auto roster = Cluster::Roster {};
    roster.members.push_back(Cluster::RosterMember {
        .id = "n1", .raftEndpoint = "10.0.0.1:6680", .seat = Cluster::MemberSeat::Voter, .publicKey = KeyOf(0x01) });
    roster.members.push_back(Cluster::RosterMember {
        .id = "n2", .raftEndpoint = "10.0.0.2:6680", .seat = Cluster::MemberSeat::Voter, .publicKey = std::nullopt });
    roster.principals.push_back(
        Cluster::ClusterPrincipal { .id = "w1", .publicKey = KeyOf(0x11), .role = Cluster::PrincipalRole::Worker });

    CHECK(RosterRecordsJoiner(roster, "n1", KeyOf(0x01), Wire::EnrollRole::Member));
    CHECK(RosterRecordsJoiner(roster, "w1", KeyOf(0x11), Wire::EnrollRole::Worker));

    // Another key under the same id is not this machine.
    CHECK_FALSE(RosterRecordsJoiner(roster, "n1", KeyOf(0x99), Wire::EnrollRole::Member));

    // A principal is not a member and a member is not a principal: a worker counted towards
    // quorum is a vote nobody can collect.
    CHECK_FALSE(RosterRecordsJoiner(roster, "w1", KeyOf(0x11), Wire::EnrollRole::Member));
    CHECK_FALSE(RosterRecordsJoiner(roster, "n1", KeyOf(0x01), Wire::EnrollRole::Worker));

    // A member with no key recorded records no key, whatever the joiner holds.
    CHECK_FALSE(RosterRecordsJoiner(roster, "n2", KeyOf(0x02), Wire::EnrollRole::Member));
}

TEST_CASE("Each enrollment role says whether it states an endpoint and what an approval records", "[enrollment][window]")
{
    // The two roles' difference is what the responder refuses and what the approval commits,
    // so it is pinned per row rather than inferred from a name.
    auto const& member = EnrollRoleRowFor(Wire::EnrollRole::Member);
    CHECK(member.name == "member");
    CHECK(member.statesEndpoint);
    CHECK_FALSE(member.principal.has_value());

    auto const& worker = EnrollRoleRowFor(Wire::EnrollRole::Worker);
    CHECK(worker.name == "worker");
    CHECK_FALSE(worker.statesEndpoint);
    CHECK(worker.principal == Cluster::PrincipalRole::Worker);
}

TEST_CASE("A second decision about a settled id is refused rather than repeated", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);

    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::AlreadyInForce);

    // Still `AlreadyInForce` after the joiner has been answered: nothing an answer does
    // moves the row any more (#178), so there is nothing a second approval could re-arm.
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Approved);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::AlreadyInForce);

    // A REJECTED row may still be approved, because an operator who refused the wrong
    // row must not have to close the window -- which would lose every other row with it.
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-b", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);
    CHECK(Offer(window, "joiner-b") == EnrollDecision::Rejected);
    CHECK(window.Decide("joiner-b", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(Offer(window, "joiner-b") == EnrollDecision::Approved);
}

TEST_CASE("Deciding about a machine nobody has heard of, or while shut, is refused by name", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // Two refusals that a `bool` would render alike, and they send an operator to
    // different places: one says open a window, the other says check the id.
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Closed);
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::UnknownSubject);
}

TEST_CASE("The open warning is due immediately and then once per interval, never in a burst", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // Nothing is owed by a shut window, which is the reading that would otherwise make
    // the driver log about a state that does not exist.
    CHECK(!window.TakeDueWarning().has_value());

    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    // Due at the OPEN, not an interval later: an operator who opens a window and
    // watches the log for a minute before seeing anything concludes it did not work.
    auto const first = window.TakeDueWarning();
    REQUIRE(first.has_value());
    CHECK(Unwrap(first).contains("OPEN"));

    // Consumed, so asking again immediately owes nothing.
    CHECK(!window.TakeDueWarning().has_value());

    clock.advance(EnrollmentWarningInterval - std::chrono::seconds { 1 });
    CHECK(!window.TakeDueWarning().has_value());
    clock.advance(std::chrono::seconds { 1 });
    CHECK(window.TakeDueWarning().has_value());

    // A driver that was LATE, or a clock that jumped, must not then owe a burst for
    // every interval it slept through: the deadline advances from NOW. Ten intervals
    // pass and exactly one line is owed, which is the reading an operator wants --
    // *it is still open* -- rather than ten copies of it.
    clock.advance(EnrollmentWarningInterval * 10);
    CHECK(window.TakeDueWarning().has_value());
    CHECK(!window.TakeDueWarning().has_value());

    // And closing stops it, rather than leaving a deadline that fires once more.
    REQUIRE(window.Close() == EnrollControlOutcome::Done);
    clock.advance(EnrollmentWarningInterval * 2);
    CHECK(!window.TakeDueWarning().has_value());
}

TEST_CASE("The warning states the age and how many are waiting, so a log line can be acted on", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-b", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);

    clock.advance(std::chrono::seconds { 120 });
    auto const line = window.TakeDueWarning();
    REQUIRE(line.has_value());

    // The age, so somebody reading at 03:00 knows whether this started at 02:59 or at
    // 14:00; and the two counts apart, because WAITING is what an operator has to act
    // on and LISTED includes the rows they have already decided.
    CHECK(Unwrap(line).contains("120s"));
    CHECK(Unwrap(line).contains("1 request(s) waiting"));
    CHECK(Unwrap(line).contains("2 listed"));
    // And what to compare before approving, which is the whole of what makes approving safe.
    CHECK(Unwrap(line).contains("compare that key"));
}

TEST_CASE("A report carries ages as durations and the state the wire spells", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // A closed window reports a zero age rather than an age since some instant that
    // means nothing, and holds no rows to report.
    auto const shut = window.Report();
    CHECK(shut.state == Wire::WireEnrollmentState::Closed);
    CHECK(shut.openForSeconds == 0);
    CHECK(shut.pending.empty());

    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    clock.advance(std::chrono::seconds { 45 });
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    clock.advance(std::chrono::seconds { 15 });

    auto const report = window.Report();
    REQUIRE(report.pending.size() == 2);
    CHECK(report.state == Wire::WireEnrollmentState::Open);
    CHECK(report.openForSeconds == 60);

    // Oldest first, and each age measured from when that row FIRST asked rather than
    // from when the window opened -- a row that arrived late must not read as having
    // waited since the beginning.
    CHECK(report.pending[0].nodeId == "joiner-a");
    CHECK(report.pending[0].firstSeenSecondsAgo == 60);
    CHECK(report.pending[1].nodeId == "joiner-b");
    CHECK(report.pending[1].firstSeenSecondsAgo == 15);
}

TEST_CASE("An open window is a LIVE condition: raised on open, clear on close, raised again",
          "[enrollment][window][conditions]")
{
    // #1364. The repeating Warn reaches only whoever reads this node's log; the row reaches the
    // leader's page and `node-conditions`. LIVE, because watching it clear is the progress an
    // operator is waiting for -- so the case drives it round twice, which a latched row cannot do.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    EnrollmentWindow window { clock, &conditions };

    // Checked at construction, not assumed: a process starts with no window open.
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);

    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Raised);
    REQUIRE(window.Close() == EnrollControlOutcome::Done);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Raised);
}

TEST_CASE("A window on a node that serves none reports nothing about itself", "[enrollment][window][conditions]")
{
    // Handed no registry -- `main`'s `AddressWhen(servesEnrollment, ...)` on a node with no cluster
    // -- the window touches nothing, so its row is left for the scope to answer `not-evaluated`
    // rather than a reassuring `clear` about a window nothing can open.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    EnrollmentWindow window { clock, nullptr };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Undecided);
}
