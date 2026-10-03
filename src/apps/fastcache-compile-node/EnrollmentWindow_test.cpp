// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

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

/// A learner's claim under @p key.
/// @param id The identity it claims.
/// @param endpoint The endpoint it claims.
/// @param key The key it asks under.
/// @return The claim.
[[nodiscard]] JoinerClaim Claim(std::string_view id,
                                std::string_view endpoint,
                                std::array<std::byte, Wire::IdentityPublicKeyBytes> const& key)
{
    return JoinerClaim { .nodeId = id, .nodeEndpoint = endpoint, .role = Wire::EnrollRole::Learner, .publicKey = key };
}

/// The detail a raised condition carries, or empty.
/// @param conditions The registry.
/// @param condition Which row.
/// @return Its detail.
[[nodiscard]] std::string DetailOf(NodeConditions const& conditions, NodeCondition condition)
{
    auto const rows = conditions.Snapshot();
    auto const row = std::ranges::find(rows, RowFor(condition).id, &Wire::NodeConditionFields::id);
    return row == rows.end() ? std::string {} : row->detail;
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

TEST_CASE("A joiner is recorded with no window opened", "[enrollment][window][formation]")
{
    // Decision D1: the approval is the gate since #178, so nothing is opened first. The row is
    // recorded under the key and the role it asked with, which is what an operator compares.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    CHECK(Offer(window, "laptop") == EnrollDecision::Pending);
    REQUIRE(window.Find("laptop").has_value());
    CHECK(Unwrap(window.Find("laptop")).role == Wire::EnrollRole::Learner);
    CHECK(Unwrap(window.Find("laptop")).publicKey == TheKey());
}

TEST_CASE("A row nobody polled for ten minutes is forgotten and the next poll records it afresh",
          "[enrollment][window][formation]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(Offer(window, "laptop") == EnrollDecision::Pending);
    REQUIRE(Offer(window, "laptop") == EnrollDecision::Pending);

    // Exactly the lifetime is still kept: the bound is "longer than", so a joiner polling on
    // the boundary is not forgotten between two of its own polls.
    clock.advance(PendingRowLifetime);
    CHECK(window.Report().pending.size() == 1);

    clock.advance(std::chrono::seconds { 1 });
    CHECK(window.Report().pending.empty());
    CHECK(Offer(window, "laptop") == EnrollDecision::Pending);
    CHECK(Unwrap(window.Find("laptop")).attempts == 1);
}

TEST_CASE("A machine that keeps asking is kept however long it waits", "[enrollment][window][formation]")
{
    // The lifetime is measured from the LAST poll: a joiner waiting an hour for a person is
    // exactly the machine that must still be on the list when the person arrives.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(Offer(window, "laptop") == EnrollDecision::Pending);
    for (auto const minute: std::views::iota(0, 60))
    {
        INFO("minute " << minute);
        clock.advance(std::chrono::minutes { 1 });
        REQUIRE(Offer(window, "laptop") == EnrollDecision::Pending);
    }
    CHECK(Unwrap(window.Find("laptop")).attempts == 61);
    CHECK(Unwrap(window.Find("laptop")).firstSeenSecondsAgo == 3600);
}

TEST_CASE("An expired row is counted when it goes, and only while nobody had decided about it",
          "[enrollment][window][formation]")
{
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    EnrollmentWindow window { clock, nullptr, &metrics };
    REQUIRE(Offer(window, "laptop") == EnrollDecision::Pending);
    REQUIRE(Offer(window, "desk") == EnrollDecision::Pending);
    REQUIRE(window.Decide("desk", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);

    clock.advance(PendingRowLifetime + std::chrono::seconds { 1 });
    CHECK(window.Report().pending.empty());
    // The waiting row is the event: a request the leader forgot with nobody having answered
    // it. The decided one went because its machine had its answer and stopped asking.
    CHECK(metrics.Read(IMetricsSink::Counter::EnrollmentRequestsExpired) == 1);
}

TEST_CASE("The pending list refuses past its bound and keeps the machine that arrived first", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // Each from a host of its own, so the list's bound is what is reached and not a host's.
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollments))
        REQUIRE(window.Offer(Claim(std::format("joiner-{}", index), "", TheKey()), std::format("10.0.1.{}", index))
                == EnrollDecision::Pending);
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
    CHECK(window.Offer(Claim("joiner-0", "", TheKey()), "10.0.1.0") == EnrollDecision::Pending);
}

TEST_CASE("A flooder from one host holds its cap and the genuine joiner is still recorded", "[enrollment][window][security]")
{
    // **The bound arranged from the other side.** One stranger that keeps polling under fresh ids
    // used to hold every row forever, refusing every real joiner `Full` until the leader
    // restarted. A host now holds `MaxPendingEnrollmentsPerHost` undecided rows and no more.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
        REQUIRE(window.Offer(Claim(std::format("junk-{}", index), "", KeyOf(0x66)), "203.0.113.7")
                == EnrollDecision::Pending);

    // The flooder's next row is refused as ITS HOST's, not as the list's.
    CHECK(window.Offer(Claim("junk-more", "", KeyOf(0x66)), "203.0.113.7") == EnrollDecision::HostFull);
    CHECK_FALSE(window.Find("junk-more").has_value());

    // A second host still enrolls, which is the property: the real machine gets a row.
    CHECK(window.Offer(Claim("joiner-real", "", TheKey()), "10.0.0.4") == EnrollDecision::Pending);
    CHECK(window.Find("joiner-real").has_value());

    // And a flooder already on the list goes on polling its OWN rows, as any joiner does.
    CHECK(window.Offer(Claim("junk-0", "", KeyOf(0x66)), "203.0.113.7") == EnrollDecision::Pending);
}

TEST_CASE("A host's cap counts only undecided rows", "[enrollment][window][security]")
{
    // A decided row is a record, and its machine has its answer: a NAT with more machines behind
    // it than the cap enrolls them a few at a time as its rows are decided.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
        REQUIRE(window.Offer(Claim(std::format("office-{}", index), "", KeyOf(0x20)), "192.0.2.1")
                == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("office-next", "", KeyOf(0x20)), "192.0.2.1") == EnrollDecision::HostFull);

    REQUIRE(window.Decide("office-0", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(window.Offer(Claim("office-next", "", KeyOf(0x20)), "192.0.2.1") == EnrollDecision::Pending);
}

TEST_CASE("Clearing drops every undecided row and keeps the decided ones", "[enrollment][window][security]")
{
    core::platform::ManualClock clock;
    NodeConditions conditions;
    EnrollmentWindow window { clock, &conditions };
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {}); // the list is the leader's
    REQUIRE(window.Offer(Claim("waiting-a", "", KeyOf(0x31)), "192.0.2.1") == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("approved", "", KeyOf(0x32)), "192.0.2.2") == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("rejected", "", KeyOf(0x33)), "192.0.2.3") == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("waiting-b", "", KeyOf(0x34)), "192.0.2.4") == EnrollDecision::Pending);
    REQUIRE(window.Decide("approved", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    REQUIRE(window.Decide("rejected", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);
    REQUIRE(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Raised);

    CHECK(window.ClearPending() == std::vector<std::string> { "waiting-a", "waiting-b" });

    auto const report = window.Report();
    REQUIRE(report.pending.size() == 2);
    CHECK(report.pending[0].nodeId == "approved");
    CHECK(report.pending[0].decision == Wire::EnrollmentDecision::Approved);
    CHECK(report.pending[1].nodeId == "rejected");
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Clear);

    // It bans nobody: a machine still asking is recorded afresh at its next poll.
    CHECK(window.Offer(Claim("waiting-a", "", KeyOf(0x31)), "192.0.2.1") == EnrollDecision::Pending);
    CHECK(Unwrap(window.Find("waiting-a")).attempts == 1);

    // And clearing a list with nothing undecided on it drops nothing.
    REQUIRE(window.Decide("waiting-a", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);
    CHECK(window.ClearPending().empty());
    CHECK(window.Report().pending.size() == 3);
}

TEST_CASE("A joiner polls, so repeat offers count attempts and refresh what it claims", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

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
    CHECK(Unwrap(entry).nodeEndpoint == "node-a.example:7100");
    CHECK(Unwrap(entry).peerId == "198.51.100.4");
    CHECK(Unwrap(entry).claimsChanged == 0);
}

TEST_CASE("A decided row stops tracking the machine, so what was approved is what travels", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
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
    CHECK(Unwrap(entry).nodeEndpoint == "10.0.0.9:7100");
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
}

TEST_CASE("The roster fingerprint a joiner was handed is recorded on its row", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
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

TEST_CASE("A roster records a joiner only as a member under the key it asked with", "[enrollment][window]")
{
    // The one question both ends ask -- the leader before it answers `Approved`, the joiner before
    // it believes it -- so it is pinned once, here, in every direction it can be wrong.
    auto roster = Cluster::Roster {};
    roster.members.push_back(Cluster::RosterMember {
        .id = "n1", .raftEndpoint = "10.0.0.1:6680", .seat = Cluster::MemberSeat::Voter, .publicKey = KeyOf(0x01) });

    CHECK(RosterRecordsJoiner(roster, "n1", KeyOf(0x01), Wire::EnrollRole::Learner));

    // Another key under the same id is not this machine.
    CHECK_FALSE(RosterRecordsJoiner(roster, "n1", KeyOf(0x99), Wire::EnrollRole::Learner));

    // A role no row serves is admitted as nothing, whatever the roster records under its id: the
    // wire still decodes the retired roles, and none of them is a member.
    for (auto const role: Wire::KnownEnrollRoles)
    {
        INFO("role byte " << static_cast<int>(role));
        if (!ServesEnrollRole(role))
            CHECK_FALSE(RosterRecordsJoiner(roster, "n1", KeyOf(0x01), role));
    }
}

TEST_CASE("The role table has one row and it seats a learner", "[enrollment][window][formation]")
{
    // A machine joins ONE way, as a learner: what the responder refuses and what an approval
    // commits are this row, pinned rather than inferred from a name.
    REQUIRE(EnrollRoleTable.size() == 1);
    auto const& learner = EnrollRoleTable.front();
    CHECK(learner.role == Wire::EnrollRole::Learner);
    CHECK(learner.name == "learner");
    CHECK_FALSE(learner.statesEndpoint);
    CHECK(learner.seat == Cluster::MemberSeat::Learner);

    // Every other role the wire still decodes is one this build refuses, and the learner is not.
    auto served = 0;
    for (auto const role: Wire::KnownEnrollRoles)
    {
        INFO("role byte " << static_cast<int>(role));
        CHECK(ServesEnrollRole(role) == (role == Wire::EnrollRole::Learner));
        served += ServesEnrollRole(role) ? 1 : 0;
    }
    CHECK(served == 1);
}

TEST_CASE("A second decision about a settled id is refused rather than repeated", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);

    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::AlreadyInForce);

    // Still `AlreadyInForce` after the joiner has been answered: nothing an answer does
    // moves the row any more (#178), so there is nothing a second approval could re-arm.
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Approved);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::AlreadyInForce);

    // A REJECTED row may still be approved, because an operator who refused the wrong
    // row must not have to wait for it to be forgotten and the machine to ask again.
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-b", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);
    CHECK(Offer(window, "joiner-b") == EnrollDecision::Rejected);
    CHECK(window.Decide("joiner-b", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(Offer(window, "joiner-b") == EnrollDecision::Approved);
}

TEST_CASE("Deciding about a machine nobody has heard of is refused by name", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // A refusal that says check the id, rather than an `AlreadyInForce` that would read as
    // the decision having been taken.
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::UnknownSubject);
}

TEST_CASE("The waiting warning is due when a machine starts waiting and then once per interval, never in a burst",
          "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // Nothing is owed while nobody waits, which is the reading that would otherwise make the
    // driver log about a state that does not exist.
    CHECK(!window.TakeDueWarning().has_value());

    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);

    // Due at the first ASK, not an interval later: somebody who started a join and watches
    // the log for a minute before seeing anything concludes it did not work.
    auto const first = window.TakeDueWarning();
    REQUIRE(first.has_value());
    CHECK(Unwrap(first).contains("joiner-a"));

    // Consumed, so asking again immediately owes nothing.
    CHECK(!window.TakeDueWarning().has_value());

    clock.advance(EnrollmentWarningInterval - std::chrono::seconds { 1 });
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    CHECK(!window.TakeDueWarning().has_value());
    clock.advance(std::chrono::seconds { 1 });
    CHECK(window.TakeDueWarning().has_value());

    // A driver that was LATE, or a clock that jumped, must not then owe a burst for every
    // interval it slept through: the deadline advances from NOW. The machine keeps asking
    // so it stays on the list.
    clock.advance(std::chrono::minutes { 5 });
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    CHECK(window.TakeDueWarning().has_value());
    CHECK(!window.TakeDueWarning().has_value());

    // And a decision stops it, rather than leaving a deadline that fires once more.
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);
    clock.advance(EnrollmentWarningInterval * 2);
    CHECK(!window.TakeDueWarning().has_value());
}

TEST_CASE("The warning names who waits, for how long, and what to compare", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-b", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);

    clock.advance(std::chrono::seconds { 120 });
    auto const line = window.TakeDueWarning();
    REQUIRE(line.has_value());

    // The age, so somebody reading at 03:00 knows whether this started at 02:59 or at
    // 14:00; the WAITING machines by name, because they are what an operator has to act on;
    // and the listed total apart, because it includes the rows already decided.
    CHECK(Unwrap(line).contains("120s"));
    CHECK(Unwrap(line).contains("1 machine(s) waiting"));
    CHECK(Unwrap(line).contains("joiner-a"));
    CHECK_FALSE(Unwrap(line).contains("joiner-b"));
    CHECK(Unwrap(line).contains("2 listed"));
    // And what to compare before approving, which is the whole of what makes approving safe.
    CHECK(Unwrap(line).contains("comparing the key"));
}

TEST_CASE("A report carries ages as durations and the state the wire spells", "[enrollment][window]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };

    // A window that has recorded nothing reports the mode and no rows, and no seconds of an
    // auto-approve deadline that was never armed.
    auto const quiet = window.Report();
    CHECK(quiet.state == Wire::WireEnrollmentState::Manual);
    CHECK(quiet.autoApproveSecondsLeft == 0);
    CHECK(quiet.pending.empty());

    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    clock.advance(std::chrono::seconds { 45 });
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    clock.advance(std::chrono::seconds { 15 });

    auto const report = window.Report();
    REQUIRE(report.pending.size() == 2);
    CHECK(report.state == Wire::WireEnrollmentState::Manual);
    CHECK(report.autoApproveSecondsLeft == 0);

    // Oldest first, and each age measured from when that row FIRST asked rather than
    // from when the window opened -- a row that arrived late must not read as having
    // waited since the beginning.
    CHECK(report.pending[0].nodeId == "joiner-a");
    CHECK(report.pending[0].firstSeenSecondsAgo == 60);
    CHECK(report.pending[1].nodeId == "joiner-b");
    CHECK(report.pending[1].firstSeenSecondsAgo == 15);
}

TEST_CASE("Waiting joiners raise enrollment-requests-waiting and a decision clears it",
          "[enrollment][window][formation][conditions]")
{
    // LIVE, because watching it clear is the progress an operator is waiting for -- so the case
    // drives it round twice, which a latched row cannot do.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    EnrollmentWindow window { clock, &conditions };

    // NOT clear at construction: the list lives in the leader's memory, and nothing has said yet
    // that this node leads. Clear once it does -- a fresh leadership starts with nobody waiting.
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::NotEvaluated);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::NotEvaluated);
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Clear);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);

    REQUIRE(Offer(window, "laptop") == EnrollDecision::Pending);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Raised);
    REQUIRE(window.Decide("laptop", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Clear);

    // Round again, and cleared the other way: by the row being forgotten.
    REQUIRE(Offer(window, "desk") == EnrollDecision::Pending);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Raised);
    clock.advance(PendingRowLifetime + std::chrono::seconds { 1 });
    CHECK(window.Summary().second == 0);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Clear);
}

TEST_CASE("A window on a node that serves none reports nothing about itself", "[enrollment][window][conditions]")
{
    // Handed no registry -- `main`'s `AddressWhen(servesEnrollment, ...)` on a node with no cluster
    // -- the window touches nothing, so its rows are left for the scope to answer `not-evaluated`
    // rather than a reassuring `clear` about a list nothing can reach.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    EnrollmentWindow window { clock, nullptr };
    REQUIRE(Offer(window, "laptop") == EnrollDecision::Pending);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Undecided);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Undecided);
}

TEST_CASE("An armed window approves a joiner before its deadline and not after", "[enrollment][auto-approve][formation]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    CHECK(Offer(window, "laptop") == EnrollDecision::AutoApprove);

    // `now == deadline` is PAST: the comparison is `now < deadline`, so the last instant is not
    // one more admission.
    clock.advance(std::chrono::minutes { 10 });
    CHECK(Offer(window, "desk") == EnrollDecision::Pending);
    CHECK_FALSE(window.AutoApproveLeft().has_value());
}

TEST_CASE("Re-arming extends from now and off ends it", "[enrollment][auto-approve][formation]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    clock.advance(std::chrono::minutes { 8 });
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    clock.advance(std::chrono::minutes { 5 });
    CHECK(window.AutoApproveLeft() == std::optional { std::chrono::seconds { 300 } });
    CHECK(Offer(window, "laptop") == EnrollDecision::AutoApprove);

    CHECK(window.DisarmAutoApprove() == EnrollControlOutcome::Done);
    CHECK(Offer(window, "desk") == EnrollDecision::Pending);
    CHECK(window.DisarmAutoApprove() == EnrollControlOutcome::AlreadyInForce);
}

TEST_CASE("A zero or over-ceiling duration is refused by name", "[enrollment][auto-approve][formation]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    CHECK(window.ArmAutoApprove(std::chrono::seconds { 0 }).error() == AutoApproveRefusal::Zero);
    CHECK(window.ArmAutoApprove(AutoApproveCeiling + std::chrono::seconds { 1 }).error() == AutoApproveRefusal::OverCeiling);
    CHECK_FALSE(window.AutoApproveLeft().has_value());
    CHECK(window.ArmAutoApprove(AutoApproveCeiling).has_value());
    CHECK(AutoApproveSentence(AutoApproveRefusal::Zero).contains("off"));
    CHECK(AutoApproveSentence(AutoApproveRefusal::OverCeiling).contains("24h"));
}

TEST_CASE("A claimsChanged joiner is never auto-approved", "[enrollment][auto-approve][formation]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Offer(Claim("laptop", "", TheKey()), "10.1.2.3") == EnrollDecision::Pending);
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    CHECK(window.Offer(Claim("laptop", "", KeyOf(0x99)), "10.1.2.3") == EnrollDecision::Pending);
    CHECK(Unwrap(window.Find("laptop")).decision == Wire::EnrollmentDecision::Pending);
    // The control: the machine that asked first is the one the deadline admits.
    CHECK(window.Offer(Claim("laptop", "", TheKey()), "10.1.2.3") == EnrollDecision::AutoApprove);
}

TEST_CASE("A leader restart ends the window", "[enrollment][auto-approve][formation]")
{
    core::platform::ManualClock clock;
    {
        EnrollmentWindow before { clock };
        REQUIRE(before.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    }
    EnrollmentWindow after { clock }; // held in memory and nowhere else
    CHECK_FALSE(after.AutoApproveLeft().has_value());
    CHECK(Offer(after, "laptop") == EnrollDecision::Pending);
}

TEST_CASE("A demotion ends the window, and leading again does not bring it back", "[enrollment][auto-approve][formation]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    CHECK(window.AutoApproveLeft().has_value());
    window.OnRoleChanged(Distributed::SchedulerRole::Follower, "10.0.0.1:6674");
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    CHECK_FALSE(window.AutoApproveLeft().has_value());
    CHECK(Offer(window, "laptop") == EnrollDecision::Pending);
}

TEST_CASE("An armed window raises enrollment-window-open naming its deadline and when it was armed",
          "[enrollment][auto-approve][formation][conditions]")
{
    // Absolute instants, never minutes left: the detail is a snapshot, and "15 min left" would
    // still say fifteen minutes an hour later.
    using namespace std::chrono_literals;
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock { std::chrono::sys_days { 2026y / 9 / 29 } + 8h + 45min };
    NodeConditions conditions;
    EnrollmentWindow window { clock, &conditions, nullptr, wallClock };
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 15 }).has_value());
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Raised);
    auto const detail = DetailOf(conditions, NodeCondition::EnrollmentWindowOpen);
    CHECK(detail.contains("auto-approve until 2026-09-29T09:00:00Z (armed 2026-09-29T08:45:00Z)"));
    CHECK_FALSE(detail.contains("min left"));
    REQUIRE(window.DisarmAutoApprove() == EnrollControlOutcome::Done);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);

    // And a deadline that passes clears it at the next call, with no timer.
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 1 }).has_value());
    clock.advance(std::chrono::minutes { 1 });
    CHECK_FALSE(window.AutoApproveLeft().has_value());
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);
}

TEST_CASE("An armed window's stated deadline is never earlier than the one that admits",
          "[enrollment][auto-approve][formation][conditions]")
{
    // Armed 700 ms into a second: the window admits until 08:50:00.700, so the detail rounds the
    // deadline UP to 08:50:01 and the arming DOWN to 08:45:00. Floored, it said 08:50:00, a
    // deadline the window was still admitting past.
    using namespace std::chrono_literals;
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock { std::chrono::sys_days { 2026y / 9 / 29 } + 8h + 45min + 700ms };
    NodeConditions conditions;
    EnrollmentWindow window { clock, &conditions, nullptr, wallClock };
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 5 }).has_value());
    CHECK(DetailOf(conditions, NodeCondition::EnrollmentWindowOpen)
              .contains("auto-approve until 2026-09-29T08:50:01Z (armed 2026-09-29T08:45:00Z)"));
}

TEST_CASE("A deadline that passes on a quiet leader is lowered by the warning tick alone",
          "[enrollment][auto-approve][formation][conditions]")
{
    // Nobody polls and nobody lists: the one-second tick is the only call. It must end the
    // condition, or an Alert says a window is open indefinitely after it shut.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    EnrollmentWindow window { clock, &conditions };
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 5 }).has_value());
    REQUIRE(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Raised);
    // Named no wall clock, the window reads the system's: the detail still states a deadline.
    CHECK(DetailOf(conditions, NodeCondition::EnrollmentWindowOpen).starts_with("auto-approve until "));

    clock.advance(std::chrono::minutes { 30 });
    (void) window.TakeDueWarning();
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);
}

TEST_CASE("A report says the mode, the seconds left and which rows the window admitted",
          "[enrollment][auto-approve][formation]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(Offer(window, "early") == EnrollDecision::Pending);
    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    REQUIRE(Offer(window, "laptop") == EnrollDecision::AutoApprove);
    REQUIRE(window.Decide("laptop", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    window.MarkAutoApproved("laptop");
    clock.advance(std::chrono::seconds { 30 });

    auto const report = window.Report();
    CHECK(report.state == Wire::WireEnrollmentState::AutoApprove);
    CHECK(report.autoApproveSecondsLeft == 570);
    REQUIRE(report.pending.size() == 2);
    CHECK_FALSE(report.pending[0].autoApprovedArmedSecondsAgo.has_value());
    CHECK(report.pending[1].autoApprovedArmedSecondsAgo == std::optional<std::uint64_t> { 30 });
    CHECK(window.Summary().first == Wire::WireEnrollmentState::AutoApprove);
}

TEST_CASE("A demotion forgets every row, and a follower's rows name the leader rather than read clear",
          "[enrollment][window][formation][conditions]")
{
    // The list lives in the leader's memory alone. A follower's empty list says nothing about who is
    // waiting, so its rows read not-evaluated, naming the leader to ask -- never `clear`.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    EnrollmentWindow window { clock, &conditions };
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    REQUIRE(window.Offer(Claim("waiting", "", KeyOf(0x31)), "192.0.2.1") == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("approved", "", KeyOf(0x32)), "192.0.2.2") == EnrollDecision::Pending);
    REQUIRE(window.Decide("approved", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    REQUIRE(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Raised);

    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    CHECK(window.Report().pending.size() == 2);

    window.OnRoleChanged(Distributed::SchedulerRole::Follower, "10.0.0.1:6674");
    CHECK(window.Report().pending.empty());
    for (auto const condition: { NodeCondition::EnrollmentRequestsWaiting, NodeCondition::EnrollmentWindowOpen })
    {
        CHECK(conditions.StateOf(condition) == Wire::ConditionState::NotEvaluated);
        CHECK(DetailOf(conditions, condition).contains("ask the leader at 10.0.0.1:6674"));
    }

    // With no leader known, it says so rather than naming one.
    window.OnRoleChanged(Distributed::SchedulerRole::Undecided, {});
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::NotEvaluated);
    CHECK(DetailOf(conditions, NodeCondition::EnrollmentRequestsWaiting).contains("knows no leader yet"));

    // And leading again answers from the list, empty now.
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Clear);
    CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == Wire::ConditionState::Clear);
}

TEST_CASE("Re-polling rows from a second address does not move them out of the first address's bound",
          "[enrollment][window][security]")
{
    // The probe that measured the evasion: four rows from A, each re-polled from B (same id, same
    // key), then A again. The displayed address follows the poll; the bound does not, so A is
    // still at its cap.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
    {
        auto const id = std::format("junk-{}", index);
        REQUIRE(window.Offer(Claim(id, "", KeyOf(0x66)), "198.51.100.7") == EnrollDecision::Pending);
        REQUIRE(window.Offer(Claim(id, "", KeyOf(0x66)), "2001:db8::7") == EnrollDecision::Pending);
        REQUIRE(Unwrap(window.Find(id)).peerId == "2001:db8::7");
        // The row says where it FIRST asked from, which is what the bound charges it to.
        CHECK(Unwrap(window.Find(id)).firstPeerId == "198.51.100.7");
    }
    CHECK(window.Offer(Claim("junk-more", "", KeyOf(0x66)), "198.51.100.7") == EnrollDecision::HostFull);
    CHECK(window.Summary().second == MaxPendingEnrollmentsPerHost);

    // And the second address, which created nothing, has its whole bound.
    CHECK(window.Offer(Claim("from-b", "", KeyOf(0x67)), "2001:db8::7") == EnrollDecision::Pending);
}

TEST_CASE("An IPv4-mapped address counts against the same bound as its IPv4 form", "[enrollment][window][security]")
{
    // A dual-stack listener reports an IPv4 client as `::ffff:a.b.c.d`; one machine, one bound.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
        REQUIRE(window.Offer(Claim(std::format("junk-{}", index), "", KeyOf(0x66)),
                             index % 2 == 0 ? "198.51.100.7" : "::ffff:198.51.100.7")
                == EnrollDecision::Pending);
    CHECK(window.Offer(Claim("junk-more", "", KeyOf(0x66)), "::ffff:198.51.100.7") == EnrollDecision::HostFull);
    CHECK(window.Offer(Claim("junk-more", "", KeyOf(0x66)), "198.51.100.7") == EnrollDecision::HostFull);
    // Recorded folded, whichever spelling created the row: the list shows one host, as the bound
    // counts one.
    CHECK(Unwrap(window.Find("junk-1")).firstPeerId == "198.51.100.7");
    CHECK(Unwrap(window.Find("junk-1")).peerId == "::ffff:198.51.100.7");
    // The control: a different IPv4 address is a different machine.
    CHECK(window.Offer(Claim("other", "", KeyOf(0x66)), "198.51.100.8") == EnrollDecision::Pending);
}

TEST_CASE("A row that lapses between another host's rows leaves every host's bound where it was",
          "[enrollment][window][security]")
{
    // The reviewer's probe: B's row, older, lapses while A's three do not. What the bound counts
    // must follow the rows that were KEPT -- charged by index to whatever sits there after the
    // sweep, A would be charged B's lapsed row and refused one short.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Offer(Claim("b0", "", KeyOf(0x61)), "2001:db8::7") == EnrollDecision::Pending);
    clock.advance(std::chrono::minutes { 6 });
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost - 1))
        REQUIRE(window.Offer(Claim(std::format("a{}", index), "", KeyOf(0x62)), "198.51.100.7") == EnrollDecision::Pending);
    clock.advance(std::chrono::minutes { 5 });

    CHECK(window.Offer(Claim("a-last", "", KeyOf(0x62)), "198.51.100.7") == EnrollDecision::Pending);
    CHECK_FALSE(window.Find("b0").has_value());
    CHECK(window.Offer(Claim("a-over", "", KeyOf(0x62)), "198.51.100.7") == EnrollDecision::HostFull);
}

TEST_CASE("A demotion leaves no host charged for rows it forgot", "[enrollment][window][security]")
{
    // Rows from A and B, then a demotion drops every row and a promotion leads again. A row B adds
    // first must not inherit A's charge from before: A's bound is whole.
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
        REQUIRE(window.Offer(Claim(std::format("a{}", index), "", KeyOf(0x62)), "198.51.100.7") == EnrollDecision::Pending);
    REQUIRE(window.Offer(Claim("b0", "", KeyOf(0x61)), "2001:db8::7") == EnrollDecision::Pending);

    window.OnRoleChanged(Distributed::SchedulerRole::Follower, "10.0.0.1:6674");
    window.OnRoleChanged(Distributed::SchedulerRole::Leader, {});
    REQUIRE(window.Report().pending.empty());

    REQUIRE(window.Offer(Claim("b1", "", KeyOf(0x61)), "2001:db8::7") == EnrollDecision::Pending);
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
        CHECK(window.Offer(Claim(std::format("again{}", index), "", KeyOf(0x62)), "198.51.100.7")
              == EnrollDecision::Pending);
    CHECK(window.Offer(Claim("again-over", "", KeyOf(0x62)), "198.51.100.7") == EnrollDecision::HostFull);
}
