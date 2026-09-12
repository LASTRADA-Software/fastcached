// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// One offer from a machine that is not already on the list.
/// @param window The window to ask.
/// @param id The identity the joiner claims.
/// @return What the window decided.
[[nodiscard]] EnrollDecision Offer(EnrollmentWindow& window, std::string_view id)
{
    return window.Offer(id, "10.0.0.9:7100", "10.0.0.9");
}
} // namespace

TEST_CASE("A window is closed until somebody opens it, and a restart is what closes it again", "[enrollment][window]")
{
    ManualClock clock;
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
    ManualClock clock;
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
    ManualClock clock;
    EnrollmentWindow window { clock };

    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    clock.Advance(std::chrono::seconds { 90 });

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
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    for (std::size_t index = 0; index < MaxPendingEnrollments; ++index)
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
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    for (std::size_t index = 0; index < MaxPendingEnrollments; ++index)
        REQUIRE(window.Offer(std::format("junk-{}", index), "10.0.0.9:7100", "203.0.113.7") == EnrollDecision::Pending);

    // The real machine arrives second and is not merely refused -- it leaves NO ROW, so
    // it cannot be approved, and an operator reading `--enroll-list` sees sixty-four
    // strangers and no sign of the joiner they are waiting for.
    CHECK(window.Offer("joiner-real", "10.0.0.4:6680", "10.0.0.4") == EnrollDecision::Full);
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
    CHECK(window.Offer("joiner-real", "10.0.0.4:6680", "10.0.0.4") == EnrollDecision::Pending);
}

TEST_CASE("A joiner polls, so repeat offers count attempts and refresh what it claims", "[enrollment][window]")
{
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);

    REQUIRE(window.Offer("joiner-a", "10.0.0.9:7100", "10.0.0.9") == EnrollDecision::Pending);
    REQUIRE(window.Offer("joiner-a", "10.0.0.9:7100", "10.0.0.9") == EnrollDecision::Pending);
    REQUIRE(window.Offer("joiner-a", "node-a.example:7100", "198.51.100.4") == EnrollDecision::Pending);

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
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(window.Offer("joiner-a", "10.0.0.9:7100", "10.0.0.9") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);

    // **The assertion that discriminates.** `--enroll-list` and `--enroll-approve` are
    // two one-shot processes minutes apart, and the joiner polls every couple of seconds
    // in between -- so while the claim refreshed unconditionally, the endpoint handed to
    // `ClusterAdmit` was not necessarily the endpoint the eye that approved it had read.
    // A test asserting only that the row still exists passes under that; this one
    // asserts the VALUE, which is the thing that travels.
    REQUIRE(window.Offer("joiner-a", "evil.example:7100", "198.51.100.4") == EnrollDecision::Approved);

    auto const entry = window.Find("joiner-a");
    REQUIRE(entry.has_value());
    CHECK(Unwrap(entry).raftEndpoint == "10.0.0.9:7100");
    CHECK(Unwrap(entry).peerId == "10.0.0.9");

    // Marked, and NEVER gated -- #242 settled that comparing hosts refuses the
    // documented setup and stops only a third host. The poll above was still answered;
    // what it changed is what an operator sees in the row.
    CHECK(Unwrap(entry).claimsChanged == 1);
}

TEST_CASE("The cluster key is spendable once, and a second collect is refused", "[enrollment][window]")
{
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);

    // The hand-over, and it is reachable exactly once per approval.
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Approved);
    REQUIRE(Unwrap(window.Find("joiner-a")).decision == Wire::EnrollmentDecision::Collected);

    // **`Collected`, not `Approved`.** The two shared an enumerator until this case
    // existed, and the caller then served the key on every poll with only a counter
    // gated -- so a second hand-over was invisible: the tally stayed at one and the real
    // joiner still got its key on its next poll. Asserting "the window still answers
    // about this id" passes under that; asserting WHICH answer is what does not.
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Collected);
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Collected);
}

TEST_CASE("A claim the caller could not serve is given back rather than spent", "[enrollment][window]")
{
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);

    // The spend is taken BEFORE the caller has the key in hand -- it has to be, or two
    // polls arriving together are both served -- so a caller that then cannot read its
    // key file must put it back. Without this, one transient permission error costs a
    // machine the only collection it has.
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Approved);
    CHECK(window.ReturnClaim("joiner-a") == ClaimReturn::Returned);
    CHECK(Unwrap(window.Find("joiner-a")).decision == Wire::EnrollmentDecision::Approved);
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Approved);

    // And it returns only what it took: a row that never collected, and an id nobody has
    // heard of, are both declined rather than moved. A `ReturnClaim` that walked a
    // `Pending` row back to `Approved` would be an approval this node invented.
    //
    // **The two nothings are told APART**, and that is the assertion rather than
    // thoroughness: they are different events with different remedies -- a window
    // somebody closed, against a row somebody decided about while this claim was out --
    // and only the second says a machine was stranded by a race. A predicate answering
    // one `false` for both is what made the caller discard it.
    CHECK(window.ReturnClaim("nobody") == ClaimReturn::NoSuchRow);
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    CHECK(window.ReturnClaim("joiner-b") == ClaimReturn::AlreadyMoved);
    CHECK(Unwrap(window.Find("joiner-b")).decision == Wire::EnrollmentDecision::Pending);

    // The race this names, arranged rather than raced for: `joiner-a`'s claim is out
    // (its row is `Collected` from the offer above), an operator REJECTS it, and the
    // return then finds it moved. The claim is not silently restored over that
    // decision -- which is the half that keeps this from being a way to undo a
    // rejection, and the reason the guard is `!= Collected` rather than `== Approved`.
    REQUIRE(Unwrap(window.Find("joiner-a")).decision == Wire::EnrollmentDecision::Collected);
    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);
    CHECK(window.ReturnClaim("joiner-a") == ClaimReturn::AlreadyMoved);
    CHECK(Unwrap(window.Find("joiner-a")).decision == Wire::EnrollmentDecision::Rejected);
}

TEST_CASE("A second decision about a settled id is refused rather than repeated", "[enrollment][window]")
{
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);

    REQUIRE(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::AlreadyInForce);

    // **But once the key has gone out, an approval is ACCEPTED and re-arms exactly one
    // more collection.** That is the whole recovery path for a joiner whose reply was
    // lost: one operator command, on the id they already approved.
    //
    // While `Collected` was folded into `Approved` for this question, this answered
    // `AlreadyInForce` and the only remedy its refusal offered was `--cluster-forget` --
    // a quorum change to recover from a dropped packet, followed by a re-approve that
    // consensus refuses because the member is already there. Four commands, two of them
    // counter-intuitive, none documented.
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Approved);
    REQUIRE(Unwrap(window.Find("joiner-a")).decision == Wire::EnrollmentDecision::Collected);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done);
    CHECK(Unwrap(window.Find("joiner-a")).decision == Wire::EnrollmentDecision::Approved);

    // Re-armed for exactly ONE: the second collection is served and the third is not,
    // so the recovery does not quietly become the idempotent behaviour it replaced.
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Approved);
    CHECK(Offer(window, "joiner-a") == EnrollDecision::Collected);

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
    ManualClock clock;
    EnrollmentWindow window { clock };

    // Two refusals that a `bool` would render alike, and they send an operator to
    // different places: one says open a window, the other says check the id.
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Closed);
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    CHECK(window.Decide("joiner-a", Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::UnknownSubject);
}

TEST_CASE("The open warning is due immediately and then once per interval, never in a burst", "[enrollment][window]")
{
    ManualClock clock;
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

    clock.Advance(EnrollmentWarningInterval - std::chrono::seconds { 1 });
    CHECK(!window.TakeDueWarning().has_value());
    clock.Advance(std::chrono::seconds { 1 });
    CHECK(window.TakeDueWarning().has_value());

    // A driver that was LATE, or a clock that jumped, must not then owe a burst for
    // every interval it slept through: the deadline advances from NOW. Ten intervals
    // pass and exactly one line is owed, which is the reading an operator wants --
    // *it is still open* -- rather than ten copies of it.
    clock.Advance(EnrollmentWarningInterval * 10);
    CHECK(window.TakeDueWarning().has_value());
    CHECK(!window.TakeDueWarning().has_value());

    // And closing stops it, rather than leaving a deadline that fires once more.
    REQUIRE(window.Close() == EnrollControlOutcome::Done);
    clock.Advance(EnrollmentWarningInterval * 2);
    CHECK(!window.TakeDueWarning().has_value());
}

TEST_CASE("The warning states the age and how many are waiting, so a log line can be acted on", "[enrollment][window]")
{
    ManualClock clock;
    EnrollmentWindow window { clock };
    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    REQUIRE(window.Decide("joiner-b", Wire::EnrollmentDecision::Rejected) == EnrollControlOutcome::Done);

    clock.Advance(std::chrono::seconds { 120 });
    auto const line = window.TakeDueWarning();
    REQUIRE(line.has_value());

    // The age, so somebody reading at 03:00 knows whether this started at 02:59 or at
    // 14:00; and the two counts apart, because WAITING is what an operator has to act
    // on and LISTED includes the rows they have already decided.
    CHECK(Unwrap(line).contains("120s"));
    CHECK(Unwrap(line).contains("1 request(s) waiting"));
    CHECK(Unwrap(line).contains("2 listed"));
    CHECK(Unwrap(line).contains("CLEARTEXT"));
}

TEST_CASE("A report carries ages as durations and the state the wire spells", "[enrollment][window]")
{
    ManualClock clock;
    EnrollmentWindow window { clock };

    // A closed window reports a zero age rather than an age since some instant that
    // means nothing, and holds no rows to report.
    auto const shut = window.Report();
    CHECK(shut.state == Wire::WireEnrollmentState::Closed);
    CHECK(shut.openForSeconds == 0);
    CHECK(shut.pending.empty());

    REQUIRE(window.Open() == EnrollControlOutcome::Done);
    REQUIRE(Offer(window, "joiner-a") == EnrollDecision::Pending);
    clock.Advance(std::chrono::seconds { 45 });
    REQUIRE(Offer(window, "joiner-b") == EnrollDecision::Pending);
    clock.Advance(std::chrono::seconds { 15 });

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
