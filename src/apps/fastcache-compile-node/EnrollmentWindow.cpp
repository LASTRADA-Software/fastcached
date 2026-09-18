// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

bool RosterRecordsJoiner(Cluster::Roster const& roster,
                         std::string_view nodeId,
                         Ed25519PublicKey const& key,
                         Wire::EnrollRole role)
{
    auto const& row = EnrollRoleRowFor(role);
    if (row.principal.has_value())
        return std::ranges::any_of(roster.principals, [&](Cluster::ClusterPrincipal const& principal) {
            return principal.id == nodeId && principal.publicKey == key && principal.role == *row.principal;
        });
    return std::ranges::any_of(roster.members, [&](Cluster::RosterMember const& member) {
        return member.id == nodeId && member.publicKey == std::optional { key };
    });
}

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
    // Live: it clears on `--enroll-close`, which is what an operator watching this row is waiting
    // to see (#1364). The repeating Warn stays; this is what somebody not reading the log can ask
    // for. Under the lock, so a racing open and close cannot report in the other order.
    if (_conditions != nullptr)
        _conditions->Raise(NodeCondition::EnrollmentWindowOpen,
                           "the enrollment window is open: any machine that can reach this node's 0xFC port may ask to "
                           "join, and approving one hands it this cluster's key in cleartext");
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
    if (_conditions != nullptr)
        _conditions->Clear(NodeCondition::EnrollmentWindowOpen);
    return EnrollControlOutcome::Done;
}

EnrollDecision EnrollmentWindow::Offer(JoinerClaim const& claim, std::string_view peerId)
{
    std::scoped_lock const guard { _mutex };
    if (!_open)
        return EnrollDecision::Closed;

    if (auto* const existing = FindLocked(claim.nodeId); existing != nullptr)
    {
        ++existing->attempts;

        // **The key and the role are the row's, whatever is asked later** (#178). A poll under
        // this id with another key is ANOTHER MACHINE: counted, answered `Pending`, never
        // recorded, and never handed what an approval of the first one leads to. Refreshing
        // them would let whoever polled last swap the key an operator compared on the list for
        // one they never saw, in the minutes between `--enroll-list` and `--enroll-approve`.
        auto const sameMachine = existing->publicKey == claim.publicKey && existing->role == claim.role;
        if (!sameMachine)
        {
            ++existing->claimsChanged;
            return EnrollDecision::Pending;
        }

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
            existing->raftEndpoint = std::string { claim.raftEndpoint };
            existing->peerId = std::string { peerId };
        }
        else if (existing->raftEndpoint != claim.raftEndpoint || existing->peerId != peerId)
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
                // Every poll, and nothing is spent: what an approval leads to is the roster,
                // which is no secret (#178). A joiner whose reply was lost simply asks again,
                // which is the whole recovery path and needs no operator.
                return EnrollDecision::Approved;
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

    _pending.push_back(Wire::EnrollmentPendingEntry { .nodeId = std::string { claim.nodeId },
                                                      .raftEndpoint = std::string { claim.raftEndpoint },
                                                      .peerId = std::string { peerId },
                                                      .firstSeenSecondsAgo = 0,
                                                      .attempts = 1,
                                                      .claimsChanged = 0,
                                                      .decision = Wire::EnrollmentDecision::Pending,
                                                      .role = claim.role,
                                                      .publicKey = claim.publicKey,
                                                      .rosterFingerprint = std::nullopt });
    _firstSeen.push_back(_clock.Now());
    return EnrollDecision::Pending;
}

void EnrollmentWindow::NoteServed(std::string_view nodeId,
                                  std::array<std::byte, Wire::RosterFingerprintBytes> const& fingerprint)
{
    std::scoped_lock const guard { _mutex };
    // A row that went -- the window closed between the answer and this -- has nobody to show
    // it to, so there is nothing to record and nothing to report.
    if (auto* const entry = FindLocked(nodeId); entry != nullptr)
        entry->rosterFingerprint = fingerprint;
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
    for (auto const index: std::views::iota(std::size_t { 0 }, report.pending.size()))
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
                       "0xFC port may ask to join, and approving one admits it under the key it asked with -- compare "
                       "that key with the one the machine printed before approving. {} request(s) waiting, {} listed "
                       "in total. Close it with --enroll-close; a restart closes it too, because the window is held "
                       "in memory and nowhere else.",
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
