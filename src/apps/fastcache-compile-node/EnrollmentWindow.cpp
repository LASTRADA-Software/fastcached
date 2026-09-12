// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

EnrollControlOutcome EnrollmentWindow::Open()
{
    std::scoped_lock const guard { _mutex };
    if (_open)
        return EnrollControlOutcome::AlreadyInForce;

    _open = true;
    _openedAt = _clock.Now();
    // Due NOW rather than one interval from now: the open is itself the thing worth
    // saying, and a driver that asks a moment later gets the first line immediately.
    _warnDueAt = _openedAt;
    return EnrollControlOutcome::Done;
}

EnrollControlOutcome EnrollmentWindow::Close()
{
    std::scoped_lock const guard { _mutex };
    if (!_open)
        return EnrollControlOutcome::AlreadyInForce;

    _open = false;
    _pending.clear();
    _firstSeen.clear();
    return EnrollControlOutcome::Done;
}

EnrollDecision EnrollmentWindow::Offer(std::string_view nodeId, std::string_view raftEndpoint, std::string_view peerId)
{
    std::scoped_lock const guard { _mutex };
    if (!_open)
        return EnrollDecision::Closed;

    if (auto* const existing = FindLocked(nodeId); existing != nullptr)
    {
        ++existing->attempts;

        // **A row a person has decided about is a RECORD of what they decided about,
        // and stops tracking the machine.** The claim refreshes only while the row is
        // still `Pending`.
        //
        // The earlier reading refreshed unconditionally, on the honest argument that an
        // operator should read whichever address was most recently true. That argument
        // is about DISPLAY and it does not carry to what gets COMMITTED: `--enroll-list`
        // and `--enroll-approve` are two one-shot processes minutes apart and a joiner
        // polls every couple of seconds in between, so the endpoint `ClusterAdmit` was
        // handed was not necessarily the endpoint the eye that approved it had read.
        // The gate here is a person looking at a list, so the value they acted on has
        // to be the value that travels.
        //
        // A machine that has genuinely moved is therefore refused and enrolled again --
        // one operator action, and the same answer `--cluster-admit` already gives for
        // recording a move.
        if (existing->decision == Wire::EnrollmentDecision::Pending)
        {
            existing->raftEndpoint = std::string { raftEndpoint };
            existing->peerId = std::string { peerId };
        }
        else if (existing->raftEndpoint != raftEndpoint || existing->peerId != peerId)
        {
            // Marked and counted, never gated. #242 settled that comparing hosts refuses
            // the documented setup -- DNS names, a node dialling itself, NAT, VPN,
            // multi-homing -- and stops only a third host, so a disagreement is shown to
            // the person rather than acted on by the node. What makes it worth showing
            // is that it is also the signature of somebody else claiming a decided id.
            ++existing->claimsChanged;
        }

        switch (existing->decision)
        {
            case Wire::EnrollmentDecision::Pending:
                return EnrollDecision::Pending;
            case Wire::EnrollmentDecision::Approved:
                // **The SPEND, taken here and not after the key is read.** It is the only
                // transition that answers `Approved`, and it happens under this lock, so
                // two polls arriving together cannot both take it -- which is the
                // property, and it cannot be had by reading the key first and marking
                // afterwards.
                //
                // A caller that then fails to produce the key calls `ReturnClaim`, so a
                // transient fault does not burn the one collection this id has. Taking
                // and returning rather than deferring the take is what keeps the race
                // closed: between the two, a concurrent poll is refused and retries,
                // where a deferred take would serve the key twice.
                existing->decision = Wire::EnrollmentDecision::Collected;
                return EnrollDecision::Approved;
            case Wire::EnrollmentDecision::Collected:
                return EnrollDecision::Collected;
            case Wire::EnrollmentDecision::Rejected:
                return EnrollDecision::Rejected;
        }
        return EnrollDecision::Pending;
    }

    // The bound is asked only of a request that would ADD a row. A machine already on
    // the list goes on polling whether or not the list is full, which is what keeps a
    // flood from silencing the joiners that arrived before it.
    if (_pending.size() >= MaxPendingEnrollments)
        return EnrollDecision::Full;

    _pending.push_back(Wire::EnrollmentPendingEntry { .nodeId = std::string { nodeId },
                                                      .raftEndpoint = std::string { raftEndpoint },
                                                      .peerId = std::string { peerId },
                                                      .firstSeenSecondsAgo = 0,
                                                      .attempts = 1,
                                                      .claimsChanged = 0,
                                                      .decision = Wire::EnrollmentDecision::Pending });
    _firstSeen.push_back(_clock.Now());
    return EnrollDecision::Pending;
}

ClaimReturn EnrollmentWindow::ReturnClaim(std::string_view nodeId)
{
    std::scoped_lock const guard { _mutex };
    auto* const entry = FindLocked(nodeId);
    if (entry == nullptr)
        return ClaimReturn::NoSuchRow;
    // Two nothings, told apart, because they are different events: a window somebody
    // closed, and a row somebody decided about while this claim was out. Only the
    // second says a machine was stranded by a race.
    if (entry->decision != Wire::EnrollmentDecision::Collected)
        return ClaimReturn::AlreadyMoved;

    entry->decision = Wire::EnrollmentDecision::Approved;
    return ClaimReturn::Returned;
}

std::optional<Wire::EnrollmentPendingEntry> EnrollmentWindow::Find(std::string_view nodeId) const
{
    std::scoped_lock const guard { _mutex };
    auto const found = std::ranges::find(_pending, nodeId, &Wire::EnrollmentPendingEntry::nodeId);
    if (found == _pending.end())
        return std::nullopt;

    auto copy = *found;
    copy.firstSeenSecondsAgo = SecondsSince(_firstSeen[static_cast<std::size_t>(found - _pending.begin())]);
    return copy;
}

EnrollControlOutcome EnrollmentWindow::Decide(std::string_view nodeId, Wire::EnrollmentDecision decision)
{
    std::scoped_lock const guard { _mutex };
    if (!_open)
        return EnrollControlOutcome::Closed;

    auto* const entry = FindLocked(nodeId);
    if (entry == nullptr)
        return EnrollControlOutcome::UnknownSubject;

    // **Approving a `Collected` row RE-ARMS exactly one more collection, and that is the
    // whole recovery path for a joiner whose reply was lost.**
    //
    // The key is spendable once, so a lost TCP reply strands a machine an operator has
    // already admitted. This is what un-strands it: one deliberate operator act, on the
    // id they already approved, auditable in the log and in both counters -- the
    // hand-over tally rises again because the key genuinely goes out again, and that is
    // the honest reading rather than a second event hidden inside the first.
    //
    // Folding `Collected` into `Approved` here -- which this did -- made that command
    // answer *already in force*, and the only instruction the refusal then offered was
    // `--cluster-forget`: a QUORUM CHANGE to recover from a dropped packet, with a
    // re-approve afterwards that consensus refuses because the member is already in
    // `ClusterState`, so the real recovery was four commands and two of them
    // counter-intuitive. A remedy that expensive is one an operator works around.
    if (entry->decision == decision)
        return EnrollControlOutcome::AlreadyInForce;

    // A `Rejected` entry may be approved afterwards: an operator who refused the wrong
    // row and corrected it within the same window would otherwise have to close the
    // window and have the joiner re-ask, which loses every other row with it.
    entry->decision = decision;
    return EnrollControlOutcome::Done;
}

Wire::EnrollmentReport EnrollmentWindow::Report() const
{
    std::scoped_lock const guard { _mutex };
    Wire::EnrollmentReport report { .state = _open ? Wire::WireEnrollmentState::Open : Wire::WireEnrollmentState::Closed,
                                    .openForSeconds = _open ? SecondsSince(_openedAt) : 0,
                                    .pending = _pending };
    for (std::size_t index = 0; index < report.pending.size(); ++index)
        report.pending[index].firstSeenSecondsAgo = SecondsSince(_firstSeen[index]);
    return report;
}

std::pair<Wire::WireEnrollmentState, std::uint32_t> EnrollmentWindow::Summary() const
{
    std::scoped_lock const guard { _mutex };
    return { _open ? Wire::WireEnrollmentState::Open : Wire::WireEnrollmentState::Closed,
             static_cast<std::uint32_t>(_pending.size()) };
}

std::optional<std::string> EnrollmentWindow::TakeDueWarning()
{
    std::scoped_lock const guard { _mutex };
    if (!_open)
        return std::nullopt;

    auto const now = _clock.Now();
    if (now < _warnDueAt)
        return std::nullopt;

    // Advanced from NOW rather than by adding an interval to the old deadline: a driver
    // that was late, or a clock that jumped, must not then owe a burst of warnings for
    // the intervals it slept through. The reading an operator wants is *it is still
    // open*, and one line per interval of real time is what says that.
    _warnDueAt = now + EnrollmentWarningInterval;

    auto const waiting =
        std::ranges::count(_pending, Wire::EnrollmentDecision::Pending, &Wire::EnrollmentPendingEntry::decision);
    return std::format("the enrollment window is OPEN and has been for {}s: any machine that can reach this node's "
                       "0xFC port may ask to join, and approving one hands it this cluster's key IN CLEARTEXT. "
                       "{} request(s) waiting, {} listed in total. Close it with --enroll-close; a restart closes "
                       "it too, because the window is held in memory and nowhere else.",
                       SecondsSince(_openedAt),
                       waiting,
                       _pending.size());
}

Wire::EnrollmentPendingEntry* EnrollmentWindow::FindLocked(std::string_view nodeId) noexcept
{
    auto const found = std::ranges::find(_pending, nodeId, &Wire::EnrollmentPendingEntry::nodeId);
    return found == _pending.end() ? nullptr : &*found;
}

std::uint64_t EnrollmentWindow::SecondsSince(TimePoint since) const noexcept
{
    auto const now = _clock.Now();
    if (now <= since)
        return 0;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - since).count());
}

} // namespace FastCache::Node
